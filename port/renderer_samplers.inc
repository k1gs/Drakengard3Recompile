/* Fragment sampler descriptors are immutable until the submission fence.
 * Each heap holds 128 complete 16-unit tables (D3D12's 2048-sampler limit).
 * More distinct states allocate another retained page, never overwrite an
 * earlier draw or silently substitute a sampler. */
#define PORT_SAMPLER_TABLES 128
typedef struct PortSamplerPage {
    struct PortSamplerPage* next;
    ID3D12DescriptorHeap* heap;
    unsigned used;
    D3D12_SAMPLER_DESC tables[PORT_SAMPLER_TABLES][PORT_TEXTURE_UNITS];
} PortSamplerPage;
static PortSamplerPage* port_sampler_pages;
static u32 port_sampler_increment;
static uint64_t port_sampler_tables_created,port_sampler_tables_reused;

static D3D12_TEXTURE_ADDRESS_MODE port_sampler_wrap(u32 value)
{
    switch(value) {
    case 1:return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 2:return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3:return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4:return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 6:return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default:return (D3D12_TEXTURE_ADDRESS_MODE)0;
    }
}

static int port_decode_sampler(const D3D12DrawRecord* dr,unsigned unit,D3D12_SAMPLER_DESC* sd)
{
    memset(sd,0,sizeof(*sd));sd->ComparisonFunc=D3D12_COMPARISON_FUNC_NEVER;
    sd->MaxAnisotropy=1;
    if(!dr->tex[unit].set) {
        sd->AddressU=sd->AddressV=sd->AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        return 1;
    }
    u32 filter=dr->tex[unit].sampler_filter,address=dr->tex[unit].sampler_address;
    u32 control=dr->tex[unit].sampler_control,border=dr->tex[unit].sampler_border;
    unsigned minf=(filter>>16)&7,magf=(filter>>24)&7;
    if(minf<1 || minf>6 || magf<1 || magf>2) return 0;
    sd->AddressU=port_sampler_wrap(address&15);
    sd->AddressV=port_sampler_wrap((address>>8)&15);
    sd->AddressW=port_sampler_wrap((address>>16)&15);
    if(!sd->AddressU || !sd->AddressV || !sd->AddressW) return 0;
    unsigned min_linear=minf==2 || minf==4 || minf==6;
    unsigned mip_linear=minf==5 || minf==6;
    sd->Filter=(D3D12_FILTER)((min_linear?0x10:0)|(magf==2?4:0)|(mip_linear?1:0));
    int bias=(int)(filter&0x1FFFu);if(bias&0x1000) bias-=0x2000;
    sd->MipLODBias=(float)bias/256.0f;
    if(minf>=3) {
        sd->MinLOD=(float)((control>>19)&0xFFFu)/256.0f;
        sd->MaxLOD=(float)((control>>7)&0xFFFu)/256.0f;
        if(sd->MaxLOD<sd->MinLOD) return 0;
    }
    static const unsigned aniso[8]={1,2,4,6,8,10,12,16};
    sd->MaxAnisotropy=aniso[(control>>4)&7];
    if(sd->MaxAnisotropy>1) {
        if(!min_linear || magf!=2) return 0;
        /* MIN_MAG_ANISOTROPIC_MIP_POINT=0x54; older SDK headers only name
         * its trilinear counterpart ANISOTROPIC=0x55. Preserve mip selection. */
        sd->Filter=(D3D12_FILTER)(0x54u|(mip_linear?1u:0u));
    }
    sd->BorderColor[0]=(float)((border>>16)&255)/255.0f;
    sd->BorderColor[1]=(float)((border>>8)&255)/255.0f;
    sd->BorderColor[2]=(float)(border&255)/255.0f;
    sd->BorderColor[3]=(float)(border>>24)/255.0f;
    return 1;
}

/* Called only after wait_for_gpu; retained heaps may now be rewritten. */
static void port_samplers_begin_batch(void)
{
    for(PortSamplerPage* p=port_sampler_pages;p;p=p->next) p->used=0;
}
static void port_samplers_release(void)
{
    while(port_sampler_pages) {
        PortSamplerPage* p=port_sampler_pages;port_sampler_pages=p->next;
        p->heap->lpVtbl->Release(p->heap);free(p);
    }
}
static D3D12_GPU_DESCRIPTOR_HANDLE port_sampler_table(const D3D12DrawRecord* dr,u32 mask,ID3D12DescriptorHeap** heap)
{
    D3D12_SAMPLER_DESC table[PORT_TEXTURE_UNITS];
    D3D12DrawRecord unset={0};
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) {
        if(!port_decode_sampler((mask&(1u<<u))?dr:&unset,u,&table[u])) {
            fprintf(stderr,"[sampler-blocker] unit=%u address=%08X filter=%08X control=%08X border=%08X\n",
                u,dr->tex[u].sampler_address,dr->tex[u].sampler_filter,
                dr->tex[u].sampler_control,dr->tex[u].sampler_border);
            abort();
        }
    }
    PortSamplerPage* free_page=NULL;PortSamplerPage** tail=&port_sampler_pages;
    for(PortSamplerPage* p=port_sampler_pages;p;p=p->next) {
        for(unsigned i=0;i<p->used;i++) if(!memcmp(p->tables[i],table,sizeof(table))) {
            D3D12_GPU_DESCRIPTOR_HANDLE h;p->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(p->heap,&h);
            h.ptr+=(u64)i*PORT_TEXTURE_UNITS*port_sampler_increment;*heap=p->heap;
            port_sampler_tables_reused++;return h;
        }
        if(!free_page && p->used<PORT_SAMPLER_TABLES) free_page=p;
        tail=&p->next;
    }
    if(!free_page) {
        free_page=(PortSamplerPage*)calloc(1,sizeof(*free_page));
        if(!free_page) abort();
        D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        hd.NumDescriptors=PORT_SAMPLER_TABLES*PORT_TEXTURE_UNITS;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr=s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device,&hd,&IID_ID3D12DescriptorHeap,(void**)&free_page->heap);
        if(FAILED(hr)) { fprintf(stderr,"[sampler-blocker] heap creation HRESULT=%08lX\n",(unsigned long)hr);abort(); }
        *tail=free_page;
        port_sampler_increment=s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }
    unsigned slot=free_page->used++;memcpy(free_page->tables[slot],table,sizeof(table));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    free_page->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(free_page->heap,&cpu);
    free_page->heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(free_page->heap,&gpu);
    cpu.ptr+=(SIZE_T)slot*PORT_TEXTURE_UNITS*port_sampler_increment;
    gpu.ptr+=(u64)slot*PORT_TEXTURE_UNITS*port_sampler_increment;
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) {
        s_d3d.device->lpVtbl->CreateSampler(s_d3d.device,&table[u],cpu);cpu.ptr+=port_sampler_increment;
    }
    *heap=free_page->heap;port_sampler_tables_created++;return gpu;
}
static void port_bind_samplers(const D3D12DrawRecord* dr,u32 mask)
{
    ID3D12DescriptorHeap* sampler=NULL;
    D3D12_GPU_DESCRIPTOR_HANDLE h=port_sampler_table(dr,mask,&sampler);
    ID3D12DescriptorHeap* heaps[2]={s_d3d.srv_heap,sampler};
    s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list,2,heaps);
    s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list,3,h);
    /* The caller rebinds SRV root table 1 after this possible heap switch. */
}

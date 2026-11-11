/* Byte-exact GPU evidence for same-base layout changes, including areas not
 * covered by an intervening smaller view. Expected data is guest byte order. */
typedef struct {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    u32 width,height,row_bytes;
    u8 expected[1024];
} PortRTLayoutRead;
static void port_rt_layout_capture(int slot,unsigned mip,ID3D12Resource* readback,
                                  const u8* guest,u32 pitch,u32 guest_offset,int morton,
                                  PortRTLayoutRead* record,u64* total)
{
    OffRT* r=&s_d3d.off_rt[slot];D3D12_RESOURCE_DESC desc;r->res->lpVtbl->GetDesc(r->res,&desc);
    record->width=r->w>>mip;if(!record->width)record->width=1;
    record->height=r->h>>mip;if(!record->height)record->height=1;
    unsigned bpp=port_rt_bpp((DXGI_FORMAT)r->dxgi);record->row_bytes=record->width*bpp;
    if((u64)record->row_bytes*record->height>sizeof(record->expected))abort();
    *total=(*total+511)&~511ull;
    s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device,&desc,mip,1,*total,&record->fp,NULL,NULL,NULL);
    *total=record->fp.Offset+(u64)record->fp.Footprint.RowPitch*record->height;
    static const unsigned morton8x4[32]={0,1,4,5,16,17,20,21,2,3,6,7,18,19,22,23,8,9,12,13,24,25,28,29,10,11,14,15,26,27,30,31};
    for(unsigned y=0;y<record->height;y++)for(unsigned x=0;x<record->width;x++)for(unsigned c=0;c<bpp;c++) {
        unsigned offset=guest_offset+(morton?morton8x4[y*8+x]*bpp:y*pitch+x*bpp);
        unsigned component=r->dxgi==DXGI_FORMAT_R8G8B8A8_UNORM?(c+1)%4:
            r->dxgi==DXGI_FORMAT_R16G16B16A16_FLOAT?(c^1):(c^3);
        record->expected[(y*record->width+x)*bpp+c]=guest[offset+component];
    }
    D3D12_RESOURCE_STATES old=r->st;off_rt_transition(slot,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=r->res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=mip;
    dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=record->fp;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list,&dst,0,0,0,&src,NULL);off_rt_transition(slot,old);
}
static void port_rt_layout_rgba(u8* guest,u32 pitch,unsigned width,unsigned height,unsigned left,unsigned top,unsigned right,unsigned bottom,const u8 rgba[4])
{
    for(unsigned y=top;y<bottom&&y<height;y++)for(unsigned x=left;x<right&&x<width;x++) {
        u8* p=guest+y*pitch+x*4;p[0]=rgba[3];p[1]=rgba[0];p[2]=rgba[1];p[3]=rgba[2];
    }
}
int port_check_rt_layouts_gpu(void)
{
    if(s_d3d.device)return 2;
    extern uint8_t* vm_base;
    ID3D12Device* device=NULL;IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12DescriptorHeap* srv_heap=NULL;ID3D12Resource* readback=NULL;ID3D12Resource* raw_upload=NULL;
    HANDLE event=NULL;int software=0,ok=0,stable_binding=0,alias_binding=0;HRESULT hr=S_OK;
    ID3D12Debug* debug=NULL;ID3D12InfoQueue* info=NULL;unsigned validation_errors=0;
    PortRTLayoutRead records[64];unsigned count=0,matched=0,expected_bytes=0;u64 total=0;
    u8 expected[1024];for(unsigned i=0;i<sizeof(expected);i++)expected[i]=(u8)(i*29+7);
    const u32 address=0x30000;memcpy(vm_base+address,expected,sizeof(expected));
#define RT_LAYOUT_TRY(call) do {hr=(call);if(FAILED(hr))goto cleanup;} while(0)
    int debug_enabled=SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug,(void**)&debug));
    if(debug_enabled){debug->lpVtbl->EnableDebugLayer(debug);debug->lpVtbl->Release(debug);debug=NULL;}
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        software=1;RT_LAYOUT_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        RT_LAYOUT_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        RT_LAYOUT_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    s_d3d.device=device;
    if(debug_enabled)RT_LAYOUT_TRY(device->lpVtbl->QueryInterface(device,&IID_ID3D12InfoQueue,(void**)&info));
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    RT_LAYOUT_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    RT_LAYOUT_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,&allocator));
    RT_LAYOUT_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,&list));
    RT_LAYOUT_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event)goto cleanup;
    s_d3d.cmd_list=list;s_d3d.cmd_queue=queue;s_d3d.fence=fence;s_d3d.fence_event=event;
    s_d3d.cmd_allocators[0]=allocator;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=DRAW_SRV_BASE+PORT_TEXTURE_UNITS;
    RT_LAYOUT_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&srv_heap));
    s_d3d.srv_heap=srv_heap;s_d3d.srv_inc=device->lpVtbl->GetDescriptorHandleIncrementSize(device,hd.Type);
    readback=port_rt_buffer(64*4096,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,0);
    D3D12DrawRecord a={0},narrow={0},packed={0},large={0};
    a.is_vp=1;a.rt_off=0x9000;a.rt_w=a.rt_h=4;a.rt_fmt=0x108;a.color[0].address=address;a.color[0].pitch=32;
    narrow=a;narrow.rt_w=narrow.rt_h=2;packed=a;packed.color[0].pitch=16;large=a;large.rt_w=large.rt_h=8;large.color[0].pitch=64;
    int targets[4];port_rt_targets(&a,targets);int slot=targets[0];if(slot<0)goto cleanup;
#define RT_LAYOUT_CAPTURE(mip,pitch,offset,morton) do {if(count>=64)abort();port_rt_layout_capture(slot,mip,readback,expected,pitch,offset,morton,&records[count++],&total);} while(0)
    RT_LAYOUT_CAPTURE(0,32,0,0);
    const float red[4]={1,0,0,1},green[4]={0,1,0,1},blue[4]={0,0,1,1},white[4]={1,1,1,1},magenta[4]={1,0,1,1},yellow[4]={1,1,0,1};
    const u8 rb[4]={255,0,0,255},gb[4]={0,255,0,255},bb[4]={0,0,255,255},wb[4]={255,255,255,255},mb[4]={255,0,255,255};
    off_rt_transition(slot,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(slot),red,0,NULL);port_rt_mark_dirty(slot);
    port_rt_layout_rgba(expected,32,4,4,0,0,4,4,rb);RT_LAYOUT_CAPTURE(0,32,0,0);
    ID3D12Resource* first=s_d3d.off_rt[slot].res;
    /* Poison RAM after a GPU producer. Only newly grown backing tails may use it. */
    memset(vm_base+address,0xCD,sizeof(expected));
    port_rt_targets(&narrow,targets);if(targets[0]!=slot || s_d3d.off_rt[slot].res==first)goto cleanup;
    RT_LAYOUT_CAPTURE(0,32,0,0);
    off_rt_transition(slot,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_RECT one={1,1,2,2};
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(slot),green,1,&one);port_rt_mark_dirty(slot);
    port_rt_layout_rgba(expected,32,2,2,1,1,2,2,gb);RT_LAYOUT_CAPTURE(0,32,0,0);
    port_rt_targets(&a,targets);stable_binding=targets[0]==slot && s_d3d.off_rt[slot].w==4 && s_d3d.off_rt[slot].source_pitch==32;
    RT_LAYOUT_CAPTURE(0,32,0,0);
    port_rt_targets(&packed,targets);RT_LAYOUT_CAPTURE(0,16,0,0);
    off_rt_transition(slot,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(slot),blue,0,NULL);port_rt_mark_dirty(slot);
    port_rt_layout_rgba(expected,16,4,4,0,0,4,4,bb);
    port_rt_targets(&a,targets);RT_LAYOUT_CAPTURE(0,32,0,0);
    /* Cross-format copies preserve bit patterns, not numeric float values. */
    if(off_rt_get(0x9000,address,32,2,4,0x10B)!=slot)goto cleanup;
    RT_LAYOUT_CAPTURE(0,32,0,0);
    D3D12_RESOURCE_DESC native;s_d3d.off_rt[slot].res->lpVtbl->GetDesc(s_d3d.off_rt[slot].res,&native);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_fp;u64 upload_size=0;
    device->lpVtbl->GetCopyableFootprints(device,&native,0,1,0,&upload_fp,NULL,NULL,&upload_size);
    raw_upload=port_rt_buffer(upload_size,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,0);
    u8* upload_bytes=NULL;D3D12_RANGE none={0,0};RT_LAYOUT_TRY(raw_upload->lpVtbl->Map(raw_upload,0,&none,(void**)&upload_bytes));
    for(unsigned y=0;y<4;y++)for(unsigned x=0;x<2;x++) {
        u16 halves[4]={(u16)(0x7E51+y*2+x),0x8000,0x3C00,0xFC00};
        memcpy(upload_bytes+y*upload_fp.Footprint.RowPitch+x*8,halves,8);
        for(unsigned c=0;c<4;c++){expected[y*32+x*8+c*2]=(u8)(halves[c]>>8);expected[y*32+x*8+c*2+1]=(u8)halves[c];}
    }
    raw_upload->lpVtbl->Unmap(raw_upload,0,NULL);off_rt_transition(slot,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=raw_upload;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=upload_fp;
    dst.pResource=s_d3d.off_rt[slot].res;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);port_rt_mark_dirty(slot);RT_LAYOUT_CAPTURE(0,32,0,0);
    if(off_rt_get(0x9000,address,32,1,4,0x10C)!=slot)goto cleanup;RT_LAYOUT_CAPTURE(0,32,0,0);
    if(off_rt_get(0x9000,address,32,4,4,0x108)!=slot)goto cleanup;RT_LAYOUT_CAPTURE(0,32,0,0);
    /* A grows beyond its original allocation. Preserve GPU prefix bytes and
     * initialize only the previously unseen tail from current guest RAM. */
    memset(expected+128,0xCD,512-128);port_rt_targets(&large,targets);RT_LAYOUT_CAPTURE(0,64,0,0);
    off_rt_transition(slot,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_RECT corner={6,6,8,8};
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(slot),magenta,1,&corner);port_rt_mark_dirty(slot);
    port_rt_layout_rgba(expected,64,8,8,6,6,8,8,mb);
    narrow.color[0].pitch=64;port_rt_targets(&narrow,targets);
    off_rt_transition(slot,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_RECT origin={0,0,1,1};
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(slot),white,1,&origin);port_rt_mark_dirty(slot);
    port_rt_layout_rgba(expected,64,2,2,0,0,1,1,wb);port_rt_targets(&large,targets);RT_LAYOUT_CAPTURE(0,64,0,0);
    /* Rectangular Morton view of the very same byte storage. */
    if(off_rt_get(0x9000,address,0,8,4,0x208)!=slot)goto cleanup;RT_LAYOUT_CAPTURE(0,0,0,1);
    off_rt_transition(slot,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(slot),yellow,0,NULL);port_rt_mark_dirty(slot);
    for(unsigned i=0;i<32;i++){expected[i*4]=255;expected[i*4+1]=255;expected[i*4+2]=255;expected[i*4+3]=0;}
    port_rt_targets(&a,targets);RT_LAYOUT_CAPTURE(0,32,0,0);
    /* Full linear mip chain from the canonical store, including levels which
     * contain initial bytes outside the base render target's written area. */
    D3D12DrawRecord sample={0};sample.tex[0].set=1;sample.tex[0].raw=0x9000;sample.tex[0].off=address;
    sample.tex[0].w=sample.tex[0].h=4;sample.tex[0].fmt=0xA5;sample.tex[0].mips=3;sample.tex[0].source_pitch=32;
    port_resolve_texture_unit(&sample,0,0);if(sample.tex_rt[0]!=slot)goto cleanup;
    s_d3d.off_rt[slot].res->lpVtbl->GetDesc(s_d3d.off_rt[slot].res,&native);if(native.MipLevels!=3)goto cleanup;
    RT_LAYOUT_CAPTURE(0,32,0,0);RT_LAYOUT_CAPTURE(1,32,128,0);RT_LAYOUT_CAPTURE(2,32,192,0);
    /* More than one RTV page; every EA retains stable metadata indices. */
    for(unsigned i=0;i<20;i++) {
        u32 ea=0x40000+i*0x1000;memset(vm_base+ea,0,64);
        int other=off_rt_get(ea,ea,16,4,4,0x108);if(other<0)goto cleanup;
        off_rt_transition(other,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float* color=(i&1)?red:green;const u8* bytes=(i&1)?rb:gb;
        list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(other),color,0,NULL);port_rt_mark_dirty(other);
        u8 other_expected[64];port_rt_layout_rgba(other_expected,16,4,4,0,0,4,4,bytes);
        if(count>=64)abort();port_rt_layout_capture(other,0,readback,other_expected,16,0,0,&records[count++],&total);
    }
    if(off_rt_find(address)!=slot || s_d3d.off_rt_count!=21)goto cleanup;
    /* Distinct base EAs refer to overlapping guest bytes. Native resources
     * must exchange actual GPU writes, including cached views and padding. */
    const u32 alias_base=0x70000;u8 overlap[512];
    for(unsigned i=0;i<sizeof(overlap);i++)overlap[i]=(u8)(i*17+11);
    memcpy(vm_base+alias_base,overlap,sizeof(overlap));
    int alias_a=off_rt_get(alias_base+32,alias_base+32,32,4,4,0x108);
    off_rt_transition(alias_a,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(alias_a),red,0,NULL);port_rt_mark_dirty(alias_a);
    port_rt_layout_rgba(overlap+32,32,4,4,0,0,4,4,rb);
    memset(vm_base+alias_base,0xCD,sizeof(overlap));
#define RT_ALIAS_CAPTURE(index,offset) do {if(count>=64)abort();port_rt_layout_capture(index,0,readback,overlap+(offset),32,0,0,&records[count++],&total);} while(0)
    RT_ALIAS_CAPTURE(alias_a,32);
    int alias_b=off_rt_get(alias_base+36,alias_base+36,32,2,2,0x108);
    RT_ALIAS_CAPTURE(alias_b,36);
    off_rt_transition(alias_b,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(alias_b),green,0,NULL);port_rt_mark_dirty(alias_b);
    port_rt_layout_rgba(overlap+36,32,2,2,0,0,2,2,gb);
    if(off_rt_get(alias_base+32,alias_base+32,32,4,4,0x108)!=alias_a)goto cleanup;
    RT_ALIAS_CAPTURE(alias_a,32);
    off_rt_transition(alias_a,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_RECT blue_pixel={2,1,3,2};
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(alias_a),blue,1,&blue_pixel);port_rt_mark_dirty(alias_a);
    port_rt_layout_rgba(overlap+32,32,4,4,2,1,3,2,bb);
    if(off_rt_get(alias_base+36,alias_base+36,32,2,2,0x108)!=alias_b)goto cleanup;
    RT_ALIAS_CAPTURE(alias_b,36);
    /* The prefix is previously unseen CPU memory; the overlap is GPU-owned. */
    memset(overlap,0xCD,32);
    int alias_c=off_rt_get(alias_base,alias_base,32,4,4,0x108);
    RT_ALIAS_CAPTURE(alias_c,0);
    off_rt_transition(alias_c,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_RECT column={0,0,1,4};
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(alias_c),white,1,&column);port_rt_mark_dirty(alias_c);
    port_rt_layout_rgba(overlap,32,4,4,0,0,1,4,wb);
    if(off_rt_get(alias_base+32,alias_base+32,32,4,4,0x108)!=alias_a)goto cleanup;
    RT_ALIAS_CAPTURE(alias_a,32);
    /* Growth joins existing GPU bytes outside C's previous span and an
     * uncovered CPU tail. Neither the old prefix nor A's tail may be lost. */
    memset(overlap+160,0xCD,96);
    if(off_rt_get(alias_base,alias_base,32,4,8,0x108)!=alias_c)goto cleanup;
    RT_ALIAS_CAPTURE(alias_c,0);
    /* The actual sampler resolver must discover an interior GPU address, not
     * only exact cache keys. Reinterpret these bytes as half-float channels. */
    D3D12DrawRecord interior={0};interior.tex[15].set=1;interior.tex[15].raw=alias_base+40;
    interior.tex[15].off=alias_base+40;interior.tex[15].w=1;interior.tex[15].h=2;
    interior.tex[15].fmt=0xBA;interior.tex[15].source_pitch=32;
    port_resolve_texture_unit(&interior,0,15);
    int alias_d=interior.tex_rt[15];
    alias_binding=alias_d>=0 && interior.tex_resource[15]==s_d3d.off_rt[alias_d].res;
    if(!alias_binding)goto cleanup;
    RT_ALIAS_CAPTURE(alias_d,40);
    /* A small active view still retains a larger backing. Writes in its
     * inactive tail must survive a subsequent publication of the small view. */
    if(off_rt_get(alias_base,alias_base,32,2,1,0x108)!=alias_c)goto cleanup;
    int tail_alias=off_rt_get(alias_base+192,alias_base+192,32,2,2,0x108);
    off_rt_transition(tail_alias,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(tail_alias),white,0,NULL);port_rt_mark_dirty(tail_alias);
    port_rt_layout_rgba(overlap+192,32,2,2,0,0,2,2,wb);
    if(off_rt_get(alias_base,alias_base,32,2,1,0x108)!=alias_c)goto cleanup;
    off_rt_transition(alias_c,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(alias_c),blue,0,NULL);port_rt_mark_dirty(alias_c);
    port_rt_layout_rgba(overlap,32,2,1,0,0,2,1,bb);
    if(off_rt_get(alias_base+192,alias_base+192,32,2,2,0x108)!=tail_alias)goto cleanup;
    RT_ALIAS_CAPTURE(tail_alias,192);
    if(off_rt_get(alias_base,alias_base,32,4,8,0x108)!=alias_c)goto cleanup;
    RT_ALIAS_CAPTURE(alias_c,0);
    RT_LAYOUT_TRY(list->lpVtbl->Close(list));ID3D12CommandList* lists[]={(ID3D12CommandList*)list};
    queue->lpVtbl->ExecuteCommandLists(queue,1,lists);port_rt_require(queue->lpVtbl->Signal(queue,fence,1));s_d3d.fence_values[0]=1;
    port_rt_require(fence->lpVtbl->SetEventOnCompletion(fence,1,event));if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)abort();
    /* Reuse the same backing/transfer buffers in a second command list.
     * Their physical state has decayed even though their bytes persist. */
    port_release_retired_rts();port_rt_buffers_decay();
    RT_LAYOUT_TRY(allocator->lpVtbl->Reset(allocator));
    RT_LAYOUT_TRY(list->lpVtbl->Reset(list,allocator,NULL));
    if(off_rt_get(0x9000,address,32,4,4,0x10D)!=slot)goto cleanup;
    RT_LAYOUT_CAPTURE(0,32,0,0);
    if(off_rt_get(alias_base+36,alias_base+36,32,2,2,0x108)!=alias_b)goto cleanup;
    off_rt_transition(alias_b,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(alias_b),yellow,0,NULL);port_rt_mark_dirty(alias_b);
    const u8 yb[4]={255,255,0,255};port_rt_layout_rgba(overlap+36,32,2,2,0,0,2,2,yb);
    if(off_rt_get(alias_base+32,alias_base+32,32,4,4,0x108)!=alias_a)goto cleanup;
    RT_ALIAS_CAPTURE(alias_a,32);
    /* A later writer through B must be published into A before NV0039 reads
     * the overlapping range. Leave B dirty across the submission boundary. */
    off_rt_transition(alias_b,D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(alias_b),blue,0,NULL);
    port_rt_mark_dirty(alias_b);
    port_rt_layout_rgba(overlap+36,32,2,2,0,0,2,2,bb);
    RT_LAYOUT_TRY(list->lpVtbl->Close(list));
    queue->lpVtbl->ExecuteCommandLists(queue,1,lists);port_rt_require(queue->lpVtbl->Signal(queue,fence,2));s_d3d.fence_values[0]=2;
    port_rt_require(fence->lpVtbl->SetEventOnCompletion(fence,2,event));if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)abort();
    extern int port_check_nv0039_gpu_source(u32,u32,u32,const u8*);
    if(!port_check_nv0039_gpu_source(alias_base+32,alias_base-1,0xA0000,overlap+32))goto cleanup;
    if(info)for(UINT64 i=0;i<info->lpVtbl->GetNumStoredMessages(info);i++) {
        SIZE_T size=0;info->lpVtbl->GetMessage(info,i,NULL,&size);
        D3D12_MESSAGE* message=(D3D12_MESSAGE*)malloc(size);if(!message)abort();
        if(SUCCEEDED(info->lpVtbl->GetMessage(info,i,message,&size)) && message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            validation_errors++;fprintf(stderr,"[rt-layout-test] D3D12 id=%u %s\n",message->ID,message->pDescription);
        }
        free(message);
    }
    u8* bytes=NULL;D3D12_RANGE range={0,(SIZE_T)total};RT_LAYOUT_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&bytes));
    for(unsigned i=0;i<count;i++)for(unsigned y=0;y<records[i].height;y++)for(unsigned x=0;x<records[i].row_bytes;x++) {
        u8 actual=bytes[records[i].fp.Offset+(size_t)y*records[i].fp.Footprint.RowPitch+x];
        u8 wanted=records[i].expected[y*records[i].row_bytes+x];expected_bytes++;
        if(actual==wanted)matched++;else if(expected_bytes-matched<=8)fprintf(stderr,"[rt-layout-test] record=%u row=%u byte=%u actual=%02X expected=%02X\n",i,y,x,actual,wanted);
    }
    readback->lpVtbl->Unmap(readback,0,NULL);ok=stable_binding && alias_binding && matched==expected_bytes && !validation_errors;
cleanup:
    fprintf(stderr,"[compat-check] RT layouts GPU device=%s ordered-binding=%d interior-alias=%d records=%u bytes=%u/%u debug=%d errors=%u %s hr=%08lX\n",
        software?"WARP":"hardware",stable_binding,alias_binding,count,matched,expected_bytes,debug_enabled,validation_errors,ok?"PASS":"FAIL",(unsigned long)hr);
    port_destroy_retired_rts();port_rt_storage_release();memset(&s_d3d,0,sizeof(s_d3d));if(event)CloseHandle(event);
#define RT_LAYOUT_RELEASE(p) if(p)p->lpVtbl->Release(p)
    RT_LAYOUT_RELEASE(readback);RT_LAYOUT_RELEASE(raw_upload);RT_LAYOUT_RELEASE(srv_heap);
    RT_LAYOUT_RELEASE(list);RT_LAYOUT_RELEASE(allocator);RT_LAYOUT_RELEASE(queue);RT_LAYOUT_RELEASE(fence);
    RT_LAYOUT_RELEASE(info);RT_LAYOUT_RELEASE(device);RT_LAYOUT_RELEASE(warp);RT_LAYOUT_RELEASE(factory);
#undef RT_LAYOUT_RELEASE
#undef RT_LAYOUT_CAPTURE
#undef RT_ALIAS_CAPTURE
#undef RT_LAYOUT_TRY
    return ok?0:1;
}

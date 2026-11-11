/* GPU guest-byte stores with coherent overlapping address ranges. A native
 * texture is a cached view, not the storage identity. Publish prior writers
 * before resolving another alias; resources remain fence-owned on replacement. */
static ID3D12RootSignature* port_rt_copy_root;
static ID3D12PipelineState* port_rt_copy_pso;
static ID3D12DescriptorHeap** port_rt_pages;
static unsigned port_rt_page_capacity;

static void port_rt_require(HRESULT hr)
{
    if(FAILED(hr)){fprintf(stderr,"[rt-layout] D3D12 operation failed %08lX\n",(unsigned long)hr);abort();}
}
static void port_rt_reserve(unsigned count)
{
    if(count<=s_d3d.off_rt_capacity)return;
    unsigned capacity=s_d3d.off_rt_capacity?s_d3d.off_rt_capacity:PORT_RT_PAGE_SIZE;
    while(capacity<count){if(capacity>UINT_MAX/2)abort();capacity*=2;}
    if((size_t)capacity>SIZE_MAX/sizeof(*s_d3d.off_rt))abort();
    OffRT* grown=(OffRT*)realloc(s_d3d.off_rt,(size_t)capacity*sizeof(*grown));if(!grown)abort();
    memset(grown+s_d3d.off_rt_capacity,0,(capacity-s_d3d.off_rt_capacity)*sizeof(*grown));
    s_d3d.off_rt=grown;s_d3d.off_rt_capacity=capacity;
    unsigned pages=(capacity+PORT_RT_PAGE_SIZE-1)/PORT_RT_PAGE_SIZE;
    ID3D12DescriptorHeap** heaps=(ID3D12DescriptorHeap**)realloc(port_rt_pages,(size_t)pages*sizeof(*heaps));if(!heaps)abort();
    memset(heaps+port_rt_page_capacity,0,(pages-port_rt_page_capacity)*sizeof(*heaps));
    port_rt_pages=heaps;port_rt_page_capacity=pages;
}
static D3D12_CPU_DESCRIPTOR_HANDLE port_rt_rtv(unsigned slot)
{
    unsigned page=slot/PORT_RT_PAGE_SIZE;
    ID3D12DescriptorHeap** heap=page?&port_rt_pages[page]:&s_d3d.rt_rtv_heap;
    if(!*heap){
        D3D12_DESCRIPTOR_HEAP_DESC d={0};d.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;d.NumDescriptors=PORT_RT_PAGE_SIZE;
        port_rt_require(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device,&d,&IID_ID3D12DescriptorHeap,(void**)heap));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE view;(*heap)->lpVtbl->GetCPUDescriptorHandleForHeapStart(*heap,&view);
    view.ptr+=(u64)(slot%PORT_RT_PAGE_SIZE)*s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return view;
}
static void port_rt_buffer_state(ID3D12Resource* resource,D3D12_RESOURCE_STATES* state,D3D12_RESOURCE_STATES to)
{
    if(*state==to)return;
    D3D12_RESOURCE_BARRIER b={0};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;
    b.Transition.StateBefore=*state;b.Transition.StateAfter=to;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list,1,&b);*state=to;
}
static void port_rt_native_state(OffRT* r,D3D12_RESOURCE_STATES to)
{
    port_rt_buffer_state(r->res,&r->st,to);
}
static ID3D12Resource* port_rt_buffer(u64 bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,int uav)
{
    D3D12_RESOURCE_DESC d={0};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
    d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=heap;ID3D12Resource* resource=NULL;
    port_rt_require(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&d,state,NULL,&IID_ID3D12Resource,(void**)&resource));
    return resource;
}
static unsigned port_rt_bpp(DXGI_FORMAT format)
{
    switch(format){
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R32_FLOAT:return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;
    default:fprintf(stderr,"[rt-layout] unsupported native format=%u\n",(unsigned)format);abort();
    }
}
static u32 port_rt_texture_format(DXGI_FORMAT format,int linear)
{
    u32 code;
    switch(format){
    case DXGI_FORMAT_R8G8B8A8_UNORM:code=0x85;break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:code=0x9A;break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:code=0x9B;break;
    case DXGI_FORMAT_R32_FLOAT:code=0x9C;break;
    default:port_rt_bpp(format);abort();
    }
    return code|(linear?0x20u:0);
}
static void port_rt_copy_pipeline(void)
{
    if(port_rt_copy_pso)return;
    D3D12_ROOT_PARAMETER p[3]={0};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[2].Constants.Num32BitValues=11;
    D3D12_ROOT_SIGNATURE_DESC rd={0};rd.NumParameters=3;rd.pParameters=p;
    ID3DBlob* root=NULL;ID3DBlob* shader=NULL;ID3DBlob* error=NULL;
    HRESULT hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&root,&error);
    if(error){error->lpVtbl->Release(error);error=NULL;}port_rt_require(hr);
    hr=s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device,0,root->lpVtbl->GetBufferPointer(root),root->lpVtbl->GetBufferSize(root),&IID_ID3D12RootSignature,(void**)&port_rt_copy_root);
    root->lpVtbl->Release(root);port_rt_require(hr);
    const char* code="ByteAddressBuffer src:register(t0);RWByteAddressBuffer dst:register(u0);"
        "cbuffer P:register(b0){uint words,height,guestPitch,nativePitch,encoding,toGuest,wordsPerPixel,swizzled,logW,logH,guestBase;}"
        "uint morton(uint x,uint y){uint r=0,b=0;for(uint i=0;i<max(logW,logH);i++){"
        "if(i<logW){r|=((x>>i)&1)<<b;b++;}if(i<logH){r|=((y>>i)&1)<<b;b++;}}return r;}"
        "[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){if(id.x>=words||id.y>=height)return;"
        /* Mode 3 takes the two D3D12 planes and emits an A8R8G8B8 view of
         * (depth24 << 8) | stencil. Its wordsPerPixel slot carries output pitch. */
        "if(encoding==3){uint z=src.Load(id.y*nativePitch+id.x*4)&0x00ffffff;"
        "uint at=guestBase+id.y*guestPitch+id.x;"
        "uint s=(src.Load(at&~3u)>>((at&3u)*8))&255;"
        "dst.Store(id.y*wordsPerPixel+id.x*4,((z>>8)&255)|((z&255)<<8)|(s<<16)|((z&0xff0000)<<8));return;}"
        "uint g=guestBase+(swizzled?(morton(id.x/wordsPerPixel,id.y)*wordsPerPixel+id.x%wordsPerPixel)*4:id.y*guestPitch+id.x*4);"
        "uint n=id.y*nativePitch+id.x*4;uint v=src.Load(toGuest?n:g);"
        "if(encoding==0)v=toGuest?((v<<8)|(v>>24)):((v>>8)|(v<<24));"
        "else if(encoding==1)v=((v&0x00ff00ff)<<8)|((v>>8)&0x00ff00ff);"
        "else v=(v>>24)|((v>>8)&0x0000ff00)|((v<<8)&0x00ff0000)|(v<<24);dst.Store(toGuest?g:n,v);}";
    hr=D3DCompile(code,strlen(code),"rt-layout-byte-copy",NULL,NULL,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&error);
    if(error){fprintf(stderr,"[rt-layout] %s\n",(const char*)error->lpVtbl->GetBufferPointer(error));error->lpVtbl->Release(error);}port_rt_require(hr);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd={0};pd.pRootSignature=port_rt_copy_root;
    pd.CS.pShaderBytecode=shader->lpVtbl->GetBufferPointer(shader);pd.CS.BytecodeLength=shader->lpVtbl->GetBufferSize(shader);
    hr=s_d3d.device->lpVtbl->CreateComputePipelineState(s_d3d.device,&pd,&IID_ID3D12PipelineState,(void**)&port_rt_copy_pso);
    shader->lpVtbl->Release(shader);port_rt_require(hr);
}
static void port_rt_dispatch_raw_copy(ID3D12Resource* source,ID3D12Resource* dest,const u32 c[11])
{
    port_rt_copy_pipeline();
    ID3D12GraphicsCommandList* list=s_d3d.cmd_list;
    list->lpVtbl->SetComputeRootSignature(list,port_rt_copy_root);
    list->lpVtbl->SetPipelineState(list,port_rt_copy_pso);
    list->lpVtbl->SetComputeRootShaderResourceView(list,0,source->lpVtbl->GetGPUVirtualAddress(source));
    list->lpVtbl->SetComputeRootUnorderedAccessView(list,1,dest->lpVtbl->GetGPUVirtualAddress(dest));
    list->lpVtbl->SetComputeRoot32BitConstants(list,2,11,c,0);
    list->lpVtbl->Dispatch(list,(c[0]+63)/64,c[1],1);
}
static void port_rt_stage(OffRT* r,u64 bytes)
{
    if(r->transfer_size>=bytes)return;
    ID3D12Resource* old=r->transfer;
    r->transfer=port_rt_buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,1);
    r->transfer_size=bytes;r->transfer_state=D3D12_RESOURCE_STATE_COMMON;
    port_retire_resources(old,NULL);
}
static unsigned port_rt_log2(u32 value)
{
    unsigned bits=0;while(value>1){value>>=1;bits++;}return bits;
}
static void port_rt_transform(OffRT* r,const rsx_tex_level* level,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp,int to_guest)
{
    unsigned bpp=port_rt_bpp((DXGI_FORMAT)r->dxgi);int swizzled=(r->layout_format&0x20u)==0;
    u32 c[11]={level->w*bpp/4,level->h,level->tl.row_bytes,fp->Footprint.RowPitch,
        r->dxgi==DXGI_FORMAT_R8G8B8A8_UNORM?0:r->dxgi==DXGI_FORMAT_R16G16B16A16_FLOAT?1:2,
        (u32)to_guest,bpp/4,(u32)swizzled,port_rt_log2(level->w),port_rt_log2(level->h),level->offset};
    ID3D12Resource* source=to_guest?r->transfer:r->backing;ID3D12Resource* dest=to_guest?r->backing:r->transfer;
    D3D12_RESOURCE_STATES* source_state=to_guest?&r->transfer_state:&r->backing_state;
    D3D12_RESOURCE_STATES* dest_state=to_guest?&r->backing_state:&r->transfer_state;
    port_rt_buffer_state(source,source_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    port_rt_buffer_state(dest,dest_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    port_rt_dispatch_raw_copy(source,dest,c);
}
static u32 port_rt_chain(OffRT* r,rsx_tex_level levels[RSX_MAX_TEXTURE_LEVELS],u32* span)
{
    return port_mip_chain(r->layout_format,r->w,r->h,r->mips,r->source_pitch,levels,span,0);
}
static int port_rt_overlap(u32 a,u64 an,u32 b,u64 bn)
{
    return an && bn && (u64)a<(u64)b+bn && (u64)b<(u64)a+an;
}
static int port_rt_noncolor_owned(u32 ea,u32 bytes)
{
    extern int port_gcm_display_range_owned(u32,u32);
    if(port_gcm_display_range_owned(ea,bytes))return 1;
    for(unsigned i=0;i<port_depth_count;i++) {
        const PortDepthKey* k=&port_depth_surfaces[i].key;
        u64 pitch=(u64)k->width*(k->format==1?2:4);if(k->pitch>pitch)pitch=k->pitch;
        if(port_rt_overlap(ea,bytes,k->address,pitch*k->height))return 1;
    }
    return 0;
}
/* FIFO memory engines may only use RAM when no native GPU store owns bytes
 * in the requested range. Their caller first submits pending draw records. */
int port_renderer_guest_range_owned(u32 ea,u32 bytes)
{
    if(port_rt_noncolor_owned(ea,bytes))return 1;
    for(unsigned i=0;i<s_d3d.off_rt_count;i++)if(port_rt_overlap(ea,bytes,
        s_d3d.off_rt[i].address,s_d3d.off_rt[i].backing_size))return 1;
    return 0;
}
static void port_rt_copy_bytes(OffRT* dst,OffRT* src,u64 begin,u64 end)
{
    port_rt_buffer_state(src->backing,&src->backing_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
    port_rt_buffer_state(dst->backing,&dst->backing_state,D3D12_RESOURCE_STATE_COPY_DEST);
    s_d3d.cmd_list->lpVtbl->CopyBufferRegion(s_d3d.cmd_list,dst->backing,begin-dst->address,
        src->backing,begin-src->address,end-begin);
    if(getenv("PORT_RENDER_TRACE")) {
        static unsigned shown;if(shown++<32)fprintf(stderr,"[rt-alias] GPU bytes %08X->%08X range=%llX+%llu\n",
            src->address,dst->address,(unsigned long long)begin,(unsigned long long)(end-begin));
    }
}
static void port_rt_publish(OffRT* r)
{
    if(!r->dirty)return;
    if(r->stale){fprintf(stderr,"[rt-layout] stale GPU writer ea=%08X\n",r->address);abort();}
    rsx_tex_level levels[RSX_MAX_TEXTURE_LEVELS];u32 span=0;
    if(!port_rt_chain(r,levels,&span))abort();
    D3D12_RESOURCE_DESC desc;r->res->lpVtbl->GetDesc(r->res,&desc);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT64 bytes=0;
    s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device,&desc,0,1,0,&fp,NULL,NULL,&bytes);
    port_rt_stage(r,bytes);D3D12_RESOURCE_STATES old=r->st;
    port_rt_native_state(r,D3D12_RESOURCE_STATE_COPY_SOURCE);
    port_rt_buffer_state(r->transfer,&r->transfer_state,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=r->res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=r->transfer;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list,&dst,0,0,0,&src,NULL);
    port_rt_transform(r,&levels[0],&fp,1);port_rt_native_state(r,old);r->dirty=0;
    /* All stores were coherent before this writer was bound. Replicate the
     * shared bytes in GPU order and invalidate native views, never CPU bytes.
     * A second dirty overlapping view would mean unsynchronized MRT writes. */
    for(unsigned i=0;i<s_d3d.off_rt_count;i++) {
        OffRT* other=&s_d3d.off_rt[i];
        if(other==r || !port_rt_overlap(r->address,r->backing_size,other->address,other->backing_size))continue;
        if(other->dirty){fprintf(stderr,"[rt-layout] simultaneous dirty aliases %08X/%08X\n",r->address,other->address);abort();}
        u64 begin=r->address>other->address?r->address:other->address;
        u64 end=(u64)r->address+r->backing_size,other_end=(u64)other->address+other->backing_size;
        if(other_end<end)end=other_end;
        port_rt_copy_bytes(other,r,begin,end);other->stale=1;
    }
}
static void port_rt_grow_backing(OffRT* r,u32 span)
{
    if(r->backing_size>=span)return;
    extern uint8_t* vm_base;
    if(!vm_base || (u64)r->address+span>0x100000000ull){fprintf(stderr,"[rt-layout] invalid guest backing %08X+%u\n",r->address,span);abort();}
    ID3D12Resource* old=r->backing;u64 old_size=r->backing_size;
    ID3D12Resource* backing=port_rt_buffer(span,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,1);
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON;
    port_rt_buffer_state(backing,&state,D3D12_RESOURCE_STATE_COPY_DEST);
    if(old){port_rt_buffer_state(old,&r->backing_state,D3D12_RESOURCE_STATE_COPY_SOURCE);s_d3d.cmd_list->lpVtbl->CopyBufferRegion(s_d3d.cmd_list,backing,0,old,0,old_size);}
    r->backing=backing;r->backing_size=span;r->backing_state=state;
    /* Import covered tail intervals from existing GPU stores. Only gaps that
     * have never been represented by any store are initialized from RAM. */
    ID3D12Resource* upload=NULL;u8* mapped=NULL;u64 cursor=(u64)r->address+old_size,limit=(u64)r->address+span;
    while(cursor<limit) {
        OffRT* owner=NULL;u64 next=limit;
        for(unsigned i=0;i<s_d3d.off_rt_count;i++) {
            OffRT* other=&s_d3d.off_rt[i];if(other==r || !other->backing_size)continue;
            u64 end=(u64)other->address+other->backing_size;
            if(other->address<=cursor && cursor<end){owner=other;if(end<next)next=end;break;}
            if(other->address>cursor && other->address<next)next=other->address;
        }
        if(owner)port_rt_copy_bytes(r,owner,cursor,next);
        else {
            if(!upload) {
                upload=port_rt_buffer(span-old_size,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,0);
                D3D12_RANGE none={0,0};port_rt_require(upload->lpVtbl->Map(upload,0,&none,(void**)&mapped));
            }
            u64 offset=cursor-r->address-old_size;
            memcpy(mapped+offset,vm_base+cursor,(size_t)(next-cursor));
            s_d3d.cmd_list->lpVtbl->CopyBufferRegion(s_d3d.cmd_list,backing,cursor-r->address,upload,offset,next-cursor);
        }
        cursor=next;
    }
    if(upload)upload->lpVtbl->Unmap(upload,0,NULL);
    port_retire_resources(old,upload);
}
static void port_rt_import(OffRT* r,const rsx_tex_level* levels,unsigned count)
{
    D3D12_RESOURCE_DESC desc;r->res->lpVtbl->GetDesc(r->res,&desc);
    for(unsigned level=0;level<count;level++){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT64 bytes=0;
        s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device,&desc,level,1,0,&fp,NULL,NULL,&bytes);
        port_rt_stage(r,bytes);port_rt_transform(r,&levels[level],&fp,0);
        port_rt_buffer_state(r->transfer,&r->transfer_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=r->transfer;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
        dst.pResource=r->res;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=level;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list,&dst,0,0,0,&src,NULL);
    }
    port_rt_native_state(r,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
static int port_rt_get_view(u32 off,u32 address,u32 pitch,u32 w,u32 h,DXGI_FORMAT format,u32 layout,u32 requested)
{
    if(!s_d3d.device || !s_d3d.cmd_list)return -1;
    if(!w)w=s_d3d.width;if(!h)h=s_d3d.height;
    unsigned bpp=port_rt_bpp(format);int linear=(layout&0x20u)!=0;
    if(!w || !h || w>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || h>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
       (linear && pitch && (pitch<w*bpp || (pitch&3))) || (!linear && ((w&(w-1)) || (h&(h-1))))) {
        fprintf(stderr,"[rt-layout] unsupported layout ea=%08X %ux%u pitch=%u fmt=%u layout=%02X\n",address,w,h,pitch,(unsigned)format,layout);abort();
    }
    u32 effective_pitch=linear?(pitch?pitch:w*bpp):0;
    rsx_tex_level levels[RSX_MAX_TEXTURE_LEVELS];u32 span=0;
    u32 count=port_mip_chain(layout,w,h,requested,effective_pitch,levels,&span,0);if(!count)abort();
    int slot=off_rt_find(address);
    /* Include a retained larger backing, even when the active view shrank.
     * Otherwise a writer outside that smaller view could leave stale padding
     * that a later full-store publication would copy over newer GPU bytes. */
    u64 sync_span=slot>=0 && s_d3d.off_rt[slot].backing_size>span?s_d3d.off_rt[slot].backing_size:span;
    for(unsigned i=0;i<s_d3d.off_rt_count;i++)if((int)i!=slot && s_d3d.off_rt[i].dirty &&
        port_rt_overlap(address,sync_span,s_d3d.off_rt[i].address,s_d3d.off_rt[i].backing_size))port_rt_publish(&s_d3d.off_rt[i]);
    if(slot<0){port_rt_reserve(s_d3d.off_rt_count+1);slot=(int)s_d3d.off_rt_count++;s_d3d.off_rt[slot].address=address;}
    OffRT* r=&s_d3d.off_rt[slot];r->used=1;
    if(r->res && r->w==w && r->h==h && r->dxgi==(u32)format && r->source_pitch==effective_pitch && r->layout_format==layout && r->mips==count) {
        if(r->stale){port_rt_native_state(r,D3D12_RESOURCE_STATE_COPY_DEST);port_rt_import(r,levels,count);r->stale=0;}
        return slot;
    }
    port_rt_publish(r);
    port_retire_resources(r->res,r->up);r->res=NULL;r->up=NULL;
    port_rt_grow_backing(r,span);
    r->off=off;r->w=w;r->h=h;r->dxgi=(u32)format;r->source_pitch=effective_pitch;r->layout_format=layout;r->mips=count;r->dirty=0;r->stale=0;
    D3D12_RESOURCE_DESC d={0};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
    d.DepthOrArraySize=1;d.MipLevels=(UINT16)count;d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    /* A fast-clear hint only; initialization still comes from GPU bytes. */
    D3D12_CLEAR_VALUE clear={0};clear.Format=format;
    port_rt_require(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&d,
        D3D12_RESOURCE_STATE_COPY_DEST,&clear,&IID_ID3D12Resource,(void**)&r->res));r->st=D3D12_RESOURCE_STATE_COPY_DEST;
    port_rt_import(r,levels,count);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device,r->res,NULL,port_rt_rtv((unsigned)slot));
    return slot;
}
static int off_rt_get(u32 off,u32 address,u32 source_pitch,u32 w,u32 h,u32 rsx_fmt)
{
    unsigned color=rsx_fmt&31;
    if(color!=8 && color!=11 && color!=12 && color!=13) {
        fprintf(stderr,"[rt-layout] unsupported guest color format=%08X ea=%08X\n",rsx_fmt,address);abort();
    }
    DXGI_FORMAT format=rsx_surface_dxgi(rsx_fmt);
    return port_rt_get_view(off,address,source_pitch,w,h,format,port_rt_texture_format(format,((rsx_fmt>>8)&15)!=2),1);
}
static void port_rt_targets(const D3D12DrawRecord* dr,int slots[4])
{
    slots[0]=dr->rt_off?off_rt_get(dr->rt_off,dr->color[0].address,dr->color[0].pitch,dr->rt_w,dr->rt_h,dr->rt_fmt):-1;
    for(unsigned i=1;i<4;i++) {
        slots[i]=dr->rt_mrt[i-1]?off_rt_get(dr->rt_mrt[i-1],dr->color[i].address,dr->color[i].pitch,dr->rt_w,dr->rt_h,dr->rt_fmt):-1;
        if(slots[i]>=0)for(unsigned j=0;j<i;j++)if(slots[j]==slots[i] && dr->color[j].pitch!=dr->color[i].pitch) {
            fprintf(stderr,"[rt-layout] incompatible simultaneous MRT aliases ea=%08X\n",dr->color[i].address);abort();
        }
        if(slots[i]>=0)for(unsigned j=0;j<i;j++)if(slots[j]>=0 && slots[j]!=slots[i] &&
            port_rt_overlap(s_d3d.off_rt[slots[j]].address,s_d3d.off_rt[slots[j]].backing_size,
                            s_d3d.off_rt[slots[i]].address,s_d3d.off_rt[slots[i]].backing_size)) {
            fprintf(stderr,"[rt-layout] overlapping simultaneous MRT ranges %08X/%08X\n",dr->color[j].address,dr->color[i].address);abort();
        }
    }
    /* Texture resolution may have read the same native resource. Restore its
     * output state even when the cached OM indices/handles did not change. */
    for(unsigned i=0;i<4;i++)if(slots[i]>=0)port_rt_native_state(&s_d3d.off_rt[slots[i]],D3D12_RESOURCE_STATE_RENDER_TARGET);
}
static void port_rt_mark_dirty(int slot)
{
    if(slot>=0)s_d3d.off_rt[slot].dirty=1;
}
static int port_rt_texture_view(D3D12DrawRecord* dr,unsigned unit)
{
    int found=off_rt_find(dr->tex[unit].off)>=0;
    rsx_tex_level levels[RSX_MAX_TEXTURE_LEVELS];u32 span=0;
    if(!port_mip_chain(dr->tex[unit].fmt,dr->tex[unit].w,dr->tex[unit].h,dr->tex[unit].mips,
                       dr->tex[unit].source_pitch,levels,&span,dr->tex[unit].cube)) {
        if(found){fprintf(stderr,"[rt-layout] invalid alias texture layout ea=%08X\n",dr->tex[unit].off);abort();}
        return -1;
    }
    for(unsigned i=0;i<s_d3d.off_rt_count;i++)if(port_rt_overlap(dr->tex[unit].off,(u64)span*(dr->tex[unit].cube?6:1),
        s_d3d.off_rt[i].address,s_d3d.off_rt[i].backing_size)){found=1;break;}
    if(!found)return -1;
    if(dr->tex[unit].cube){fprintf(stderr,"[rt-layout] cube alias unsupported ea=%08X\n",dr->tex[unit].off);abort();}
    rsx_tex_layout layout;rsx_texture_layout(dr->tex[unit].fmt,dr->tex[unit].w,dr->tex[unit].h,&layout);
    DXGI_FORMAT format=port_mip_format(&layout);port_rt_bpp(format);
    return port_rt_get_view(dr->tex[unit].raw,dr->tex[unit].off,dr->tex[unit].source_pitch,dr->tex[unit].w,dr->tex[unit].h,
        format,port_rt_texture_format(format,(dr->tex[unit].fmt&0x20u)!=0),dr->tex[unit].mips);
}
/* Default-heap buffers decay to COMMON at an ExecuteCommandLists boundary.
 * Native render-target textures do not share that buffer decay rule. */
static void port_rt_buffers_decay(void)
{
    if(s_d3d.off_rt_count && s_d3d.fence &&
       s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence)<s_d3d.fence_values[s_d3d.frame_index]) {
        fprintf(stderr,"[rt-layout] buffer decay before completed submission\n");abort();
    }
    for(unsigned i=0;i<s_d3d.off_rt_count;i++) {
        s_d3d.off_rt[i].backing_state=D3D12_RESOURCE_STATE_COMMON;
        s_d3d.off_rt[i].transfer_state=D3D12_RESOURCE_STATE_COMMON;
    }
}

static void port_rt_wait_for_readback(void)
{
    wait_for_gpu();
    u64 completed=s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
    if(completed==UINT64_MAX || completed<s_d3d.fence_values[s_d3d.frame_index]) {
        fprintf(stderr,"[rt-layout] color readback fence did not complete\n");
        abort();
    }
}

/* Called on the FIFO owner after pending draws have been submitted. A single
 * backing must cover the entire source envelope. Publishing overlapping
 * writers first makes that backing authoritative even if its view is stale. */
u8* port_renderer_read_color(u32 ea,u32 bytes)
{
    if(!bytes || port_rt_noncolor_owned(ea,bytes))return NULL;
    OffRT* source=NULL;
    for(unsigned i=0;i<s_d3d.off_rt_count;i++) {
        OffRT* r=&s_d3d.off_rt[i];
        if(ea>=r->address && (u64)ea+bytes<=(u64)r->address+r->backing_size) {
            source=r;
            break;
        }
    }
    if(!source)return NULL;
    u8* result=(u8*)malloc(bytes);
    if(!result)return NULL;
    ID3D12Resource* readback=port_rt_buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,0);
    port_rt_wait_for_readback();
    port_rt_buffers_decay();
    ID3D12GraphicsCommandList* list=s_d3d.cmd_list;
    ID3D12CommandAllocator* allocator=s_d3d.cmd_allocators[s_d3d.frame_index];
    port_rt_require(allocator->lpVtbl->Reset(allocator));
    port_rt_require(list->lpVtbl->Reset(list,allocator,NULL));
    for(unsigned i=0;i<s_d3d.off_rt_count;i++) {
        OffRT* r=&s_d3d.off_rt[i];
        if(port_rt_overlap(ea,bytes,r->address,r->backing_size))port_rt_publish(r);
    }
    port_rt_buffer_state(source->backing,&source->backing_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->lpVtbl->CopyBufferRegion(list,readback,0,source->backing,ea-source->address,bytes);
    port_rt_require(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[]={(ID3D12CommandList*)list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue,1,lists);
    port_rt_wait_for_readback();
    void* mapped=NULL;
    D3D12_RANGE range={0,bytes},written={0,0};
    port_rt_require(readback->lpVtbl->Map(readback,0,&range,&mapped));
    memcpy(result,mapped,bytes);
    readback->lpVtbl->Unmap(readback,0,&written);
    readback->lpVtbl->Release(readback);
    return result;
}

static void port_rt_storage_release(void)
{
    for(unsigned i=0;i<s_d3d.off_rt_count;i++){
        OffRT* r=&s_d3d.off_rt[i];if(r->res)r->res->lpVtbl->Release(r->res);if(r->up)r->up->lpVtbl->Release(r->up);
        if(r->backing)r->backing->lpVtbl->Release(r->backing);if(r->transfer)r->transfer->lpVtbl->Release(r->transfer);
    }
    free(s_d3d.off_rt);s_d3d.off_rt=NULL;s_d3d.off_rt_count=s_d3d.off_rt_capacity=0;
    for(unsigned i=1;i<port_rt_page_capacity;i++)if(port_rt_pages[i])port_rt_pages[i]->lpVtbl->Release(port_rt_pages[i]);
    free(port_rt_pages);port_rt_pages=NULL;port_rt_page_capacity=0;
    if(s_d3d.rt_rtv_heap)s_d3d.rt_rtv_heap->lpVtbl->Release(s_d3d.rt_rtv_heap);s_d3d.rt_rtv_heap=NULL;
    if(port_rt_copy_pso)port_rt_copy_pso->lpVtbl->Release(port_rt_copy_pso);port_rt_copy_pso=NULL;
    if(port_rt_copy_root)port_rt_copy_root->lpVtbl->Release(port_rt_copy_root);port_rt_copy_root=NULL;
}

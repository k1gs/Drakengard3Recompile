/* The shared decoder owns per-level endian/swizzle rules. RSX linear levels
 * share a row pitch; packed cube faces align to 128 bytes, linear faces do not.
 * Keep this source layout separate from D3D12's destination footprints. */
static u32 port_mip_chain(u32 fmt,u32 w,u32 h,u32 requested,u32 pitch,
                          rsx_tex_level out[RSX_MAX_TEXTURE_LEVELS],u32* face_stride,int cube)
{
    if(!w || !h) return 0;
    u32 count=0;u64 offset=0;
    rsx_tex_layout base;rsx_texture_layout(fmt,w,h,&base);
    int linear=(fmt&0x20u)!=0;
    u32 row_pitch=linear?(pitch?pitch:base.row_bytes):0;
    if(linear && row_pitch<base.row_bytes) return 0;
    u32 mw=w,mh=h;
    if(!requested) requested=1;
    while(count<requested && count<RSX_MAX_TEXTURE_LEVELS) {
        rsx_tex_level* lv=&out[count];lv->w=mw;lv->h=mh;lv->offset=(u32)offset;
        rsx_texture_layout(fmt,mw,mh,&lv->tl);
        if(linear) lv->tl.row_bytes=row_pitch;
        u64 bytes=(u64)lv->tl.row_bytes*lv->tl.rows;
        if(bytes>0xFFFFFFFFu || offset+bytes>0xFFFFFFFFu) return 0;
        lv->tl.face_bytes=(u32)bytes;offset+=bytes;count++;
        if(mw==1 && mh==1) break;
        if(mw>1) mw>>=1;if(mh>1) mh>>=1;
    }
    u64 stride=cube && !linear?(offset+127u)&~127ull:offset;
    if(stride>0xFFFFFFFFu) return 0;
    *face_stride=(u32)stride;return count;
}

static void port_mip_decode(void* dst,u32 host_pitch,const u8* src,const rsx_tex_level* lv)
{
    if(lv->tl.compressed) {
        /* The generic decoder assumes packed BC rows; linear BC can instead
         * have source padding, which must not be copied past the host row. */
        for(u32 row=0;row<lv->tl.rows;row++)
            memcpy((u8*)dst+(size_t)row*host_pitch,src+(size_t)row*lv->tl.row_bytes,lv->tl.dst_row_bytes);
    } else rsx_texture_decode(dst,host_pitch,src,lv->w,lv->h,&lv->tl,rsx_texture_argb_is_rgba());
}
static DXGI_FORMAT port_mip_format(const rsx_tex_layout* tl)
{
    switch(tl->fmt) {
    case RSX_TEXFMT_R8:return tl->base_format==0x81?DXGI_FORMAT_R8_UNORM:DXGI_FORMAT_UNKNOWN;
    case RSX_TEXFMT_R8G8:return DXGI_FORMAT_R8G8_UNORM;
    case RSX_TEXFMT_R8G8B8A8:return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RSX_TEXFMT_BC1:return DXGI_FORMAT_BC1_UNORM;
    case RSX_TEXFMT_BC2:return DXGI_FORMAT_BC2_UNORM;
    case RSX_TEXFMT_BC3:return DXGI_FORMAT_BC3_UNORM;
    case RSX_TEXFMT_R16:return DXGI_FORMAT_R16_UNORM;
    case RSX_TEXFMT_R16G16:return DXGI_FORMAT_R16G16_UNORM;
    case RSX_TEXFMT_R16G16F:return DXGI_FORMAT_R16G16_FLOAT;
    case RSX_TEXFMT_R16G16B16A16F:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RSX_TEXFMT_R32F:return DXGI_FORMAT_R32_FLOAT;
    case RSX_TEXFMT_R32G32B32A32F:return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}

/* The key includes the complete effective layout, not just level-0 shape. */
static int port_mip_key_equal(const VPTexSlot* c,u32 off,u32 w,u32 h,u32 fmt,
                             int cube,u32 levels,u32 pitch)
{
    return c->res && c->key==off && c->w==w && c->h==h && c->fmt==fmt &&
        c->cube==cube && c->mip_levels==levels && c->source_pitch==pitch;
}

static int vp_upload_tex_slot(u32 off,u32 w,u32 h,u32 fmt,int cube,u32 mips,u32 source_pitch)
{
    extern uint8_t* vm_base;
    if(!off || !w || !h || !vm_base || !s_d3d.device || !s_d3d.cmd_list) return -1;
    if(w>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || h>D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) return -1;
    rsx_tex_level levels[RSX_MAX_TEXTURE_LEVELS];
    u32 stride=0;
    u32 count=port_mip_chain(fmt,w,h,mips,source_pitch,levels,&stride,cube);
    if(!count) return -1;
    DXGI_FORMAT format=port_mip_format(&levels[0].tl);
    if(format==DXGI_FORMAT_UNKNOWN) return -1;
    u32 faces=cube?6u:1u;
    u32 chain_bytes=levels[count-1].offset+levels[count-1].tl.face_bytes;
    u64 span=(u64)(faces-1)*stride+chain_bytes;
    if((u64)off+span>0x100000000ull || span>SIZE_MAX) return -1;
    u32 pitch=(fmt&0x20u)?levels[0].tl.row_bytes:0;
    static int nocache=-1;
    if(nocache<0) nocache=getenv("TEX_NOCACHE")?atoi(getenv("TEX_NOCACHE")):0;
    int slot=-1;
    for(unsigned i=0;i<s_d3d.vp_tex_capacity;i++) {
        VPTexSlot* c=&s_d3d.vp_tex[i];
        if(!port_mip_key_equal(c,off,w,h,fmt,cube,count,pitch)) continue;
        /* Compare every source byte, including tiny lower levels and all
         * faces. An exact retained copy replaces the old sparse L0 checksum. */
        if(!nocache && c->source_size==(size_t)span && c->source_bytes &&
           !memcmp(c->source_bytes,vm_base+off,(size_t)span)) {
            c->used=1;port_texture_touch(c);return (int)i;
        }
        /* A descriptor already recorded in this batch keeps its old resource. */
        if(!c->used) {slot=(int)i;break;}
    }
    if(slot<0) slot=port_texture_free_slot(s_d3d.vp_tex,s_d3d.vp_tex_capacity);
    if(slot<0) slot=port_texture_grow(&s_d3d.vp_tex,&s_d3d.vp_tex_capacity);
    if(slot<0) {fprintf(stderr,"[mip-upload] cache allocation failed\n");abort();}
    VPTexSlot* t=&s_d3d.vp_tex[slot];
    if(t->used) abort();
    if(t->res && (t->w!=w || t->h!=h || t->fmt!=fmt || t->cube!=cube || t->mip_levels!=count)) {
        t->res->lpVtbl->Release(t->res);t->res=NULL;
        if(t->up) {t->up->lpVtbl->Release(t->up);t->up=NULL;}
    }
    D3D12_RESOURCE_DESC td={0};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=w;td.Height=h;td.DepthOrArraySize=(UINT16)faces;td.MipLevels=(UINT16)count;
    td.Format=format;td.SampleDesc.Count=1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[6*RSX_MAX_TEXTURE_LEVELS];
    UINT64 upload_bytes=0;
    s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device,&td,0,faces*count,0,footprints,NULL,NULL,&upload_bytes);
    int fresh=!t->res;
    if(fresh) {
        D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr=s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&td,
            D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&t->res);
        if(FAILED(hr)) return -1;
        D3D12_RESOURCE_DESC bd={0};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=upload_bytes;
        bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hp.Type=D3D12_HEAP_TYPE_UPLOAD;
        hr=s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&bd,
            D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)&t->up);
        if(FAILED(hr)) {t->res->lpVtbl->Release(t->res);t->res=NULL;return -1;}
    }
    u8* snapshot=(u8*)realloc(t->source_bytes,(size_t)span);
    if(!snapshot) {fprintf(stderr,"[mip-upload] source snapshot allocation failed\n");abort();}
    t->source_bytes=snapshot;t->source_size=0; /* invalid until upload succeeds */
    memcpy(snapshot,vm_base+off,(size_t)span);
    void* mapped=NULL;D3D12_RANGE read={0,0};
    if(FAILED(t->up->lpVtbl->Map(t->up,0,&read,&mapped)) || !mapped) {
        if(fresh) {t->res->lpVtbl->Release(t->res);t->res=NULL;t->up->lpVtbl->Release(t->up);t->up=NULL;}
        return -1;
    }
    for(u32 face=0;face<faces;face++) for(u32 mip=0;mip<count;mip++) {
        u32 index=mip+face*count;
        const rsx_tex_level* lv=&levels[mip];
        port_mip_decode((u8*)mapped+footprints[index].Offset,footprints[index].Footprint.RowPitch,
            snapshot+(u64)face*stride+lv->offset,lv);
    }
    t->up->lpVtbl->Unmap(t->up,0,NULL);
    D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=t->res;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    if(!fresh) s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list,1,&barrier);
    for(u32 index=0;index<faces*count;index++) {
        D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};
        dst.pResource=t->res;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=index;
        src.pResource=t->up;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[index];
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list,&dst,0,0,0,&src,NULL);
    }
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list,1,&barrier);
    t->off=t->key=off;t->w=w;t->h=h;t->fmt=fmt;t->cube=cube;t->mip_levels=count;t->source_pitch=pitch;
    t->source_size=(size_t)span;t->used=1;port_texture_touch(t);
    static int trace=-1;static unsigned traces;
    if(trace<0) trace=getenv("PORT_MIP_TRACE")?1:0;
    if(trace && traces++<128) fprintf(stderr,"[mip-upload] source=%08X size=%ux%u fmt=%02X levels=%u faces=%u pitch=%u span=%llu slot=%d\n",
        off,w,h,fmt,count,faces,pitch,(unsigned long long)span,slot);
    return slot;
}

/* Call only after the owning submission fence, or from an isolated fixture. */
static void port_mip_release_cache(void)
{
    for(unsigned i=0;i<s_d3d.vp_tex_capacity;i++) {
        VPTexSlot* t=&s_d3d.vp_tex[i];
        if(t->res) t->res->lpVtbl->Release(t->res);
        if(t->up) t->up->lpVtbl->Release(t->up);
        free(t->source_bytes);
    }
    free(s_d3d.vp_tex);s_d3d.vp_tex=NULL;s_d3d.vp_tex_capacity=0;
}

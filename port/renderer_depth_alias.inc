/* An exact linear A8R8G8B8 view of the native Z24S8 producer.
 * The depth attachment stays bound as depth. A separate sampled texture
 * carries the packed bytes as they stood immediately before the consumer.
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/VK/VKCompute.h */
static void port_depth_note_write(const D3D12DrawRecord* dr)
{
    int writes=dr->is_clear ? port_depth_clear_flags(dr)!=0 :
        (dr->depth_key&3u)==3u || (dr->stencil_key.enabled && (dr->stencil_key.masks&0xFF00u));
    if(!dr->depth_required || !writes)return;
    int slot=port_find_depth(port_depth_surfaces,port_depth_count,&dr->depth_surface);
    if(slot<0)abort();
    PortDepthSurface* s=&port_depth_surfaces[slot];
    s->written=1;
    s->linear_writer=((dr->rt_fmt>>8)&15u)==1;
    s->packed_current=0;
}

static void port_depth_pack_view(PortDepthSurface* s)
{
    ID3D12Device* device=s_d3d.device;
    ID3D12GraphicsCommandList* list=s_d3d.cmd_list;
    D3D12_RESOURCE_DESC depth;
    s->resource->lpVtbl->GetDesc(s->resource,&depth);
    D3D12_RESOURCE_DESC color=depth;
    color.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    color.Flags=D3D12_RESOURCE_FLAG_NONE;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT planes[2],packed;
    UINT rows[2];
    UINT64 row_bytes[2];
    device->lpVtbl->GetCopyableFootprints(device,&depth,0,2,0,planes,rows,row_bytes,NULL);
    device->lpVtbl->GetCopyableFootprints(device,&color,0,1,0,&packed,NULL,NULL,NULL);
    if(rows[0]!=s->key.height || rows[1]!=s->key.height || row_bytes[0]!=s->key.width*4u ||
        row_bytes[1]!=s->key.width || planes[0].Offset) {
        fprintf(stderr,"[depth-alias] unexpected footprint %ux%u rows=%u/%u bytes=%llu/%llu pitches=%u/%u/%u formats=%u/%u\n",
            s->key.width,s->key.height,rows[0],rows[1],(unsigned long long)row_bytes[0],
            (unsigned long long)row_bytes[1],planes[0].Footprint.RowPitch,planes[1].Footprint.RowPitch,
            packed.Footprint.RowPitch,planes[0].Footprint.Format,planes[1].Footprint.Format);
        abort();
    }
    int first=s->packed_view==NULL;
    if(first) {
        D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        port_rt_require(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,
            &color,D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&s->packed_view));
        s->packed_planes=port_rt_buffer(planes[1].Offset+(u64)planes[1].Footprint.RowPitch*rows[1],
            D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,0);
        s->packed_rgba=port_rt_buffer((u64)packed.Footprint.RowPitch*rows[0],
            D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,1);
    }

    D3D12_RESOURCE_STATES depth_state=D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES view_state=first?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES input_state=D3D12_RESOURCE_STATE_COMMON,output_state=D3D12_RESOURCE_STATE_COMMON;
    port_rt_buffer_state(s->resource,&depth_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
    port_rt_buffer_state(s->packed_planes,&input_state,D3D12_RESOURCE_STATE_COPY_DEST);
    for(unsigned i=0;i<2;i++) {
        D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
        src.pResource=s->resource;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=i;
        dst.pResource=s->packed_planes;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=planes[i];
        list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    }
    port_rt_buffer_state(s->resource,&depth_state,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    port_rt_buffer_state(s->packed_planes,&input_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    port_rt_buffer_state(s->packed_rgba,&output_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const u32 constants[11]={s->key.width,s->key.height,planes[1].Footprint.RowPitch,
        planes[0].Footprint.RowPitch,3,0,packed.Footprint.RowPitch,0,0,0,(u32)planes[1].Offset};
    port_rt_dispatch_raw_copy(s->packed_planes,s->packed_rgba,constants);
    port_rt_buffer_state(s->packed_planes,&input_state,D3D12_RESOURCE_STATE_COMMON);
    port_rt_buffer_state(s->packed_rgba,&output_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
    port_rt_buffer_state(s->packed_view,&view_state,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
    src.pResource=s->packed_rgba;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=packed;
    dst.pResource=s->packed_view;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    port_rt_buffer_state(s->packed_rgba,&output_state,D3D12_RESOURCE_STATE_COMMON);
    port_rt_buffer_state(s->packed_view,&view_state,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s->packed_current=1;
}

static ID3D12Resource* port_depth_texture_alias(const D3D12DrawRecord* dr,unsigned unit)
{
    PortDepthSurface* source=NULL;
    const u32 ea=dr->tex[unit].off,w=dr->tex[unit].w,h=dr->tex[unit].h;
    const u32 pitch=dr->tex[unit].source_pitch?dr->tex[unit].source_pitch:w*4u;
    if((dr->tex[unit].fmt&0xBFu)!=0xA5 || dr->tex[unit].cube || dr->tex[unit].mips!=1)return NULL;
    for(unsigned i=0;i<port_depth_count;i++) {
        PortDepthSurface* s=&port_depth_surfaces[i];
        const PortDepthKey* k=&s->key;
        u64 row=(u64)k->width*(k->format==1?2:4);
        if(k->pitch>row)row=k->pitch;
        if(!s->written || !port_rt_overlap(ea,(u64)pitch*h,k->address,row*k->height))continue;
        if(source || k->address!=ea || k->width!=w || k->height!=h || k->format!=2 ||
            !s->linear_writer || pitch!=row || pitch!=w*4u)return NULL;
        source=s;
    }
    if(!source)return NULL;
    for(unsigned i=0;i<s_d3d.off_rt_count;i++)if(port_rt_overlap(ea,(u64)pitch*h,
        s_d3d.off_rt[i].address,s_d3d.off_rt[i].backing_size))return NULL;
    if(!source->packed_current)port_depth_pack_view(source);
    return source->packed_view;
}

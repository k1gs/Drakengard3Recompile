/* Standalone D3D12 contract check. No window, swapchain or guest assets.
 * Reuses the production attachment cache and clear-mask decoder. Readback
 * copies each complete depth/stencil plane as required by CopyTextureRegion:
 * https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion */
static void port_depth_note_write(const D3D12DrawRecord* dr);
static void port_resolve_texture_unit(D3D12DrawRecord* dr,u32 draw,int unit);
static void port_rt_storage_release(void);
static void port_rt_buffer_state(ID3D12Resource*,D3D12_RESOURCE_STATES*,D3D12_RESOURCE_STATES);
int port_check_depth_surfaces_gpu(void)
{
    if(s_d3d.device || port_depth_count || port_depth_surfaces) return 2;
    ID3D12Device* device=NULL;
    IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12Resource* readback=NULL;ID3D12DescriptorHeap* srv_heap=NULL;
    ID3D12InfoQueue* info=NULL;ID3D12Debug* debug=NULL;
    HANDLE event=NULL;unsigned packed_bytes=0,validation_errors=0;
    int ok=0,software=0;HRESULT hr;
#define DEPTH_GPU_TRY(call) do { hr=(call);if(FAILED(hr)) goto cleanup; } while(0)
    int debug_enabled=SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug,(void**)&debug));
    if(debug_enabled){debug->lpVtbl->EnableDebugLayer(debug);debug->lpVtbl->Release(debug);}
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        /* CI may have no physical adapter. WARP is only a test device. */
        software=1;
        DEPTH_GPU_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        DEPTH_GPU_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        DEPTH_GPU_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    s_d3d.device=device;
    if(debug_enabled)DEPTH_GPU_TRY(device->lpVtbl->QueryInterface(device,&IID_ID3D12InfoQueue,(void**)&info));
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    DEPTH_GPU_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,(void**)&queue));
    DEPTH_GPU_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,(void**)&allocator));
    DEPTH_GPU_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,(void**)&list));
    DEPTH_GPU_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event) goto cleanup;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors=DRAW_SRV_BASE+PORT_TEXTURE_UNITS*2;
    DEPTH_GPU_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,(void**)&srv_heap));
    s_d3d.cmd_list=list;s_d3d.srv_heap=srv_heap;
    s_d3d.srv_inc=device->lpVtbl->GetDescriptorHandleIncrementSize(device,hd.Type);

    D3D12DrawRecord a={0},b={0},d16={0},sample={0};
    a.depth_required=b.depth_required=d16.depth_required=1;
    a.depth_surface=(PortDepthKey){0x30000,0xFEED0000u,32,8,8,2};
    b.depth_surface=(PortDepthKey){0x2000,0xFEED0000u,16,4,4,2};
    d16.depth_surface=(PortDepthKey){0x3000,0xFEED0000u,16,8,8,1};
    D3D12_CPU_DESCRIPTOR_HANDLE va=port_depth_view(&a),vb=port_depth_view(&b),v16=port_depth_view(&d16);
    D3D12_CPU_DESCRIPTOR_HANDLE va_again=port_depth_view(&a);
    if(port_depth_count!=3 || va.ptr!=va_again.ptr || va.ptr==vb.ptr || va.ptr==v16.ptr) goto cleanup;
    extern int port_renderer_guest_range_owned(u32,u32);
    for(unsigned i=0;i<port_depth_count;i++) {
        const PortDepthKey* key=&port_depth_surfaces[i].key;
        u32 last=key->address+key->width*key->height*(key->format==1?2:4)-1;
        if(!port_renderer_guest_range_owned(key->address,1) || !port_renderer_guest_range_owned(last,1))goto cleanup;
    }
    list->lpVtbl->OMSetRenderTargets(list,0,NULL,FALSE,&va);
    a.is_clear=1;a.clear_flags=3;
    a.rt_fmt=0x148;
    const u32 depth_a=0x563412;
    list->lpVtbl->ClearDepthStencilView(list,va,port_depth_clear_flags(&a),depth_a/16777215.0f,11,0,NULL);
    port_depth_note_write(&a);
    list->lpVtbl->OMSetRenderTargets(list,0,NULL,FALSE,&vb);
    b.is_clear=1;b.clear_flags=3;
    list->lpVtbl->ClearDepthStencilView(list,vb,port_depth_clear_flags(&b),0.75f,22,0,NULL);
    list->lpVtbl->OMSetRenderTargets(list,0,NULL,FALSE,&va_again);
    d16.is_clear=1;d16.clear_flags=1;
    list->lpVtbl->ClearDepthStencilView(list,v16,port_depth_clear_flags(&d16),0.5f,0,0,NULL);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[5];
    const unsigned surface_index[5]={0,0,1,1,2},plane[5]={0,1,0,1,0};
    UINT64 total=0;
    for(unsigned i=0;i<5;i++) {
        D3D12_RESOURCE_DESC desc;
        port_depth_surfaces[surface_index[i]].resource->lpVtbl->GetDesc(port_depth_surfaces[surface_index[i]].resource,&desc);
        total=(total+511)&~511ull;UINT rows=0;
        device->lpVtbl->GetCopyableFootprints(device,&desc,plane[i],1,total,&footprints[i],&rows,NULL,NULL);
        total=footprints[i].Offset+(UINT64)footprints[i].Footprint.RowPitch*rows;
    }
    D3D12_RESOURCE_DESC packed_desc;
    port_depth_surfaces[0].resource->lpVtbl->GetDesc(port_depth_surfaces[0].resource,&packed_desc);
    packed_desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;packed_desc.Flags=D3D12_RESOURCE_FLAG_NONE;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT packed[2];
    for(unsigned i=0;i<2;i++) {
        total=(total+511)&~511ull;
        device->lpVtbl->GetCopyableFootprints(device,&packed_desc,0,1,total,&packed[i],NULL,NULL,NULL);
        total=packed[i].Offset+(u64)packed[i].Footprint.RowPitch*8;
    }
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd={0};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width=total;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;
    rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DEPTH_GPU_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback));
    extern u8* vm_base;
    memset(vm_base+a.depth_surface.address,0xCD,256);
    sample.tex[0].set=1;sample.tex[0].off=a.depth_surface.address;
    sample.tex[0].w=sample.tex[0].h=8;sample.tex[0].fmt=0xA5;
    sample.tex[0].source_pitch=32;sample.tex[0].mips=1;sample.tex[0].ctrl1=0xAAE4;
    for(unsigned i=0;i<2;i++) {
        port_resolve_texture_unit(&sample,i,0);
        ID3D12Resource* view=sample.tex_resource[0];
        if(!view || view!=port_depth_surfaces[0].packed_view)goto cleanup;
        D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        port_rt_buffer_state(view,&state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
        src.pResource=view;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=packed[i];
        list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
        port_rt_buffer_state(view,&state,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if(i==0) {
            a.clear_flags=2;
            const D3D12_RECT half={0,0,4,8};
            list->lpVtbl->ClearDepthStencilView(list,va_again,port_depth_clear_flags(&a),0.9f,33,1,&half);
            port_depth_note_write(&a);
        }
    }
    a.clear_flags=0xF0;if(port_depth_clear_flags(&a))goto cleanup;
    for(unsigned i=0;i<3;i++) {
        D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=port_depth_surfaces[i].resource;
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->lpVtbl->ResourceBarrier(list,1,&barrier);
    }
    for(unsigned i=0;i<5;i++) {
        D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
        src.pResource=port_depth_surfaces[surface_index[i]].resource;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=plane[i];
        dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprints[i];
        list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    }
    DEPTH_GPU_TRY(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[1]={(ID3D12CommandList*)list};queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    DEPTH_GPU_TRY(queue->lpVtbl->Signal(queue,fence,1));
    DEPTH_GPU_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0) {
        fprintf(stderr,"[compat-check] depth GPU fence timeout\n");abort();
    }
    unsigned char* data=NULL;D3D12_RANGE range={0,(SIZE_T)total};
    DEPTH_GPU_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&data));
    u32 da=0,db=0;u16 dz16=0;
    memcpy(&da,data+footprints[0].Offset,4);memcpy(&db,data+footprints[2].Offset,4);
    memcpy(&dz16,data+footprints[4].Offset,2);da&=0xFFFFFF;db&=0xFFFFFF;
    unsigned sa=data[footprints[1].Offset],sb=data[footprints[3].Offset];
    ok=da==depth_a && db>=0xBFFFFE && db<=0xC00000 && sa==33 && sb==22 && dz16>=32767 && dz16<=32769;
    for(unsigned i=0;i<2;i++)for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
        const u8* pixel=data+packed[i].Offset+y*packed[i].Footprint.RowPitch+x*4;
        const u8 expected[4]={(u8)(da>>8),(u8)da,(i && x<4)?33:11,(u8)(da>>16)};
        for(unsigned c=0;c<4;c++)if(pixel[c]==expected[c])packed_bytes++;
    }
    D3D12_RANGE written={0,0};readback->lpVtbl->Unmap(readback,0,&written);
    if(info)for(UINT64 i=0;i<info->lpVtbl->GetNumStoredMessages(info);i++) {
        SIZE_T size=0;info->lpVtbl->GetMessage(info,i,NULL,&size);
        D3D12_MESSAGE* message=(D3D12_MESSAGE*)malloc(size);if(!message)abort();
        if(SUCCEEDED(info->lpVtbl->GetMessage(info,i,message,&size)) &&
            message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            validation_errors++;
            fprintf(stderr,"[depth-test] D3D12 id=%u %s\n",message->ID,message->pDescription);
        }
        free(message);
    }
    ok=ok && packed_bytes==512 && !validation_errors;
    fprintf(stderr,"[compat-check] depth GPU A-B-A device=%s depth=%06X/%06X stencil=%u/%u d16=%u packed=%u/512 debug=%d errors=%u %s\n",
        software?"WARP":"hardware",da,db,sa,sb,dz16,packed_bytes,debug_enabled,validation_errors,ok?"PASS":"FAIL");
cleanup:
    if(FAILED(hr)) fprintf(stderr,"[compat-check] depth GPU HRESULT=%08lX\n",(unsigned long)hr);
    if(event) CloseHandle(event);
    if(readback) readback->lpVtbl->Release(readback);
    if(srv_heap) srv_heap->lpVtbl->Release(srv_heap);
    if(info) info->lpVtbl->Release(info);
    if(list) list->lpVtbl->Release(list);
    if(allocator) allocator->lpVtbl->Release(allocator);
    if(queue) queue->lpVtbl->Release(queue);
    if(fence) fence->lpVtbl->Release(fence);
    port_release_depth_surfaces();port_rt_storage_release();
    s_d3d.device=NULL;s_d3d.cmd_list=NULL;s_d3d.srv_heap=NULL;
    if(device) device->lpVtbl->Release(device);
    if(warp) warp->lpVtbl->Release(warp);
    if(factory) factory->lpVtbl->Release(factory);
#undef DEPTH_GPU_TRY
    return ok?0:1;
}

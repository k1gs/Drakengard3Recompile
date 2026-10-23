/* Standalone D3D12 contract check. No window, swapchain or guest assets.
 * Reuses the production attachment cache and clear-mask decoder. Readback
 * copies each complete depth/stencil plane as required by CopyTextureRegion:
 * https://learn.microsoft.com/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion */
int port_check_depth_surfaces_gpu(void)
{
    if(s_d3d.device || port_depth_count || port_depth_surfaces) return 2;
    ID3D12Device* device=NULL;
    IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12Resource* readback=NULL;HANDLE event=NULL;
    int ok=0,software=0;HRESULT hr;
#define DEPTH_GPU_TRY(call) do { hr=(call);if(FAILED(hr)) goto cleanup; } while(0)
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        /* CI may have no physical adapter. WARP is only a test device. */
        software=1;
        DEPTH_GPU_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        DEPTH_GPU_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        DEPTH_GPU_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    s_d3d.device=device;
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    DEPTH_GPU_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,(void**)&queue));
    DEPTH_GPU_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,(void**)&allocator));
    DEPTH_GPU_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,(void**)&list));
    DEPTH_GPU_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void**)&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event) goto cleanup;

    D3D12DrawRecord a={0},b={0},d16={0};
    a.depth_required=b.depth_required=d16.depth_required=1;
    a.depth_surface=(PortDepthKey){0x1000,0xFEED0000u,32,8,8,2};
    b.depth_surface=(PortDepthKey){0x2000,0xFEED0000u,16,4,4,2};
    d16.depth_surface=(PortDepthKey){0x3000,0xFEED0000u,16,8,8,1};
    D3D12_CPU_DESCRIPTOR_HANDLE va=port_depth_view(&a),vb=port_depth_view(&b),v16=port_depth_view(&d16);
    D3D12_CPU_DESCRIPTOR_HANDLE va_again=port_depth_view(&a);
    if(port_depth_count!=3 || va.ptr!=va_again.ptr || va.ptr==vb.ptr || va.ptr==v16.ptr) goto cleanup;
    list->lpVtbl->OMSetRenderTargets(list,0,NULL,FALSE,&va);
    a.is_clear=1;a.clear_flags=3;
    list->lpVtbl->ClearDepthStencilView(list,va,port_depth_clear_flags(&a),0.25f,11,0,NULL);
    list->lpVtbl->OMSetRenderTargets(list,0,NULL,FALSE,&vb);
    b.is_clear=1;b.clear_flags=3;
    list->lpVtbl->ClearDepthStencilView(list,vb,port_depth_clear_flags(&b),0.75f,22,0,NULL);
    list->lpVtbl->OMSetRenderTargets(list,0,NULL,FALSE,&va_again);
    a.clear_flags=2; /* Stencil-only clear must preserve A's depth and B. */
    list->lpVtbl->ClearDepthStencilView(list,va_again,port_depth_clear_flags(&a),0.9f,33,0,NULL);
    a.clear_flags=0xF0;if(port_depth_clear_flags(&a)) goto cleanup;
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
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd={0};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width=total;rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;
    rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    DEPTH_GPU_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&rd,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback));
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
    ok=da>=0x3FFFFF && da<=0x400001 && db>=0xBFFFFE && db<=0xC00000 && sa==33 && sb==22 && dz16>=32767 && dz16<=32769;
    fprintf(stderr,"[compat-check] depth GPU A-B-A device=%s depth=%06X/%06X stencil=%u/%u d16=%u %s\n",
        software?"WARP":"hardware",da,db,sa,sb,dz16,ok?"PASS":"FAIL");
    D3D12_RANGE written={0,0};readback->lpVtbl->Unmap(readback,0,&written);
cleanup:
    if(FAILED(hr)) fprintf(stderr,"[compat-check] depth GPU HRESULT=%08lX\n",(unsigned long)hr);
    if(event) CloseHandle(event);
    if(readback) readback->lpVtbl->Release(readback);
    if(list) list->lpVtbl->Release(list);
    if(allocator) allocator->lpVtbl->Release(allocator);
    if(queue) queue->lpVtbl->Release(queue);
    if(fence) fence->lpVtbl->Release(fence);
    port_release_depth_surfaces();s_d3d.device=NULL;
    if(device) device->lpVtbl->Release(device);
    if(warp) warp->lpVtbl->Release(warp);
    if(factory) factory->lpVtbl->Release(factory);
#undef DEPTH_GPU_TRY
    return ok?0:1;
}

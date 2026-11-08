/* Replacing a target must not destroy queued copies to/from its old version. */
int port_check_rt_lifetime_gpu(void)
{
    if(s_d3d.device || port_retired_rt_count) return 2;
    extern uint8_t* vm_base;
    ID3D12Device* device=NULL;IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12Resource* readback=NULL;HANDLE event=NULL;
    ID3D12DescriptorHeap* rtv_heap=NULL;
    int ok=0,software=0;unsigned pixels=0;HRESULT hr=S_OK;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp={0};UINT rows=0;UINT64 total=0;
#define RT_LIFE_TRY(call) do {hr=(call);if(FAILED(hr))goto cleanup;} while(0)
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        software=1;RT_LIFE_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        RT_LIFE_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        RT_LIFE_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    RT_LIFE_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    RT_LIFE_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,&allocator));
    RT_LIFE_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,&list));
    RT_LIFE_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event)goto cleanup;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=PORT_RT_PAGE_SIZE;
    RT_LIFE_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&rtv_heap));
    s_d3d.device=device;s_d3d.cmd_list=list;s_d3d.cmd_queue=queue;s_d3d.fence=fence;
    s_d3d.fence_event=event;s_d3d.rt_rtv_heap=rtv_heap;
    /* Asset-free committed scratch bytes, not game assets. */
    memset(vm_base+0x10000,0,128);
    int slot=off_rt_get(0x10000,0x10000,0,2,2,8);if(slot<0)goto cleanup;
    OffRT* rt=&s_d3d.off_rt[slot];
    D3D12_RESOURCE_DESC td={0};rt->res->lpVtbl->GetDesc(rt->res,&td);
    device->lpVtbl->GetCopyableFootprints(device,&td,0,1,0,&fp,&rows,NULL,&total);
    D3D12_RESOURCE_DESC bd={0};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=total;
    bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    RT_LIFE_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&bd,
        D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,(void**)&rt->up));
    hp.Type=D3D12_HEAP_TYPE_READBACK;
    RT_LIFE_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&bd,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback));
    unsigned char* mapped=NULL;D3D12_RANGE none={0,0};
    RT_LIFE_TRY(rt->up->lpVtbl->Map(rt->up,0,&none,(void**)&mapped));
    memset(mapped,0,(size_t)total);
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++) {
        unsigned char* p=mapped+fp.Offset+(size_t)y*fp.Footprint.RowPitch+x*4;
        p[0]=(unsigned char)(17+x*50);p[1]=(unsigned char)(31+y*70);p[2]=123;p[3]=255;
    }
    rt->up->lpVtbl->Unmap(rt->up,0,NULL);
    off_rt_transition(slot,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};
    src.pResource=rt->up;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=rt->res;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    port_rt_mark_dirty(slot);
    off_rt_transition(slot,D3D12_RESOURCE_STATE_COPY_SOURCE);
    src=dst;dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    /* No Close/Execute before either replacement: fence waits alone cannot
     * protect the resources referenced by the pending upload and readback. */
    if(off_rt_get(0x10000,0x10000,0,4,2,8)!=slot || off_rt_get(0x10000,0x10000,0,4,2,0x0B)!=slot)goto cleanup;
    if(!port_retired_rt_count)goto cleanup;
    RT_LIFE_TRY(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[]={(ID3D12CommandList*)list};queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    RT_LIFE_TRY(queue->lpVtbl->Signal(queue,fence,1));s_d3d.fence_values[0]=1;
    RT_LIFE_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0 || fence->lpVtbl->GetCompletedValue(fence)<1)abort();
    D3D12_RANGE range={0,(SIZE_T)total};RT_LIFE_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&mapped));
    for(unsigned y=0;y<2;y++)for(unsigned x=0;x<2;x++) {
        const unsigned char* p=mapped+fp.Offset+(size_t)y*fp.Footprint.RowPitch+x*4;
        if(p[0]==17+x*50 && p[1]==31+y*70 && p[2]==123 && p[3]==255)pixels++;
    }
    readback->lpVtbl->Unmap(readback,0,NULL);
    port_release_retired_rts();ok=pixels==4 && !port_retired_rt_count;
cleanup:
    fprintf(stderr,"[compat-check] RT lifetime GPU device=%s queued-copy-pixels=%u/4 %s hr=%08lX\n",
        software?"WARP":"hardware",pixels,ok?"PASS":"FAIL",(unsigned long)hr);
    /* Any successfully submitted work has completed above. Failed Close means
     * the list was never submitted, so releasing its raw references is safe. */
    for(unsigned i=0;i<port_retired_rt_count;i++) {
        if(port_retired_rts[i].resource)port_retired_rts[i].resource->lpVtbl->Release(port_retired_rts[i].resource);
        if(port_retired_rts[i].upload)port_retired_rts[i].upload->lpVtbl->Release(port_retired_rts[i].upload);
    }
    free(port_retired_rts);port_retired_rts=NULL;port_retired_rt_count=port_retired_rt_capacity=0;
    if(s_d3d.rt_rtv_heap==rtv_heap)rtv_heap=NULL;
    port_rt_storage_release();
    memset(&s_d3d,0,sizeof(s_d3d));
    if(event)CloseHandle(event);
#define RT_LIFE_RELEASE(p) if(p)p->lpVtbl->Release(p)
    RT_LIFE_RELEASE(readback);RT_LIFE_RELEASE(rtv_heap);
    RT_LIFE_RELEASE(list);RT_LIFE_RELEASE(allocator);RT_LIFE_RELEASE(queue);RT_LIFE_RELEASE(fence);
    RT_LIFE_RELEASE(device);RT_LIFE_RELEASE(warp);RT_LIFE_RELEASE(factory);
#undef RT_LIFE_RELEASE
#undef RT_LIFE_TRY
    return ok?0:1;
}

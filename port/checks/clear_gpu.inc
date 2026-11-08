/* Exercise the production recorder and clear helpers, not a second clear shader. */
int port_check_clear_gpu(void)
{
    if(s_d3d.device || port_depth_count || port_clear_pipeline_count)return 2;
    ID3D12Device* device=NULL;IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12Resource* colors[2]={NULL,NULL};ID3D12Resource* readback=NULL;
    ID3D12DescriptorHeap* heap=NULL;HANDLE event=NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE views[2]={{0},{0}};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[4];UINT rows=0;UINT64 total=0;
    int ok=0,software=0,snapshot=0;unsigned color_pixels=0,depth_pixels=0,stencil_pixels=0;HRESULT hr=S_OK;
#define CLEAR_GPU_TRY(call) do {hr=(call);if(FAILED(hr))goto cleanup;} while(0)
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        software=1;CLEAR_GPU_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        CLEAR_GPU_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        CLEAR_GPU_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    s_d3d.device=device;
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    CLEAR_GPU_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    CLEAR_GPU_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,&allocator));
    CLEAR_GPU_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,&list));
    CLEAR_GPU_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event)goto cleanup;
    s_d3d.cmd_list=list;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
    CLEAR_GPU_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&heap));
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap,&views[0]);views[1]=views[0];
    views[1].ptr+=device->lpVtbl->GetDescriptorHandleIncrementSize(device,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_RESOURCE_DESC td={0};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=8;td.Height=8;
    td.DepthOrArraySize=1;td.MipLevels=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    for(unsigned i=0;i<2;i++) {
        CLEAR_GPU_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&td,
            D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&colors[i]));
        device->lpVtbl->CreateRenderTargetView(device,colors[i],NULL,views[i]);
    }
    rsx_state st;rsx_state_init(&st);st.surface_format=8|(2<<5);st.surface_clip_w=st.surface_clip_h=8;
    st.surface_zeta_pitch=32;st.surface_zeta_offset=0x10000;st.scissor_w=st.scissor_h=8;
    st.stencil_test_enable=0;s_d3d.current_rsx_state=&st;
    d3d12_clear(NULL,0xF3,0xFF0000FF,0.75f,0xA5);
    st.scissor_x=1;st.scissor_y=1;st.scissor_w=3;st.scissor_h=2;
    port_renderer_stencil_method(0x32C,0x0F);d3d12_clear(NULL,0x93,0x00FF0000,0.25f,0x3C);
    st.scissor_x=2;st.scissor_y=2;st.scissor_w=2;st.scissor_h=3;
    port_renderer_stencil_method(0x32C,0xF0);d3d12_clear(NULL,2,0,0,0x50);
    st.scissor_x=st.scissor_y=0;st.scissor_w=st.scissor_h=1;
    d3d12_clear(NULL,1,0,0.5f,0);
    port_renderer_stencil_method(0x32C,0);unsigned before=s_d3d.draw_count;
    d3d12_clear(NULL,2,0,0,0);int zero_mask=before==s_d3d.draw_count;
    st.scissor_w=0;st.scissor_h=8;d3d12_clear(NULL,0xF3,0,0,0);
    st.scissor_x=st.scissor_y=6;st.scissor_w=st.scissor_h=2;
    port_renderer_stencil_method(0x32C,255);d3d12_clear(NULL,0xF2,0xFF00FFFF,0,0x77);
    /* Mutate state before execution. Every prior clear must keep its own mask,
     * rectangle, depth and stencil even with the guest stencil test disabled. */
    st.scissor_w=st.scissor_h=0;port_renderer_stencil_method(0x32C,0);st.stencil_test_enable=1;
    snapshot=zero_mask && s_d3d.draw_count==6 && s_d3d.draws[1].clear_stencil_mask==15 &&
        s_d3d.draws[2].clear_stencil_mask==240 && s_d3d.draws[1].sc_x==1;
    if(!snapshot)goto cleanup;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_view={0};
    for(unsigned i=0;i<s_d3d.draw_count;i++) {
        D3D12DrawRecord* dr=&s_d3d.draws[i];
        if(dr->depth_required){depth_view=port_depth_view(dr);port_clear_depth_view(dr,depth_view);}
        for(unsigned target=0;target<2;target++) {
            D3D12DrawRecord clear=*dr;
            if(i==0 && target==1){clear.cc[0]=0;clear.cc[1]=1;clear.cc[2]=0;clear.cc[3]=1;}
            port_clear_color_view(&clear,views[target],8,8,td.Format);
        }
    }
    if(port_depth_count!=1)goto cleanup;
    for(unsigned i=0;i<4;i++) {
        D3D12_RESOURCE_DESC desc=td;
        if(i>=2)port_depth_surfaces[0].resource->lpVtbl->GetDesc(port_depth_surfaces[0].resource,&desc);
        total=(total+511)&~511ull;
        device->lpVtbl->GetCopyableFootprints(device,&desc,i<2?0:i-2,1,total,&fp[i],&rows,NULL,NULL);
        total=fp[i].Offset+(UINT64)fp[i].Footprint.RowPitch*rows;
    }
    D3D12_RESOURCE_DESC rb={0};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=total;
    rb.Height=1;rb.DepthOrArraySize=1;rb.MipLevels=1;rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type=D3D12_HEAP_TYPE_READBACK;
    CLEAR_GPU_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&rb,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback));
    for(unsigned i=0;i<3;i++) {
        D3D12_RESOURCE_BARRIER b={0};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource=i<2?colors[i]:port_depth_surfaces[0].resource;
        b.Transition.StateBefore=i<2?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_DEPTH_WRITE;
        b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->lpVtbl->ResourceBarrier(list,1,&b);
    }
    for(unsigned i=0;i<4;i++) {
        D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=i<2?colors[i]:port_depth_surfaces[0].resource;
        src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=i<2?0:i-2;
        dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp[i];
        list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    }
    CLEAR_GPU_TRY(list->lpVtbl->Close(list));ID3D12CommandList* lists[]={(ID3D12CommandList*)list};
    queue->lpVtbl->ExecuteCommandLists(queue,1,lists);CLEAR_GPU_TRY(queue->lpVtbl->Signal(queue,fence,1));
    CLEAR_GPU_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0 || fence->lpVtbl->GetCompletedValue(fence)<1)abort();
    unsigned char* bytes=NULL;D3D12_RANGE range={0,(SIZE_T)total};
    CLEAR_GPU_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&bytes));
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++) {
        int a=x>=1&&x<4&&y>=1&&y<3,b=x>=2&&x<4&&y>=2&&y<5,c=x>=6&&y>=6;
        for(unsigned target=0;target<2;target++) {
            unsigned char expected[4]={0,(unsigned char)(target?255:0),(unsigned char)(target?0:255),255};
            if(a){expected[0]=255;expected[3]=0;}
            if(c){expected[0]=0;expected[1]=255;expected[2]=255;expected[3]=255;}
            const unsigned char* actual=bytes+fp[target].Offset+(size_t)y*fp[target].Footprint.RowPitch+x*4;
            if(!memcmp(expected,actual,4))color_pixels++;
        }
        const unsigned char* dp=bytes+fp[2].Offset+(size_t)y*fp[2].Footprint.RowPitch+x*4;
        u32 packed;memcpy(&packed,dp,4);float depth=(float)(packed&0xFFFFFFu)/16777215.0f;
        float expected_depth=x==0&&y==0?0.5f:a?0.25f:0.75f;
        if(fabsf(depth-expected_depth)<0.000001f)depth_pixels++;
        const unsigned char* sp=bytes+fp[3].Offset+(size_t)y*fp[3].Footprint.RowPitch+x;
        unsigned expected_stencil=c?0x77:b?(a?0x5C:0x55):a?0xAC:0xA5;
        if(sp[0]==expected_stencil)stencil_pixels++;
    }
    readback->lpVtbl->Unmap(readback,0,NULL);
    ok=snapshot&&color_pixels==128&&depth_pixels==64&&stencil_pixels==64;
cleanup:
    fprintf(stderr,"[compat-check] clear GPU device=%s snapshot=%d color=%u/128 depth=%u/64 stencil=%u/64 %s hr=%08lX\n",
        software?"WARP":"hardware",snapshot,color_pixels,depth_pixels,stencil_pixels,ok?"PASS":"FAIL",(unsigned long)hr);
    port_release_clear_pipelines();port_release_depth_surfaces();port_renderer_stencil_reset();
    memset(&s_d3d,0,sizeof(s_d3d));if(event)CloseHandle(event);
#define CLEAR_GPU_RELEASE(p) if(p)p->lpVtbl->Release(p)
    CLEAR_GPU_RELEASE(colors[0]);CLEAR_GPU_RELEASE(colors[1]);CLEAR_GPU_RELEASE(readback);CLEAR_GPU_RELEASE(heap);
    CLEAR_GPU_RELEASE(list);CLEAR_GPU_RELEASE(allocator);CLEAR_GPU_RELEASE(queue);CLEAR_GPU_RELEASE(fence);
    CLEAR_GPU_RELEASE(device);CLEAR_GPU_RELEASE(warp);CLEAR_GPU_RELEASE(factory);
#undef CLEAR_GPU_RELEASE
#undef CLEAR_GPU_TRY
    return ok?0:1;
}

/* Standalone D3D12 contract check for the native render-to-texture feedback
 * snapshot.  The production helper owns the snapshot resources; this check
 * only supplies two small native RTs and a readback target. */
int port_check_feedback_surfaces_gpu(void)
{
    if (s_d3d.device) return 2;
    ID3D12Device* device=NULL; IDXGIFactory4* factory=NULL; IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL; ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL; ID3D12Fence* fence=NULL;
    ID3D12DescriptorHeap* rtv_heap=NULL; ID3D12Resource* readback=NULL;
    ID3D12Resource* src[2]={NULL,NULL}; HANDLE event=NULL;
    ID3D12Resource* snap1=NULL; ID3D12Resource* snap2=NULL; ID3D12Resource* snap3=NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2]={{0},{0}};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[3]; UINT rows=0; UINT64 total=0;
    int ok=0,software=0; HRESULT hr=S_OK;
#define FEEDBACK_GPU_TRY(call) do { hr=(call); if(FAILED(hr)) goto cleanup; } while(0)

    /* The mask parser must count reads only; declarations alone are not
     * texture reads.  This runs before any device work so a parser regression
     * cannot be hidden by the GPU fixture. */
    if(port_fp_sample_mask("Texture2D rsx_tex0; TextureCube rsx_tex15; rsx_tex0.Sample(x,y); rsx_tex15.SampleLevel(x,y,z);")!=0x8001u ||
       port_fp_sample_mask("Texture2D rsx_tex[16];")!=0u) goto cleanup;

    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if (FAILED(hr)) {
        software=1;
        FEEDBACK_GPU_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        FEEDBACK_GPU_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        FEEDBACK_GPU_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,
                                           &IID_ID3D12Device,(void**)&device));
    }
    s_d3d.device=device;
    D3D12_COMMAND_QUEUE_DESC qd={0}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    FEEDBACK_GPU_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    FEEDBACK_GPU_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,
                                                            &IID_ID3D12CommandAllocator,&allocator));
    FEEDBACK_GPU_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,
                                                       &IID_ID3D12GraphicsCommandList,&list));
    FEEDBACK_GPU_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,
                                                 &IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL); if(!event) goto cleanup;

    D3D12_DESCRIPTOR_HEAP_DESC hd={0}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors=2;
    FEEDBACK_GPU_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,
                                                          &rtv_heap));
    UINT rtv_inc=device->lpVtbl->GetDescriptorHandleIncrementSize(device,
                                                                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap,&rtv[0]);
    rtv[1]=rtv[0]; rtv[1].ptr += rtv_inc;

    D3D12_HEAP_PROPERTIES hp={0}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td={0}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=2; td.Height=2; td.DepthOrArraySize=1; td.MipLevels=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1;
    td.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv={0}; cv.Format=td.Format;
    for (int i=0;i<2;i++) {
        FEEDBACK_GPU_TRY(device->lpVtbl->CreateCommittedResource(
            device,&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,
            &cv,&IID_ID3D12Resource,&src[i]));
        device->lpVtbl->CreateRenderTargetView(device,src[i],NULL,rtv[i]);
        s_d3d.off_rt[i].res=src[i]; s_d3d.off_rt[i].up=NULL;
        s_d3d.off_rt[i].off=0x1000u+(u32)i*0x1000u;
        s_d3d.off_rt[i].w=2; s_d3d.off_rt[i].h=2;
        s_d3d.off_rt[i].dxgi=(u32)DXGI_FORMAT_R8G8B8A8_UNORM;
        s_d3d.off_rt[i].st=D3D12_RESOURCE_STATE_RENDER_TARGET;
        s_d3d.off_rt[i].used=1;
    }
    s_d3d.cmd_list=list;

    int mrt_none[3]={-1,-1,-1}; int mrt_alias[3]={-1,0,-1};
    D3D12DrawRecord bind_probe={0};
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) bind_probe.tex_rt[u]=-1;
    bind_probe.tex_rt[0]=0;
    if(!port_feedback_alias(0,0,mrt_none) ||
       !port_feedback_alias(0,1,mrt_alias) ||
       port_feedback_alias(0,1,mrt_none) || port_feedback_alias(-1,0,mrt_none)) goto cleanup;
    port_feedback_bind(&bind_probe,0,0,mrt_none,0);
    if(port_feedback_count!=0) goto cleanup;
    D3D12_RESOURCE_DESC rd=td;
    for (unsigned i=0;i<3;i++) {
        total=(total+511u)&~511ull;
        UINT64 bytes=0;
        device->lpVtbl->GetCopyableFootprints(device,&rd,0,1,total,&fp[i],&rows,NULL,&bytes);
        total=fp[i].Offset+(UINT64)fp[i].Footprint.RowPitch*rows;
    }
    D3D12_RESOURCE_DESC rb={0}; rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    rb.Width=total; rb.Height=1; rb.DepthOrArraySize=1; rb.MipLevels=1;
    rb.SampleDesc.Count=1; rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES rhp={0}; rhp.Type=D3D12_HEAP_TYPE_READBACK;
    FEEDBACK_GPU_TRY(device->lpVtbl->CreateCommittedResource(device,&rhp,D3D12_HEAP_FLAG_NONE,&rb,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,&readback));

    port_feedback_begin_batch();
    /* Test copy ordering, not adapter-dependent UNORM midpoint rounding. */
    const float ca[4]={0.0f,1.0f,0.0f,1.0f};
    const float cb[4]={1.0f,0.0f,1.0f,1.0f};
    const float cc[4]={1.0f,1.0f,0.0f,1.0f};
    list->lpVtbl->ClearRenderTargetView(list,rtv[0],ca,0,NULL);
    snap1=port_feedback_copy(0);
    if(!snap1 || port_feedback_count<1 ||
       port_feedback_surfaces[0].state!=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
       s_d3d.off_rt[0].st!=D3D12_RESOURCE_STATE_RENDER_TARGET) goto cleanup;
    D3D12_RESOURCE_BARRIER b={0}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=snap1; b.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->lpVtbl->ResourceBarrier(list,1,&b);
    D3D12_TEXTURE_COPY_LOCATION dst={0},ss={0}; dst.pResource=readback;
    dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=fp[0];
    ss.pResource=snap1; ss.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&ss,NULL);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&b);

    list->lpVtbl->ClearRenderTargetView(list,rtv[0],cb,0,NULL);
    snap2=port_feedback_copy(0);
    if(!snap2 || snap2!=snap1 || port_feedback_count<1 ||
       port_feedback_surfaces[0].state!=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
       s_d3d.off_rt[0].st!=D3D12_RESOURCE_STATE_RENDER_TARGET) goto cleanup;
    b.Transition.pResource=snap2; b.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&b);
    ss.pResource=snap2;
    dst.PlacedFootprint=fp[1]; list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&ss,NULL);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&b);

    list->lpVtbl->ClearRenderTargetView(list,rtv[1],cc,0,NULL);
    snap3=port_feedback_copy(1);
    if(!snap3 || snap3==snap1 || port_feedback_count<2 ||
       port_feedback_surfaces[0].state!=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
       port_feedback_surfaces[1].state!=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
       s_d3d.off_rt[1].st!=D3D12_RESOURCE_STATE_RENDER_TARGET) goto cleanup;
    b.Transition.pResource=snap3; b.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&b);
    ss.pResource=snap3; dst.PlacedFootprint=fp[2];
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&ss,NULL);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&b);

    FEEDBACK_GPU_TRY(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[1]={(ID3D12CommandList*)list};
    queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    FEEDBACK_GPU_TRY(queue->lpVtbl->Signal(queue,fence,1));
    FEEDBACK_GPU_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0 || fence->lpVtbl->GetCompletedValue(fence)<1) {
        fprintf(stderr,"[compat-check] feedback GPU fence incomplete\n"); goto cleanup;
    }
    unsigned char* data=NULL; D3D12_RANGE range={0,(SIZE_T)total};
    FEEDBACK_GPU_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&data));
    const unsigned char want[3][4]={{0,255,0,255},{255,0,255,255},{255,255,0,255}};
    for(unsigned k=0;k<3;k++) {
        const unsigned char* p=data+fp[k].Offset;
        fprintf(stderr,"[compat-check] feedback phase%u rgba=%u,%u,%u,%u\n",k,p[0],p[1],p[2],p[3]);
    }
    int pixels=1;
    for(unsigned k=0;k<3;k++) for(unsigned y=0;y<2;y++) for(unsigned x=0;x<2;x++) {
        const unsigned char* p=data+fp[k].Offset+y*fp[k].Footprint.RowPitch+x*4;
        for(unsigned c=0;c<4;c++) if(p[c]!=want[k][c]) pixels=0;
    }
    D3D12_RANGE written={0,0}; readback->lpVtbl->Unmap(readback,0,&written);
    int source_rt=s_d3d.off_rt[0].st==D3D12_RESOURCE_STATE_RENDER_TARGET &&
                  s_d3d.off_rt[1].st==D3D12_RESOURCE_STATE_RENDER_TARGET;
    int snapshots_psr=port_feedback_count>=2 &&
        port_feedback_surfaces[0].state==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE &&
        port_feedback_surfaces[1].state==D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    ok=pixels && source_rt && snapshots_psr;
    fprintf(stderr,"[compat-check] feedback GPU device=%s phase1/phase2/distinct=%d source-rt=%d snapshots-psr=%d %s\n",
        software?"WARP":"hardware",pixels,source_rt,snapshots_psr,ok?"PASS":"FAIL");
cleanup:
    if(FAILED(hr)) fprintf(stderr,"[compat-check] feedback GPU HRESULT=%08lX\n",(unsigned long)hr);
    port_feedback_release();
    for(int i=0;i<2;i++) { s_d3d.off_rt[i].res=NULL; if(src[i]) src[i]->lpVtbl->Release(src[i]); }
    s_d3d.cmd_list=NULL; s_d3d.device=NULL;
    if(readback) readback->lpVtbl->Release(readback);
    if(rtv_heap) rtv_heap->lpVtbl->Release(rtv_heap);
    if(event) CloseHandle(event);
    if(list) list->lpVtbl->Release(list);
    if(allocator) allocator->lpVtbl->Release(allocator);
    if(queue) queue->lpVtbl->Release(queue);
    if(fence) fence->lpVtbl->Release(fence);
    if(device) device->lpVtbl->Release(device);
    if(warp) warp->lpVtbl->Release(warp);
    if(factory) factory->lpVtbl->Release(factory);
#undef FEEDBACK_GPU_TRY
    return ok?0:1;
}

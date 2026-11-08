/* Actual rasterization/readback: A-B-A state snapshots, clipped and empty
 * scissors, with a full viewport throughout. No window or game is created. */
int port_check_scissor_gpu(void)
{
    rsx_state st;memset(&st,0,sizeof(st));
    D3D12DrawRecord a={0},b={0},empty={0},missing={0};
    st.scissor_x=1;st.scissor_y=1;st.scissor_w=2;st.scissor_h=3;
    port_snapshot_scissor(&a,&st);
    st.scissor_x=5;st.scissor_y=0;st.scissor_w=10;st.scissor_h=2;
    port_snapshot_scissor(&b,&st);
    st.scissor_x=0;st.scissor_w=0;st.scissor_h=8;
    port_snapshot_scissor(&empty,&st);port_snapshot_scissor(&missing,NULL);
    D3D12_RECT rects[4]={port_draw_scissor(&a,8,8),port_draw_scissor(&b,8,8),
                         port_draw_scissor(&a,8,8),port_draw_scissor(&empty,8,8)};
    D3D12_RECT full=port_draw_scissor(&missing,8,8);
    D3D12DrawRecord overflow={0};overflow.sc_x=0xFFFFFFFEu;overflow.sc_w=10;
    overflow.sc_y=0xFFFFFFFEu;overflow.sc_h=10;
    D3D12_RECT outside=port_draw_scissor(&overflow,8,8);
    int snapshot=rects[0].left==1 && rects[0].top==1 && rects[0].right==3 &&
        rects[0].bottom==4 && !memcmp(&rects[0],&rects[2],sizeof(full));
    int clipped=rects[1].left==5 && rects[1].right==8 && rects[1].bottom==2;
    int defaults=full.left==0 && full.top==0 && full.right==8 && full.bottom==8;
    int bounds=outside.left==8 && outside.top==8 && outside.right==8 && outside.bottom==8;
    if(!snapshot || !clipped || !defaults || !bounds || rects[3].right!=rects[3].left)
        return 1;

    ID3D12Device* device=NULL;IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12DescriptorHeap* heap=NULL;ID3D12Resource* target=NULL;ID3D12Resource* readback=NULL;
    ID3D12RootSignature* root=NULL;ID3D12PipelineState* pso=NULL;
    ID3DBlob *sig=NULL,*vs=NULL,*ps=NULL,*error=NULL;
    HANDLE event=NULL;HRESULT hr=S_OK;int ok=0,software=0;unsigned colored=0;
#define SCISSOR_TRY(call) do { hr=(call);if(FAILED(hr)) goto cleanup; } while(0)
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        software=1;
        SCISSOR_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        SCISSOR_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        SCISSOR_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    SCISSOR_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    SCISSOR_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,&allocator));
    SCISSOR_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,&list));
    SCISSOR_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event) goto cleanup;
    D3D12_ROOT_PARAMETER param={0};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues=4;param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs={0};rs.NumParameters=1;rs.pParameters=&param;
    SCISSOR_TRY(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&error));
    SCISSOR_TRY(device->lpVtbl->CreateRootSignature(device,0,sig->lpVtbl->GetBufferPointer(sig),
        sig->lpVtbl->GetBufferSize(sig),&IID_ID3D12RootSignature,(void**)&root));
    const char* shader="cbuffer C:register(b0){float4 color;}"
        "float4 vs(uint i:SV_VertexID):SV_Position{"
        "return float4(i==2?3:-1,i==1?3:-1,0,1);}"
        "float4 ps():SV_Target{return color;}";
    SCISSOR_TRY(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"vs","vs_5_0",0,0,&vs,NULL));
    SCISSOR_TRY(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"ps","ps_5_0",0,0,&ps,NULL));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={0};pd.pRootSignature=root;
    pd.VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs);pd.VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode=ps->lpVtbl->GetBufferPointer(ps);pd.PS.BytecodeLength=ps->lpVtbl->GetBufferSize(ps);
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable=TRUE;pd.SampleMask=UINT_MAX;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.SampleDesc.Count=1;
    SCISSOR_TRY(device->lpVtbl->CreateGraphicsPipelineState(device,&pd,&IID_ID3D12PipelineState,(void**)&pso));
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    SCISSOR_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap,&rtv);
    D3D12_RESOURCE_DESC td={0};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=8;td.Height=8;td.DepthOrArraySize=1;td.MipLevels=1;td.Format=pd.RTVFormats[0];td.SampleDesc.Count=1;
    td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    SCISSOR_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&td,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,&target));
    device->lpVtbl->CreateRenderTargetView(device,target,NULL,rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT64 total=0;
    device->lpVtbl->GetCopyableFootprints(device,&td,0,1,0,&fp,NULL,NULL,&total);
    D3D12_RESOURCE_DESC rb={0};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=total;
    rb.Height=1;rb.DepthOrArraySize=1;rb.MipLevels=1;rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type=D3D12_HEAP_TYPE_READBACK;
    SCISSOR_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&rb,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,&readback));
    const float black[4]={0,0,0,1},colors[4][4]={{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,1,1}};
    list->lpVtbl->ClearRenderTargetView(list,rtv,black,0,NULL);
    list->lpVtbl->SetGraphicsRootSignature(list,root);list->lpVtbl->SetPipelineState(list,pso);
    list->lpVtbl->OMSetRenderTargets(list,1,&rtv,FALSE,NULL);
    D3D12_VIEWPORT vp={0,0,8,8,0,1};list->lpVtbl->RSSetViewports(list,1,&vp);
    list->lpVtbl->IASetPrimitiveTopology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(unsigned i=0;i<4;i++) {
        list->lpVtbl->RSSetScissorRects(list,1,&rects[i]);
        list->lpVtbl->SetGraphicsRoot32BitConstants(list,0,4,colors[i],0);
        list->lpVtbl->DrawInstanced(list,3,1,0,0);
    }
    D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=target;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};dst.pResource=readback;
    dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    src.pResource=target;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    SCISSOR_TRY(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[]={(ID3D12CommandList*)list};queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    SCISSOR_TRY(queue->lpVtbl->Signal(queue,fence,1));
    SCISSOR_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0 || fence->lpVtbl->GetCompletedValue(fence)<1) abort();
    unsigned char* data=NULL;D3D12_RANGE range={0,(SIZE_T)total};
    SCISSOR_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&data));
    ok=1;
    for(unsigned y=0;y<8;y++) for(unsigned x=0;x<8;x++) {
        unsigned char* px=data+fp.Offset+y*fp.Footprint.RowPitch+x*4;
        unsigned blue=x>=1 && x<3 && y>=1 && y<4,green=x>=5 && y<2;
        if(px[0]!=0 || px[1]!=(green?255:0) || px[2]!=(blue?255:0) || px[3]!=255) ok=0;
        if(px[0] || px[1] || px[2]) colored++;
    }
    D3D12_RANGE written={0,0};readback->lpVtbl->Unmap(readback,0,&written);
cleanup:
    fprintf(stderr,"[compat-check] scissor GPU device=%s snapshot=%d clipped=%d default=%d overflow=%d colored=%u/12 %s hr=%08lX\n",
        software?"WARP":"hardware",snapshot,clipped,defaults,bounds,colored,ok?"PASS":"FAIL",(unsigned long)hr);
    if(event) CloseHandle(event);
#define SCISSOR_RELEASE(p) if(p) p->lpVtbl->Release(p)
    SCISSOR_RELEASE(readback);SCISSOR_RELEASE(target);SCISSOR_RELEASE(heap);
    SCISSOR_RELEASE(pso);SCISSOR_RELEASE(root);SCISSOR_RELEASE(sig);SCISSOR_RELEASE(vs);SCISSOR_RELEASE(ps);SCISSOR_RELEASE(error);
    SCISSOR_RELEASE(list);SCISSOR_RELEASE(allocator);SCISSOR_RELEASE(queue);SCISSOR_RELEASE(fence);
    SCISSOR_RELEASE(device);SCISSOR_RELEASE(warp);SCISSOR_RELEASE(factory);
#undef SCISSOR_RELEASE
#undef SCISSOR_TRY
    return ok?0:1;
}

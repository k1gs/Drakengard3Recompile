/* Parser -> draw snapshot -> actual stencil/depth rasterization -> readback. */
static int port_stencil_fixture_draw(ID3D12Device* device,ID3D12GraphicsCommandList* list,
    ID3D12RootSignature* root,ID3DBlob* vs,ID3DBlob* ps,const rsx_state* st,
    unsigned column,float z,int reverse,int depth_test,const float color[4],
    ID3D12PipelineState** owned)
{
    PortStencilKey key;u32 reference;
    if(!port_pack_stencil(st,&key,&reference))return 0;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={0};pd.pRootSignature=root;
    pd.VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs);pd.VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode=ps->lpVtbl->GetBufferPointer(ps);pd.PS.BytecodeLength=ps->lpVtbl->GetBufferSize(ps);
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable=TRUE;pd.SampleMask=UINT_MAX;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.SampleDesc.Count=1;
    pd.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.DepthStencilState.DepthEnable=depth_test;pd.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;port_apply_stencil(&pd.DepthStencilState,&key);
    if(FAILED(device->lpVtbl->CreateGraphicsPipelineState(device,&pd,&IID_ID3D12PipelineState,(void**)owned)))return 0;
    list->lpVtbl->SetPipelineState(list,*owned);list->lpVtbl->OMSetStencilRef(list,reference);
    float constants[8]={color[0],color[1],color[2],color[3],z,(float)reverse,0,0};
    list->lpVtbl->SetGraphicsRoot32BitConstants(list,0,8,constants,0);
    D3D12_RECT rect={(LONG)column,0,(LONG)column+1,4};list->lpVtbl->RSSetScissorRects(list,1,&rect);
    list->lpVtbl->DrawInstanced(list,3,1,0,0);return 1;
}
int port_check_stencil_gpu(void)
{
    rsx_state st;rsx_state_init(&st);
    rsx_process_method(&st,0x328,1);rsx_process_method(&st,0x32C,0x3F);
    rsx_process_method(&st,0x334,0x12);rsx_process_method(&st,0x338,0x7F);
    D3D12DrawRecord a={0},b={0};a.depth_surface.format=b.depth_surface.format=2;
    port_snapshot_stencil(&a,&st);rsx_process_method(&st,0x334,0x13);port_snapshot_stencil(&b,&st);
    int snapshot=port_stencil_equal(&a.stencil_key,&b.stencil_key) && a.stencil_ref==0x12 && b.stencil_ref==0x13;
    rsx_process_method(&st,0x348,1);rsx_process_method(&st,0x34C,0x3F);rsx_process_method(&st,0x358,0x7F);
    rsx_process_method(&st,0x354,0x14);
    PortStencilKey invalid;u32 ref;int asymmetric=!port_pack_stencil(&st,&invalid,&ref);
    if(!snapshot || !asymmetric)return 1;
    ID3D12Device* device=NULL;IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12DescriptorHeap* heap=NULL;ID3D12Resource* target=NULL;ID3D12Resource* readback=NULL;
    ID3D12RootSignature* root=NULL;ID3D12PipelineState* psos[32]={0};unsigned pso_count=0;
    ID3DBlob *sig=NULL,*vs=NULL,*ps=NULL,*error=NULL;
    HANDLE event=NULL;HRESULT hr=S_OK;int ok=0,software=0;unsigned pixels=0;
    ID3D12Resource* depth=NULL;ID3D12DescriptorHeap* dsv_heap=NULL;
#define STENCIL_TRY(call) do { hr=(call);if(FAILED(hr)) goto cleanup; } while(0)
    HRESULT debug_hr=S_OK;ID3D12Debug* debug=NULL;
    debug_hr=D3D12GetDebugInterface(&IID_ID3D12Debug,(void**)&debug);
    if(SUCCEEDED(debug_hr)) {debug->lpVtbl->EnableDebugLayer(debug);debug->lpVtbl->Release(debug);}
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        software=1;
        STENCIL_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        STENCIL_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        STENCIL_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    STENCIL_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    STENCIL_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,&allocator));
    STENCIL_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,&list));
    STENCIL_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event) goto cleanup;
    D3D12_ROOT_PARAMETER param={0};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues=8;param.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rs={0};rs.NumParameters=1;rs.pParameters=&param;
    STENCIL_TRY(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&error));
    STENCIL_TRY(device->lpVtbl->CreateRootSignature(device,0,sig->lpVtbl->GetBufferPointer(sig),
        sig->lpVtbl->GetBufferSize(sig),&IID_ID3D12RootSignature,(void**)&root));
    const char* shader="cbuffer C:register(b0){float4 color;float4 config;}"
        "float4 vs(uint i:SV_VertexID):SV_Position{"
        "float2 p=float2(i==2?3:-1,i==1?3:-1);if(config.y>0)p.x=-p.x;return float4(p,config.x,1);}"
        "float4 ps():SV_Target{return color;}";
    STENCIL_TRY(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"vs","vs_5_0",0,0,&vs,NULL));
    STENCIL_TRY(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"ps","ps_5_0",0,0,&ps,NULL));
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    STENCIL_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap,&rtv);
    D3D12_RESOURCE_DESC td={0};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=9;td.Height=4;td.DepthOrArraySize=1;td.MipLevels=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;
    td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    STENCIL_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&td,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,&target));
    device->lpVtbl->CreateRenderTargetView(device,target,NULL,rtv);
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    STENCIL_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&dsv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(dsv_heap,&dsv);
    D3D12_RESOURCE_DESC dd=td;dd.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    STENCIL_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&dd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,NULL,&IID_ID3D12Resource,&depth));
    device->lpVtbl->CreateDepthStencilView(device,depth,NULL,dsv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2];UINT64 total=0,bytes=0;
    device->lpVtbl->GetCopyableFootprints(device,&td,0,1,0,&fp[0],NULL,NULL,&bytes);
    total=(bytes+511)&~511ull;
    device->lpVtbl->GetCopyableFootprints(device,&dd,1,1,total,&fp[1],NULL,NULL,&bytes);
    total=fp[1].Offset+(UINT64)fp[1].Footprint.RowPitch*dd.Height;
    D3D12_RESOURCE_DESC rb={0};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=total;
    rb.Height=1;rb.DepthOrArraySize=1;rb.MipLevels=1;rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES rh={0};rh.Type=D3D12_HEAP_TYPE_READBACK;
    STENCIL_TRY(device->lpVtbl->CreateCommittedResource(device,&rh,D3D12_HEAP_FLAG_NONE,&rb,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,&readback));
    const float black[4]={0,0,0,1},red[4]={1,0,0,1},green[4]={0,1,0,1},blue[4]={0,0,1,1};
    const float yellow[4]={1,1,0,1},magenta[4]={1,0,1,1},cyan[4]={0,1,1,1},white[4]={1,1,1,1},orange[4]={1,0.5f,0,1};
    list->lpVtbl->ClearRenderTargetView(list,rtv,black,0,NULL);
    list->lpVtbl->ClearDepthStencilView(list,dsv,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,0.5f,0xA0,0,NULL);
    list->lpVtbl->SetGraphicsRootSignature(list,root);list->lpVtbl->OMSetRenderTargets(list,1,&rtv,FALSE,&dsv);
    D3D12_VIEWPORT vp={0,0,9,4,0,1};list->lpVtbl->RSSetViewports(list,1,&vp);
    list->lpVtbl->IASetPrimitiveTopology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
#define DRAW(col,z,rev,dep,color) do {if(pso_count>=32 || !port_stencil_fixture_draw(device,list,root,vs,ps,&st,col,z,rev,dep,color,&psos[pso_count]))goto cleanup;pso_count++;} while(0)
#define BASE() do {rsx_state_init(&st);st.stencil_test_enable=1;} while(0)
#define PROBE(col,refval,mask,color) do {BASE();st.stencil_func=0x202;st.stencil_ref=refval;st.stencil_mask=mask;port_renderer_stencil_method(0x32C,0);DRAW(col,0,0,0,color);} while(0)
    BASE();st.stencil_ref=0x35;st.stencil_op_zpass=0x1E01;port_renderer_stencil_method(0x32C,0x0F);DRAW(0,0,0,0,black);
    PROBE(0,0xA5,255,red);PROBE(0,0xA6,255,white); /* wrong dynamic ref must fail */
    BASE();st.stencil_ref=0x3B;st.stencil_op_zpass=0x1E01;port_renderer_stencil_method(0x32C,0x0F);DRAW(1,0,0,0,black);
    PROBE(1,0x0B,0x0F,green);
    BASE();st.stencil_op_zfail=0x1E02;DRAW(2,0.75f,0,1,black);PROBE(2,0xA1,255,blue);
    BASE();st.stencil_func=0x200;st.stencil_op_fail=0;DRAW(3,0,0,0,black);PROBE(3,0,255,yellow);
    BASE();st.stencil_op_zpass=0x150A;DRAW(4,0,0,0,black);PROBE(4,0x5F,255,magenta);
    D3D12_RECT five={5,0,6,4};list->lpVtbl->ClearDepthStencilView(list,dsv,D3D12_CLEAR_FLAG_STENCIL,0,255,1,&five);
    BASE();st.stencil_op_zpass=0x1E02;DRAW(5,0,0,0,black);PROBE(5,255,255,blue);
    BASE();st.stencil_op_zpass=0x8507;DRAW(5,0,0,0,black);PROBE(5,0,255,cyan);
    for(unsigned col=6;col<8;col++) {
        BASE();st.stencil_ref=0x66;st.stencil_op_zpass=0x1E01;
        port_renderer_stencil_method(0x348,1);port_renderer_stencil_method(0x354,0x66);port_renderer_stencil_method(0x364,0);
        DRAW(col,0,col==7,0,black);PROBE(col,0x66,255,white);
    }
    BASE();st.stencil_test_enable=0;DRAW(8,0,0,0,orange);
#undef PROBE
#undef BASE
#undef DRAW
    D3D12_RESOURCE_BARRIER barriers[2]={{0},{0}};
    for(unsigned i=0;i<2;i++) {
        barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barriers[i].Transition.pResource=i?depth:target;
        barriers[i].Transition.StateBefore=i?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[i].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barriers[i].Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->lpVtbl->ResourceBarrier(list,2,barriers);
    for(unsigned i=0;i<2;i++) {
        D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=i?depth:target;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=i;
        dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp[i];
        list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    }
    STENCIL_TRY(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[]={(ID3D12CommandList*)list};queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    STENCIL_TRY(queue->lpVtbl->Signal(queue,fence,1));STENCIL_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0 || fence->lpVtbl->GetCompletedValue(fence)<1)abort();
    unsigned char* data=NULL;D3D12_RANGE range={0,(SIZE_T)total};STENCIL_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&data));
    static const u8 expected[7][4]={{255,0,0,0xA5},{0,255,0,0xAB},{0,0,255,0xA1},{255,255,0,0},{255,0,255,0x5F},{0,255,255,0},{255,128,0,0xA0}};
    ok=1;
    for(unsigned y=0;y<4;y++) {
        for(unsigned x=0;x<9;x++) {
            u8* p=data+fp[0].Offset+y*fp[0].Footprint.RowPitch+x*4;
            u8 stencil=data[fp[1].Offset+y*fp[1].Footprint.RowPitch+x];int valid=p[3]==255;
            if(x<6 || x==8) {
                const u8* e=expected[x==8?6:x];for(unsigned c=0;c<3;c++)if(abs((int)p[c]-e[c])>1)valid=0;
                if(stencil!=e[3])valid=0;
            } else {
                if(stencil!=0 && stencil!=0x66)valid=0;
                u8 shade=stencil==0x66?255:0;
                if(p[0]!=shade || p[1]!=shade || p[2]!=shade)valid=0;
            }
            if(valid)pixels++;else {ok=0;fprintf(stderr,"[stencil-test] pixel=%u,%u rgba=%u,%u,%u,%u stencil=%02X\n",x,y,p[0],p[1],p[2],p[3],stencil);}
        }
        u8 left=data[fp[1].Offset+y*fp[1].Footprint.RowPitch+6],right=data[fp[1].Offset+y*fp[1].Footprint.RowPitch+7];
        if(!((left==0 && right==0x66)||(left==0x66 && right==0)))ok=0;
    }
    D3D12_RANGE written={0,0};readback->lpVtbl->Unmap(readback,0,&written);
cleanup:
    fprintf(stderr,"[compat-check] stencil GPU device=%s snapshot=%d asymmetric-rejected=%d pixels=%u/36 %s hr=%08lX\n",
        software?"WARP":"hardware",snapshot,asymmetric,pixels,ok?"PASS":"FAIL",(unsigned long)hr);
    if(event)CloseHandle(event);
    for(unsigned i=0;i<pso_count;i++)if(psos[i])psos[i]->lpVtbl->Release(psos[i]);
#define STENCIL_RELEASE(p) if(p)p->lpVtbl->Release(p)
    STENCIL_RELEASE(depth);STENCIL_RELEASE(dsv_heap);STENCIL_RELEASE(readback);STENCIL_RELEASE(target);STENCIL_RELEASE(heap);
    STENCIL_RELEASE(root);STENCIL_RELEASE(sig);STENCIL_RELEASE(vs);STENCIL_RELEASE(ps);STENCIL_RELEASE(error);
    STENCIL_RELEASE(list);STENCIL_RELEASE(allocator);STENCIL_RELEASE(queue);STENCIL_RELEASE(fence);STENCIL_RELEASE(device);STENCIL_RELEASE(warp);STENCIL_RELEASE(factory);
#undef STENCIL_RELEASE
#undef STENCIL_TRY
    return ok?0:1;
}

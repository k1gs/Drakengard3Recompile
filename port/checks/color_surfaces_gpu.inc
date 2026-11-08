/* Actual FIFO DMA registers -> record snapshots -> RT upload/cache -> texture
 * resolver -> shader sampling. LOCAL and MAIN share raw offsets deliberately. */
int port_check_color_surfaces_gpu(void)
{
    if(s_d3d.device)return 2;
    extern uint8_t* vm_base;
    extern void port_gcm_contract_begin(u32);
    extern int cellGcmSetDisplayBuffer(u8,u32,u32,u32,u32);
    ID3D12Device* device=NULL;IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12DescriptorHeap* rtv_heap=NULL;ID3D12DescriptorHeap* srv_heap=NULL;
    ID3D12DescriptorHeap* output_heap=NULL;ID3D12Resource* output=NULL;ID3D12Resource* readback=NULL;
    ID3D12RootSignature* root=NULL;ID3D12PipelineState* pso=NULL;
    ID3DBlob* vs=NULL;ID3DBlob* ps=NULL;ID3DBlob* blob=NULL;ID3DBlob* error=NULL;
    HANDLE event=NULL;int software=0,ok=0,snapshots=0,routing=0;HRESULT hr=S_OK;
    unsigned pixels=0,samples=0,remapped=0;int slots[4]={-1,-1,-1,-1};
    enum { REMAPS=32, OUTPUTS=4+REMAPS*2 };
    u32 controls[REMAPS];u8 expected_remap[REMAPS][4];unsigned cases=0;
    /* Independent oracle: source lanes A,R,G,B of a GPU-written pixel.
     * Cover every permutation, broadcasts, constants, mixed ops and unset. */
    const u8 argb[4]={255,64,128,191};
    for(unsigned a=0;a<4;a++)for(unsigned r=0;r<4;r++)
    for(unsigned g=0;g<4;g++)for(unsigned b=0;b<4;b++) {
        if(a==r||a==g||a==b||r==g||r==b||g==b)continue;
        controls[cases]=0xAA00u|a|(r<<2)|(g<<4)|(b<<6);
        expected_remap[cases][0]=argb[r];expected_remap[cases][1]=argb[g];
        expected_remap[cases][2]=argb[b];expected_remap[cases][3]=argb[a];cases++;
    }
    for(unsigned lane=0;lane<4;lane++) {
        controls[cases]=0xAA00u|lane*0x55u;
        memset(expected_remap[cases++],argb[lane],4);
    }
    controls[cases]=0x00E4;memset(expected_remap[cases++],0,4);
    controls[cases]=0x55E4;memset(expected_remap[cases++],255,4);
    controls[cases]=0xA1E4;
    expected_remap[cases][0]=0;expected_remap[cases][1]=128;
    expected_remap[cases][2]=191;expected_remap[cases++][3]=255;
    controls[cases]=0;memcpy(expected_remap[cases++],(const u8[]){64,128,191,255},4);
    if(cases!=REMAPS)return 2;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[5];UINT rows=0;UINT64 total=0;
#define COLOR_GPU_TRY(call) do {hr=(call);if(FAILED(hr))goto cleanup;} while(0)
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        software=1;COLOR_GPU_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        COLOR_GPU_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        COLOR_GPU_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    s_d3d.device=device;s_d3d.width=s_d3d.height=16;
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    COLOR_GPU_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    COLOR_GPU_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,&allocator));
    COLOR_GPU_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,&list));
    COLOR_GPU_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event)goto cleanup;
    s_d3d.cmd_list=list;s_d3d.cmd_queue=queue;s_d3d.fence=fence;s_d3d.fence_event=event;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=MAX_OFF_RTS;
    COLOR_GPU_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&rtv_heap));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=DRAW_SRV_BASE+OUTPUTS*PORT_TEXTURE_UNITS;
    hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    COLOR_GPU_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&srv_heap));
    s_d3d.rt_rtv_heap=rtv_heap;s_d3d.srv_heap=srv_heap;
    s_d3d.srv_inc=device->lpVtbl->GetDescriptorHandleIncrementSize(device,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    port_gcm_contract_begin(0x60000000u);
    rsx_state st;rsx_state_init(&st);st.surface_format=0x108;st.surface_clip_w=st.surface_clip_h=16;st.color_target=0x1F;
    const u32 methods[4]={0x194,0x18C,0x1B4,0x1B8};
    for(unsigned i=0;i<4;i++) {
        st.surface_color_offset[i]=i<2?0x10000:0x20000;
        st.surface_color_pitch[i]=80+i*16;
        if(rsx_process_method(&st,methods[i],0xFEED0000u+(i&1)))goto cleanup;
    }
    D3D12DrawRecord recorded={0},selected_b={0};port_snapshot_color(&recorded,&st);
    st.color_target=2;port_snapshot_color(&selected_b,&st);
    snapshots=selected_b.color[0].address==recorded.color[1].address;
    for(unsigned i=0;i<4;i++) {
        snapshots=snapshots && recorded.color[i].pitch==80+i*16;
        for(unsigned j=0;j<i;j++)snapshots=snapshots && recorded.color[i].address!=recorded.color[j].address;
        /* Poison row padding: tight-row initialization would ingest it. */
        memset(vm_base+recorded.color[i].address,0xCD,16*recorded.color[i].pitch);
        for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++) {
            u8* p=vm_base+recorded.color[i].address+y*recorded.color[i].pitch+x*4;
            p[0]=255;p[1]=(u8)(20+i*40+x);p[2]=(u8)(30+y*4);p[3]=(u8)(40+i*20);
        }
    }
    /* A registered LOCAL display offset must not swallow a MAIN target with
     * that same offset. No window or actual flip is involved. */
    if(cellGcmSetDisplayBuffer(0,0x10000,80,16,16))goto cleanup;
    s_d3d.current_rsx_state=&st;u32 w=0,h=0,mrt[3];
    st.color_target=1;u32 local=current_rt_off(&w,&h,mrt);
    rsx_process_method(&st,0x194,0xFEED0001u);u32 main=current_rt_off(&w,&h,mrt);
    routing=local==0 && main==0x10000;
    /* Later DMA/pitch changes cannot change the already recorded sources. */
    for(unsigned i=0;i<4;i++){rsx_process_method(&st,methods[i],0xFEED0001u);st.surface_color_pitch[i]=256;}
    if(!snapshots || !routing)goto cleanup;
    for(unsigned i=0;i<4;i++) {
        slots[i]=off_rt_get(i<2?0x10000:0x20000,recorded.color[i].address,recorded.color[i].pitch,16,16,0x108);
        if(slots[i]<0 || off_rt_find(recorded.color[i].address)!=slots[i])goto cleanup;
    }
    D3D12_RESOURCE_DESC td={0};s_d3d.off_rt[slots[0]].res->lpVtbl->GetDesc(s_d3d.off_rt[slots[0]].res,&td);
    for(unsigned i=0;i<5;i++) {
        D3D12_RESOURCE_DESC desc=td;if(i==4){desc.Width=OUTPUTS;desc.Height=1;}
        total=(total+511)&~511ull;
        device->lpVtbl->GetCopyableFootprints(device,&desc,0,1,total,&fp[i],&rows,NULL,NULL);
        total=fp[i].Offset+(UINT64)fp[i].Footprint.RowPitch*rows;
    }
    D3D12_RESOURCE_DESC rb={0};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=total;
    rb.Height=1;rb.DepthOrArraySize=1;rb.MipLevels=1;rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;
    COLOR_GPU_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&rb,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void**)&readback));
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    for(unsigned i=0;i<4;i++) {
        off_rt_transition(slots[i],D3D12_RESOURCE_STATE_COPY_SOURCE);
        src.pResource=s_d3d.off_rt[slots[i]].res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.PlacedFootprint=fp[i];
        list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
        off_rt_transition(slots[i],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    off_rt_transition(slots[0],D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float magenta[4]={1,0,1,1};
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(slots[0]),magenta,0,NULL);
    port_rt_mark_dirty(slots[0]);
    off_rt_transition(slots[0],D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    memset(vm_base+recorded.color[0].address,0,16*recorded.color[0].pitch);
    if(off_rt_get(0x10000,recorded.color[0].address,recorded.color[0].pitch,16,16,0x108)!=slots[0])goto cleanup;
    /* Actual resolver must use GPU contents, not re-upload these stale zeros. */
    for(unsigned i=0;i<4;i++) {
        D3D12DrawRecord sample={0};sample.tex[0].set=1;sample.tex[0].raw=i<2?0x10000:0x20000;
        sample.tex[0].off=recorded.color[i].address;sample.tex[0].w=sample.tex[0].h=16;sample.tex[0].fmt=0xA5;
        sample.tex[0].source_pitch=recorded.color[i].pitch;
        port_resolve_texture_unit(&sample,i,0);
        if(sample.tex_rt[0]!=slots[i])goto cleanup;
    }
    off_rt_transition(slots[1],D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float asymmetric[4]={64.0f/255,128.0f/255,191.0f/255,1};
    list->lpVtbl->ClearRenderTargetView(list,off_rt_rtv(slots[1]),asymmetric,0,NULL);
    port_rt_mark_dirty(slots[1]);
    memset(vm_base+recorded.color[1].address,0,16*recorded.color[1].pitch);
    port_feedback_begin_batch();
    for(unsigned c=0;c<REMAPS;c++)for(unsigned feedback=0;feedback<2;feedback++) {
        unsigned draw=4+c*2+feedback,unit=c%PORT_TEXTURE_UNITS;
        D3D12DrawRecord sample={0};sample.tex[unit].set=1;sample.tex[unit].raw=0x10000;
        sample.tex[unit].off=recorded.color[1].address;
        sample.tex[unit].w=sample.tex[unit].h=16;sample.tex[unit].fmt=0xA5;
        sample.tex[unit].source_pitch=recorded.color[1].pitch;sample.tex[unit].ctrl1=controls[c];
        port_resolve_texture_unit(&sample,draw,(int)unit);
        if(sample.tex_rt[unit]!=slots[1])goto cleanup;
        if(feedback) {const int mrt[3]={-1,slots[1],-1};port_feedback_bind(&sample,draw,-1,mrt,1u<<unit);}
    }
    td.Width=OUTPUTS;td.Height=1;hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    COLOR_GPU_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&td,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,(void**)&output));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;hd.NumDescriptors=1;
    COLOR_GPU_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&output_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE output_view;output_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(output_heap,&output_view);
    device->lpVtbl->CreateRenderTargetView(device,output,NULL,output_view);
    D3D12_DESCRIPTOR_RANGE range={0};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;
    D3D12_ROOT_PARAMETER param={0};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges=1;param.DescriptorTable.pDescriptorRanges=&range;param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler={0};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd={0};rd.NumParameters=1;rd.pParameters=&param;rd.NumStaticSamplers=1;rd.pStaticSamplers=&sampler;
    COLOR_GPU_TRY(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    COLOR_GPU_TRY(device->lpVtbl->CreateRootSignature(device,0,blob->lpVtbl->GetBufferPointer(blob),blob->lpVtbl->GetBufferSize(blob),&IID_ID3D12RootSignature,(void**)&root));
    blob->lpVtbl->Release(blob);blob=NULL;
    const char* code="Texture2D t:register(t0);SamplerState s:register(s0);"
        "float4 vs(uint id:SV_VertexID):SV_Position{return float4(id==2?3:-1,id==1?3:-1,0,1);}"
        "float4 ps():SV_Target0{return t.SampleLevel(s,float2(0.5,0.5),0);}";
    COLOR_GPU_TRY(D3DCompile(code,strlen(code),"color-address",NULL,NULL,"vs","vs_5_0",0,0,&vs,&error));
    COLOR_GPU_TRY(D3DCompile(code,strlen(code),"color-address",NULL,NULL,"ps","ps_5_0",0,0,&ps,&error));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={0};pd.pRootSignature=root;
    pd.VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs);pd.VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode=ps->lpVtbl->GetBufferPointer(ps);pd.PS.BytecodeLength=ps->lpVtbl->GetBufferSize(ps);
    pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;pd.NumRenderTargets=1;pd.RTVFormats[0]=td.Format;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=15;
    pd.BlendState.RenderTarget[0].SrcBlend=pd.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend=pd.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp=pd.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;
    COLOR_GPU_TRY(device->lpVtbl->CreateGraphicsPipelineState(device,&pd,&IID_ID3D12PipelineState,(void**)&pso));
    list->lpVtbl->SetGraphicsRootSignature(list,root);list->lpVtbl->SetPipelineState(list,pso);
    list->lpVtbl->SetDescriptorHeaps(list,1,&srv_heap);list->lpVtbl->OMSetRenderTargets(list,1,&output_view,FALSE,NULL);
    list->lpVtbl->IASetPrimitiveTopology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(unsigned i=0;i<OUTPUTS;i++) {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap,&gpu);
        gpu.ptr+=(u64)(DRAW_SRV_BASE+i*PORT_TEXTURE_UNITS)*s_d3d.srv_inc;
        if(i>=4)gpu.ptr+=(u64)(((i-4)/2)%PORT_TEXTURE_UNITS)*s_d3d.srv_inc;
        list->lpVtbl->SetGraphicsRootDescriptorTable(list,0,gpu);
        D3D12_VIEWPORT vp={(float)i,0,1,1,0,1};D3D12_RECT sc={(LONG)i,0,(LONG)i+1,1};
        list->lpVtbl->RSSetViewports(list,1,&vp);list->lpVtbl->RSSetScissorRects(list,1,&sc);list->lpVtbl->DrawInstanced(list,3,1,0,0);
    }
    D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=output;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->lpVtbl->ResourceBarrier(list,1,&barrier);src.pResource=output;dst.PlacedFootprint=fp[4];
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    COLOR_GPU_TRY(list->lpVtbl->Close(list));ID3D12CommandList* lists[]={(ID3D12CommandList*)list};
    queue->lpVtbl->ExecuteCommandLists(queue,1,lists);COLOR_GPU_TRY(queue->lpVtbl->Signal(queue,fence,1));
    COLOR_GPU_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0 || fence->lpVtbl->GetCompletedValue(fence)<1)abort();
    u8* bytes=NULL;D3D12_RANGE read_range={0,(SIZE_T)total};COLOR_GPU_TRY(readback->lpVtbl->Map(readback,0,&read_range,(void**)&bytes));
    for(unsigned i=0;i<4;i++)for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++) {
        const u8* p=bytes+fp[i].Offset+(size_t)y*fp[i].Footprint.RowPitch+x*4;
        if(p[0]==20+i*40+x && p[1]==30+y*4 && p[2]==40+i*20 && p[3]==255)pixels++;
    }
    for(unsigned i=0;i<4;i++) {
        const u8* p=bytes+fp[4].Offset+i*4;
        u8 expected[4]={(u8)(20+i*40+8),62,(u8)(40+i*20),255};
        if(!i){expected[0]=255;expected[1]=0;expected[2]=255;}
        /* Source 1 was subsequently cleared by the GPU, before these samples. */
        if(i==1)memcpy(expected,(const u8[]){64,128,191,255},4);
        if(!memcmp(p,expected,4))samples++;
    }
    for(unsigned c=0;c<REMAPS;c++)for(unsigned feedback=0;feedback<2;feedback++) {
        const u8* p=bytes+fp[4].Offset+(4+c*2+feedback)*4;
        if(!memcmp(p,expected_remap[c],4))remapped++;
        else fprintf(stderr,"[compat-check] RT remap c1=%04X feedback=%u got=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
            controls[c],feedback,p[0],p[1],p[2],p[3],expected_remap[c][0],expected_remap[c][1],expected_remap[c][2],expected_remap[c][3]);
    }
    readback->lpVtbl->Unmap(readback,0,NULL);ok=pixels==1024 && samples==4 && remapped==REMAPS*2 && snapshots && routing;
cleanup:
    fprintf(stderr,"[compat-check] color surfaces GPU device=%s source-pixels=%u/1024 sampled=%u/4 remapped=%u/64 snapshots=%d local-display-routing=%d %s hr=%08lX\n",
        software?"WARP":"hardware",pixels,samples,remapped,snapshots,routing,ok?"PASS":"FAIL",(unsigned long)hr);
    port_feedback_release();
    port_destroy_retired_rts();
    if(s_d3d.rt_rtv_heap==rtv_heap)rtv_heap=NULL;
    port_rt_storage_release();
    memset(&s_d3d,0,sizeof(s_d3d));port_renderer_color_reset();if(event)CloseHandle(event);
#define COLOR_GPU_RELEASE(p) if(p)p->lpVtbl->Release(p)
    COLOR_GPU_RELEASE(output);COLOR_GPU_RELEASE(readback);COLOR_GPU_RELEASE(rtv_heap);COLOR_GPU_RELEASE(srv_heap);COLOR_GPU_RELEASE(output_heap);
    COLOR_GPU_RELEASE(root);COLOR_GPU_RELEASE(pso);COLOR_GPU_RELEASE(vs);COLOR_GPU_RELEASE(ps);COLOR_GPU_RELEASE(blob);COLOR_GPU_RELEASE(error);
    COLOR_GPU_RELEASE(list);COLOR_GPU_RELEASE(allocator);COLOR_GPU_RELEASE(queue);COLOR_GPU_RELEASE(fence);COLOR_GPU_RELEASE(device);COLOR_GPU_RELEASE(warp);COLOR_GPU_RELEASE(factory);
#undef COLOR_GPU_RELEASE
#undef COLOR_GPU_TRY
    return ok?0:1;
}

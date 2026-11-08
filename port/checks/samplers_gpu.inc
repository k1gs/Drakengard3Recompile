/* Real texture sampling/readback, including independent s0/s15 descriptors,
 * mip selection, filter/address changes, cache reuse and heap rollover. */
int port_check_samplers_gpu(void)
{
    if(s_d3d.device) return 2;
    D3D12DrawRecord dr={0};dr.tex[0].set=dr.tex[15].set=1;
    dr.tex[0].sampler_address=dr.tex[15].sampler_address=0x00030303;
    dr.tex[0].sampler_filter=dr.tex[15].sampler_filter=0x01010000;
    dr.tex[0].sampler_control=dr.tex[15].sampler_control=0xFFFu<<7;
    D3D12_SAMPLER_DESC decoded;
    int decode=port_decode_sampler(&dr,0,&decoded) && decoded.Filter==D3D12_FILTER_MIN_MAG_MIP_POINT;
    dr.tex[0].sampler_filter=0x02060000|0x1F00;
    dr.tex[0].sampler_control=(3u<<19)|(12u<<7)|(7u<<4);
    decode=decode && port_decode_sampler(&dr,0,&decoded) && decoded.MipLODBias==-1.0f &&
        decoded.MaxAnisotropy==16 && decoded.Filter==D3D12_FILTER_ANISOTROPIC && decoded.MinLOD==3.0f/256;
    dr.tex[0].sampler_filter=0x01070000;
    int unsupported=!port_decode_sampler(&dr,0,&decoded);
    /* Exact register tuple first observed in the ordinary renderer. */
    dr.tex[0].sampler_address=0x60710101;dr.tex[0].sampler_filter=0x02042000;
    dr.tex[0].sampler_control=0x80060020;
    decode=decode && port_decode_sampler(&dr,0,&decoded) &&
        decoded.Filter==(D3D12_FILTER)0x54 && decoded.MaxAnisotropy==4;
    if(!decode || !unsupported) return 1;
    ID3D12Device* device=NULL;IDXGIFactory4* factory=NULL;IDXGIAdapter* warp=NULL;
    ID3D12CommandQueue* queue=NULL;ID3D12CommandAllocator* allocator=NULL;
    ID3D12GraphicsCommandList* list=NULL;ID3D12Fence* fence=NULL;
    ID3D12DescriptorHeap* heap=NULL;ID3D12Resource* target=NULL;ID3D12Resource* readback=NULL;
    ID3D12RootSignature* root=NULL;ID3D12PipelineState* pso=NULL;
    ID3DBlob *sig=NULL,*vs=NULL,*ps=NULL,*error=NULL;
    HANDLE event=NULL;HRESULT hr=S_OK;int ok=0,software=0;unsigned matched=0;
    ID3D12Resource *source_texture=NULL,*upload=NULL;ID3D12DescriptorHeap* srv_heap=NULL;
#define SAMPLER_TRY(call) do { hr=(call);if(FAILED(hr)) goto cleanup; } while(0)
    hr=D3D12CreateDevice(NULL,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device);
    if(FAILED(hr)) {
        software=1;
        SAMPLER_TRY(CreateDXGIFactory1(&IID_IDXGIFactory4,(void**)&factory));
        SAMPLER_TRY(factory->lpVtbl->EnumWarpAdapter(factory,&IID_IDXGIAdapter,(void**)&warp));
        SAMPLER_TRY(D3D12CreateDevice((IUnknown*)warp,D3D_FEATURE_LEVEL_11_0,&IID_ID3D12Device,(void**)&device));
    }
    D3D12_COMMAND_QUEUE_DESC qd={0};qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    SAMPLER_TRY(device->lpVtbl->CreateCommandQueue(device,&qd,&IID_ID3D12CommandQueue,&queue));
    SAMPLER_TRY(device->lpVtbl->CreateCommandAllocator(device,qd.Type,&IID_ID3D12CommandAllocator,&allocator));
    SAMPLER_TRY(device->lpVtbl->CreateCommandList(device,0,qd.Type,allocator,NULL,&IID_ID3D12GraphicsCommandList,&list));
    SAMPLER_TRY(device->lpVtbl->CreateFence(device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,&fence));
    event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!event) goto cleanup;
    s_d3d.device=device;s_d3d.cmd_list=list;
    D3D12_DESCRIPTOR_RANGE ranges[2]={{0},{0}};
    ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=1;
    ranges[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;ranges[1].NumDescriptors=PORT_TEXTURE_UNITS;
    D3D12_ROOT_PARAMETER rp[4]={{0},{0},{0},{0}};
    rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[0].Constants.Num32BitValues=4;
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[1].DescriptorTable.NumDescriptorRanges=1;rp[1].DescriptorTable.pDescriptorRanges=&ranges[0];
    rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;rp[2].Descriptor.ShaderRegister=1;
    rp[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[3].DescriptorTable.NumDescriptorRanges=1;rp[3].DescriptorTable.pDescriptorRanges=&ranges[1];
    D3D12_ROOT_SIGNATURE_DESC rs={0};rs.NumParameters=4;rs.pParameters=rp;
    SAMPLER_TRY(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&error));
    SAMPLER_TRY(device->lpVtbl->CreateRootSignature(device,0,sig->lpVtbl->GetBufferPointer(sig),
        sig->lpVtbl->GetBufferSize(sig),&IID_ID3D12RootSignature,(void**)&root));
    const char* shader="cbuffer C:register(b0){float4 coords;} Texture2D tex:register(t0); SamplerState ss[16]:register(s0);"
        "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}"
        "float4 ps():SV_Target{return coords.w>0?tex.SampleLevel(ss[15],coords.xy,coords.z):tex.SampleLevel(ss[0],coords.xy,coords.z);}";
    SAMPLER_TRY(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"vs","vs_5_0",0,0,&vs,NULL));
    SAMPLER_TRY(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"ps","ps_5_0",0,0,&ps,NULL));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={0};pd.pRootSignature=root;
    pd.VS.pShaderBytecode=vs->lpVtbl->GetBufferPointer(vs);pd.VS.BytecodeLength=vs->lpVtbl->GetBufferSize(vs);
    pd.PS.pShaderBytecode=ps->lpVtbl->GetBufferPointer(ps);pd.PS.BytecodeLength=ps->lpVtbl->GetBufferSize(ps);
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable=TRUE;pd.SampleMask=UINT_MAX;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets=1;pd.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pd.SampleDesc.Count=1;
    SAMPLER_TRY(device->lpVtbl->CreateGraphicsPipelineState(device,&pd,&IID_ID3D12PipelineState,(void**)&pso));
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    SAMPLER_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap,&rtv);
    D3D12_RESOURCE_DESC td={0};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=13;td.Height=1;td.DepthOrArraySize=1;td.MipLevels=1;td.Format=pd.RTVFormats[0];td.SampleDesc.Count=1;
    td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    SAMPLER_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&td,
        D3D12_RESOURCE_STATE_RENDER_TARGET,NULL,&IID_ID3D12Resource,&target));
    device->lpVtbl->CreateRenderTargetView(device,target,NULL,rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;UINT64 total=0;
    device->lpVtbl->GetCopyableFootprints(device,&td,0,1,0,&fp,NULL,NULL,&total);
    D3D12_RESOURCE_DESC rb={0};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=total;
    rb.Height=1;rb.DepthOrArraySize=1;rb.MipLevels=1;rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hp.Type=D3D12_HEAP_TYPE_READBACK;
    SAMPLER_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&rb,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,&readback));
    /* Two-level real texture: base has red/blue columns, mip1 is green. */
    D3D12_RESOURCE_DESC image=td;image.Width=2;image.Height=2;image.MipLevels=2;image.Flags=0;
    hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    SAMPLER_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&image,
        D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,&source_texture));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT image_fp[2];UINT64 image_bytes=0;
    device->lpVtbl->GetCopyableFootprints(device,&image,0,2,0,image_fp,NULL,NULL,&image_bytes);
    rb.Width=image_bytes;hp.Type=D3D12_HEAP_TYPE_UPLOAD;
    SAMPLER_TRY(device->lpVtbl->CreateCommittedResource(device,&hp,D3D12_HEAP_FLAG_NONE,&rb,
        D3D12_RESOURCE_STATE_GENERIC_READ,NULL,&IID_ID3D12Resource,&upload));
    unsigned char* pixels=NULL;D3D12_RANGE no_read={0,0};
    SAMPLER_TRY(upload->lpVtbl->Map(upload,0,&no_read,(void**)&pixels));
    const unsigned char row[8]={255,0,0,255,0,0,255,255},green[4]={0,255,0,255};
    for(unsigned y=0;y<2;y++) memcpy(pixels+image_fp[0].Offset+y*image_fp[0].Footprint.RowPitch,row,8);
    memcpy(pixels+image_fp[1].Offset,green,4);upload->lpVtbl->Unmap(upload,0,NULL);
    for(unsigned mip=0;mip<2;mip++) {
        D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};dst.pResource=source_texture;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=mip;
        src.pResource=upload;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=image_fp[mip];
        list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    }
    D3D12_RESOURCE_BARRIER texture_barrier={0};texture_barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    texture_barrier.Transition.pResource=source_texture;texture_barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    texture_barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;texture_barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&texture_barrier);
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    SAMPLER_TRY(device->lpVtbl->CreateDescriptorHeap(device,&hd,&IID_ID3D12DescriptorHeap,&srv_heap));
    s_d3d.srv_heap=srv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE srv;srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(srv_heap,&srv);
    D3D12_SHADER_RESOURCE_VIEW_DESC view={0};view.Format=image.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=2;
    device->lpVtbl->CreateShaderResourceView(device,source_texture,&view,srv);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap,&srv_gpu);
    const float black[4]={0,0,0,1};
    list->lpVtbl->ClearRenderTargetView(list,rtv,black,0,NULL);
    list->lpVtbl->SetGraphicsRootSignature(list,root);list->lpVtbl->SetPipelineState(list,pso);
    list->lpVtbl->OMSetRenderTargets(list,1,&rtv,FALSE,NULL);
    D3D12_VIEWPORT vp={0,0,13,1,0,1};list->lpVtbl->RSSetViewports(list,1,&vp);
    list->lpVtbl->IASetPrimitiveTopology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    port_samplers_begin_batch();
    D3D12_GPU_DESCRIPTOR_HANDLE first={0};ID3D12DescriptorHeap* first_heap=NULL;int reuse=0,pages=0;
    for(unsigned i=0;i<13;i++) {
        dr.tex[0].sampler_address=0x00030301;dr.tex[0].sampler_filter=0x01010000;
        dr.tex[0].sampler_control=0xFFFu<<7;dr.tex[0].sampler_border=0xFFFFFF00;
        float coords[4]={1.25f,0.5f,0,0};
        if(i==1 || i==4 || i==5 || i==11) dr.tex[0].sampler_address=0x00030303;
        if(i==2) dr.tex[0].sampler_address=0x00030302;
        if(i==3) dr.tex[0].sampler_address=0x00030304;
        if(i==4 || i==5) {dr.tex[0].sampler_filter=0x02020000;coords[0]=i==4?0.5f:0.25f;}
        if(i>=6 && i<=10) {dr.tex[0].sampler_filter=0x01030000;coords[0]=0.25f;coords[2]=1;}
        if(i==8) dr.tex[0].sampler_filter=0x01010000;
        if(i==7) {dr.tex[0].sampler_filter=0x02042000;dr.tex[0].sampler_control=0x80060020;}
        if(i==9) {dr.tex[0].sampler_filter=0x01050000;coords[2]=0.5f;}
        if(i==10) {dr.tex[0].sampler_control=(256u<<19)|(0xFFFu<<7);coords[2]=0;}
        if(i==11) coords[3]=1;
        if(i==6) {
            /* Force more than 2048 descriptors before any fence. */
            D3D12DrawRecord varied=dr;
            for(unsigned n=0;n<130;n++) {varied.tex[0].sampler_border=n;ID3D12DescriptorHeap* heap2=NULL;port_sampler_table(&varied,1,&heap2);}
        }
        if(i==0) first=port_sampler_table(&dr,1,&first_heap);
        if(i==12) {ID3D12DescriptorHeap* again_heap=NULL;D3D12_GPU_DESCRIPTOR_HANDLE again=port_sampler_table(&dr,1,&again_heap);reuse=again.ptr==first.ptr && again_heap==first_heap;}
        port_bind_samplers(&dr,1u|0x8000u);
        list->lpVtbl->SetGraphicsRootDescriptorTable(list,1,srv_gpu);
        list->lpVtbl->SetGraphicsRoot32BitConstants(list,0,4,coords,0);
        D3D12_RECT rect={(LONG)i,0,(LONG)i+1,1};list->lpVtbl->RSSetScissorRects(list,1,&rect);
        list->lpVtbl->DrawInstanced(list,3,1,0,0);
    }
    for(PortSamplerPage* page=port_sampler_pages;page;page=page->next) pages++;
    D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=target;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->lpVtbl->ResourceBarrier(list,1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION dst={0},src={0};dst.pResource=readback;
    dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
    src.pResource=target;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->lpVtbl->CopyTextureRegion(list,&dst,0,0,0,&src,NULL);
    SAMPLER_TRY(list->lpVtbl->Close(list));
    ID3D12CommandList* lists[]={(ID3D12CommandList*)list};queue->lpVtbl->ExecuteCommandLists(queue,1,lists);
    SAMPLER_TRY(queue->lpVtbl->Signal(queue,fence,1));
    SAMPLER_TRY(fence->lpVtbl->SetEventOnCompletion(fence,1,event));
    if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0 || fence->lpVtbl->GetCompletedValue(fence)<1) abort();
    unsigned char* data=NULL;D3D12_RANGE range={0,(SIZE_T)total};
    SAMPLER_TRY(readback->lpVtbl->Map(readback,0,&range,(void**)&data));
    static const unsigned char expected[13][3]={{255,0,0},{0,0,255},{0,0,255},{255,255,0},
        {128,0,128},{255,0,0},{0,255,0},{0,255,0},{255,0,0},{128,128,0},{0,255,0},{0,0,255},{255,0,0}};
    ok=reuse && pages>=2;
    for(unsigned x=0;x<13;x++) {
        unsigned char* px=data+fp.Offset+x*4;int pixel_ok=px[3]==255;
        for(unsigned c=0;c<3;c++) if(abs((int)px[c]-(int)expected[x][c])>1) pixel_ok=0;
        if(pixel_ok) matched++;else {ok=0;fprintf(stderr,"[sampler-test] case=%u rgba=%u,%u,%u,%u\n",x,px[0],px[1],px[2],px[3]);}
    }
    D3D12_RANGE written={0,0};readback->lpVtbl->Unmap(readback,0,&written);
cleanup:
    fprintf(stderr,"[compat-check] sampler GPU device=%s decode=%d unsupported=%d pixels=%u/13 %s hr=%08lX\n",
        software?"WARP":"hardware",decode,unsupported,matched,ok?"PASS":"FAIL",(unsigned long)hr);
    port_samplers_release();s_d3d.device=NULL;s_d3d.cmd_list=NULL;s_d3d.srv_heap=NULL;
    if(event) CloseHandle(event);
#define SAMPLER_RELEASE(p) if(p) p->lpVtbl->Release(p)
    SAMPLER_RELEASE(source_texture);SAMPLER_RELEASE(upload);SAMPLER_RELEASE(srv_heap);
    SAMPLER_RELEASE(readback);SAMPLER_RELEASE(target);SAMPLER_RELEASE(heap);
    SAMPLER_RELEASE(pso);SAMPLER_RELEASE(root);SAMPLER_RELEASE(sig);SAMPLER_RELEASE(vs);SAMPLER_RELEASE(ps);SAMPLER_RELEASE(error);
    SAMPLER_RELEASE(list);SAMPLER_RELEASE(allocator);SAMPLER_RELEASE(queue);SAMPLER_RELEASE(fence);
    SAMPLER_RELEASE(device);SAMPLER_RELEASE(warp);SAMPLER_RELEASE(factory);
#undef SAMPLER_RELEASE
#undef SAMPLER_TRY
    return ok?0:1;
}

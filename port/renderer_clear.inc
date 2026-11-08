/* Clear state is independent of draw depth/stencil-test enables. Fast clears
 * handle full write masks; masked clears rasterize a scissored triangle. */
typedef struct {
    DXGI_FORMAT color_format,depth_format;
    unsigned color_mask,depth,stencil_mask;
    ID3D12PipelineState* pso;
} PortClearPipeline;
static ID3D12RootSignature* port_clear_root;
static ID3DBlob* port_clear_vs;
static ID3DBlob* port_clear_color_ps;
static ID3DBlob* port_clear_depth_ps;
static PortClearPipeline* port_clear_pipelines;
static unsigned port_clear_pipeline_count,port_clear_pipeline_capacity;

static void port_clear_require(HRESULT hr)
{
    if(FAILED(hr)) {fprintf(stderr,"[clear] GPU operation failed %08lX\n",(unsigned long)hr);abort();}
}

static ID3D12PipelineState* port_clear_pipeline(DXGI_FORMAT color,DXGI_FORMAT depth,
                                               unsigned mask,unsigned write_depth,unsigned stencil)
{
    for(unsigned i=0;i<port_clear_pipeline_count;i++) {
        PortClearPipeline* p=&port_clear_pipelines[i];
        if(p->color_format==color && p->depth_format==depth && p->color_mask==mask &&
           p->depth==write_depth && p->stencil_mask==stencil) return p->pso;
    }
    if(!port_clear_root) {
        D3D12_ROOT_PARAMETER param={0};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.ShaderRegister=0;param.Constants.Num32BitValues=5;param.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rd={0};rd.NumParameters=1;rd.pParameters=&param;
        ID3DBlob* blob=NULL;ID3DBlob* error=NULL;
        HRESULT hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
        if(error)error->lpVtbl->Release(error);port_clear_require(hr);
        hr=s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device,0,blob->lpVtbl->GetBufferPointer(blob),
            blob->lpVtbl->GetBufferSize(blob),&IID_ID3D12RootSignature,(void**)&port_clear_root);
        blob->lpVtbl->Release(blob);port_clear_require(hr);
        const char* code="cbuffer C:register(b0){float4 color;float depth;}"
            "float4 vs(uint id:SV_VertexID):SV_Position{"
            "return float4(id==2?3:-1,id==1?3:-1,0,1);}"
            "float4 color_ps():SV_Target0{return color;}"
            "float depth_ps():SV_Depth{return depth;}";
        hr=D3DCompile(code,strlen(code),"ordered-clear",NULL,NULL,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&port_clear_vs,&error);
        if(error){error->lpVtbl->Release(error);error=NULL;}port_clear_require(hr);
        hr=D3DCompile(code,strlen(code),"ordered-clear",NULL,NULL,"color_ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&port_clear_color_ps,&error);
        if(error){error->lpVtbl->Release(error);error=NULL;}port_clear_require(hr);
        hr=D3DCompile(code,strlen(code),"ordered-clear",NULL,NULL,"depth_ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&port_clear_depth_ps,&error);
        if(error)error->lpVtbl->Release(error);port_clear_require(hr);
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={0};pd.pRootSignature=port_clear_root;
    pd.VS.pShaderBytecode=port_clear_vs->lpVtbl->GetBufferPointer(port_clear_vs);pd.VS.BytecodeLength=port_clear_vs->lpVtbl->GetBufferSize(port_clear_vs);
    ID3DBlob* ps=color==DXGI_FORMAT_UNKNOWN?port_clear_depth_ps:port_clear_color_ps;
    pd.PS.pShaderBytecode=ps->lpVtbl->GetBufferPointer(ps);pd.PS.BytecodeLength=ps->lpVtbl->GetBufferSize(ps);
    pd.SampleMask=UINT_MAX;pd.SampleDesc.Count=1;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    for(unsigned i=0;i<8;i++) {
        D3D12_RENDER_TARGET_BLEND_DESC* b=&pd.BlendState.RenderTarget[i];
        b->SrcBlend=b->SrcBlendAlpha=D3D12_BLEND_ONE;b->DestBlend=b->DestBlendAlpha=D3D12_BLEND_ZERO;
        b->BlendOp=b->BlendOpAlpha=D3D12_BLEND_OP_ADD;b->LogicOp=D3D12_LOGIC_OP_NOOP;
        b->RenderTargetWriteMask=(UINT8)mask;
    }
    pd.NumRenderTargets=color!=DXGI_FORMAT_UNKNOWN?1:0;pd.RTVFormats[0]=color;pd.DSVFormat=depth;
    pd.DepthStencilState.DepthEnable=write_depth?TRUE:FALSE;
    pd.DepthStencilState.DepthWriteMask=write_depth?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DepthStencilState.StencilEnable=stencil?TRUE:FALSE;
    pd.DepthStencilState.StencilReadMask=255;pd.DepthStencilState.StencilWriteMask=(UINT8)stencil;
    pd.DepthStencilState.FrontFace.StencilFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DepthStencilState.FrontFace.StencilFailOp=pd.DepthStencilState.FrontFace.StencilDepthFailOp=D3D12_STENCIL_OP_KEEP;
    pd.DepthStencilState.FrontFace.StencilPassOp=D3D12_STENCIL_OP_REPLACE;
    pd.DepthStencilState.BackFace=pd.DepthStencilState.FrontFace;
    ID3D12PipelineState* pso=NULL;
    port_clear_require(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device,&pd,&IID_ID3D12PipelineState,(void**)&pso));
    if(port_clear_pipeline_count==port_clear_pipeline_capacity) {
        unsigned capacity=port_clear_pipeline_capacity?port_clear_pipeline_capacity*2:8;
        if(capacity<=port_clear_pipeline_capacity || (size_t)capacity>SIZE_MAX/sizeof(*port_clear_pipelines))abort();
        PortClearPipeline* grown=(PortClearPipeline*)realloc(port_clear_pipelines,(size_t)capacity*sizeof(*grown));
        if(!grown)abort();port_clear_pipelines=grown;port_clear_pipeline_capacity=capacity;
    }
    port_clear_pipelines[port_clear_pipeline_count++]=(PortClearPipeline){color,depth,mask,write_depth,stencil,pso};
    return pso;
}

static void port_masked_clear(const D3D12DrawRecord* dr,D3D12_CPU_DESCRIPTOR_HANDLE view,
                              u32 width,u32 height,DXGI_FORMAT color,DXGI_FORMAT depth,
                              unsigned mask,unsigned write_depth,unsigned stencil)
{
    D3D12_RECT rect=port_draw_scissor(dr,width,height);
    if(rect.left>=rect.right || rect.top>=rect.bottom)return;
    ID3D12PipelineState* pso=port_clear_pipeline(color,depth,mask,write_depth,stencil);
    ID3D12GraphicsCommandList* list=s_d3d.cmd_list;
    list->lpVtbl->SetGraphicsRootSignature(list,port_clear_root);
    list->lpVtbl->SetPipelineState(list,pso);
    float constants[5]={dr->cc[0],dr->cc[1],dr->cc[2],dr->cc[3],dr->clear_depth};
    list->lpVtbl->SetGraphicsRoot32BitConstants(list,0,5,constants,0);
    list->lpVtbl->OMSetStencilRef(list,dr->clear_stencil);
    if(color!=DXGI_FORMAT_UNKNOWN)list->lpVtbl->OMSetRenderTargets(list,1,&view,FALSE,NULL);
    else list->lpVtbl->OMSetRenderTargets(list,0,NULL,FALSE,&view);
    D3D12_VIEWPORT vp={0,0,(float)width,(float)height,0,1};
    list->lpVtbl->RSSetViewports(list,1,&vp);list->lpVtbl->RSSetScissorRects(list,1,&rect);
    list->lpVtbl->IASetPrimitiveTopology(list,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->lpVtbl->DrawInstanced(list,3,1,0,0);
}

static void port_clear_color_view(const D3D12DrawRecord* dr,D3D12_CPU_DESCRIPTOR_HANDLE view,
                                  u32 width,u32 height,DXGI_FORMAT format)
{
    unsigned mask=(dr->clear_flags>>4)&15;
    D3D12_RECT rect=port_draw_scissor(dr,width,height);
    if(!mask || rect.left>=rect.right || rect.top>=rect.bottom)return;
    if(mask==15)s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list,view,dr->cc,1,&rect);
    else port_masked_clear(dr,view,width,height,format,DXGI_FORMAT_UNKNOWN,mask,0,0);
}

static void port_clear_depth_view(const D3D12DrawRecord* dr,D3D12_CPU_DESCRIPTOR_HANDLE view)
{
    UINT flags=port_depth_clear_flags(dr);
    unsigned stencil=(flags&D3D12_CLEAR_FLAG_STENCIL)?dr->clear_stencil_mask:0;
    if(!stencil)flags&=~D3D12_CLEAR_FLAG_STENCIL;
    D3D12_RECT rect=port_draw_scissor(dr,dr->depth_surface.width,dr->depth_surface.height);
    if(!flags || rect.left>=rect.right || rect.top>=rect.bottom)return;
    if(stencil && stencil!=255) {
        port_masked_clear(dr,view,dr->depth_surface.width,dr->depth_surface.height,
            DXGI_FORMAT_UNKNOWN,port_depth_format(dr->depth_surface.format),0,(flags&D3D12_CLEAR_FLAG_DEPTH)!=0,stencil);
    } else s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list,view,flags,dr->clear_depth,dr->clear_stencil,1,&rect);
}

static void port_release_clear_pipelines(void)
{
    for(unsigned i=0;i<port_clear_pipeline_count;i++)port_clear_pipelines[i].pso->lpVtbl->Release(port_clear_pipelines[i].pso);
    free(port_clear_pipelines);port_clear_pipelines=NULL;port_clear_pipeline_count=port_clear_pipeline_capacity=0;
    if(port_clear_root)port_clear_root->lpVtbl->Release(port_clear_root);port_clear_root=NULL;
    if(port_clear_vs)port_clear_vs->lpVtbl->Release(port_clear_vs);port_clear_vs=NULL;
    if(port_clear_color_ps)port_clear_color_ps->lpVtbl->Release(port_clear_color_ps);port_clear_color_ps=NULL;
    if(port_clear_depth_ps)port_clear_depth_ps->lpVtbl->Release(port_clear_depth_ps);port_clear_depth_ps=NULL;
}

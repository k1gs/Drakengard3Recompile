/* A report counts samples from guest draws since CLEAR_REPORT_VALUE.
 * Keep the generation on each record so a clear never splits a render batch. */
static u64 port_zpass_epoch=1,port_zpass_resolved_epoch;
static int port_zpass_enabled,port_zcull_enabled=1;
static u32 port_zpass_total,port_zpass_queries;
static ID3D12QueryHeap* port_zpass_heap;
static ID3D12Resource* port_zpass_readback;

void port_renderer_zpass_clear(void)
{
    port_zpass_epoch++;
    port_zpass_total=0;
}

void port_renderer_zpass_reset(void)
{
    port_renderer_zpass_clear();
    port_zpass_enabled=0;
    port_zcull_enabled=1;
}

void port_renderer_zpass_enable(u32 method,u32 value)
{
    if(method==0x17CCu)port_zpass_enabled=value!=0;
    else port_zcull_enabled=value!=0;
}

static u64 port_zpass_draw_epoch(void)
{
    return port_zpass_enabled && port_zcull_enabled ? port_zpass_epoch : 0;
}

static void port_zpass_draw(const D3D12DrawRecord* draw,u32 start_vertex)
{
    ID3D12GraphicsCommandList* list=s_d3d.cmd_list;
    const int counted=draw->zpass_epoch==port_zpass_epoch;
    if(counted) {
        if(!port_zpass_heap) {
            D3D12_QUERY_HEAP_DESC desc={0};
            desc.Type=D3D12_QUERY_HEAP_TYPE_OCCLUSION;
            desc.Count=MAX_DRAWS;
            port_rt_require(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device,&desc,
                &IID_ID3D12QueryHeap,(void**)&port_zpass_heap));
            port_zpass_readback=port_rt_buffer(MAX_DRAWS*sizeof(u64),
                D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,0);
        }
        if(port_zpass_queries>=MAX_DRAWS)abort();
        list->lpVtbl->BeginQuery(list,port_zpass_heap,D3D12_QUERY_TYPE_OCCLUSION,port_zpass_queries);
    }
    list->lpVtbl->DrawInstanced(list,draw->vertex_count,1,start_vertex,0);
    if(counted)
        list->lpVtbl->EndQuery(list,port_zpass_heap,D3D12_QUERY_TYPE_OCCLUSION,port_zpass_queries++);
}

static void port_zpass_resolve(void)
{
    if(!port_zpass_queries)return;
    ID3D12GraphicsCommandList* list=s_d3d.cmd_list;
    list->lpVtbl->ResolveQueryData(list,port_zpass_heap,D3D12_QUERY_TYPE_OCCLUSION,
        0,port_zpass_queries,port_zpass_readback,0);
    port_zpass_resolved_epoch=port_zpass_epoch;
}

/* The renderer already fences at the next batch boundary. GET_REPORT can
 * collect early, after submitting and waiting for any relevant pending draws. */
static void port_zpass_collect(void)
{
    if(!port_zpass_queries)return;
    u64 completed=s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence);
    if(completed==UINT64_MAX || completed<s_d3d.fence_values[s_d3d.frame_index]) {
        fprintf(stderr,"[zpass] query fence did not complete\n");
        abort();
    }
    if(port_zpass_resolved_epoch==port_zpass_epoch) {
        u64* samples=NULL;
        D3D12_RANGE read={0,port_zpass_queries*sizeof(u64)},written={0,0};
        port_rt_require(port_zpass_readback->lpVtbl->Map(port_zpass_readback,0,&read,(void**)&samples));
        for(u32 i=0;i<port_zpass_queries;i++)port_zpass_total+=(u32)samples[i];
        port_zpass_readback->lpVtbl->Unmap(port_zpass_readback,0,&written);
    }
    port_zpass_queries=0;
}

int port_renderer_zpass_read(u32* samples)
{
    if(!s_d3d.initialized)return 0;
    for(u32 i=0;i<s_d3d.draw_count;i++) {
        if(s_d3d.draws[i].zpass_epoch!=port_zpass_epoch)continue;
        extern void port_renderer_submit_pending(void);
        port_renderer_submit_pending();
        port_zpass_collect();
        *samples=port_zpass_total;
        return 1;
    }
    if(port_zpass_queries) {
        wait_for_gpu();
        port_zpass_collect();
    }
    *samples=port_zpass_total;
    return 1;
}

static void port_zpass_release(void)
{
    if(port_zpass_heap)port_zpass_heap->lpVtbl->Release(port_zpass_heap);
    if(port_zpass_readback)port_zpass_readback->lpVtbl->Release(port_zpass_readback);
    port_zpass_heap=NULL;
    port_zpass_readback=NULL;
    port_zpass_queries=0;
    port_renderer_zpass_reset();
}

/* Native read views for colour feedback. GPU commands preserve draw order;
 * a PSR -> COPY_DEST transition waits for earlier readers before reuse. */
typedef struct {
    ID3D12Resource* resource;
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_STATES state;
    int source;
    u64 batch;
} PortFeedbackSurface;
static PortFeedbackSurface* port_feedback_surfaces;
static unsigned port_feedback_count,port_feedback_capacity;
static u64 port_feedback_batch;

static int port_feedback_same_layout(const D3D12_RESOURCE_DESC* a,const D3D12_RESOURCE_DESC* b)
{
    return a->Dimension==b->Dimension && a->Width==b->Width && a->Height==b->Height &&
        a->DepthOrArraySize==b->DepthOrArraySize && a->MipLevels==b->MipLevels &&
        a->Format==b->Format && a->SampleDesc.Count==b->SampleDesc.Count &&
        a->SampleDesc.Quality==b->SampleDesc.Quality;
}
static void port_feedback_release(void)
{
    for(unsigned i=0;i<port_feedback_count;i++)
        if(port_feedback_surfaces[i].resource)
            port_feedback_surfaces[i].resource->lpVtbl->Release(port_feedback_surfaces[i].resource);
    free(port_feedback_surfaces);port_feedback_surfaces=NULL;
    port_feedback_count=port_feedback_capacity=0;port_feedback_batch=0;
}
static void port_feedback_begin_batch(void)
{
    /* The caller has already waited for the submission fence. A timed-out
     * wait must not allow replacing resources that commands still reference. */
    if(port_feedback_count && s_d3d.fence &&
       (FAILED(s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device)) ||
        s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence)<s_d3d.fence_values[s_d3d.frame_index])) {
        fprintf(stderr,"[feedback] submission fence incomplete\n");abort();
    }
    port_feedback_batch++;
}
static ID3D12Resource* port_feedback_copy(int source)
{
    OffRT* rt=&s_d3d.off_rt[source];
    D3D12_RESOURCE_DESC desc={0};rt->res->lpVtbl->GetDesc(rt->res,&desc);
    PortFeedbackSurface* slot=NULL;
    for(unsigned i=0;i<port_feedback_count;i++)
        if(port_feedback_surfaces[i].source==source &&
           port_feedback_same_layout(&port_feedback_surfaces[i].desc,&desc)) {
            slot=&port_feedback_surfaces[i];break;
        }
    if(!slot) {
        for(unsigned i=0;i<port_feedback_count;i++)
            if(port_feedback_surfaces[i].batch!=port_feedback_batch) {
                slot=&port_feedback_surfaces[i];break;
            }
        if(!slot) {
            if(port_feedback_count==port_feedback_capacity) {
                unsigned capacity=port_feedback_capacity?port_feedback_capacity*2:8;
                if(capacity<port_feedback_capacity || capacity>MAX_DRAWS*4u) {
                    fprintf(stderr,"[feedback] surface capacity exhausted\n");abort();
                }
                PortFeedbackSurface* grown=realloc(port_feedback_surfaces,(size_t)capacity*sizeof(*grown));
                if(!grown) {fprintf(stderr,"[feedback] allocation failed\n");abort();}
                memset(grown+port_feedback_capacity,0,(capacity-port_feedback_capacity)*sizeof(*grown));
                port_feedback_surfaces=grown;port_feedback_capacity=capacity;
            }
            slot=&port_feedback_surfaces[port_feedback_count++];
        }
        if(slot->resource) slot->resource->lpVtbl->Release(slot->resource);
        memset(slot,0,sizeof(*slot));slot->desc=desc;slot->source=source;
        D3D12_RESOURCE_DESC read_desc=desc;read_desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr=s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,
            D3D12_HEAP_FLAG_NONE,&read_desc,D3D12_RESOURCE_STATE_COPY_DEST,NULL,
            &IID_ID3D12Resource,(void**)&slot->resource);
        if(FAILED(hr)) {fprintf(stderr,"[feedback] resource creation failed %08lX\n",(unsigned long)hr);abort();}
        slot->state=D3D12_RESOURCE_STATE_COPY_DEST;
    }
    slot->batch=port_feedback_batch;
    D3D12_RESOURCE_STATES old=rt->st;
    off_rt_transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=slot->resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if(slot->state!=D3D12_RESOURCE_STATE_COPY_DEST) {
        barrier.Transition.StateBefore=slot->state;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list,1,&barrier);
    }
    s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list,slot->resource,rt->res);
    barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list,1,&barrier);
    slot->state=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    off_rt_transition(source,old);
    return slot->resource;
}
static int port_feedback_alias(int source,int color,const int mrt[3])
{
    if(source<0 || source>=MAX_OFF_RTS || !s_d3d.off_rt[source].res) return 0;
    ID3D12Resource* res=s_d3d.off_rt[source].res;
    if(color>=0 && res==s_d3d.off_rt[color].res) return 1;
    for(unsigned i=0;i<3;i++) if(mrt[i]>=0 && res==s_d3d.off_rt[mrt[i]].res) return 1;
    return 0;
}
static void port_feedback_bind(const D3D12DrawRecord* dr,u32 draw,int color,const int mrt[3],u32 used)
{
    int copied[PORT_TEXTURE_UNITS];ID3D12Resource* snapshots[PORT_TEXTURE_UNITS];unsigned count=0;
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) {
        int source=dr->tex_rt[u];
        if(!(used&(1u<<u)) || !port_feedback_alias(source,color,mrt)) continue;
        unsigned k=0;for(;k<count;k++) if(copied[k]==source) break;
        if(k==count) {copied[count]=source;snapshots[count++]=port_feedback_copy(source);}
        /* Every draw has a separate descriptor window. The snapshot itself
         * can be reused in GPU order after its previous readers finish. */
        srv_write(DRAW_SRV_BASE+draw*PORT_TEXTURE_UNITS+u,snapshots[k],
            (DXGI_FORMAT)s_d3d.off_rt[source].dxgi,
            rsx_remap_to_d3d(dr->tex[u].ctrl1,dr->tex[u].fmt&0x9Fu));
        if(getenv("PORT_RENDER_TRACE")) {
            static unsigned shown;if(shown++<32)
                fprintf(stderr,"[feedback] draw=%u unit=%u source=%08X snapshot=%p\n",draw,u,s_d3d.off_rt[source].off,snapshots[k]);
        }
    }
}

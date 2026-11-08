/* An open command list owns neither its resources nor its upload buffers.
 * Retire replaced targets until the batch has been submitted and fenced. */
typedef struct {
    ID3D12Resource* resource;
    ID3D12Resource* upload;
} PortRetiredRT;
static PortRetiredRT* port_retired_rts;
static unsigned port_retired_rt_count,port_retired_rt_capacity;

static void port_retire_resources(ID3D12Resource* resource,ID3D12Resource* upload)
{
    if(!resource && !upload) return;
    if(port_retired_rt_count==port_retired_rt_capacity) {
        unsigned capacity=port_retired_rt_capacity ? port_retired_rt_capacity*2 : 8;
        if(capacity<=port_retired_rt_capacity || (size_t)capacity>SIZE_MAX/sizeof(*port_retired_rts)) abort();
        PortRetiredRT* grown=(PortRetiredRT*)realloc(port_retired_rts,(size_t)capacity*sizeof(*grown));
        if(!grown) abort();
        port_retired_rts=grown;port_retired_rt_capacity=capacity;
    }
    port_retired_rts[port_retired_rt_count++]=(PortRetiredRT){resource,upload};
}

/* Called only after the queue fence, never from the allocation prepass. */
static void port_release_retired_rts(void)
{
    if(port_retired_rt_count && (!s_d3d.fence ||
       s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence)<s_d3d.fence_values[s_d3d.frame_index])) {
        fprintf(stderr,"[render-target] retirement before GPU completion\n");abort();
    }
    for(unsigned i=0;i<port_retired_rt_count;i++) {
        if(port_retired_rts[i].resource) port_retired_rts[i].resource->lpVtbl->Release(port_retired_rts[i].resource);
        if(port_retired_rts[i].upload) port_retired_rts[i].upload->lpVtbl->Release(port_retired_rts[i].upload);
    }
    port_retired_rt_count=0;
}

static void port_destroy_retired_rts(void)
{
    port_release_retired_rts();
    free(port_retired_rts);port_retired_rts=NULL;port_retired_rt_capacity=0;
}

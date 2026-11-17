"""Retain PSOs and share compiled fragment code across state variants."""


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned pipeline-storage anchor changed: ' + old[:90])
        text = text.replace(old, new)

    once('VPFPEntry             vp_fp[VP_FP_CACHE];',
         'VPFPEntry*            vp_fp;\n    int                   vp_fp_capacity;')
    once('static ID3D12PipelineState* port_find_fp_pso(', r'''
/* The compiler options are fixed for guest fragment programs. Cache the
 * final source, including sampler declarations and diagnostic rewrites. */
#define PORT_FP_BYTECODE_SLOTS 256u
#define PORT_FP_BYTECODE_MAX_BYTES (64u * 1024u)
static struct {
    char* source;
    size_t size;
    ID3DBlob* blob;
} port_fp_bytecode[PORT_FP_BYTECODE_SLOTS];
static unsigned s_perf_fp_bytecode_hits;

static void port_drop_fp_bytecode(unsigned slot)
{
    if(port_fp_bytecode[slot].blob)
        port_fp_bytecode[slot].blob->lpVtbl->Release(port_fp_bytecode[slot].blob);
    free(port_fp_bytecode[slot].source);
    memset(&port_fp_bytecode[slot],0,sizeof(port_fp_bytecode[slot]));
}

static HRESULT port_compile_fp(const char* source,ID3DBlob** blob,ID3DBlob** error,int* cache_hit)
{
    const size_t size=strlen(source);
    const int cacheable=size<=PORT_FP_BYTECODE_MAX_BYTES;
    unsigned slot=0;
    *cache_hit=0;
    if(cacheable) {
        u32 hash=2166136261u;
        for(size_t i=0;i<size;i++) hash=(hash^(u8)source[i])*16777619u;
        slot=(hash^(hash>>16))&(PORT_FP_BYTECODE_SLOTS-1);
        if(port_fp_bytecode[slot].blob && port_fp_bytecode[slot].size==size &&
           !memcmp(port_fp_bytecode[slot].source,source,size)) {
            *blob=port_fp_bytecode[slot].blob;
            (*blob)->lpVtbl->AddRef(*blob);
            *cache_hit=1;
            s_perf_fp_bytecode_hits++;
            return S_OK;
        }
    }
    HRESULT result=D3DCompile(source,size,"guest_fp",NULL,NULL,"main","ps_5_0",0,0,blob,error);
    if(SUCCEEDED(result) && *blob && cacheable &&
       (*blob)->lpVtbl->GetBufferSize(*blob)<=PORT_FP_BYTECODE_MAX_BYTES) {
        char* copy=(char*)malloc(size);
        if(copy) {
            memcpy(copy,source,size);
            port_drop_fp_bytecode(slot);
            port_fp_bytecode[slot].source=copy;
            port_fp_bytecode[slot].size=size;
            port_fp_bytecode[slot].blob=*blob;
            (*blob)->lpVtbl->AddRef(*blob);
        }
    }
    return result;
}

/* Only CPU metadata moves. Commands and callers keep the owned COM object. */
static void port_reserve_fp_slot(void)
{
    if(s_d3d.vp_fp_n<s_d3d.vp_fp_capacity) return;
    if(s_d3d.vp_fp_capacity>0x3FFFFFFF) {
        fprintf(stderr,"[port-renderer] pipeline cache capacity overflow\n");abort();
    }
    int capacity=s_d3d.vp_fp_capacity?s_d3d.vp_fp_capacity*2:VP_FP_CACHE;
    if((size_t)capacity>SIZE_MAX/sizeof(VPFPEntry)) {
        fprintf(stderr,"[port-renderer] pipeline cache size overflow\n");abort();
    }
    VPFPEntry* entries=(VPFPEntry*)realloc(s_d3d.vp_fp,(size_t)capacity*sizeof(*entries));
    if(!entries) { fprintf(stderr,"[port-renderer] pipeline cache allocation failed\n");abort(); }
    memset(entries+s_d3d.vp_fp_capacity,0,
           (size_t)(capacity-s_d3d.vp_fp_capacity)*sizeof(*entries));
    s_d3d.vp_fp=entries;s_d3d.vp_fp_capacity=capacity;
}

static void port_release_fp_cache(void)
{
    for(unsigned slot=0;slot<PORT_FP_BYTECODE_SLOTS;slot++) port_drop_fp_bytecode(slot);
    for(int i=0;i<s_d3d.vp_fp_n;i++) {
        ID3D12PipelineState* pso=s_d3d.vp_fp[i].pso;
        if(pso) pso->lpVtbl->Release(pso);
    }
    free(s_d3d.vp_fp);s_d3d.vp_fp=NULL;
    s_d3d.vp_fp_n=0;s_d3d.vp_fp_capacity=0;
}

static ID3D12PipelineState* port_find_fp_pso(''')
    once('''    /* insert (evict oldest when full) */
    if (s_d3d.vp_fp_n >= VP_FP_CACHE) {
        if (s_d3d.vp_fp[0].pso) s_d3d.vp_fp[0].pso->lpVtbl->Release(s_d3d.vp_fp[0].pso);
        memmove(&s_d3d.vp_fp[0], &s_d3d.vp_fp[1], sizeof(VPFPEntry) * (VP_FP_CACHE - 1));
        s_d3d.vp_fp_n = VP_FP_CACHE - 1;
    }''', '''    /* Retain each compiled state until GPU-idle shutdown. */
    port_reserve_fp_slot();''')
    once('    if(s_d3d.vp_fp_n) return 2;\n    VPFPEntry key={0};',
         '    if(s_d3d.vp_fp_n) return 2;\n    port_reserve_fp_slot();\n    VPFPEntry key={0};')
    once('int port_check_pipeline_cache(void)', r'''
static unsigned port_check_pso_releases;
static ULONG STDMETHODCALLTYPE port_check_pso_release(ID3D12PipelineState* pso)
{
    (void)pso;port_check_pso_releases++;return 0;
}
int port_check_pipeline_cache(void)''')
    once(r'''    memset(&s_d3d.vp_fp[0],0,sizeof(VPFPEntry));s_d3d.vp_fp_n=0;
    fprintf(stderr,"[compat-check] pipeline cache relocated hits=%u state misses=%u %s\n",hits,misses,ok?"PASS":"FAIL");''', r'''
    /* More unique states than the old 256-entry FIFO can retain. Check every
     * identity again after each growth, using the production lookup. */
    VPFPEntry template=s_d3d.vp_fp[0];
    memset(&s_d3d.vp_fp[0],0,sizeof(VPFPEntry));s_d3d.vp_fp_n=0;
    for(unsigned i=0;i<1025;i++) {
        port_reserve_fp_slot();key=template;key.ucode_hash=i;
        key.pso=(ID3D12PipelineState*)(uintptr_t)(0x1000u+i);
        s_d3d.vp_fp[s_d3d.vp_fp_n++]=key;
        if(i==256 || i==512 || i==1024) {
            for(unsigned j=0;j<=i;j++) {
                key=template;key.ucode_hash=j;
                if(port_find_fp_pso(&key)!=(ID3D12PipelineState*)(uintptr_t)(0x1000u+j)) ok=0;
            }
        }
    }
    int retained=s_d3d.vp_fp_n;
    /* Fixture identities are never COM objects. Clear them before the real
     * shutdown helper, whose empty/reset path is also exercised here. */
    memset(s_d3d.vp_fp,0,(size_t)s_d3d.vp_fp_n*sizeof(*s_d3d.vp_fp));
    port_release_fp_cache();
    if(s_d3d.vp_fp || s_d3d.vp_fp_capacity || s_d3d.vp_fp_n) ok=0;
    /* Exercise real ownership cleanup with mock COM methods, without a GPU. */
    ID3D12PipelineStateVtbl methods={0};methods.Release=port_check_pso_release;
    ID3D12PipelineState mock={0};mock.lpVtbl=&methods;
    port_check_pso_releases=0;port_reserve_fp_slot();
    for(unsigned i=0;i<3;i++) s_d3d.vp_fp[s_d3d.vp_fp_n++].pso=&mock;
    port_release_fp_cache();
    if(port_check_pso_releases!=3 || s_d3d.vp_fp || s_d3d.vp_fp_n || s_d3d.vp_fp_capacity) ok=0;
    fprintf(stderr,"[compat-check] pipeline cache relocated hits=%u state misses=%u retained=%d releases=%u %s\n",hits,misses,retained,port_check_pso_releases,ok?"PASS":"FAIL");''')
    # The existing backend shutdown waits for GPU completion before this point.
    once('    /* Release retained texture resources after the shutdown fence. */',
         '    port_release_fp_cache();\n    /* Release retained texture resources after the shutdown fence. */')
    once(r'''    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "guest_fp", NULL, NULL,
                            "main", "ps_5_0", 0, 0, &pb, &e);
    if(trace) fprintf(stderr,"[RENDER] FP compile end seq=%u ms=%llu hr=%08lX\n",seq,GetTickCount64()-compile_started,(unsigned long)hr);''',r'''    int bytecode_hit;
    HRESULT hr = port_compile_fp(hlsl,&pb,&e,&bytecode_hit);
    if(trace) fprintf(stderr,"[RENDER] FP compile end seq=%u ms=%llu hr=%08lX cache_hit=%d\n",seq,GetTickCount64()-compile_started,(unsigned long)hr,bytecode_hit);''')
    once('%d calls %d MISS %dKB hashed] | guest',
         '%d calls %d MISS %dKB hashed, %u FP bytecode hits] | guest')
    once('s_perf_pso_miss, s_perf_pso_hashbytes / 1024,',
         's_perf_pso_miss, s_perf_pso_hashbytes / 1024, s_perf_fp_bytecode_hits,')
    once('s_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;',
         's_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0; s_perf_fp_bytecode_hits = 0;')
    return text

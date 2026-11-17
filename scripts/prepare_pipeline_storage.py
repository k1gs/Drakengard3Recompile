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
 * final source, including sampler declarations and diagnostic rewrites.
 * Retain up to 4096 live blobs within 48 MiB and up to 4096 evicted exact
 * sources within 16 MiB (preserving the 64 MiB dynamic ceiling) so first-time
 * compilations and post-eviction recompilations are measured separately. */
#define PORT_FP_BYTECODE_SLOTS 4096u
#define PORT_FP_HISTORY_SLOTS 4096u
#define PORT_FP_POOL_SLOTS (PORT_FP_BYTECODE_SLOTS + PORT_FP_HISTORY_SLOTS)
#define PORT_FP_HASH_BUCKETS 16384u
#define PORT_FP_BYTECODE_MAX_BYTES (64u * 1024u)
#define PORT_FP_BYTECODE_MAX_TOTAL_BYTES (48u * 1024u * 1024u)
#define PORT_FP_HISTORY_MAX_TOTAL_BYTES (16u * 1024u * 1024u)

typedef struct PortFpCacheLimits {
    unsigned max_live_slots;
    size_t max_entry_bytes;
    size_t max_live_bytes;
    unsigned max_history_slots;
    size_t max_history_bytes;
} PortFpCacheLimits;

typedef HRESULT (*PortFpCompilerFn)(const char* source,size_t size,ID3DBlob** blob,ID3DBlob** error);
typedef double (*PortFpClockFn)(void);

typedef struct PortFpCacheEntry {
    char* source;
    size_t size;
    size_t blob_size;
    ID3DBlob* blob;
    u32 hash;
    int hash_next;
    int lru_prev;
    int lru_next;
} PortFpCacheEntry;

static struct {
    PortFpCacheEntry entries[PORT_FP_POOL_SLOTS];
    int buckets[PORT_FP_HASH_BUCKETS];
    int live_head,live_tail;
    int hist_head,hist_tail;
    int free_head;
    unsigned live_count,hist_count;
    size_t live_bytes,hist_bytes;
    int initialized;
} s_fp_cache;

static const PortFpCacheLimits s_fp_default_limits = {
    PORT_FP_BYTECODE_SLOTS,
    PORT_FP_BYTECODE_MAX_BYTES,
    PORT_FP_BYTECODE_MAX_TOTAL_BYTES,
    PORT_FP_HISTORY_SLOTS,
    PORT_FP_HISTORY_MAX_TOTAL_BYTES
};

static unsigned s_perf_fp_bytecode_hits;
static unsigned s_perf_fp_compiles,s_perf_fp_first_compiles,s_perf_fp_recompiles;
static unsigned s_perf_fp_evictions,s_perf_fp_uncached,s_perf_fp_creates,s_perf_fp_history_drops;
static double s_perf_fp_compile_seconds,s_perf_fp_first_compile_seconds;
static double s_perf_fp_recompile_seconds,s_perf_fp_create_seconds;

static u32 port_fp_hash_bytes(const char* source,size_t size)
{
    u32 hash=2166136261u;
    for(size_t i=0;i<size;i++) hash=(hash^(u8)source[i])*16777619u;
    return hash;
}

static unsigned port_fp_bucket(u32 hash)
{
    return (hash^(hash>>16))&(PORT_FP_HASH_BUCKETS-1u);
}

static void port_fp_cache_ensure_init(void)
{
    if(s_fp_cache.initialized) return;
    for(unsigned b=0;b<PORT_FP_HASH_BUCKETS;b++) s_fp_cache.buckets[b]=-1;
    for(unsigned i=0;i<PORT_FP_POOL_SLOTS;i++) {
        memset(&s_fp_cache.entries[i],0,sizeof(s_fp_cache.entries[i]));
        s_fp_cache.entries[i].hash_next=(i+1u<PORT_FP_POOL_SLOTS)?(int)(i+1u):-1;
        s_fp_cache.entries[i].lru_prev=-1;
        s_fp_cache.entries[i].lru_next=-1;
    }
    s_fp_cache.live_head=s_fp_cache.live_tail=-1;
    s_fp_cache.hist_head=s_fp_cache.hist_tail=-1;
    s_fp_cache.free_head=0;
    s_fp_cache.live_count=s_fp_cache.hist_count=0;
    s_fp_cache.live_bytes=s_fp_cache.hist_bytes=0;
    s_fp_cache.initialized=1;
}

static void port_fp_lru_push(int* head,int* tail,int idx)
{
    PortFpCacheEntry* e=&s_fp_cache.entries[idx];
    e->lru_prev=*tail;
    e->lru_next=-1;
    if(*tail>=0) s_fp_cache.entries[*tail].lru_next=idx;
    else *head=idx;
    *tail=idx;
}

static void port_fp_lru_remove(int* head,int* tail,int idx)
{
    PortFpCacheEntry* e=&s_fp_cache.entries[idx];
    if(e->lru_prev>=0) s_fp_cache.entries[e->lru_prev].lru_next=e->lru_next;
    else *head=e->lru_next;
    if(e->lru_next>=0) s_fp_cache.entries[e->lru_next].lru_prev=e->lru_prev;
    else *tail=e->lru_prev;
    e->lru_prev=e->lru_next=-1;
}

static void port_fp_bucket_insert(int idx)
{
    unsigned b=port_fp_bucket(s_fp_cache.entries[idx].hash);
    s_fp_cache.entries[idx].hash_next=s_fp_cache.buckets[b];
    s_fp_cache.buckets[b]=idx;
}

static void port_fp_bucket_unlink(int idx)
{
    unsigned b=port_fp_bucket(s_fp_cache.entries[idx].hash);
    int* link=&s_fp_cache.buckets[b];
    while(*link>=0) {
        if(*link==idx) {
            *link=s_fp_cache.entries[idx].hash_next;
            s_fp_cache.entries[idx].hash_next=-1;
            return;
        }
        link=&s_fp_cache.entries[*link].hash_next;
    }
    abort();
}

static void port_fp_free_push(int idx)
{
    PortFpCacheEntry* e=&s_fp_cache.entries[idx];
    memset(e,0,sizeof(*e));
    e->hash_next=s_fp_cache.free_head;
    e->lru_prev=e->lru_next=-1;
    s_fp_cache.free_head=idx;
}

static int port_fp_free_pop(void)
{
    int idx=s_fp_cache.free_head;
    if(idx>=0) {
        s_fp_cache.free_head=s_fp_cache.entries[idx].hash_next;
        s_fp_cache.entries[idx].hash_next=-1;
    }
    return idx;
}

static void port_fp_drop_oldest_history(void)
{
    int idx=s_fp_cache.hist_head;
    if(idx<0) abort();
    PortFpCacheEntry* e=&s_fp_cache.entries[idx];
    port_fp_lru_remove(&s_fp_cache.hist_head,&s_fp_cache.hist_tail,idx);
    port_fp_bucket_unlink(idx);
    s_fp_cache.hist_bytes-=e->size;
    s_fp_cache.hist_count--;
    free(e->source);
    port_fp_free_push(idx);
    s_perf_fp_history_drops++;
}

static void port_fp_evict_oldest_live(const PortFpCacheLimits* limits)
{
    int idx=s_fp_cache.live_head;
    if(idx<0) abort();
    PortFpCacheEntry* e=&s_fp_cache.entries[idx];
    port_fp_lru_remove(&s_fp_cache.live_head,&s_fp_cache.live_tail,idx);
    s_fp_cache.live_bytes-=e->size+e->blob_size;
    s_fp_cache.live_count--;
    e->blob->lpVtbl->Release(e->blob);
    e->blob=NULL;
    e->blob_size=0;
    s_perf_fp_evictions++;
    if(limits->max_history_slots>0 && e->size<=limits->max_history_bytes) {
        while(s_fp_cache.hist_count>=limits->max_history_slots ||
              s_fp_cache.hist_bytes>limits->max_history_bytes ||
              e->size>limits->max_history_bytes-s_fp_cache.hist_bytes) {
            port_fp_drop_oldest_history();
        }
        port_fp_lru_push(&s_fp_cache.hist_head,&s_fp_cache.hist_tail,idx);
        s_fp_cache.hist_bytes+=e->size;
        s_fp_cache.hist_count++;
    } else {
        port_fp_bucket_unlink(idx);
        free(e->source);
        port_fp_free_push(idx);
        s_perf_fp_history_drops++;
    }
}

static HRESULT port_d3d_compile_fp(const char* source,size_t size,ID3DBlob** blob,ID3DBlob** error)
{
    return D3DCompile(source,size,"guest_fp",NULL,NULL,"main","ps_5_0",0,0,blob,error);
}

static HRESULT port_compile_fp_with(const char* source,ID3DBlob** blob,ID3DBlob** error,int* cache_hit,
                                    PortFpCompilerFn compiler,PortFpClockFn clock_fn,
                                    const PortFpCacheLimits* raw_limits)
{
    if(!source || !blob || !compiler || !raw_limits) return E_INVALIDARG;
    *blob=NULL;
    if(error) *error=NULL;
    if(cache_hit) *cache_hit=0;
    port_fp_cache_ensure_init();

    PortFpCacheLimits limits=*raw_limits;
    if(limits.max_live_slots>PORT_FP_BYTECODE_SLOTS) limits.max_live_slots=PORT_FP_BYTECODE_SLOTS;
    if(limits.max_history_slots>PORT_FP_HISTORY_SLOTS) limits.max_history_slots=PORT_FP_HISTORY_SLOTS;
    if(limits.max_entry_bytes>PORT_FP_BYTECODE_MAX_BYTES) limits.max_entry_bytes=PORT_FP_BYTECODE_MAX_BYTES;
    if(limits.max_live_bytes>PORT_FP_BYTECODE_MAX_TOTAL_BYTES) limits.max_live_bytes=PORT_FP_BYTECODE_MAX_TOTAL_BYTES;
    if(limits.max_history_bytes>PORT_FP_HISTORY_MAX_TOTAL_BYTES) limits.max_history_bytes=PORT_FP_HISTORY_MAX_TOTAL_BYTES;

    const size_t size=strlen(source);
    const int cacheable=size<=limits.max_entry_bytes;
    u32 hash=0;
    int seen_idx=-1;
    if(cacheable) {
        hash=port_fp_hash_bytes(source,size);
        unsigned b=port_fp_bucket(hash);
        for(int idx=s_fp_cache.buckets[b];idx>=0;idx=s_fp_cache.entries[idx].hash_next) {
            PortFpCacheEntry* e=&s_fp_cache.entries[idx];
            if(e->hash==hash && e->size==size && (!size || !memcmp(e->source,source,size))) {
                if(e->blob) {
                    if(s_fp_cache.live_tail!=idx) {
                        port_fp_lru_remove(&s_fp_cache.live_head,&s_fp_cache.live_tail,idx);
                        port_fp_lru_push(&s_fp_cache.live_head,&s_fp_cache.live_tail,idx);
                    }
                    *blob=e->blob;
                    (*blob)->lpVtbl->AddRef(*blob);
                    if(cache_hit) *cache_hit=1;
                    s_perf_fp_bytecode_hits++;
                    return S_OK;
                }
                seen_idx=idx;
                break;
            }
        }
    }

    const double started=clock_fn ? clock_fn() : 0.0;
    HRESULT result=compiler(source,size,blob,error);
    const double elapsed=clock_fn ? (clock_fn()-started) : 0.0;
    s_perf_fp_compile_seconds+=elapsed;
    s_perf_fp_compiles++;

    if(FAILED(result) || !*blob) {
        if(*blob) {
            (*blob)->lpVtbl->Release(*blob);
            *blob=NULL;
        }
        s_perf_fp_uncached++;
        return FAILED(result) ? result : E_FAIL;
    }

    const size_t blob_size=(*blob)->lpVtbl->GetBufferSize(*blob);
    const int can_cache=cacheable && blob_size<=limits.max_entry_bytes &&
        limits.max_live_slots>0 && size<=limits.max_live_bytes &&
        blob_size<=limits.max_live_bytes-size;
    if(!can_cache) {
        s_perf_fp_uncached++;
        return result;
    }

    const size_t live_cost=size+blob_size;
    if(seen_idx>=0) {
        PortFpCacheEntry* e=&s_fp_cache.entries[seen_idx];
        port_fp_lru_remove(&s_fp_cache.hist_head,&s_fp_cache.hist_tail,seen_idx);
        s_fp_cache.hist_bytes-=e->size;
        s_fp_cache.hist_count--;
        while(s_fp_cache.live_count>=limits.max_live_slots ||
              s_fp_cache.live_bytes>limits.max_live_bytes ||
              live_cost>limits.max_live_bytes-s_fp_cache.live_bytes) {
            port_fp_evict_oldest_live(&limits);
        }
        e->blob=*blob;
        e->blob_size=blob_size;
        (*blob)->lpVtbl->AddRef(*blob);
        port_fp_lru_push(&s_fp_cache.live_head,&s_fp_cache.live_tail,seen_idx);
        s_fp_cache.live_bytes+=live_cost;
        s_fp_cache.live_count++;
        s_perf_fp_recompiles++;
        s_perf_fp_recompile_seconds+=elapsed;
        return result;
    }

    char* copy=(char*)malloc(size?size:1u);
    if(!copy) {
        s_perf_fp_uncached++;
        return result;
    }
    if(size) memcpy(copy,source,size);
    while(s_fp_cache.live_count>=limits.max_live_slots ||
          s_fp_cache.live_bytes>limits.max_live_bytes ||
          live_cost>limits.max_live_bytes-s_fp_cache.live_bytes) {
        port_fp_evict_oldest_live(&limits);
    }
    if(s_fp_cache.free_head<0 && s_fp_cache.hist_head>=0)
        port_fp_drop_oldest_history();
    int idx=port_fp_free_pop();
    if(idx<0) {
        free(copy);
        s_perf_fp_uncached++;
        return result;
    }
    PortFpCacheEntry* e=&s_fp_cache.entries[idx];
    e->source=copy;
    e->size=size;
    e->blob_size=blob_size;
    e->blob=*blob;
    e->hash=hash;
    (*blob)->lpVtbl->AddRef(*blob);
    port_fp_bucket_insert(idx);
    port_fp_lru_push(&s_fp_cache.live_head,&s_fp_cache.live_tail,idx);
    s_fp_cache.live_bytes+=live_cost;
    s_fp_cache.live_count++;
    s_perf_fp_first_compiles++;
    s_perf_fp_first_compile_seconds+=elapsed;
    return result;
}

static HRESULT port_compile_fp(const char* source,ID3DBlob** blob,ID3DBlob** error,int* cache_hit)
{
    return port_compile_fp_with(source,blob,error,cache_hit,port_d3d_compile_fp,
                                perf_on()?perf_now:NULL,&s_fp_default_limits);
}

static void port_reset_fp_perf_window(void)
{
    s_perf_fp_bytecode_hits=0;
    s_perf_fp_compiles=s_perf_fp_first_compiles=s_perf_fp_recompiles=0;
    s_perf_fp_evictions=s_perf_fp_uncached=s_perf_fp_creates=s_perf_fp_history_drops=0;
    s_perf_fp_compile_seconds=s_perf_fp_first_compile_seconds=0.0;
    s_perf_fp_recompile_seconds=s_perf_fp_create_seconds=0.0;
}

static void port_emit_fp_perf_window(void)
{
    if(s_perf_fp_compiles || s_perf_fp_creates)
        fprintf(stderr,"[FP-perf] compile=%.3fs/%u first=%.3fs/%u recompile=%.3fs/%u create=%.3fs/%u hits=%u evictions=%u uncached=%u live=%u/%zuKB seen=%u drops=%u\n",
            s_perf_fp_compile_seconds,s_perf_fp_compiles,
            s_perf_fp_first_compile_seconds,s_perf_fp_first_compiles,
            s_perf_fp_recompile_seconds,s_perf_fp_recompiles,
            s_perf_fp_create_seconds,s_perf_fp_creates,
            s_perf_fp_bytecode_hits,s_perf_fp_evictions,s_perf_fp_uncached,
            s_fp_cache.live_count,(s_fp_cache.live_bytes+1023u)/1024u,
            s_fp_cache.live_count+s_fp_cache.hist_count,s_perf_fp_history_drops);
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
    if(perf_on()) {
        port_emit_fp_perf_window();
        port_reset_fp_perf_window();
    }
    if(s_fp_cache.initialized) {
        for(unsigned i=0;i<PORT_FP_POOL_SLOTS;i++) {
            if(s_fp_cache.entries[i].blob)
                s_fp_cache.entries[i].blob->lpVtbl->Release(s_fp_cache.entries[i].blob);
            free(s_fp_cache.entries[i].source);
        }
        memset(&s_fp_cache,0,sizeof(s_fp_cache));
    }
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

typedef struct PortMockBlob {
    ID3DBlob com;
    ULONG refs;
    SIZE_T size;
    u8 tag[8];
} PortMockBlob;

static unsigned s_mock_live_blobs,s_mock_addrefs,s_mock_releases,s_mock_next_id;
static SIZE_T s_mock_next_blob_size;
static int s_mock_should_fail;
static double s_mock_clock_now,s_mock_compile_step;

static ULONG STDMETHODCALLTYPE port_mock_blob_addref(ID3D10Blob* self)
{
    PortMockBlob* b=(PortMockBlob*)self;
    s_mock_addrefs++;
    return ++b->refs;
}
static ULONG STDMETHODCALLTYPE port_mock_blob_release(ID3D10Blob* self)
{
    PortMockBlob* b=(PortMockBlob*)self;
    s_mock_releases++;
    if(!b->refs) abort();
    ULONG r=--b->refs;
    if(!r) { s_mock_live_blobs--;free(b); }
    return r;
}
static LPVOID STDMETHODCALLTYPE port_mock_blob_ptr(ID3D10Blob* self)
{
    PortMockBlob* b=(PortMockBlob*)self;
    return b->tag;
}
static SIZE_T STDMETHODCALLTYPE port_mock_blob_size(ID3D10Blob* self)
{
    PortMockBlob* b=(PortMockBlob*)self;
    return b->size;
}
static ID3D10BlobVtbl s_mock_blob_vtbl = {
    NULL,port_mock_blob_addref,port_mock_blob_release,port_mock_blob_ptr,port_mock_blob_size
};
static ID3DBlob* port_mock_blob_create(SIZE_T size)
{
    PortMockBlob* b=(PortMockBlob*)calloc(1,sizeof(*b));
    if(!b) abort();
    b->com.lpVtbl=&s_mock_blob_vtbl;
    b->refs=1;
    b->size=size;
    u32 id=++s_mock_next_id;
    memcpy(b->tag,&id,sizeof(id));
    s_mock_live_blobs++;
    return &b->com;
}
static double port_mock_clock(void)
{
    return s_mock_clock_now;
}
static HRESULT port_mock_compile_fp(const char* source,size_t size,ID3DBlob** blob,ID3DBlob** error)
{
    (void)source;(void)size;
    s_mock_clock_now+=s_mock_compile_step;
    if(s_mock_should_fail) {
        if(error) *error=port_mock_blob_create(32);
        return E_FAIL;
    }
    *blob=port_mock_blob_create(s_mock_next_blob_size);
    return S_OK;
}

static int port_fp_cache_validate_invariants(const PortFpCacheLimits* limits)
{
    if(!s_fp_cache.initialized) return 0;
    unsigned live_n=0,hist_n=0,free_n=0,bucket_n=0;
    size_t live_b=0,hist_b=0;
    int prev=-1;
    for(int idx=s_fp_cache.live_head;idx>=0;idx=s_fp_cache.entries[idx].lru_next) {
        const PortFpCacheEntry* e=&s_fp_cache.entries[idx];
        if(e->lru_prev!=prev || !e->blob || !e->source) return 0;
        live_b+=e->size+e->blob_size;
        prev=idx;
        if(++live_n>PORT_FP_POOL_SLOTS) return 0;
    }
    if(prev!=s_fp_cache.live_tail) return 0;
    prev=-1;
    for(int idx=s_fp_cache.hist_head;idx>=0;idx=s_fp_cache.entries[idx].lru_next) {
        const PortFpCacheEntry* e=&s_fp_cache.entries[idx];
        if(e->lru_prev!=prev || e->blob || !e->source) return 0;
        hist_b+=e->size;
        prev=idx;
        if(++hist_n>PORT_FP_POOL_SLOTS) return 0;
    }
    if(prev!=s_fp_cache.hist_tail) return 0;
    for(int idx=s_fp_cache.free_head;idx>=0;idx=s_fp_cache.entries[idx].hash_next) {
        const PortFpCacheEntry* e=&s_fp_cache.entries[idx];
        if(e->blob || e->source) return 0;
        if(++free_n>PORT_FP_POOL_SLOTS) return 0;
    }
    for(unsigned b=0;b<PORT_FP_HASH_BUCKETS;b++) {
        for(int idx=s_fp_cache.buckets[b];idx>=0;idx=s_fp_cache.entries[idx].hash_next) {
            const PortFpCacheEntry* e=&s_fp_cache.entries[idx];
            if(!e->source || port_fp_bucket(e->hash)!=b) return 0;
            for(int other=e->hash_next;other>=0;other=s_fp_cache.entries[other].hash_next) {
                const PortFpCacheEntry* o=&s_fp_cache.entries[other];
                if(o->size==e->size && (!e->size || !memcmp(o->source,e->source,e->size))) return 0;
            }
            if(++bucket_n>PORT_FP_POOL_SLOTS) return 0;
        }
    }
    return live_n==s_fp_cache.live_count && hist_n==s_fp_cache.hist_count &&
           live_b==s_fp_cache.live_bytes && hist_b==s_fp_cache.hist_bytes &&
           bucket_n==live_n+hist_n && live_n+hist_n+free_n==PORT_FP_POOL_SLOTS &&
           live_n<=limits->max_live_slots && live_b<=limits->max_live_bytes &&
           hist_n<=limits->max_history_slots && hist_b<=limits->max_history_bytes;
}

static void port_sim_legacy_512(const char* const* sources,unsigned count,unsigned passes,
                                unsigned* out_compiles,unsigned* out_hits,unsigned* out_evictions)
{
    enum { SLOTS=512 };
    const char* slot_src[SLOTS]={0};
    size_t slot_len[SLOTS]={0};
    u32 slot_hash[SLOTS]={0};
    unsigned compiles=0,hits=0,evictions=0;
    for(unsigned p=0;p<passes;p++) {
        for(unsigned i=0;i<count;i++) {
            const char* s=sources[i];
            size_t len=strlen(s);
            u32 h=port_fp_hash_bytes(s,len);
            unsigned first=(h^(h>>16))&(SLOTS-1u);
            unsigned slot=first;
            int hit=0;
            for(unsigned probe=0;probe<SLOTS;probe++) {
                unsigned cand=(first+probe)&(SLOTS-1u);
                if(!slot_src[cand]) { slot=cand;break; }
                if(slot_hash[cand]==h && slot_len[cand]==len && !memcmp(slot_src[cand],s,len)) {
                    hits++;hit=1;break;
                }
            }
            if(!hit) {
                compiles++;
                if(slot_src[slot]) evictions++;
                slot_src[slot]=s;slot_len[slot]=len;slot_hash[slot]=h;
            }
        }
    }
    *out_compiles=compiles;*out_hits=hits;*out_evictions=evictions;
}

static int port_check_fp_cache_collision(void)
{
    port_release_fp_cache();
    port_reset_fp_perf_window();
    /* Pair 1: old 512-bucket collision. Pair 2: exact 62-byte + 32-bit FNV-1a
     * collision (both hash to 0x8171CC7D), exercising full-source memcmp. */
    const char* bucket_pair[]={"float4 main():SV_Target{return float4(12.0,0,0,1);}",
                               "float4 main():SV_Target{return float4(26.0,0,0,1);}"};
    const char* exact_hash_pair[]={
        "/*018139*/float4 main():SV_Target{return float4(035.0,0,0,1);}",
        "/*054178*/float4 main():SV_Target{return float4(119.0,0,0,1);}"};
    int ok=(strlen(exact_hash_pair[0])==strlen(exact_hash_pair[1]) &&
            port_fp_hash_bytes(exact_hash_pair[0],strlen(exact_hash_pair[0]))==
            port_fp_hash_bytes(exact_hash_pair[1],strlen(exact_hash_pair[1])));
    const char** pairs[2]={bucket_pair,exact_hash_pair};
    for(unsigned p=0;p<2;p++) {
        ID3DBlob* blobs[4]={0};
        for(unsigned i=0;i<4;i++) {
            ID3DBlob* error=NULL;
            int hit=0;
            HRESULT hr=port_compile_fp(pairs[p][i%2],&blobs[i],&error,&hit);
            if(error) error->lpVtbl->Release(error);
            if(FAILED(hr) || !blobs[i] || hit!=(int)(i>=2)) ok=0;
        }
        ok=ok && blobs[0]!=blobs[1] && blobs[0]==blobs[2] && blobs[1]==blobs[3];
        if(blobs[0] && blobs[1]) {
            SIZE_T n0=blobs[0]->lpVtbl->GetBufferSize(blobs[0]);
            SIZE_T n1=blobs[1]->lpVtbl->GetBufferSize(blobs[1]);
            ok=ok && (n0!=n1 || memcmp(blobs[0]->lpVtbl->GetBufferPointer(blobs[0]),
                                       blobs[1]->lpVtbl->GetBufferPointer(blobs[1]),n0)!=0);
        }
        for(unsigned i=0;i<4;i++) if(blobs[i]) blobs[i]->lpVtbl->Release(blobs[i]);
    }
    ok=ok && s_perf_fp_compiles==4 && s_perf_fp_first_compiles==4 &&
       s_perf_fp_recompiles==0 && s_perf_fp_bytecode_hits==4 &&
       port_fp_cache_validate_invariants(&s_fp_default_limits);

    /* Evicted-history collision: exact_hash_pair[0] sits in hist_lru with the
     * identical 32-bit FNV-1a hash and length as exact_hash_pair[1]; compiling
     * exact_hash_pair[1] for the first time must NOT be counted as recompile. */
    port_release_fp_cache();
    port_reset_fp_perf_window();
    s_mock_live_blobs=s_mock_addrefs=s_mock_releases=s_mock_next_id=0;
    s_mock_should_fail=0;s_mock_next_blob_size=128;s_mock_clock_now=1.0;s_mock_compile_step=0.01;
    PortFpCacheLimits tiny={1,PORT_FP_BYTECODE_MAX_BYTES,PORT_FP_BYTECODE_MAX_TOTAL_BYTES,
                            4,PORT_FP_HISTORY_MAX_TOTAL_BYTES};
    const char* filler="float4 main():SV_Target{return float4(0.5,0.5,0.5,1);}";
    const char* seq[4]={exact_hash_pair[0],filler,exact_hash_pair[1],exact_hash_pair[0]};
    for(unsigned i=0;i<4;i++) {
        ID3DBlob* b=NULL;int hit=-1;
        HRESULT hr=port_compile_fp_with(seq[i],&b,NULL,&hit,port_mock_compile_fp,port_mock_clock,&tiny);
        if(FAILED(hr) || !b || hit!=0) ok=0;
        if(b) b->lpVtbl->Release(b);
        if(i==2 && (s_perf_fp_first_compiles!=3 || s_perf_fp_recompiles!=0)) ok=0;
    }
    ok=ok && s_perf_fp_first_compiles==3 && s_perf_fp_recompiles==1 &&
       s_perf_fp_evictions==3 && port_fp_cache_validate_invariants(&tiny);
    port_release_fp_cache();
    port_reset_fp_perf_window();
    ok=ok && s_mock_live_blobs==0;
    fprintf(stderr,"[compat-check] colliding fragment programs retained=%d\n",ok);
    return ok;
}

static int port_check_fp_cache_bounds_and_errors(void)
{
    port_release_fp_cache();
    port_reset_fp_perf_window();
    s_mock_live_blobs=s_mock_addrefs=s_mock_releases=s_mock_next_id=0;
    s_mock_should_fail=0;s_mock_next_blob_size=100;s_mock_clock_now=10.0;s_mock_compile_step=0.025;
    int ok=1;

    /* 1. LRU order, COM refcounts, and exact first vs post-eviction timing. */
    PortFpCacheLimits lru_lim={4,512,4096,8,4096};
    char shaders[8][64];
    for(unsigned i=0;i<8;i++)
        snprintf(shaders[i],sizeof(shaders[i]),"float4 main():SV_Target{return float4(%u.0,1,0,1);}",i);
    for(unsigned i=0;i<4;i++) {
        ID3DBlob* b=NULL;int hit=-1;
        HRESULT hr=port_compile_fp_with(shaders[i],&b,NULL,&hit,port_mock_compile_fp,port_mock_clock,&lru_lim);
        PortMockBlob* mb=(PortMockBlob*)b;
        if(FAILED(hr) || !b || hit!=0 || mb->refs!=2) ok=0;
        if(b) b->lpVtbl->Release(b);
        if(mb && mb->refs!=1) ok=0;
    }
    /* Touch shader 0 -> shader 1 becomes LRU victim. */
    {
        ID3DBlob* b=NULL;int hit=0;
        HRESULT hr=port_compile_fp_with(shaders[0],&b,NULL,&hit,port_mock_compile_fp,port_mock_clock,&lru_lim);
        PortMockBlob* mb=(PortMockBlob*)b;
        if(FAILED(hr) || !b || hit!=1 || mb->refs!=2) ok=0;
        if(b) b->lpVtbl->Release(b);
        if(mb && mb->refs!=1) ok=0;
    }
    /* Insert shaders 4 and 5 -> evicts shaders 1 and 2 into history; shader 0 stays live. */
    for(unsigned i=4;i<6;i++) {
        ID3DBlob* b=NULL;int hit=-1;
        HRESULT hr=port_compile_fp_with(shaders[i],&b,NULL,&hit,port_mock_compile_fp,port_mock_clock,&lru_lim);
        if(FAILED(hr) || !b || hit!=0) ok=0;
        if(b) b->lpVtbl->Release(b);
    }
    ok=ok && s_mock_live_blobs==4 && s_perf_fp_first_compiles==6 &&
       s_perf_fp_recompiles==0 && s_perf_fp_evictions==2 && s_fp_cache.hist_count==2;
    /* Shader 0 hits live; shader 1 recompiles from history with distinct timing. */
    {
        ID3DBlob* b0=NULL;int hit0=0;
        if(FAILED(port_compile_fp_with(shaders[0],&b0,NULL,&hit0,port_mock_compile_fp,port_mock_clock,&lru_lim)) || !b0 || hit0!=1) ok=0;
        if(b0) b0->lpVtbl->Release(b0);
        s_mock_compile_step=0.040;
        ID3DBlob* b1=NULL;int hit1=1;
        if(FAILED(port_compile_fp_with(shaders[1],&b1,NULL,&hit1,port_mock_compile_fp,port_mock_clock,&lru_lim)) || !b1 || hit1!=0) ok=0;
        if(b1) b1->lpVtbl->Release(b1);
    }
    ok=ok && s_perf_fp_compiles==7 && s_perf_fp_first_compiles==6 && s_perf_fp_recompiles==1 &&
       s_perf_fp_bytecode_hits==2 && s_perf_fp_evictions==3 &&
       fabs(s_perf_fp_first_compile_seconds-0.150)<1e-6 &&
       fabs(s_perf_fp_recompile_seconds-0.040)<1e-6 &&
       fabs(s_perf_fp_compile_seconds-0.190)<1e-6 &&
       port_fp_cache_validate_invariants(&lru_lim);

    /* 2. Mock compiler error blob ownership + cache non-pollution. */
    s_mock_should_fail=1;
    {
        ID3DBlob* b=(ID3DBlob*)(uintptr_t)1;ID3DBlob* err=NULL;int hit=1;
        HRESULT hr=port_compile_fp_with("broken_shader",&b,&err,&hit,port_mock_compile_fp,port_mock_clock,&lru_lim);
        if(SUCCEEDED(hr) || b!=NULL || !err || hit!=0) ok=0;
        if(err) err->lpVtbl->Release(err);
        /* Retry as valid: must be first_compile, proving failure did not enter history. */
        s_mock_should_fail=0;
        hr=port_compile_fp_with("broken_shader",&b,&err,&hit,port_mock_compile_fp,port_mock_clock,&lru_lim);
        if(FAILED(hr) || !b || err!=NULL || hit!=0) ok=0;
        if(b) b->lpVtbl->Release(b);
    }
    ok=ok && s_perf_fp_uncached==1 && s_perf_fp_first_compiles==7 && s_perf_fp_recompiles==1 &&
       s_mock_live_blobs==4 && port_fp_cache_validate_invariants(&lru_lim);

    /* 3. Memory bounds: per-entry source/blob cap, multi-entry byte eviction,
     * single-entry > max_live_bytes preservation guard, and history drops. */
    port_release_fp_cache();
    port_reset_fp_perf_window();
    ok=ok && s_mock_live_blobs==0;
    PortFpCacheLimits mem_lim={16,300,1000,2,180};
    char pad_src[640];
    for(unsigned i=0;i<4;i++) {
        memset(pad_src,'a'+(int)i,60);pad_src[60]=0;
        s_mock_next_blob_size=180; /* live_cost = 60 + 180 = 240; 4 * 240 = 960 <= 1000 */
        ID3DBlob* b=NULL;int hit=-1;
        if(FAILED(port_compile_fp_with(pad_src,&b,NULL,&hit,port_mock_compile_fp,NULL,&mem_lim)) || !b || hit!=0) ok=0;
        if(b) b->lpVtbl->Release(b);
    }
    ok=ok && s_fp_cache.live_count==4 && s_fp_cache.live_bytes==960 && s_perf_fp_evictions==0;
    /* Oversized source (> 300) and oversized blob (> 300): uncached, zero evictions. */
    memset(pad_src,'z',350);pad_src[350]=0;s_mock_next_blob_size=64;
    {
        ID3DBlob* b=NULL;int hit=-1;
        if(FAILED(port_compile_fp_with(pad_src,&b,NULL,&hit,port_mock_compile_fp,NULL,&mem_lim)) || !b || hit!=0) ok=0;
        if(b) b->lpVtbl->Release(b);
    }
    memset(pad_src,'y',60);pad_src[60]=0;s_mock_next_blob_size=350;
    {
        ID3DBlob* b=NULL;int hit=-1;
        if(FAILED(port_compile_fp_with(pad_src,&b,NULL,&hit,port_mock_compile_fp,NULL,&mem_lim)) || !b || hit!=0) ok=0;
        if(b) b->lpVtbl->Release(b);
    }
    ok=ok && s_fp_cache.live_count==4 && s_fp_cache.live_bytes==960 &&
       s_perf_fp_uncached==2 && s_perf_fp_evictions==0 && s_mock_live_blobs==4;
    /* Insert 550-byte entry (250 src + 300 blob): must evict 3 of 4 240-byte entries
     * (960 - 720 + 550 = 790 <= 1000) and overflow history (hist cap 2 slots / 180 B). */
    memset(pad_src,'m',250);pad_src[250]=0;s_mock_next_blob_size=300;
    {
        ID3DBlob* b=NULL;int hit=-1;
        if(FAILED(port_compile_fp_with(pad_src,&b,NULL,&hit,port_mock_compile_fp,NULL,&mem_lim)) || !b || hit!=0) ok=0;
        if(b) b->lpVtbl->Release(b);
    }
    ok=ok && s_fp_cache.live_count==2 && s_fp_cache.live_bytes==790 &&
       s_perf_fp_evictions==3 && s_fp_cache.hist_count==2 && s_fp_cache.hist_bytes==120 &&
       s_perf_fp_history_drops==1 && port_fp_cache_validate_invariants(&mem_lim);
    /* Single entry exceeding max_live_bytes (550 + 550 = 1100 > 1000) must
     * return valid bytecode without evicting existing live entries. */
    PortFpCacheLimits guard_lim={16,600,1000,2,180};
    memset(pad_src,'q',550);pad_src[550]=0;s_mock_next_blob_size=550;
    {
        ID3DBlob* b=NULL;int hit=-1;
        if(FAILED(port_compile_fp_with(pad_src,&b,NULL,&hit,port_mock_compile_fp,NULL,&guard_lim)) || !b || hit!=0) ok=0;
        if(b) b->lpVtbl->Release(b);
    }
    ok=ok && s_fp_cache.live_count==2 && s_fp_cache.live_bytes==790 &&
       s_perf_fp_evictions==3 && s_perf_fp_uncached==3 &&
       port_fp_cache_validate_invariants(&guard_lim);
    port_release_fp_cache();
    port_reset_fp_perf_window();
    ok=ok && s_mock_live_blobs==0 && s_fp_cache.live_bytes==0 && s_fp_cache.hist_bytes==0;

    /* 4. Real D3DCompile failure path must preserve live entries and clean error blob. */
    {
        ID3DBlob* valid=NULL;ID3DBlob* err=NULL;int hit=-1;
        if(FAILED(port_compile_fp("float4 main():SV_Target{return float4(0.25,0.5,0.75,1);}",&valid,&err,&hit)) || !valid || hit!=0) ok=0;
        if(err) { err->lpVtbl->Release(err);err=NULL; }
        ID3DBlob* bad=(ID3DBlob*)(uintptr_t)1;
        HRESULT hr=port_compile_fp("float4 main(:SV_Target{}",&bad,&err,&hit);
        if(SUCCEEDED(hr) || bad!=NULL || !err || hit!=0) ok=0;
        if(err) { err->lpVtbl->Release(err);err=NULL; }
        ID3DBlob* again=NULL;
        if(FAILED(port_compile_fp("float4 main():SV_Target{return float4(0.25,0.5,0.75,1);}",&again,&err,&hit)) || again!=valid || hit!=1 || err!=NULL) ok=0;
        if(again) again->lpVtbl->Release(again);
        if(valid) valid->lpVtbl->Release(valid);
        ok=ok && s_perf_fp_compiles==2 && s_perf_fp_first_compiles==1 &&
           s_perf_fp_recompiles==0 && s_perf_fp_uncached==1 && s_perf_fp_bytecode_hits==1;
    }
    port_release_fp_cache();
    port_reset_fp_perf_window();
    fprintf(stderr,"[compat-check] fp cache bounds/lru/errors=%d\n",ok);
    return ok;
}

static int port_check_fp_cache_working_set(void)
{
    enum { COUNT=640, PASSES=3 };
    char storage[COUNT][80];
    const char* ptrs[COUNT];
    ID3DBlob* first_blobs[COUNT]={0};
    for(unsigned i=0;i<COUNT;i++) {
        snprintf(storage[i],sizeof(storage[i]),
            "float4 main():SV_Target{return float4(%u.0,%u.0,0,1);}",i,i%17u);
        ptrs[i]=storage[i];
    }
    unsigned leg_compiles=0,leg_hits=0,leg_evictions=0;
    port_sim_legacy_512(ptrs,COUNT,PASSES,&leg_compiles,&leg_hits,&leg_evictions);

    port_release_fp_cache();
    port_reset_fp_perf_window();
    int ok=1;
    for(unsigned p=0;p<PASSES;p++) {
        for(unsigned i=0;i<COUNT;i++) {
            ID3DBlob* b=NULL;ID3DBlob* err=NULL;int hit=-1;
            HRESULT hr=port_compile_fp_with(ptrs[i],&b,&err,&hit,port_d3d_compile_fp,perf_now,&s_fp_default_limits);
            if(err) err->lpVtbl->Release(err);
            if(FAILED(hr) || !b || hit!=(int)(p>0)) { ok=0;if(b) b->lpVtbl->Release(b);continue; }
            if(p==0) {
                first_blobs[i]=b;
                if(i>0 && first_blobs[i]==first_blobs[i-1]) ok=0;
            } else {
                if(b!=first_blobs[i]) ok=0;
                b->lpVtbl->Release(b);
            }
        }
    }
    for(unsigned i=0;i<COUNT;i++) if(first_blobs[i]) first_blobs[i]->lpVtbl->Release(first_blobs[i]);
    const unsigned cur_compiles=s_perf_fp_compiles;
    const unsigned cur_first=s_perf_fp_first_compiles;
    const unsigned cur_recomp=s_perf_fp_recompiles;
    const unsigned cur_hits=s_perf_fp_bytecode_hits;
    const unsigned cur_evictions=s_perf_fp_evictions;
    const unsigned eliminated=leg_compiles>cur_compiles ? (leg_compiles-cur_compiles) : 0u;
    ok=ok && COUNT>512u && leg_compiles>COUNT && leg_evictions>0 &&
       cur_compiles==COUNT && cur_first==COUNT && cur_recomp==0 &&
       cur_hits==COUNT*(PASSES-1u) && cur_evictions==0 &&
       s_perf_fp_first_compile_seconds>0.0 && s_perf_fp_recompile_seconds==0.0 &&
       s_fp_cache.live_count==COUNT && port_fp_cache_validate_invariants(&s_fp_default_limits);
    fprintf(stderr,"[compat-check] fp working-set (%u shaders x %u passes): legacy512 compiles=%u hits=%u evictions=%u -> current compiles=%u (first=%u recompile=%u) hits=%u evictions=%u eliminated=%u ok=%d\n",
        (unsigned)COUNT,(unsigned)PASSES,leg_compiles,leg_hits,leg_evictions,
        cur_compiles,cur_first,cur_recomp,cur_hits,cur_evictions,eliminated,ok);
    port_release_fp_cache();
    port_reset_fp_perf_window();
    return ok;
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
    ok=port_check_fp_cache_collision() && ok;
    ok=port_check_fp_cache_bounds_and_errors() && ok;
    ok=port_check_fp_cache_working_set() && ok;
    port_release_fp_cache();
    port_reset_fp_perf_window();
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
         's_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;\n'
         '            port_reset_fp_perf_window();')
    once(r'''    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
        s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if(trace) fprintf(stderr,"[RENDER] PSO end seq=%u ms=%llu hr=%08lX\n",seq,GetTickCount64()-compile_started,(unsigned long)hr);''',r'''    const int profile_create=perf_on();
    const double create_started=profile_create ? perf_now() : 0.0;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
        s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
    if(profile_create) {
        s_perf_fp_create_seconds+=perf_now()-create_started;
        s_perf_fp_creates++;
    }
    if(trace) fprintf(stderr,"[RENDER] PSO end seq=%u ms=%llu hr=%08lX\n",seq,GetTickCount64()-compile_started,(unsigned long)hr);''')
    once('            s_perf_frame = 0.0; s_perf_tex',r'''            port_emit_fp_perf_window();
            s_perf_frame = 0.0; s_perf_tex''')
    return text

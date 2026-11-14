/* The FIFO owner records the same bytes used for constants, shader identity
 * and translation. A guest upload can reuse the original address before replay. */
static u8* port_fp_snapshots;
static size_t port_fp_snapshot_used,port_fp_snapshot_capacity;

static void port_snapshot_fp(D3D12DrawRecord* dr)
{
    dr->fp_size=0;
    dr->fp_offset=0;
    if(!dr->fp_addr)return;
    extern u8* vm_base;
    extern u32 cellGcmResolveLocated(int,u32);
    u32 ea=cellGcmResolveLocated((dr->fp_addr&3u)==1,dr->fp_addr&~3u);
    if(!vm_base || ea==0xFFFFFFFFu)return;
    const u8* code=vm_base+ea;
    u32 size=rsx_fp_program_size(code,PORT_FP_PROGRAM_BYTES);
    if(!size)return;
    size_t required=port_fp_snapshot_used+size;
    if(required>port_fp_snapshot_capacity) {
        size_t capacity=port_fp_snapshot_capacity?port_fp_snapshot_capacity*2:65536;
        while(capacity<required)capacity*=2;
        u8* grown=(u8*)realloc(port_fp_snapshots,capacity);
        if(!grown)abort();
        port_fp_snapshots=grown;
        port_fp_snapshot_capacity=capacity;
    }
    dr->fp_offset=port_fp_snapshot_used;
    dr->fp_size=size;
    memcpy(port_fp_snapshots+dr->fp_offset,code,size);
    port_fp_snapshot_used=required;
}

static const u8* port_draw_fp(const D3D12DrawRecord* dr)
{
    if(!dr->fp_size || dr->fp_offset+dr->fp_size>port_fp_snapshot_used)return NULL;
    return port_fp_snapshots+dr->fp_offset;
}

static void port_release_fp_snapshots(void)
{
    free(port_fp_snapshots);
    port_fp_snapshots=NULL;
    port_fp_snapshot_used=port_fp_snapshot_capacity=0;
}

static int port_check_fp_snapshot(void)
{
    extern u8* vm_base;
    extern u32 cellGcmResolveLocated(int,u32);
    D3D12DrawRecord a={0},b={0};
    a.fp_addr=b.fp_addr=0x501;
    u32 ea=cellGcmResolveLocated(1,0x500);
    u8* code=vm_base+ea;
    u8 saved[32];memcpy(saved,code,sizeof(saved));
    memset(code,0,32);
    /* RSX MOV CONST r0, followed by an inline float4. Words are half-swapped. */
    const u8 code_a[32]={0x1E,0x01,0x01,0x00,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0x40,0};
    memcpy(code,code_a,sizeof(code_a));
    port_snapshot_fp(&a);
    code[1]=3;
    code[18]=0x41;code[19]=0x10;
    port_snapshot_fp(&b);
    memset(code,0xCD,32);
    float constants[PORT_FP_CONSTANT_CAPACITY*4];
    int distinct=a.fp_size==32 && b.fp_size==32 &&
        !memcmp(port_draw_fp(&a),code_a,32) &&
        rsx_fp_code_hash(port_draw_fp(&a),a.fp_size)!=rsx_fp_code_hash(port_draw_fp(&b),b.fp_size);
    int collected=distinct && port_collect_guest_fp(port_draw_fp(&a),a.fp_size,constants)==1 && constants[0]==2.0f &&
        port_collect_guest_fp(port_draw_fp(&b),b.fp_size,constants)==1 && constants[0]==9.0f;
    memcpy(code,saved,sizeof(saved));
    port_release_fp_snapshots();
    fprintf(stderr,"[compat-check] FP address reuse code=%d constants=%d\n",distinct,collected);
    return distinct && collected;
}

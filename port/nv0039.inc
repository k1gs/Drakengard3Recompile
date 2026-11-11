/* NV0039 byte-column copies into ordinary guest memory. Color surfaces are
 * read from GPU backing. Depth, display and report DMA stay unsupported. */
/* Register/byte-stride reference (independent implementation below):
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/NV47/HW/nv0039.cpp */
extern int port_renderer_guest_range_owned(u32,u32);
extern u8* port_renderer_read_color(u32,u32);
extern u32 cellGcmResolveIO(u32);
typedef struct {
    u32 in_dma,out_dma,in_off,out_off,in_pitch,out_pitch,width,lines,format;
} PortNv0039;
static PortNv0039 port_nv0039;
enum { PORT_NV_COPY=1,PORT_NV_NOP=0,PORT_NV_FORMAT=-1,PORT_NV_MAP=-2,
       PORT_NV_GPU=-3,PORT_NV_OVERLAP=-4,PORT_NV_CONTEXT=-5,PORT_NV_NOTIFY=-6 };
static int port_nv0039_last;
static u64 port_nv0039_copies,port_nv0039_rejected,port_nv0039_noops,port_nv0039_results[8];
static void port_nv0039_reset(void)
{
    memset(&port_nv0039,0,sizeof(port_nv0039));port_nv0039_last=0;
    port_nv0039_copies=port_nv0039_rejected=port_nv0039_noops=0;
    memset(port_nv0039_results,0,sizeof(port_nv0039_results));
}
int port_gcm_display_range_owned(u32 ea,u32 bytes)
{
    for(unsigned i=0;i<CELL_GCM_MAX_DISPLAY_BUFFER_NUM;i++)if(s_display_buffer_set[i]) {
        u64 start=(u64)s_config.localAddress+s_display_buffers[i].offset;
        u64 span=(u64)s_display_buffers[i].pitch*s_display_buffers[i].height;
        if(span && (u64)ea<start+span && start<(u64)ea+bytes)return 1;
    }
    return 0;
}
static u32 port_nv0039_ea(u32 dma,u32 offset)
{
    if(dma==0xFEED0000u) {
        if(offset>=s_config.localSize || (u64)s_config.localAddress+offset>0xFFFFFFFFull)return 0;
        return cellGcmResolveLocated(1,offset);
    }
    return dma==0xFEED0001u?cellGcmResolveIO(offset):0;
}
static int port_nv0039_host(u32 ea,u32 bytes,int write,int local)
{
#ifdef _WIN32
    extern u8* vm_base;
    if(!ea || !vm_base || (u64)ea+bytes>0x100000000ull)return 0;
    uintptr_t at=(uintptr_t)(vm_base+ea),end=at+bytes;
    while(at<end) {
        MEMORY_BASIC_INFORMATION info={0};
        if(!VirtualQuery((const void*)at,&info,sizeof(info)))return 0;
        if(info.State==MEM_RESERVE && write && local && info.AllocationBase==(void*)vm_base) {
            /* LOCAL DMA bounds and GPU ownership were validated by the caller.
             * Materialize only a virgin destination shadow, inside our reserved
             * guest window. Never manufacture an unbacked source or MAIN page. */
            SYSTEM_INFO system;GetSystemInfo(&system);uintptr_t page=system.dwPageSize;
            uintptr_t first=at-at%page,limit=(uintptr_t)info.BaseAddress+info.RegionSize;
            if(limit>end)limit=end;limit=(limit+page-1)/page*page;
            if(!VirtualAlloc((void*)first,limit-first,MEM_COMMIT,PAGE_READWRITE))return 0;
            continue;
        }
        if(info.State!=MEM_COMMIT || (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) {
            static unsigned shown[2];unsigned kind=write?1:0;
            if(shown[kind]++<16)fprintf(stderr,"[nv0039-memory] %s ea=%08X bytes=%u state=%lX protect=%lX local=%d\n",
                write?"write":"read",ea,bytes,(unsigned long)info.State,(unsigned long)info.Protect,local);
            return 0;
        }
        DWORD access=info.Protect&255;
        if(write && access!=PAGE_READWRITE && access!=PAGE_WRITECOPY &&
                    access!=PAGE_EXECUTE_READWRITE && access!=PAGE_EXECUTE_WRITECOPY)return 0;
        if(!write && access!=PAGE_READONLY && access!=PAGE_READWRITE && access!=PAGE_WRITECOPY &&
                     access!=PAGE_EXECUTE_READ && access!=PAGE_EXECUTE_READWRITE && access!=PAGE_EXECUTE_WRITECOPY)return 0;
        uintptr_t next=(uintptr_t)info.BaseAddress+info.RegionSize;if(next<=at)return 0;at=next;
    }
    return 1;
#else
    return 0;
#endif
}
static int port_nv0039_row(u32 offset,u32 pitch,u32 row,u64 bytes,u32* result)
{
    int64_t at=(int64_t)offset+(int64_t)(int32_t)pitch*row;
    if(at<0 || (u64)at+bytes>0x100000000ull)return 0;
    *result=(u32)at;return 1;
}
static int port_nv0039_span(u32 dma,u32 offset,u64 bytes,int write,u64* low,u64* high,int* gpu_source)
{
    while(bytes) {
        u32 chunk=(u32)(0x100000u-(offset&0xFFFFFu));if(chunk>bytes)chunk=(u32)bytes;
        u32 ea=port_nv0039_ea(dma,offset);
        if(dma==0xFEED0000u && (u64)offset+chunk>s_config.localSize)return PORT_NV_MAP;
        if(!ea || (u64)ea+chunk>0x100000000ull)return PORT_NV_MAP;
        if(port_renderer_guest_range_owned(ea,chunk)) {
            if(write)return PORT_NV_GPU;
            *gpu_source=1;
        } else if(!port_nv0039_host(ea,chunk,write,dma==0xFEED0000u))return PORT_NV_MAP;
        if(ea<*low)*low=ea;if((u64)ea+chunk>*high)*high=(u64)ea+chunk;
        offset+=chunk;bytes-=chunk;
    }
    return PORT_NV_COPY;
}
static int port_nv0039_copy(const PortNv0039* p,u32 notify)
{
    if(!p->width || !p->lines)return PORT_NV_NOP;
    u32 in=p->format&255u,out=(p->format>>8)&255u;
    if((p->format&0xFFFF0000u) || !in || in>4 || !out || out>4)return PORT_NV_FORMAT;
    if(notify)return PORT_NV_NOTIFY;
    if((p->in_dma!=0xFEED0000u && p->in_dma!=0xFEED0001u) ||
       (p->out_dma!=0xFEED0000u && p->out_dma!=0xFEED0001u))return PORT_NV_CONTEXT;
    /* A queued draw may be the source producer or a previous destination
     * reader. Execute it through the normal FIFO/renderer boundary first. */
    port_renderer_submit_pending();
    u64 in_bytes=(u64)(p->width-1)*in+1,out_bytes=(u64)(p->width-1)*out+1;
    u64 src_low=0x100000000ull,src_high=0,dst_low=0x100000000ull,dst_high=0;
    int gpu_source=0;
    for(u32 row=0;row<p->lines;row++) {
        u32 src,dst;
        if(!port_nv0039_row(p->in_off,p->in_pitch,row,in_bytes,&src) ||
           !port_nv0039_row(p->out_off,p->out_pitch,row,out_bytes,&dst))return PORT_NV_MAP;
        int result=port_nv0039_span(p->in_dma,src,in_bytes,0,&src_low,&src_high,&gpu_source);
        if(result!=PORT_NV_COPY)return result;
        result=port_nv0039_span(p->out_dma,dst,out_bytes,1,&dst_low,&dst_high,&gpu_source);
        if(result!=PORT_NV_COPY)return result;
    }
    /* Deliberately disjoint-only. Bounding gaps in discontiguous mappings may
     * reject an otherwise safe case; no guessed overlapping-DMA semantics. */
    if(src_low<dst_high && dst_low<src_high)return PORT_NV_OVERLAP;
    u8* color=NULL;
    if(gpu_source) {
        u64 span=src_high-src_low;
        if(span>UINT32_MAX)return PORT_NV_GPU;
        color=port_renderer_read_color((u32)src_low,(u32)span);
        if(!color)return PORT_NV_GPU;
    }
    extern u8* vm_base;
    for(u32 row=0;row<p->lines;row++) {
        u32 src,dst;port_nv0039_row(p->in_off,p->in_pitch,row,in_bytes,&src);
        port_nv0039_row(p->out_off,p->out_pitch,row,out_bytes,&dst);
        for(u32 column=0;column<p->width;) {
            u32 so=(u32)((u64)src+(u64)column*in),doff=(u32)((u64)dst+(u64)column*out);
            u32 columns=p->width-column;
            u32 si=(0xFFFFFu-(so&0xFFFFFu))/in+1,di=(0xFFFFFu-(doff&0xFFFFFu))/out+1;
            if(columns>si)columns=si;if(columns>di)columns=di;
            u32 se=port_nv0039_ea(p->in_dma,so),de=port_nv0039_ea(p->out_dma,doff);
            const u8* source=color?color+(se-src_low):vm_base+se;
            if(in==1 && out==1)vm_memcpy_to(de,source,columns);
            else for(u32 x=0;x<columns;x++)vm_write8(de+x*out,source[x*in]);
            port_gcm_notify_write(de,(columns-1)*out+1);column+=columns;
        }
    }
    free(color);
    return PORT_NV_COPY;
}
static int port_nv0039_method(u32 method,u32 value)
{
    switch(method) {
    case 0:case 0x100:case 0x180:return 1;
    case 0x184:port_nv0039.in_dma=value;return 1;
    case 0x188:port_nv0039.out_dma=value;return 1;
    case 0x30C:port_nv0039.in_off=value;return 1;
    case 0x310:port_nv0039.out_off=value;return 1;
    case 0x314:port_nv0039.in_pitch=value;return 1;
    case 0x318:port_nv0039.out_pitch=value;return 1;
    case 0x31C:port_nv0039.width=value;return 1;
    case 0x320:port_nv0039.lines=value;return 1;
    case 0x324:port_nv0039.format=value;return 1;
    case 0x328: {
        PortNv0039 p=port_nv0039;port_nv0039_last=port_nv0039_copy(&p,value);
        if(port_nv0039_last==PORT_NV_COPY)port_nv0039_copies++;
        else if(port_nv0039_last==PORT_NV_NOP)port_nv0039_noops++;
        else port_nv0039_rejected++;
        u64 count=++port_nv0039_results[port_nv0039_last+6];
        const char* trace=getenv("PORT_NV0039_TRACE");int cap=trace?atoi(trace):8;if(cap<8)cap=8;
        if(count<=(u64)cap)fprintf(stderr,
            "[nv0039] result=%d in=%08X:%08X out=%08X:%08X width=%u lines=%u pitch=%08X/%08X format=%08X notify=%08X count=%llu\n",
            port_nv0039_last,p.in_dma,p.in_off,p.out_dma,p.out_off,p.width,p.lines,p.in_pitch,p.out_pitch,p.format,value,(unsigned long long)count);
        return 1;
    }
    default:return 0;
    }
}

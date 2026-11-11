/* NV4097 report methods, with the existing HLE local-report table as the
 * consumer. Main-memory reports and other counters need separate producers.
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/NV47/HW/nv4097.cpp */
extern void port_renderer_zpass_clear(void);
extern void port_renderer_zpass_reset(void);
extern void port_renderer_zpass_enable(u32,u32);
extern int port_renderer_zpass_read(u32*);

/* The local report DMA object is a 32 KiB window of 16-byte guest records.
 * The HLE table uses host byte order and also serves timestamp-only calls.
 * Only FIFO report completions publish bytes into the DMA window.
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/lv2/sys_rsx.h */
enum { PORT_LOCAL_REPORT_DMA=0x66626660u, PORT_LOCAL_REPORT_COUNT=2048,
       PORT_REPORT_SIZE=16, PORT_REPORT_PUBLISHED=1, PORT_REPORT_UNSUPPORTED=0x100 };
static u32 port_report_context=PORT_LOCAL_REPORT_DMA;
static u8 port_local_report_bytes[PORT_LOCAL_REPORT_COUNT*PORT_REPORT_SIZE];
static u16 port_local_report_state[PORT_LOCAL_REPORT_COUNT];

static void port_gcm_report_publish(u32 index,u64 timestamp,u32 samples)
{
    s_report_data[index]=(CellGcmReportData){timestamp,samples,0};
    u8* bytes=port_local_report_bytes+index*PORT_REPORT_SIZE;
    for(unsigned i=0;i<8;i++)bytes[i]=(u8)(timestamp>>(56-8*i));
    for(unsigned i=0;i<4;i++)bytes[8+i]=(u8)(samples>>(24-8*i));
    memset(bytes+12,0,4);
    port_local_report_state[index]=PORT_REPORT_PUBLISHED;
}

void port_gcm_report_method(u32 method,u32 value)
{
    switch(method) {
    case 0x1A8:
        port_report_context=value;
        return;
    case 0x17C8:
        if(value&1u)port_renderer_zpass_clear();
        return;
    case 0x17CC:
    case 0x1D84:
        port_renderer_zpass_enable(method,value);
        return;
    case 0x1800:
        break;
    default:
        return;
    }

    const u32 type=value>>24,offset=value&0xFFFFFFu;
    const u32 index=offset/PORT_REPORT_SIZE;
    const int local_slot=port_report_context==PORT_LOCAL_REPORT_DMA &&
        offset%PORT_REPORT_SIZE==0 && index<PORT_LOCAL_REPORT_COUNT;
    u32 samples;
    if(!local_slot || type!=1 || !port_renderer_zpass_read(&samples)) {
        if(local_slot)port_local_report_state[index]=(u16)(PORT_REPORT_UNSUPPORTED|type);
        static unsigned shown;
        if(shown++<16)fprintf(stderr,"[rsx-report] unsupported type=%u context=%08X offset=%06X\n",
            type,port_report_context,offset);
        return;
    }
    port_gcm_report_publish(index,get_timestamp_ns(),samples);
    static unsigned shown;
    if(shown++<16)fprintf(stderr,"[rsx-report] zpass offset=%06X samples=%u\n",offset,samples);
}

static void port_gcm_report_reset(void)
{
    port_report_context=PORT_LOCAL_REPORT_DMA;
    memset(port_local_report_state,0,sizeof(port_local_report_state));
    port_renderer_zpass_reset();
}

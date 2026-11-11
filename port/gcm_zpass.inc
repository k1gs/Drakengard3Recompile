/* NV4097 report methods, with the existing HLE local-report table as the
 * consumer. Main-memory reports and other counters need separate producers.
 * https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/NV47/HW/nv4097.cpp */
extern void port_renderer_zpass_clear(void);
extern void port_renderer_zpass_reset(void);
extern void port_renderer_zpass_enable(u32,u32);
extern int port_renderer_zpass_read(u32*);
static u32 port_report_context=0x66626660u;

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
    u32 samples;
    if(type!=1 || port_report_context!=0x66626660u || (offset&15u) ||
        offset/16>=CELL_GCM_MAX_REPORT_COUNT || !port_renderer_zpass_read(&samples)) {
        static unsigned shown;
        if(shown++<16)fprintf(stderr,"[rsx-report] unsupported type=%u context=%08X offset=%06X\n",
            type,port_report_context,offset);
        return;
    }
    CellGcmReportData* report=&s_report_data[offset/16];
    report->timestamp=get_timestamp_ns();
    report->value=samples;
    report->pad=0;
    static unsigned shown;
    if(shown++<16)fprintf(stderr,"[rsx-report] zpass offset=%06X samples=%u\n",offset,samples);
}

static void port_gcm_report_reset(void)
{
    port_report_context=0x66626660u;
    port_renderer_zpass_reset();
}

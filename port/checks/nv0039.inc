static int port_check_report_dma(void)
{
    extern u8* vm_base;
    static const u8 record[16]={1,2,3,4,5,6,7,8,0x81,0x02,0xFE,0x39,0,0,0,0};
    u8 expected[80];
    for(unsigned i=0;i<5;i++) {
        port_gcm_report_publish(i,0x0102030405060708ull+i,0x8102FE39u-i);
        memcpy(expected+i*16,record,16);
        expected[i*16+7]+=i;
        expected[i*16+11]-=i;
    }
    /* Match the 80-byte title transfer, crossing two disjoint IO mappings. */
    gcm_2d_method(1,0x184,PORT_LOCAL_REPORT_DMA);gcm_2d_method(1,0x188,0xFEED0001u);
    gcm_2d_method(1,0x30C,0);gcm_2d_method(1,0x310,0xFFFFCu);
    gcm_2d_method(1,0x314,0);gcm_2d_method(1,0x318,0);
    gcm_2d_method(1,0x31C,80);gcm_2d_method(1,0x320,1);gcm_2d_method(1,0x324,0x101);
    gcm_2d_method(1,0x328,0);
    int full=port_nv0039_last==PORT_NV_COPY && !memcmp(vm_base+0x1FFFFC,expected,4) &&
        !memcmp(vm_base+0x300000,expected+4,76);

    const u32 scratch=s_config.localAddress+0x900;
    u8 sparse[16];memset(sparse,0xCD,sizeof(sparse));memset(vm_base+scratch,0xCD,sizeof(sparse));
    PortNv0039 strided={PORT_LOCAL_REPORT_DMA,0xFEED0000u,20,0x900,(u32)-16,8,4,2,0x202};
    for(unsigned i=0;i<4;i++) {
        sparse[i*2]=expected[20+i*2];
        sparse[8+i*2]=expected[4+i*2];
    }
    int steps=port_nv0039_copy(&strided,0)==PORT_NV_COPY &&
        !memcmp(vm_base+scratch,sparse,sizeof(sparse));
    /* No earlier row may be committed if a later report slot is unavailable. */
    PortNv0039 partial={PORT_LOCAL_REPORT_DMA,0xFEED0000u,64,0x900,16,8,8,2,0x101};
    int unwritten=port_nv0039_copy(&partial,0)==PORT_NV_REPORT &&
        !memcmp(vm_base+scratch,sparse,sizeof(sparse));
    port_gcm_report_method(0x1800,0x02000040);
    partial.lines=1;
    int unsupported=port_nv0039_copy(&partial,0)==PORT_NV_REPORT &&
        !memcmp(vm_base+scratch,sparse,sizeof(sparse));
    port_gcm_report_publish(2047,0x0102030405060708ull,0x8102FE39u);
    partial.in_off=0x7FF0;partial.width=16;
    int last=port_nv0039_copy(&partial,0)==PORT_NV_COPY && !memcmp(vm_base+scratch,record,16);
    partial.width=17;
    int bounds=port_nv0039_copy(&partial,0)==PORT_NV_MAP && !memcmp(vm_base+scratch,record,16);
    port_gcm_report_reset();
    partial.width=16;
    int reset=port_nv0039_copy(&partial,0)==PORT_NV_REPORT && !memcmp(vm_base+scratch,record,16);
    fprintf(stderr,"[compat-check] report DMA bytes=%d strided=%d unwritten=%d unsupported=%d last=%d bounds=%d reset=%d\n",
        full,steps,unwritten,unsupported,last,bounds,reset);
    return full && steps && unwritten && unsupported && last && bounds && reset;
}

/* Exercise the actual 2D method dispatcher, with independent guest bytes. */
int port_check_nv0039(void)
{
    extern u8* vm_base;
    CellGcmConfig saved=s_config;
    CellGcmDisplayInfo saved_display=s_display_buffers[0];int saved_display_set=s_display_buffer_set[0];
    s_config.localAddress=0x40000;s_config.localSize=0x10000;
    memset(s_ea_address_table,0xFF,sizeof(s_ea_address_table));
    s_ea_address_table[0]=1;s_ea_address_table[1]=3;s_ea_address_table[2]=5;
    u32 source=0x40100,dest=0x40300;
    const u8 expected[8]={0x81,0x02,0xFE,0x39,0,0xAA,0x10,0xFF};
    memcpy(vm_base+source,expected,8);memset(vm_base+dest,0xCD,16);
    gcm_2d_method(1,0x184,0xFEED0000);gcm_2d_method(1,0x188,0xFEED0000);
    gcm_2d_method(1,0x30C,0x100);gcm_2d_method(1,0x310,0x300);
    gcm_2d_method(1,0x314,0);gcm_2d_method(1,0x318,0);
    gcm_2d_method(1,0x31C,8);gcm_2d_method(1,0x320,1);gcm_2d_method(1,0x324,0x101);
    int deferred=vm_base[dest]==0xCD;
    gcm_2d_method(1,0x328,0);
    int copied=!memcmp(vm_base+dest,expected,8),padding=1;
    for(unsigned i=8;i<16;i++)padding=padding && vm_base[dest+i]==0xCD;
    unsigned matrix=0,rejected=0;
    for(unsigned in=1;in<=4;in++)for(unsigned out=1;out<=4;out++)
    for(unsigned locations=0;locations<4;locations++)for(unsigned negative=0;negative<2;negative++) {
        u32 si=(locations&1)?0xFEED0001u:0xFEED0000u,di=(locations&2)?0xFEED0001u:0xFEED0000u;
        u32 so=0x100+(negative?96:0),doff=0x500+(negative?128:0);
        int sp=negative?-48:48,dp=negative?-64:64;
        u8 want[256];memset(want,0xCD,sizeof(want));
        u32 sb=port_nv0039_ea(si,0x100),db=port_nv0039_ea(di,0x500);
        memset(vm_base+sb,0xAB,256);memset(vm_base+db,0xCD,256);
        for(unsigned row=0;row<3;row++)for(unsigned col=0;col<7;col++) {
            u8 value=(u8)(row*31+col*17+in*7+out);
            vm_base[port_nv0039_ea(si,(u32)((int64_t)so+sp*(int)row))+col*in]=value;
            want[(negative?128:0)+dp*(int)row+col*out]=value;
        }
        gcm_2d_method(1,0x184,si);gcm_2d_method(1,0x188,di);
        gcm_2d_method(1,0x30C,so);gcm_2d_method(1,0x310,doff);
        gcm_2d_method(1,0x314,(u32)sp);gcm_2d_method(1,0x318,(u32)dp);
        gcm_2d_method(1,0x31C,7);gcm_2d_method(1,0x320,3);gcm_2d_method(1,0x324,in|(out<<8));
        gcm_2d_method(1,0x328,0);
        if(port_nv0039_last==PORT_NV_COPY && !memcmp(vm_base+db,want,256))matrix++;
    }
    /* Discontiguous MAIN pages: resolve both sides at each IO-page boundary. */
    memcpy(vm_base+0x1FFFFC,expected,4);memcpy(vm_base+0x300000,expected+4,4);
    memset(vm_base+0x3FFFFC,0xCD,4);memset(vm_base+0x500000,0xCD,4);
    gcm_2d_method(1,0x184,0xFEED0001u);gcm_2d_method(1,0x188,0xFEED0001u);
    gcm_2d_method(1,0x30C,0xFFFFC);gcm_2d_method(1,0x310,0x1FFFFC);
    gcm_2d_method(1,0x314,0);gcm_2d_method(1,0x318,0);
    gcm_2d_method(1,0x31C,8);gcm_2d_method(1,0x320,1);gcm_2d_method(1,0x324,0x101);
    gcm_2d_method(1,0x328,0);
    int pages=port_nv0039_last==PORT_NV_COPY && !memcmp(vm_base+0x3FFFFC,expected,4) && !memcmp(vm_base+0x500000,expected+4,4);
    int reports=port_check_report_dma();
    /* Reject every unsupported operation before changing any destination. */
    PortNv0039 valid={0xFEED0000u,0xFEED0000u,0x100,0x300,16,16,8,2,0x101};
    u8 local_before[0x10000];
    for(unsigned test=0;test<9;test++) {
        PortNv0039 command=valid;u32 notify=0;
        if(test==0)command.in_dma=0x66626660;
        if(test==1)command.out_dma=0x12345678;
        if(test==2)command.format=0x501;
        if(test==3)notify=1;
        if(test==4){command.in_off=4;command.in_pitch=(u32)-8;}
        if(test==5){command.out_off=0xFFF8;command.out_pitch=16;}
        if(test==6){command.in_dma=0xFEED0001;command.in_off=0x2FFFFC;}
        if(test==7){command.width=0x80000000u;command.format=0x404;}
        if(test==8){command.in_dma=0xFEED0001;command.in_off=0x200;command.out_off=0x208;s_ea_address_table[0]=0xFFFF;}
        for(unsigned i=0;i<sizeof(local_before);i++)vm_base[0x40000+i]=(u8)((i*29+(i>>8)*7)^0xA5);
        memcpy(local_before,vm_base+0x40000,sizeof(local_before));
        int result=port_nv0039_copy(&command,notify);
        int untouched=!memcmp(local_before,vm_base+0x40000,sizeof(local_before));
        if(result<0 && untouched)rejected++;
        s_ea_address_table[0]=1;
    }
    PortNv0039 overlap=valid;overlap.out_off=0x102;
    int disjoint=port_nv0039_copy(&overlap,0)==PORT_NV_OVERLAP;
    /* Canonical EA ownership also protects display buffers addressed through
     * MAIN aliases, not merely exact LOCAL raw offsets. */
    s_config.localAddress=0x100000;
    if(cellGcmSetDisplayBuffer(0,0x800,128,16,4))return 1;
    memset(vm_base+0x100800,0xCD,8);
    PortNv0039 display={0xFEED0001u,0xFEED0001u,0x100,0x804,0,0,4,1,0x101};
    int display_guard=port_nv0039_copy(&display,0)==PORT_NV_GPU;
    for(unsigned i=0;i<8;i++)display_guard=display_guard && vm_base[0x100800+i]==0xCD;
    s_display_buffers[0]=saved_display;s_display_buffer_set[0]=saved_display_set;
    int lazy_dest=0,unbacked_source=0,main_not_materialized=0,gpu_not_materialized=0;
#ifdef _WIN32
    /* A valid LOCAL destination is physical VRAM even when its CPU shadow
     * page has never been touched. A virgin source is deliberately different. */
    for(u32 candidate=0x70000000u;candidate<0x80000000u;candidate+=0x1000000u) {
        MEMORY_BASIC_INFORMATION a,b;
        if(!VirtualQuery(vm_base+candidate,&a,sizeof(a)) || !VirtualQuery(vm_base+candidate+0x8000,&b,sizeof(b)) ||
            a.State!=MEM_RESERVE || b.State!=MEM_RESERVE)continue;
        if(!VirtualAlloc(vm_base+candidate,0x1000,MEM_COMMIT,PAGE_READWRITE))break;
        memcpy(vm_base+candidate,expected,8);
        s_config.localAddress=candidate;s_config.localSize=0x10000;
        PortNv0039 fresh={0xFEED0000u,0xFEED0000u,0,0x8000,0,0,8,1,0x101};
        int result=port_nv0039_copy(&fresh,0);
        VirtualQuery(vm_base+candidate+0x8000,&b,sizeof(b));
        lazy_dest=result==PORT_NV_COPY && b.State==MEM_COMMIT && !memcmp(vm_base+candidate+0x8000,expected,8);
        if(lazy_dest) {
            fresh.in_off=0x2000;memset(vm_base+candidate+0x8000,0xCD,8);
            result=port_nv0039_copy(&fresh,0);VirtualQuery(vm_base+candidate+0x2000,&a,sizeof(a));
            unbacked_source=result==PORT_NV_MAP && a.State==MEM_RESERVE;
            for(unsigned i=0;i<8;i++)unbacked_source=unbacked_source && vm_base[candidate+0x8000+i]==0xCD;
            fresh.in_off=0;fresh.out_off=0x9000;fresh.out_dma=0xFEED0001u;
            s_ea_address_table[0]=(u16)(candidate>>20);
            result=port_nv0039_copy(&fresh,0);VirtualQuery(vm_base+candidate+0x9000,&a,sizeof(a));
            main_not_materialized=result==PORT_NV_MAP && a.State==MEM_RESERVE;
            s_ea_address_table[0]=1;
            if(cellGcmSetDisplayBuffer(0,0x9000,128,16,4))return 1;
            fresh.out_dma=0xFEED0000u;result=port_nv0039_copy(&fresh,0);
            VirtualQuery(vm_base+candidate+0x9000,&a,sizeof(a));
            gpu_not_materialized=result==PORT_NV_GPU && a.State==MEM_RESERVE;
            s_display_buffers[0]=saved_display;s_display_buffer_set[0]=saved_display_set;
        }
        break;
    }
#endif
    port_nv0039_reset();gcm_2d_method(1,0x328,0);
    int reset=port_nv0039_last==PORT_NV_NOP;
    s_config=saved;
    fprintf(stderr,"[compat-check] NV0039 actual dispatcher deferred=%d bytes=%d padding=%d %s\n",deferred,copied,padding,
        deferred&&copied&&padding?"PASS":"FAIL");
    fprintf(stderr,"[compat-check] NV0039 matrix=%u/128 discontiguous-pages=%d rejected=%u/9 disjoint=%d display-guard=%d reset=%d\n",matrix,pages,rejected,disjoint,display_guard,reset);
    fprintf(stderr,"[compat-check] NV0039 lazy-destination=%d refuses-unbacked-source=%d MAIN-not-materialized=%d GPU-not-materialized=%d\n",
        lazy_dest,unbacked_source,main_not_materialized,gpu_not_materialized);
    return deferred&&copied&&padding&&matrix==128&&pages&&reports&&rejected==9&&disjoint&&display_guard&&reset&&lazy_dest&&unbacked_source&&main_not_materialized&&gpu_not_materialized?0:1;
}

/* Called by the hardware ZPASS check after two actual GET_REPORT commands. */
int port_check_gpu_report_dma(void)
{
    extern u8* vm_base;
    const u32 ea=0x100100;
    u16 saved_page=s_ea_address_table[0];
    s_ea_address_table[0]=1;
    memset(vm_base+ea,0xCD,32);
    PortNv0039 transfer={PORT_LOCAL_REPORT_DMA,0xFEED0001u,0x100,0x100,0,0,32,1,0x101};
    int ok=port_nv0039_copy(&transfer,0)==PORT_NV_COPY;
    for(u32 i=0;i<2;i++) {
        u32 at=ea+i*16;
        u64 timestamp=((u64)vm_read32(at)<<32)|vm_read32(at+4);
        ok=ok && timestamp && timestamp==s_report_data[16+i].timestamp &&
            vm_read32(at+8)==(i?0:4) && vm_read32(at+12)==0;
    }
    s_ea_address_table[0]=saved_page;
    fprintf(stderr,"[compat-check] GPU ZPASS to NV0039 MAIN bytes=%d\n",ok);
    return ok;
}
/* The producer is a real GPU write. Guest RAM still contains poison bytes. */
int port_check_nv0039_gpu_source(u32 gpu_ea,u32 outside_ea,u32 scratch,const u8* expected)
{
    extern u8* vm_base;
    CellGcmConfig saved=s_config;
    u16 saved_page=s_ea_address_table[0];
    s_ea_address_table[0]=1;
    s_config.localAddress=0;s_config.localSize=0xFFFFFFFFu;
    const u32 main_ea=0x100100;
    u8 gpu_before[40],want[16];
    memcpy(gpu_before,vm_base+gpu_ea,sizeof(gpu_before));
    memset(vm_base+scratch,0x5A,16);
    memset(vm_base+main_ea,0x5A,16);
    PortNv0039 local={0xFEED0000u,0xFEED0000u,gpu_ea,scratch,0,0,16,1,0x101};
    int contiguous=port_nv0039_copy(&local,0)==PORT_NV_COPY &&
        !memcmp(expected,vm_base+scratch,16);
    PortNv0039 main={0xFEED0000u,0xFEED0001u,gpu_ea+33,0x100,(u32)-32,8,4,2,0x202};
    memset(want,0x5A,sizeof(want));
    for(unsigned i=0;i<4;i++) {
        want[i*2]=expected[33+i*2];
        want[8+i*2]=expected[1+i*2];
    }
    int strided=port_nv0039_copy(&main,0)==PORT_NV_COPY && !memcmp(want,vm_base+main_ea,16);
    PortNv0039 mixed=local;
    mixed.in_off=outside_ea;
    int partial=port_nv0039_copy(&mixed,0)==PORT_NV_GPU && !memcmp(expected,vm_base+scratch,16);
    local.in_off=scratch;local.out_off=gpu_ea;
    int dest=port_nv0039_copy(&local,0)==PORT_NV_GPU;
    int ram_untouched=!memcmp(gpu_before,vm_base+gpu_ea,sizeof(gpu_before));
    s_config=saved;
    s_ea_address_table[0]=saved_page;
    fprintf(stderr,"[compat-check] NV0039 GPU readback local=%d main-strided=%d mixed-rejected=%d destination-rejected=%d RAM-untouched=%d\n",
        contiguous,strided,partial,dest,ram_untouched);
    return contiguous && strided && partial && dest && ram_untouched;
}

/* FIFO-owned colour DMA state. rsx_state's SDK ABI has no colour-location
 * fields, so preserve these registers beside it, just like depth DMA. */
static u32 port_color_dma[4]={0xFEED0000u,0xFEED0000u,0xFEED0000u,0xFEED0000u};
void port_renderer_color_reset(void)
{
    for(unsigned i=0;i<4;i++)port_color_dma[i]=0xFEED0000u;
}
int port_renderer_color_method(u32 method,u32 value)
{
    unsigned index;
    switch(method) {
    case 0x0194:index=0;break;
    case 0x018C:index=1;break;
    case 0x01B4:index=2;break;
    case 0x01B8:index=3;break;
    default:return 0;
    }
    port_color_dma[index]=value;return 1;
}

static u32 port_color_address(u32 dma,u32 offset)
{
    extern u32 cellGcmResolveLocated(int,u32);
    extern u32 cellGcmResolveIO(u32);
    if(dma==0xFEED0000u)return cellGcmResolveLocated(1,offset);
    if(dma==0xFEED0001u) {
        u32 address=cellGcmResolveIO(offset);
        if(address)return address;
    }
    fprintf(stderr,"[color-surface] unsupported/unmapped DMA=%08X offset=%08X\n",dma,offset);abort();
}

/* Native colour slot 0 is A or B; MRT slots 1..3 are guest B/C/D. Capture
 * effective addresses here so later IO mapping/state changes cannot redirect
 * an already recorded attachment. */
static void port_snapshot_color(D3D12DrawRecord* dr,const rsx_state* st)
{
    memset(dr->color,0,sizeof(dr->color));
    if(!st)return;
    unsigned count=st->color_target>=0x1F?4:st->color_target>=0x17?3:st->color_target>=0x13?2:1;
    for(unsigned i=0;i<count;i++) {
        unsigned guest=i?i:st->color_target==2?1:0;
        dr->color[i].address=port_color_address(port_color_dma[guest],st->surface_color_offset[guest]);
        /* Swizzled surfaces have no linear pitch. Their native-view layout is
         * derived from the separately recorded surface format. */
        dr->color[i].pitch=((st->surface_format>>8)&15)==1?st->surface_color_pitch[guest]:0;
    }
}

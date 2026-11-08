/* Extra NV4097 registers missing from the SDK's rsx_state. There is one
 * hardware register bank on the FIFO owner; no vendor struct ABI is changed. */
static struct {
    u32 write_mask,two_sided,back_write,back_func,back_ref,back_read;
    u32 back_fail,back_zfail,back_zpass;
} port_stencil;

void port_renderer_stencil_reset(void)
{
    memset(&port_stencil,0,sizeof(port_stencil));
    port_stencil.write_mask=port_stencil.back_write=port_stencil.back_read=255;
    port_stencil.back_func=0x207;
    port_stencil.back_fail=port_stencil.back_zfail=port_stencil.back_zpass=0x1E00;
}
int port_renderer_stencil_method(u32 method,u32 value)
{
    switch(method) {
    case 0x032C:port_stencil.write_mask=value;break;
    case 0x0348:port_stencil.two_sided=value&1;break;
    case 0x034C:port_stencil.back_write=value;break;
    case 0x0350:port_stencil.back_func=value;break;
    case 0x0354:port_stencil.back_ref=value;break;
    case 0x0358:port_stencil.back_read=value;break;
    case 0x035C:port_stencil.back_fail=value;break;
    case 0x0360:port_stencil.back_zfail=value;break;
    case 0x0364:port_stencil.back_zpass=value;break;
    default:return 0;
    }
    return 1;
}
static int port_stencil_equal(const PortStencilKey* a,const PortStencilKey* b)
{
    return a->enabled==b->enabled && a->masks==b->masks && a->front==b->front && a->back==b->back;
}
static int port_stencil_op(u32 op)
{
    switch(op) {
    case 0x1E00:return D3D12_STENCIL_OP_KEEP;
    case 0:return D3D12_STENCIL_OP_ZERO;
    case 0x1E01:return D3D12_STENCIL_OP_REPLACE;
    case 0x1E02:return D3D12_STENCIL_OP_INCR_SAT;
    case 0x1E03:return D3D12_STENCIL_OP_DECR_SAT;
    case 0x150A:return D3D12_STENCIL_OP_INVERT;
    case 0x8507:return D3D12_STENCIL_OP_INCR;
    case 0x8508:return D3D12_STENCIL_OP_DECR;
    default:return 0;
    }
}
static int port_stencil_face(u32 func,u32 fail,u32 zfail,u32 pass,u32* packed)
{
    int a=port_stencil_op(fail),b=port_stencil_op(zfail),c=port_stencil_op(pass);
    if(func<0x200 || func>0x207 || !a || !b || !c) return 0;
    *packed=(func-0x200)|((u32)(a-1)<<3)|((u32)(b-1)<<6)|((u32)(c-1)<<9);return 1;
}
static int port_pack_stencil(const rsx_state* st,PortStencilKey* key,u32* reference)
{
    memset(key,0,sizeof(*key));*reference=0;
    if(!st || !st->stencil_test_enable) return 1;
    key->enabled=1;*reference=st->stencil_ref&255;
    key->masks=(st->stencil_mask&255)|((port_stencil.write_mask&255)<<8);
    if(!port_stencil_face(st->stencil_func,st->stencil_op_fail,st->stencil_op_zfail,st->stencil_op_zpass,&key->front))return 0;
    key->back=key->front;
    if(port_stencil.two_sided) {
        /* The baseline D3D12 PSO has shared masks and a shared dynamic ref.
         * Do not silently apply front-face settings to asymmetric requests. */
        if((port_stencil.back_write&255)!=(port_stencil.write_mask&255) ||
           (port_stencil.back_read&255)!=(st->stencil_mask&255) ||
           (port_stencil.back_ref&255)!=(st->stencil_ref&255))return 0;
        if(!port_stencil_face(port_stencil.back_func,port_stencil.back_fail,port_stencil.back_zfail,port_stencil.back_zpass,&key->back))return 0;
    }
    return 1;
}
static void port_snapshot_stencil(D3D12DrawRecord* dr,const rsx_state* st)
{
    if(!port_pack_stencil(st,&dr->stencil_key,&dr->stencil_ref) ||
       (dr->stencil_key.enabled && dr->depth_surface.format!=2)) {
        fprintf(stderr,"[stencil-blocker] func=%X ref=%X read=%X write=%X two-sided=%u back=%X/%X/%X dsv=%u\n",
            st?st->stencil_func:0,st?st->stencil_ref:0,st?st->stencil_mask:0,
            port_stencil.write_mask,port_stencil.two_sided,port_stencil.back_ref,
            port_stencil.back_read,port_stencil.back_write,dr->depth_surface.format);abort();
    }
}
static D3D12_DEPTH_STENCILOP_DESC port_stencil_face_desc(u32 packed)
{
    D3D12_DEPTH_STENCILOP_DESC d;
    d.StencilFunc=(D3D12_COMPARISON_FUNC)((packed&7)+1);
    d.StencilFailOp=(D3D12_STENCIL_OP)(((packed>>3)&7)+1);
    d.StencilDepthFailOp=(D3D12_STENCIL_OP)(((packed>>6)&7)+1);
    d.StencilPassOp=(D3D12_STENCIL_OP)(((packed>>9)&7)+1);return d;
}
static void port_apply_stencil(D3D12_DEPTH_STENCIL_DESC* desc,const PortStencilKey* key)
{
    desc->StencilEnable=key->enabled?TRUE:FALSE;
    desc->StencilReadMask=(UINT8)(key->masks&255);desc->StencilWriteMask=(UINT8)((key->masks>>8)&255);
    desc->FrontFace=port_stencil_face_desc(key->front);desc->BackFace=port_stencil_face_desc(key->back);
}

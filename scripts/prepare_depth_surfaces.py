"""Record native depth attachments independently of colour targets."""


def prepare(text):
    def once(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise ValueError('Pinned depth-surface anchor changed: ' + old[:100])
        text = text.replace(old, new)

    once('typedef struct {\n    u32 vb_byte_offset;', '''typedef struct {
    u32 address, dma, pitch, width, height, format;
} PortDepthKey;

typedef struct {
    PortDepthKey depth_surface;
    int depth_required;
    u32 vb_byte_offset;''')
    helper = r'''
/* Attachment identity is captured on the FIFO owner, never inferred from the
 * currently bound colour target or from later guest state. */
static u32 port_depth_dma=0xFEED0000u;
void port_renderer_depth_dma(u32 handle) { port_depth_dma=handle; }

static int port_depth_equal(const PortDepthKey* a,const PortDepthKey* b)
{
    return a->address==b->address && a->dma==b->dma && a->pitch==b->pitch &&
        a->width==b->width && a->height==b->height && a->format==b->format;
}

static DXGI_FORMAT port_depth_format(u32 format)
{
    return format==1 ? DXGI_FORMAT_D16_UNORM : format==2 ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
}

static UINT port_depth_clear_flags(const D3D12DrawRecord* dr)
{
    if(!dr->is_clear) return 0;
    return ((dr->clear_flags&1) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
        ((dr->clear_flags&2) && dr->depth_surface.format==2 ? D3D12_CLEAR_FLAG_STENCIL : 0);
}

static void port_snapshot_depth(D3D12DrawRecord* dr,const rsx_state* st)
{
    memset(&dr->depth_surface,0,sizeof(dr->depth_surface));dr->depth_required=0;
    if(!st) return;
    PortDepthKey* k=&dr->depth_surface;
    k->format=(st->surface_format>>5)&7;
    dr->depth_required=dr->is_clear ? port_depth_clear_flags(dr)!=0 :
        st->depth_test_enable || st->stencil_test_enable;
    /* A pipeline with no DSV must specify UNKNOWN, even with depth disabled.
     * DSV format is part of the existing complete PSO depth-state key. */
    dr->depth_key=(dr->depth_key&0xFFFFu)|(dr->depth_required ? k->format<<16 : 0);
    if(!dr->depth_required) return;
    if((k->format!=1 && k->format!=2) || ((st->surface_format>>12)&15) ||
       (port_depth_dma!=0xFEED0000u && port_depth_dma!=0xFEED0001u)) {
        fprintf(stderr,"[depth] unsupported attachment format=%08X dma=%08X\n",st->surface_format,port_depth_dma);abort();
    }
    extern u32 cellGcmResolveLocated(int,u32);
    extern u32 cellGcmResolveIO(u32);
    k->dma=port_depth_dma;
    k->address=k->dma==0xFEED0000u ? cellGcmResolveLocated(1,st->surface_zeta_offset) :
        cellGcmResolveIO(st->surface_zeta_offset);
    if(k->dma==0xFEED0001u && !k->address) {
        fprintf(stderr,"[depth] unmapped main-memory attachment\n");abort();
    }
    k->pitch=st->surface_zeta_pitch;
    k->width=st->surface_clip_x+st->surface_clip_w;
    k->height=st->surface_clip_y+st->surface_clip_h;
    u32 bpp=k->format==1 ? 2 : 4;
    /* Linear pitch may cover a wider allocation than the current clip. */
    if(k->pitch/bpp>k->width) k->width=k->pitch/bpp;
    if(k->address==0xFFFFFFFFu || !k->width || !k->height || k->width>16384 || k->height>16384) {
        fprintf(stderr,"[depth] invalid attachment layout/address\n");abort();
    }
}

typedef struct {
    PortDepthKey key;
    ID3D12Resource* resource;
    ID3D12DescriptorHeap* heap;
    D3D12_CPU_DESCRIPTOR_HANDLE view;
} PortDepthSurface;
static PortDepthSurface* port_depth_surfaces;
static unsigned port_depth_count,port_depth_capacity;

static int port_find_depth(const PortDepthSurface* surfaces,unsigned count,const PortDepthKey* key)
{
    for(unsigned i=0;i<count;i++) if(port_depth_equal(&surfaces[i].key,key)) return (int)i;
    return -1;
}

static D3D12_CPU_DESCRIPTOR_HANDLE port_depth_view(const D3D12DrawRecord* dr)
{
    D3D12_CPU_DESCRIPTOR_HANDLE none={0};
    if(!dr->depth_required) return none;
    int found=port_find_depth(port_depth_surfaces,port_depth_count,&dr->depth_surface);
    if(found>=0) return port_depth_surfaces[found].view;
    if(port_depth_count==port_depth_capacity) {
        unsigned count=port_depth_capacity ? port_depth_capacity*2 : 8;
        if(count<=port_depth_capacity || (size_t)count>SIZE_MAX/sizeof(*port_depth_surfaces)) abort();
        PortDepthSurface* grown=(PortDepthSurface*)realloc(port_depth_surfaces,(size_t)count*sizeof(*grown));
        if(!grown) abort();
        port_depth_surfaces=grown;port_depth_capacity=count;
    }
    PortDepthSurface* s=&port_depth_surfaces[port_depth_count];memset(s,0,sizeof(*s));
    s->key=dr->depth_surface;
    D3D12_DESCRIPTOR_HEAP_DESC hd={0};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
    HRESULT hr=s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device,&hd,&IID_ID3D12DescriptorHeap,(void**)&s->heap);
    if(FAILED(hr)) { fprintf(stderr,"[depth] DSV allocation failed %08lX\n",(unsigned long)hr);abort(); }
    D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd={0};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width=s->key.width;rd.Height=s->key.height;rd.DepthOrArraySize=1;rd.MipLevels=1;
    rd.Format=s->key.format==1 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    rd.SampleDesc.Count=1;
    rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    /* Native fast-clear hint only; resource contents still change solely when
     * a recorded guest clear or draw actually executes. */
    D3D12_CLEAR_VALUE cv={0};cv.Format=port_depth_format(s->key.format);
    cv.DepthStencil.Depth=dr->is_clear && (dr->clear_flags&1) ? dr->clear_depth : 1.0f;
    cv.DepthStencil.Stencil=dr->is_clear && (dr->clear_flags&2) ? dr->clear_stencil : 0;
    hr=s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,
        &rd,D3D12_RESOURCE_STATE_DEPTH_WRITE,&cv,&IID_ID3D12Resource,(void**)&s->resource);
    if(FAILED(hr)) { fprintf(stderr,"[depth] resource allocation failed %08lX\n",(unsigned long)hr);abort(); }
    D3D12_DEPTH_STENCIL_VIEW_DESC vd={0};vd.Format=port_depth_format(s->key.format);vd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    s->heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s->heap,&s->view);
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device,s->resource,&vd,s->view);
    port_depth_count++;
    if(getenv("D3D12_DBG")) fprintf(stderr,"[depth] surface=%u address=%08X dma=%08X %ux%u format=%u\n",
        port_depth_count,s->key.address,s->key.dma,s->key.width,s->key.height,s->key.format);
    return s->view;
}

/* Called only after the existing shutdown GPU fence. Each view has its own
 * non-shader-visible heap, so growing CPU metadata never invalidates handles. */
static void port_release_depth_surfaces(void)
{
    for(unsigned i=0;i<port_depth_count;i++) {
        PortDepthSurface* s=&port_depth_surfaces[i];
        if(s->resource) s->resource->lpVtbl->Release(s->resource);
        if(s->heap) s->heap->lpVtbl->Release(s->heap);
    }
    free(port_depth_surfaces);port_depth_surfaces=NULL;port_depth_count=port_depth_capacity=0;
}

int port_check_depth_surfaces(void)
{
    rsx_state st={0};D3D12DrawRecord a={0},b={0},again={0};
    u32 saved_dma=port_depth_dma;
    int dma=rsx_process_method(&st,NV4097_SET_CONTEXT_DMA_ZETA,0xFEED0001u)==0 && port_depth_dma==0xFEED0001u;
    dma=dma && rsx_process_method(&st,NV4097_SET_CONTEXT_DMA_ZETA,0xFEED0000u)==0 && port_depth_dma==0xFEED0000u;
    st.surface_format=2<<5;st.surface_clip_w=1280;st.surface_clip_h=720;
    st.surface_zeta_pitch=5120;st.surface_zeta_offset=0x10000;st.depth_test_enable=1;
    port_snapshot_depth(&a,&st);
    st.surface_clip_w=512;st.surface_clip_h=512;st.surface_zeta_pitch=2048;st.surface_zeta_offset=0x20000;
    port_snapshot_depth(&b,&st);
    PortDepthSurface slots[2]={0};slots[0].key=a.depth_surface;slots[1].key=b.depth_surface;
    int separate=port_find_depth(slots,2,&a.depth_surface)==0 && port_find_depth(slots,2,&b.depth_surface)==1;
    st.surface_clip_w=1280;st.surface_clip_h=720;st.surface_zeta_pitch=5120;st.surface_zeta_offset=0x10000;
    st.surface_color_offset[0]=0x30000;port_snapshot_depth(&again,&st);
    int retained=port_find_depth(slots,2,&again.depth_surface)==0 && port_depth_equal(&a.depth_surface,&again.depth_surface);
    int snapshot=a.depth_surface.width==1280 && a.depth_surface.height==720 && b.depth_surface.width==512;
    PortDepthKey changed=a.depth_surface;changed.dma^=1;
    int location=port_find_depth(slots,2,&changed)==-1;
    changed=a.depth_surface;changed.address++;
    int address=port_find_depth(slots,2,&changed)==-1;
    changed=a.depth_surface;changed.pitch+=4;
    int layout=port_find_depth(slots,2,&changed)==-1;
    changed=a.depth_surface;changed.format=1;
    int format=port_find_depth(slots,2,&changed)==-1;
    a.is_clear=1;unsigned masks=0;
    for(unsigned flags=0;flags<256;flags++) {
        a.clear_flags=flags;
        UINT expected=((flags&1)?D3D12_CLEAR_FLAG_DEPTH:0)|((flags&2)?D3D12_CLEAR_FLAG_STENCIL:0);
        if(port_depth_clear_flags(&a)==expected) masks++;
    }
    a.depth_surface.format=1;a.clear_flags=2;
    int d16=port_depth_format(1)==DXGI_FORMAT_D16_UNORM && !port_depth_clear_flags(&a);
    a.is_clear=0;a.clear_flags=3;
    int draw_no_clear=!port_depth_clear_flags(&a);
    st.depth_test_enable=0;st.stencil_test_enable=0;port_snapshot_depth(&again,&st);
    int no_depth=!again.depth_required && !port_depth_view(&again).ptr &&
        port_depth_format(again.depth_key>>16)==DXGI_FORMAT_UNKNOWN;
    port_depth_dma=saved_dma;
    int ok=dma&&separate&&retained&&snapshot&&location&&address&&layout&&format&&masks==256&&d16&&draw_no_clear&&no_depth;
    fprintf(stderr,"[compat-check] depth attachments dma=%d separate=%d retained=%d snapshot=%d location=%d address=%d layout=%d format=%d masks=%u d16=%d draw-no-clear=%d no-depth=%d %s\n",
        dma,separate,retained,snapshot,location,address,layout,format,masks,d16,draw_no_clear,no_depth,ok?"PASS":"FAIL");
    return ok?0:1;
}

'''
    from pathlib import Path
    gpu_check = (Path(__file__).resolve().parents[1] / 'port/checks/depth_surfaces_gpu.inc').read_text(encoding='utf-8')
    once('static void port_snapshot_textures(', helper + gpu_check + '\nstatic void port_snapshot_textures(')
    once('static void port_snapshot_textures(D3D12DrawRecord* dr)\n{',
         'static void port_snapshot_textures(D3D12DrawRecord* dr)\n{\n    port_snapshot_depth(dr,s_d3d.current_rsx_state);')
    once('    r->clear_flags=flags;r->clear_depth=depth;r->clear_stencil=stencil;',
         '    r->clear_flags=flags;r->clear_depth=depth;r->clear_stencil=stencil;\n    port_snapshot_depth(r,s_d3d.current_rsx_state);')
    # The format must distinguish PSOs even when depth testing is disabled.
    start = text.index('static ID3D12PipelineState* vp_get_fp_pso(')
    before, rest = text[:start], text[start:]
    old = '    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;'
    if rest.count(old) != 1:
        raise ValueError('Pinned guest PSO DSV format changed')
    text = before + rest.replace(old, '    pd.DSVFormat = port_depth_format(depth_key>>16);')
    once('''                /* Render-to-texture: retarget when this op's surfaces differ.
                 * Depth is a single shared buffer, so clear it per switch. */''', '''                /* Colour and depth attachments have independent identities. */
                D3D12_CPU_DESCRIPTOR_HANDLE want_dsv=port_depth_view(dr);
                int depth_changed=want_dsv.ptr!=dsv_handle.ptr;
                dsv_handle=want_dsv;''')
    once('wantm[1] != cur_m[1] || wantm[2] != cur_m[2]) {',
         'wantm[1] != cur_m[1] || wantm[2] != cur_m[2] || depth_changed) {')
    once('''                    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, dsv_handle,
                        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);''', '')
    once('''                    UINT clear_depth_flags=0;
                    if(dr->clear_flags&1) clear_depth_flags|=D3D12_CLEAR_FLAG_DEPTH;
                    if(dr->clear_flags&2) clear_depth_flags|=D3D12_CLEAR_FLAG_STENCIL;''',
         '                    UINT clear_depth_flags=port_depth_clear_flags(dr);')
    text = text.replace('FALSE, &dsv_handle)', 'FALSE, dsv_handle.ptr ? &dsv_handle : NULL)')
    once('    /* Release D3D12 resources */',
         '    port_release_depth_surfaces();\n    /* Release D3D12 resources */')
    return text


def prepare_commands(sdk, output):
    from compat_source import resolve_local_includes, write_if_changed
    source = sdk / 'libs/video/rsx_commands.c'
    text = source.read_text(encoding='utf-8')
    anchor = 'int rsx_process_method(rsx_state* state, u32 method, u32 data)\n{'
    if text.count(anchor) != 1:
        raise ValueError('Pinned RSX method entry changed')
    text = text.replace(anchor, anchor + '\n    if(method==NV4097_SET_CONTEXT_DMA_ZETA) { port_renderer_depth_dma(data);return 0; }')
    anchor = 'void rsx_state_init(rsx_state* state)\n{'
    if text.count(anchor) != 1:
        raise ValueError('Pinned RSX state initialization changed')
    text = text.replace(anchor, anchor + '\n    port_renderer_depth_dma(0xFEED0000u);')
    text = 'void port_renderer_depth_dma(unsigned int);\n' + text
    write_if_changed(output / 'rsx_commands_compat.c', resolve_local_includes(text, source))

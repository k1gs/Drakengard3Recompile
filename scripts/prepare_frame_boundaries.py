"""Keep guest clears ordered and present completed FIFO frames."""


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned frame-boundary anchor changed: ' + old[:80])
        text = text.replace(old, new)

    once('    float cc[4];\n} D3D12DrawRecord;',
         '    float cc[4];\n    u32 clear_flags;\n    float clear_depth;\n'
         '    u8 clear_stencil;\n} D3D12DrawRecord;')
    once('static void render_frame(void)', r'''
static int s_port_color_pending;
static int port_batch_has_color(void)
{
    for(u32 i=0;i<s_d3d.draw_count;i++) {
        const D3D12DrawRecord* r=&s_d3d.draws[i];
        if(!r->rt_off && ((r->is_clear && (r->clear_flags&0xF0u)) ||
            (!r->is_clear && (!r->is_vp || r->cmask)))) return 1;
    }
    return 0;
}
static void render_frame(void)''')
    once('    port_note_movie_draws();\n    double _rf0',
         '    if(port_batch_has_color()) s_port_color_pending=1;\n'
         '    port_note_movie_draws();\n    double _rf0')
    once('    if (s_composite_src) has_display = 1;',
         '    if (s_composite_src || s_port_color_pending || port_batch_has_color()) has_display = 1;')
    for name in ('flip_has_display','has_display'):
        once('    for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++)\n'
             '        if (!s_d3d.draws[_i].is_clear && s_d3d.draws[_i].rt_off == 0) {\n'
             '            '+name+' = 1;\n            break;\n        }',
             '    if(port_batch_has_color() || s_port_color_pending) '+name+'=1;')
    once('    s_d3d.frame_count++;', '    if(s_present_this_frame) s_d3d.frame_count++;')
    once('        s_tframes++;', '        if(s_present_this_frame) s_tframes++;')
    once('        if (++s_n % 20 == 0) {', '        if (s_present_this_frame && ++s_n % 20 == 0) {')
    once('''    if (s_present_this_frame)
        s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 1, 0);''', '''    if (s_present_this_frame) {
        s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, 1, 0);
        s_port_color_pending=0;
    }''')
    # Count the requested interval in presented frames. Fence submissions may
    # render many partial batches between flips and must not consume the delay.
    start = text.index('    /* CELLMARK_DUMP_MINDRAWS=<N>:')
    end = text.index('    guest_fb_present(fi);', start)
    if 'skip_dump_consider' not in text[start:end]:
        raise ValueError('Pinned readback interval changed')
    text = text[:start] + '''    int port_dump_due=s_present_this_frame && s_d3d.dump_skip_left==0;
    if(s_present_this_frame && s_d3d.dump_skip_left>0 && s_d3d.dump_frames_left>0)
        s_d3d.dump_skip_left--;
''' + text[end:]
    once('&& s_d3d.dump_skip_left == 0);', '&& port_dump_due);')
    # Include every actual present, even an empty/black one: filtering by
    # pending color work would bias the flicker diagnostic toward good frames.
    once('    if (dumping) {\n        static int s_every = -1;',
         '    dumping=dumping && s_present_this_frame;\n'
         '    if (dumping) {\n        static int s_every = -1;')
    once('        if (s_every > 1) s_d3d.dump_skip_left = s_every - 1;', r'''        int interval=s_every;
        const char* interval_path=getenv("PORT_CAPTURE_INTERVAL");
        if(interval_path) {
            FILE* f=fopen(interval_path,"r");int requested=0;
            if(f) { if(fscanf(f,"%d",&requested)==1 && requested>=1 && requested<=3600) interval=requested;fclose(f); }
        }
        if (interval > 1) s_d3d.dump_skip_left = interval - 1;''')
    start = text.index('static void d3d12_clear(void* ud,')
    end = text.index('\n}\n', start) + 3
    if 'have_display_draws' not in text[start:end] or 's_clear_presents++' not in text[start:end]:
        raise ValueError('Pinned clear-boundary implementation changed')
    text = text[:start] + r'''static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    flags &= 0xF3u;
    if(!flags) return;
    if((flags&0xF0u) && (flags&0xF0u)!=0xF0u) {
        fprintf(stderr,"[clear] unsupported partial color mask %02X\n",flags);abort();
    }
    extern void port_renderer_submit_pending(void);
    if(s_d3d.draw_count>=MAX_DRAWS) port_renderer_submit_pending();
    if(s_d3d.draw_count>=MAX_DRAWS) {
        fprintf(stderr,"[clear] ordered command capacity exhausted\n");abort();
    }
    u32 w=0,h=0,mrt[3]={0};
    u32 rt=current_rt_off(&w,&h,mrt);
    D3D12DrawRecord* r=&s_d3d.draws[s_d3d.draw_count++];
    memset(r,0,sizeof(*r));
    r->is_vp=1;r->is_clear=1;r->tex_slot=-1;
    r->rt_off=rt;r->rt_w=w;r->rt_h=h;memcpy(r->rt_mrt,mrt,sizeof(mrt));
    r->rt_fmt=s_d3d.current_rsx_state?s_d3d.current_rsx_state->surface_format:0;
    r->clear_flags=flags;r->clear_depth=depth;r->clear_stencil=stencil;
    r->cc[0]=((color>>16)&255)/255.0f;r->cc[1]=((color>>8)&255)/255.0f;
    r->cc[2]=(color&255)/255.0f;r->cc[3]=((color>>24)&255)/255.0f;
    s_dbg_clears_since_present++;
}
''' + text[end:]
    start = text.index('    /* Clear color and depth */')
    end = text.index('    /* Set viewport and scissor */', start)
    if 'ClearDepthStencilView' not in text[start:end]:
        raise ValueError('Pinned implicit frame clear changed')
    text = text[:start] + '    /* Guest clears are replayed in command order below. */\n\n' + text[end:]
    once('''                if (dr->is_clear) {
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =''', '''                if (dr->is_clear) {
                    UINT clear_depth_flags=0;
                    if(dr->clear_flags&1) clear_depth_flags|=D3D12_CLEAR_FLAG_DEPTH;
                    if(dr->clear_flags&2) clear_depth_flags|=D3D12_CLEAR_FLAG_STENCIL;
                    if(clear_depth_flags)
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list,dsv_handle,
                            clear_depth_flags,dr->clear_depth,dr->clear_stencil,0,NULL);
                    if(!(dr->clear_flags&0xF0u)) continue;
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =''')
    start = text.index('    /* Once frame-boundary presents are active')
    end = text.index('    /* Same display gate as d3d12_present:', start)
    if 's_clear_presents > 0' not in text[start:end]:
        raise ValueError('Pinned ticker suppression changed')
    text = text[:start] + '    /* The FIFO owner presents after an actual flip boundary. */\n' + text[end:]
    text += r'''
/* Execute real pending GPU work before publishing a FIFO fence. Never present
 * an incomplete frame. Its color contents stay available for the final flip. */
void port_renderer_submit_pending(void)
{
    if(!s_d3d.initialized || !s_d3d.draw_count) return;
    s_present_this_frame=0;
    render_frame();
    s_present_this_frame=1;
    wait_for_gpu();
}
'''
    text += r'''
/* CPU contract: clears preserve prior draws and carry their actual mask/value. */
int port_check_present_boundary(void)
{
    if(s_d3d.initialized || s_d3d.draw_count) return 2;
    s_d3d.draw_count=1;memset(s_d3d.draws,0,sizeof(s_d3d.draws));
    s_d3d.draws[0].vertex_count=135;
    d3d12_clear(NULL,3,0xFFFFFFFF,0.25f,7);
    int depth=s_d3d.draw_count==2 && s_d3d.draws[0].vertex_count==135 &&
        s_d3d.draws[1].clear_flags==3 && s_d3d.draws[1].clear_depth==0.25f &&
        s_d3d.draws[1].clear_stencil==7;
    d3d12_clear(NULL,0xF0,0x11223344,1,0);
    int color=s_d3d.draw_count==3 && s_d3d.draws[2].clear_flags==0xF0 &&
        s_d3d.draws[2].cc[0]==0x22/255.0f && s_d3d.draws[2].cc[3]==0x11/255.0f;
    d3d12_clear(NULL,0,0,0,0);
    int noop=s_d3d.draw_count==3;
    s_d3d.draw_count=1;memset(s_d3d.draws,0,sizeof(s_d3d.draws));
    s_d3d.draws[0].is_clear=1;s_d3d.draws[0].clear_flags=3;
    int depth_hidden=!port_batch_has_color();
    s_d3d.draws[0].clear_flags=0xF0;
    int color_visible=port_batch_has_color();
    int ok=depth&&color&&noop&&depth_hidden&&color_visible;
    fprintf(stderr,"[compat-check] ordered clears: prior-draw-held=%d depth/stencil=%d color=%d zero-mask-noop=%d %s\n",
        depth,depth,color,noop,ok?"PASS":"FAIL");
    memset(s_d3d.draws,0,sizeof(s_d3d.draws));s_d3d.draw_count=0;
    return ok?0:1;
}
'''
    return text

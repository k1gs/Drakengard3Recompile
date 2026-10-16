"""Build-local renderer fixes, capture diagnostics and CPU contract fixtures."""
import re


def prepare(sdk, output):
    source=sdk/'libs/video/rsx_d3d12_backend.c'
    text=source.read_text(encoding='utf-8')
    start=text.index('    /* Garbage-projection fallback (vkcube; see the original comment). */')
    end=text.index('\n}\n',start)
    old=text[start:end]
    if 'c[0]=1.358f' not in old or '!(c00 > 0.0f && c00 < 8.0f)' not in old:
        raise ValueError('Pinned projection replacement changed')
    text=text[:start]+'    /* Upload the guest constants exactly. Their sign or magnitude does not\n     * identify a projection matrix, and valid camera rows may start negative. */\n'+text[end:]
    def replace(old,new,count=1):
        nonlocal text
        if text.count(old)!=count:
            raise ValueError('Pinned renderer anchor changed: '+old[:100])
        text=text.replace(old,new)
    # A 16-KiB decode window can carry at most 512 CONST instructions, each
    # consuming 16 instruction bytes and 16 payload bytes. b1 also has five
    # metadata rows; round (512+5)*16 to the D3D12 256-byte CBV alignment.
    replace('#define VP_FPCB_STRIDE    1280','#define VP_FPCB_STRIDE    8448')
    replace('VPVSEntry             vp_vs[VP_VS_CACHE];',
            'VPVSEntry*            vp_vs;\n    int                   vp_vs_capacity;')
    # Slots referenced by pending draws are immutable until those draws have
    # been submitted. Grow only when all existing slots are pinned; after the
    # batch resets, slots can be reused without moving any other entry.
    cache_helper=r'''
static int vp_reserve_vs_slot(void)
{
    if (s_d3d.vp_vs_n < s_d3d.vp_vs_capacity) return s_d3d.vp_vs_n++;
    for (int slot=0;slot<s_d3d.vp_vs_n;slot++) {
        int pinned=0;
        for (u32 d=0;d<s_d3d.draw_count && d<MAX_DRAWS;d++) {
            const D3D12DrawRecord* dr=&s_d3d.draws[d];
            if(dr->is_vp && !dr->is_clear && dr->vs_idx==slot) { pinned=1;break; }
        }
        if (!pinned) {
            if(s_d3d.vp_vs[slot].vs) s_d3d.vp_vs[slot].vs->lpVtbl->Release(s_d3d.vp_vs[slot].vs);
            memset(&s_d3d.vp_vs[slot],0,sizeof(VPVSEntry));
            return slot;
        }
    }
    int capacity=s_d3d.vp_vs_capacity+VP_VS_CACHE;
    VPVSEntry* entries=(VPVSEntry*)realloc(s_d3d.vp_vs,sizeof(VPVSEntry)*(size_t)capacity);
    if(!entries) { fprintf(stderr,"[VP] shader cache allocation failed\n");abort(); }
    memset(entries+s_d3d.vp_vs_capacity,0,sizeof(VPVSEntry)*(size_t)(capacity-s_d3d.vp_vs_capacity));
    s_d3d.vp_vs=entries;s_d3d.vp_vs_capacity=capacity;
    return s_d3d.vp_vs_n++;
}

'''
    replace('static int vp_get_vs(const rsx_state* st)',cache_helper+'static int vp_get_vs(const rsx_state* st)')
    replace('''    int slot;
    if (s_d3d.vp_vs_n < VP_VS_CACHE) slot = s_d3d.vp_vs_n++;
    else {  /* evict slot 0 */
        if (s_d3d.vp_vs[0].vs) s_d3d.vp_vs[0].vs->lpVtbl->Release(s_d3d.vp_vs[0].vs);
        memmove(&s_d3d.vp_vs[0], &s_d3d.vp_vs[1], sizeof(VPVSEntry)*(VP_VS_CACHE-1));
        slot = VP_VS_CACHE - 1;
    }''','    int slot = vp_reserve_vs_slot();')
    # Snapshot identity at record time so captures can detect stale bindings.
    replace('    int vs_idx;         /* VPVSEntry slot',
            '    u32 vs_hash;\n    int vs_idx;         /* VPVSEntry slot')
    replace('dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);',
            'dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);\n            dr->vs_hash = dr->vs_idx>=0 ? s_d3d.vp_vs[dr->vs_idx].hash : 0;',3)
    fp_helper=r'''
#define PORT_FP_PROGRAM_BYTES 16384u
#define PORT_FP_CONSTANT_CAPACITY (PORT_FP_PROGRAM_BYTES/32u)
static int port_collect_guest_fp(const u8* ucode,u32 bytes,float* out)
{
    rsx_fp_constant_block block;
    int count=rsx_fp_collect_constants(ucode,bytes,&block);
    if(count<0 || (u32)count>PORT_FP_CONSTANT_CAPACITY) {
        extern uint8_t* vm_base;
        uintptr_t off=(uintptr_t)ucode-(uintptr_t)vm_base;
        u32 extended=bytes;
        if(off>=0xC0000000u && off<0xCFFF0000u) extended=16384;
        fprintf(stderr,"[FP] malformed/oversized constant bank count=%d vmoff=%08llX limit=%u extended_size=%u\n",
            count,(unsigned long long)off,bytes,rsx_fp_program_size(ucode,extended));
        const char* dir=getenv("RSX_CAP_DIR");
        if(dir) {
            char path[512];CreateDirectoryA(dir,NULL);
            snprintf(path,sizeof(path),"%s/invalid_fp_%08llX.ucode",dir,(unsigned long long)off);
            FILE* f=fopen(path,"wb");if(f) { fwrite(ucode,1,extended,f);fclose(f); }
        }
        abort();
    }
    memset(out,0,PORT_FP_CONSTANT_CAPACITY*16);
    memcpy(out,block.values,(size_t)count*16);
    return count;
}
static int port_decompile_guest_fp(const u8* ucode,u32 bytes,int exp32,u32 cube_mask,char* out,u32 size)
{
    u32 count=0;
    int n=rsx_fp_decompile_buffered_ex(ucode,bytes,exp32?0x40u:0u,cube_mask,out,size,&count);
    if(n<=0) return n;
    if(count>PORT_FP_CONSTANT_CAPACITY) { fprintf(stderr,"[FP] constant bank overflow: %u slots\n",count);abort(); }
    /* Keep the existing per-draw b1 allocation: four texture-scale rows,
     * alpha-test row, then 512 exact inline-constant rows. The modern SDK
     * decompiler otherwise puts constants first in a variable-size bank. */
    char* begin=strstr(out,"cbuffer PSConstants : register(b1) {");
    char* end=begin?strstr(begin,"};\n"):NULL;
    if(!end) { fprintf(stderr,"[FP] buffered cbuffer declaration missing\n");abort(); }
    end+=3;
    const char layout[]="cbuffer PSConstants : register(b1) {\n"
        "    float4 rsx_texscale[4];\n    float4 rsx_alphatest;\n    float4 fp_constants[512];\n};\n";
    size_t tail=strlen(end)+1;
    if((size_t)(begin-out)+sizeof(layout)-1+tail>size) return -1;
    memmove(begin+sizeof(layout)-1,end,tail);memcpy(begin,layout,sizeof(layout)-1);
    return n;
}

'''
    replace('static ID3D12PipelineState* vp_get_fp_pso(',fp_helper+'static ID3D12PipelineState* vp_get_fp_pso(')
    replace('rsx_fp_extract_consts(vm_base + foff, 4096, &ts[20], 64);',
            'port_collect_guest_fp(vm_base + foff, 4096, &ts[20]);')
    replace('int n = rsx_fp_decompile(vm_base + off, 4096, RSX_FP_CTRL_AUTO, hlsl, sizeof(hlsl));',
            'int n = port_decompile_guest_fp(vm_base + off, 4096, exp32, cube_mask, hlsl, sizeof(hlsl));')
    replace('    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "guest_fp", NULL, NULL,',r'''    static unsigned compile_seq=0;
    unsigned seq=++compile_seq;
    int trace=getenv("PORT_RENDER_TRACE") && seq<=128;
    ULONGLONG compile_started=GetTickCount64();
    if(trace) fprintf(stderr,"[RENDER] FP compile begin seq=%u fp=%08X vs=%08X\n",seq,fp_addr,vs_hash);
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "guest_fp", NULL, NULL,''')
    replace('    int n = port_decompile_guest_fp(vm_base + off, 4096, exp32, cube_mask, hlsl, sizeof(hlsl));',r'''    int n = port_decompile_guest_fp(vm_base + off, 4096, exp32, cube_mask, hlsl, sizeof(hlsl));
    const char* fp_capdir=getenv("RSX_CAP_DIR");
    if(fp_capdir && n>0) {
        char path[512];CreateDirectoryA(fp_capdir,NULL);
        snprintf(path,sizeof(path),"%s/fs_%08X_%08X.hlsl",fp_capdir,fp_addr,uhash);
        FILE* f=fopen(path,"w");
        if(f) { fprintf(f,"/* fp=%08X code_hash=%08X vmoff=%08X instrs=%d */\n%s",fp_addr,uhash,off,n,hlsl);fclose(f); }
    }''')
    replace('"main", "ps_5_0", 0, 0, &pb, &e);\n    if (FAILED(hr) || !pb)',
            '"main", "ps_5_0", 0, 0, &pb, &e);\n    if(trace) fprintf(stderr,"[RENDER] FP compile end seq=%u ms=%llu hr=%08lX\\n",seq,GetTickCount64()-compile_started,(unsigned long)hr);\n    if (FAILED(hr) || !pb)')
    replace('    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(\n        s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);',
            '    if(trace) fprintf(stderr,"[RENDER] PSO begin seq=%u\\n",seq);\n    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(\n        s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);\n    if(trace) fprintf(stderr,"[RENDER] PSO end seq=%u ms=%llu hr=%08lX\\n",seq,GetTickCount64()-compile_started,(unsigned long)hr);')
    # Depth is draw state: the renderer batches multiple guest passes before
    # submission, so reading the final rsx_state when building a PSO is too late.
    replace('    u32 cull;', '    u32 depth_key;\n    u32 cull;',2)
    replace('static u32 rsx_cull_key(',r'''static u32 rsx_depth_key(const rsx_state* st)
{
    if (!st) return 0;
    return (st->depth_test_enable ? 1u : 0u) |
           (st->depth_mask ? 2u : 0u) | ((st->depth_func & 7u) << 2);
}

static void guest_depth_state(D3D12_DEPTH_STENCIL_DESC* desc, u32 key)
{
    desc->DepthEnable = (key & 1u) ? TRUE : FALSE;
    desc->DepthWriteMask = (key & 2u) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    /* RSX NEVER..ALWAYS = 0x200..0x207; D3D12 NEVER..ALWAYS = 1..8. */
    desc->DepthFunc = (D3D12_COMPARISON_FUNC)(((key >> 2) & 7u) + 1u);
}

static u32 rsx_cull_key(''')
    replace('u32 cube_mask)\n{','u32 cube_mask, u32 depth_key)\n{')
    replace('s_d3d.vp_fp[i].cull == cull &&',
            's_d3d.vp_fp[i].cull == cull && s_d3d.vp_fp[i].depth_key == depth_key &&')
    replace('    pd.DepthStencilState.DepthEnable = TRUE;\n    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;\n    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;\n    pd.DepthStencilState.StencilEnable = FALSE;\n    /* DEPTH_OFF=1:',
            '    guest_depth_state(&pd.DepthStencilState, depth_key);\n    pd.DepthStencilState.StencilEnable = FALSE;\n    /* DEPTH_OFF=1:')
    replace('s_d3d.vp_fp[s_d3d.vp_fp_n].cube_mask = cube_mask;',
            's_d3d.vp_fp[s_d3d.vp_fp_n].cube_mask = cube_mask;\n    s_d3d.vp_fp[s_d3d.vp_fp_n].depth_key = depth_key;')
    replace('dr_cube_mask(dr))','dr_cube_mask(dr), dr->depth_key)',2)
    replace('dr->cull = rsx_cull_key(s_d3d.current_rsx_state);',
            'dr->depth_key = rsx_depth_key(s_d3d.current_rsx_state);\n        dr->cull = rsx_cull_key(s_d3d.current_rsx_state);',3)
    replace('r->tex[0].raw, r->tex[1].raw, r->tex[2].raw, r->tex[3].raw);',
            'r->tex[0].raw, r->tex[1].raw, r->tex[2].raw, r->tex[3].raw);\n                fprintf(mf, "      depth enable=%u write=%u func=0x%X key=0x%X\\n", r->depth_key&1u, (r->depth_key>>1)&1u, 0x200u+((r->depth_key>>2)&7u),r->depth_key);')
    # Preserve the exact source shader alongside optional captures. This is
    # diagnostic only and runs on cache misses, before any diagnostic rewrite.
    replace('    int ni = rsx_vp_decompile(vuc, vlen, hlsl, sizeof hlsl);',r'''    int ni = rsx_vp_decompile(vuc, vlen, hlsl, sizeof hlsl);
    const char* capdir=getenv("RSX_CAP_DIR");
    if (capdir && ni>0) {
        char path[512];CreateDirectoryA(capdir,NULL);
        snprintf(path,sizeof(path),"%s/vs_%08X.hlsl",capdir,hash);
        FILE* f=fopen(path,"w");
        if(f) { fprintf(f,"/* hash=%08X start=%u instrs=%d */\n%s",hash,vstart/16,ni,hlsl);fclose(f); }
        snprintf(path,sizeof(path),"%s/vs_%08X.ucode",capdir,hash);
        f=fopen(path,"wb");if(f) { fwrite(vuc,16,(size_t)ni,f);fclose(f); }
    }''')
    replace('        /* Per-draw VP constant banks for the first few offscreen-RT geometry',r'''        /* Capture uploaded inputs for a few scene draws. cb_slot, rather
         * than draw index, is authoritative when adjacent batches merge. */
        if (s_d3d.vp_cb_mapped && s_d3d.vp_vb_mapped) {
            u32 par=(u32)(s_d3d.vp_parity^1);unsigned shown=0;
            for(u32 d=0;d<ndraws && d<MAX_DRAWS && shown<4;d++) {
                const D3D12DrawRecord* r=&s_d3d.draws[d];
                if(r->is_clear || !r->is_vp || r->vertex_count<1000 || !r->rt_off) continue;
                const char* cb=(const char*)s_d3d.vp_cb_mapped+((u64)par*MAX_DRAWS+r->cb_slot)*VP_CB_STRIDE;
                const char* vb=(const char*)s_d3d.vp_vb_mapped+(u64)par*MAX_VERTICES*256+r->vb_byte_offset;
                snprintf(path,sizeof(path),"%s/op%03u.cb",dir,d);
                FILE* f=fopen(path,"wb");if(f) { fwrite(cb,1,RSX_MAX_VERTEX_CONSTANTS*16+32,f);fclose(f); }
                if(s_d3d.vp_fpcb_mapped) {
                    const char* fp=(const char*)s_d3d.vp_fpcb_mapped+((u64)par*MAX_DRAWS+r->cb_slot)*VP_FPCB_STRIDE;
                    snprintf(path,sizeof(path),"%s/op%03u.fpcb",dir,d);
                    f=fopen(path,"wb");if(f) { fwrite(fp,1,VP_FPCB_STRIDE,f);fclose(f); }
                }
                snprintf(path,sizeof(path),"%s/op%03u.vb",dir,d);
                f=fopen(path,"wb");if(f) { fwrite(vb,256,r->vertex_count,f);fclose(f); }
                u32 hash=r->vs_idx>=0 && r->vs_idx<s_d3d.vp_vs_n ? s_d3d.vp_vs[r->vs_idx].hash : 0;
                fprintf(mf,"inputs op%03u vs_hash=%08X recorded_hash=%08X cb_slot=%u vb_byte_offset=%u vertices=%u\n",d,hash,r->vs_hash,r->cb_slot,r->vb_byte_offset,r->vertex_count);
                fprintf(mf,"      inputs fp=%08X exp32=%d alpha_ctl=%08X cull=%X\n",r->fp_addr,r->fp_exp32,r->alpha_ctl,r->cull);
                shown++;
            }
        }
        /* Per-draw VP constant banks for the first few offscreen-RT geometry''')
    # Optional capture filter selects geometry batches rather than repeatedly
    # capturing a lone fullscreen composite. It does not change rendering.
    old='if (s_d3d.frame_count >= target && s_dbg_last_draws > 0) {'
    if text.count(old)!=1: raise ValueError('Pinned RSX capture predicate changed')
    text=text.replace(old,'''int eligible = s_dbg_last_draws > 0;
            if (getenv("RSX_CAP_OFFSCREEN")) {
                eligible = 0;
                for (u32 d=0;d<s_dbg_last_draws && d<MAX_DRAWS;d++)
                    if (!s_d3d.draws[d].is_clear && s_d3d.draws[d].rt_off) { eligible=1;break; }
            }
            if (s_d3d.frame_count >= target && eligible) {''')
    # Fixture executes the same constant-upload function used for every draw.
    anchor='/* Which offscreen surface (if any) does the current RSX state render to?'
    if text.count(anchor)!=1: raise ValueError('Pinned VP fixture anchor changed')
    fixture=r'''
static void port_fp_word(u8* p,u32 word)
{
    u32 swapped=(word<<16)|(word>>16);
    p[0]=(u8)(swapped>>24);p[1]=(u8)(swapped>>16);p[2]=(u8)(swapped>>8);p[3]=(u8)swapped;
}
int port_check_fragment_constants(void)
{
    u8 prog[32]={0};
    port_fp_word(prog,(1u<<24)|(15u<<9)|1u); /* MOV CONST to r0, END */
    port_fp_word(prog+4,2u|(1u<<11)|(2u<<13)|(3u<<15)|(7u<<18)|(1u<<23)|(2u<<25)|(3u<<27));
    char* first=(char*)calloc(1,RSX_FP_HLSL_BUFFER_SIZE);
    char* second=(char*)calloc(1,RSX_FP_HLSL_BUFFER_SIZE);
    if(!first || !second) { free(first);free(second);return 2; }
    port_fp_word(prog+16,0x40000000u); /* 2.0 */
    int n1=port_decompile_guest_fp(prog,sizeof(prog),1,0,first,RSX_FP_HLSL_BUFFER_SIZE);
    float k1[4]={0},k2[4]={0};rsx_fp_extract_consts(prog,sizeof(prog),k1,1);
    u32 hash=rsx_fp_code_hash(prog,sizeof(prog));
    port_fp_word(prog+16,0x41100000u); /* 9.0, same instructions */
    int n2=port_decompile_guest_fp(prog,sizeof(prog),1,0,second,RSX_FP_HLSL_BUFFER_SIZE);
    rsx_fp_extract_consts(prog,sizeof(prog),k2,1);
    int stable=n1>0 && n2>0 && !strcmp(first,second) && hash==rsx_fp_code_hash(prog,sizeof(prog));
    int buffer=strstr(first,"fp_constants[0]") && strstr(first,"float4 rsx_texscale[4];") && strstr(first,"float4 fp_constants[512];");
    int values=k1[0]==2.0f && k2[0]==9.0f;
    port_decompile_guest_fp(prog,sizeof(prog),0,0,second,RSX_FP_HLSL_BUFFER_SIZE);
    int exports=strstr(first,"float4 _o = r[0];") && strstr(second,"float4 _o = h[0];");
    u8 large[151*32];
    float uploaded[PORT_FP_CONSTANT_CAPACITY*4];
    int big=1;
    const unsigned banks[]={72,151};
    for(unsigned b=0;b<2;b++) {
        unsigned slots=banks[b];memset(large,0,sizeof(large));
        for(unsigned i=0;i<slots;i++) {
            memcpy(large+i*32,prog,16);
            port_fp_word(large+i*32,(1u<<24)|(15u<<9)|(i+1==slots?1u:0u));
            float value=(float)(i+1);u32 bits;memcpy(&bits,&value,4);port_fp_word(large+i*32+16,bits);
        }
        int collected=port_collect_guest_fp(large,slots*32,uploaded);
        char needle[64];snprintf(needle,sizeof(needle),"fp_constants[%u]",slots-1);
        int bank=collected==(int)slots && uploaded[(slots-1)*4]==(float)slots &&
            port_decompile_guest_fp(large,slots*32,1,0,second,RSX_FP_HLSL_BUFFER_SIZE)>0 && strstr(second,needle);
        if(!bank) big=0;
        fprintf(stderr,"[compat-check] fragment bank bytes=%u collected=%d last=%g stride=%u %s\n",slots*32,collected,uploaded[(slots-1)*4],VP_FPCB_STRIDE,bank?"PASS":"FAIL");
    }
    int ok=stable && buffer && values && exports && big;
    fprintf(stderr,"[compat-check] fragment constants source-stable=%d buffered=%d values=%d exports=%d %s\n",stable,buffer,values,exports,ok?"PASS":"FAIL");
    free(first);free(second);return ok?0:1;
}

int port_check_vertex_cache(void)
{
    if(s_d3d.vp_vs || s_d3d.vp_vs_n || s_d3d.draw_count) return 2;
    int ok=1;
    for(unsigned n=0;n<VP_VS_CACHE*2+3;n++) {
        int slot=vp_reserve_vs_slot();s_d3d.vp_vs[slot].hash=100+n;
        D3D12DrawRecord* dr=&s_d3d.draws[s_d3d.draw_count++];
        memset(dr,0,sizeof(*dr));dr->is_vp=1;dr->vs_idx=slot;dr->vs_hash=100+n;
        for(u32 d=0;d<s_d3d.draw_count;d++) {
            const D3D12DrawRecord* pending=&s_d3d.draws[d];
            if(s_d3d.vp_vs[pending->vs_idx].hash!=pending->vs_hash) ok=0;
        }
    }
    int capacity=s_d3d.vp_vs_capacity;
    /* Once the batch is retired, reuse an unpinned slot without moving the
     * one still referenced by the next batch. */
    s_d3d.draw_count=1;
    s_d3d.draws[0].vs_idx=1;s_d3d.draws[0].vs_hash=s_d3d.vp_vs[1].hash;
    s_d3d.vp_vs_n=s_d3d.vp_vs_capacity;
    int slot=vp_reserve_vs_slot();
    if(slot==1 || s_d3d.vp_vs_capacity!=capacity ||
       s_d3d.vp_vs[1].hash!=s_d3d.draws[0].vs_hash) ok=0;
    fprintf(stderr,"[compat-check] vertex cache pending bindings=35 stable=%d retired-slot reuse=%d %s\n",ok,slot!=1,ok?"PASS":"FAIL");
    free(s_d3d.vp_vs);s_d3d.vp_vs=NULL;s_d3d.vp_vs_n=0;s_d3d.vp_vs_capacity=0;
    memset(s_d3d.draws,0,sizeof(s_d3d.draws));s_d3d.draw_count=0;
    return ok?0:1;
}

int port_check_depth_state(void)
{
    rsx_state st;memset(&st,0,sizeof(st));
    unsigned cases=0;int ok=1;
    for (unsigned enable=0;enable<2;enable++)
    for (unsigned write=0;write<2;write++)
    for (unsigned func=0;func<8;func++) {
        st.depth_test_enable=enable;st.depth_mask=write;st.depth_func=0x200+func;
        D3D12DrawRecord dr;memset(&dr,0,sizeof(dr));dr.depth_key=rsx_depth_key(&st);
        /* A later state change must not affect this recorded draw. */
        st.depth_test_enable=!enable;st.depth_mask=!write;st.depth_func=0x207-func;
        D3D12_DEPTH_STENCIL_DESC desc;memset(&desc,0,sizeof(desc));
        guest_depth_state(&desc,dr.depth_key);
        if(desc.DepthEnable!=enable || (unsigned)desc.DepthWriteMask!=write ||
           (unsigned)desc.DepthFunc!=func+1 || dr.depth_key!=enable+2*write+4*func) ok=0;
        cases++;
    }
    fprintf(stderr,"[compat-check] per-draw depth state %u combinations %s\n",cases,ok?"PASS":"FAIL");
    return ok?0:1;
}

int port_check_vertex_constants(void)
{
    rsx_state* st=(rsx_state*)calloc(1,sizeof(*st));
    unsigned char* cb=(unsigned char*)calloc(1,VP_CB_STRIDE);
    if(!st || !cb) { free(st);free(cb);return 2; }
    const rsx_state* saved_state=s_d3d.current_rsx_state;
    void* saved_cb=s_d3d.vp_cb_mapped;
    void* saved_fp=s_d3d.vp_fpcb_mapped;
    int saved_uses=s_d3d.vp_uses_c03;
    u32 saved_parity=s_d3d.vp_parity;
    s_d3d.current_rsx_state=st;s_d3d.vp_cb_mapped=cb;
    s_d3d.vp_fpcb_mapped=NULL;s_d3d.vp_parity=0;s_d3d.vp_uses_c03=1;
    st->viewport_scale[2]=0.5f;st->viewport_offset[2]=0.25f;
    int ok=1;
    const float first[]={-0.831469f,0.0f,16.0f};
    for(unsigned n=0;n<3;n++) {
        float* constants=(float*)st->vertex_constants;
        for(unsigned k=0;k<RSX_MAX_VERTEX_CONSTANTS*4;k++) constants[k]=(float)(k%17)-8.0f;
        constants[0]=first[n];
        vp_record_cb(0,-1,NULL);
        float* viewport=(float*)(cb+RSX_MAX_VERTEX_CONSTANTS*16);
        int same=!memcmp(cb,st->vertex_constants,RSX_MAX_VERTEX_CONSTANTS*16);
        int depth=viewport[2]==0.5f && viewport[6]==0.25f;
        fprintf(stderr,"[compat-check] VP constants c00=%g exact=%d depth=%d %s\n",first[n],same,depth,same&&depth?"PASS":"FAIL");
        if(!same || !depth) ok=0;
    }
    s_d3d.current_rsx_state=saved_state;s_d3d.vp_cb_mapped=saved_cb;
    s_d3d.vp_fpcb_mapped=saved_fp;s_d3d.vp_parity=saved_parity;s_d3d.vp_uses_c03=saved_uses;
    free(st);free(cb);return ok?0:1;
}

'''
    text=text.replace(anchor,fixture+anchor)
    replace('vm_base + off, 4096','vm_base + off, PORT_FP_PROGRAM_BYTES',3)
    replace('port_collect_guest_fp(vm_base + foff, 4096,',
            'port_collect_guest_fp(vm_base + foff, PORT_FP_PROGRAM_BYTES,')
    replace('    const u32 key_off = off;         /* lookup key: the offset as bound */',r'''    const char* tex_capdir=getenv("RSX_CAP_DIR");
    if(tex_capdir && w>=16 && h>=16) {
        static u32 seen[64];static unsigned seen_n;
        int known=0;for(unsigned i=0;i<seen_n;i++) if(seen[i]==off) known=1;
        if(!known && seen_n<64) {
            seen[seen_n++]=off;
            u32 size=dxt?blkrow*blkrows:w*h*bpp;
            char path[512];CreateDirectoryA(tex_capdir,NULL);
            snprintf(path,sizeof(path),"%s/tex_%08X_%ux%u_%02X.bin",tex_capdir,off,w,h,fmt);
            FILE* f=fopen(path,"wb");if(f) { fwrite(vm_base+off,1,size,f);fclose(f); }
            fprintf(stderr,"[TEXCAP] off=%08X %ux%u fmt=%02X bytes=%u\n",off,w,h,fmt,size);
        }
    }
    const u32 key_off = off;         /* lookup key: the offset as bound */''')
    def include(m):
        path=source.parent/m[1]
        return '#include "'+path.resolve().as_posix()+'"' if path.is_file() else m[0]
    text=re.sub(r'#include "([^"]+)"',include,text)
    target=output/'rsx_d3d12_backend_compat.c'
    if not target.exists() or target.read_text(encoding='utf-8')!=text:
        target.write_text(text,encoding='utf-8')

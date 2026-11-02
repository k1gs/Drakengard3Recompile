/* Exercise the production PSO-cache mask and slot-dispatch helpers. The
 * callbacks observe which unchanged resolver would run, without a device. */
static u32 port_check_texture_resolved,port_check_texture_null;
static int port_check_texture_payload;
static void port_check_texture_resolve(D3D12DrawRecord* dr,u32 draw,int unit)
{
    port_check_texture_resolved|=1u<<unit;
    if(draw!=9 || dr->tex[unit].raw!=0x1000u+(u32)unit*0x100u ||
       dr->tex[unit].cube!=(unit==15) || dr->tex_rt[unit]!=-1)
        port_check_texture_payload=0;
    dr->tex_rt[unit]=100+unit;
}
static void port_check_texture_null_view(D3D12DrawRecord* dr,u32 draw,int unit)
{
    port_check_texture_null|=1u<<unit;
    if(draw!=9 || dr->tex_rt[unit]!=-1) port_check_texture_payload=0;
}
int port_check_texture_prepass(void)
{
    if(s_d3d.vp_fp_n) return 2;
    const u32 all=(1u<<PORT_TEXTURE_UNITS)-1u;
    const char* programs[3]={"Texture2D rsx_tex[16]; return float4(1,0,0,1);",
        "rsx_tex[0].Sample(a,b); rsx_tex7.Sample(a,b); rsx_tex15.SampleLevel(a,b,c);",
        "rsx_tex0.Sample(a,b);"};
    const u32 expected[3]={0,0x8081u,1};
    for(unsigned i=0;i<3;i++) {
        port_reserve_fp_slot();VPFPEntry* entry=&s_d3d.vp_fp[s_d3d.vp_fp_n++];
        memset(entry,0,sizeof(*entry));entry->fp_addr=0x1000u+i*0x100u;
        entry->vs_hash=7;entry->pso=(ID3D12PipelineState*)(uintptr_t)(i+1);
        entry->sample_mask=port_fp_sample_mask(programs[i]);
    }
    int parser=1,dispatch=1,unchanged=1,fallback=1;port_check_texture_payload=1;
    D3D12DrawRecord dr={0};
    for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) {
        dr.tex[u].set=1;dr.tex[u].raw=0x1000u+u*0x100u;dr.tex[u].cube=(u==15);
    }
    D3D12DrawRecord original=dr;
    const unsigned sequence[6]={1,0,2,1,3,0};
    for(unsigned step=0;step<6;step++) {
        unsigned i=sequence[step];VPFPEntry key={0};
        if(i<3) { key=s_d3d.vp_fp[i];parser=parser && key.sample_mask==expected[i]; }
        else { key.fp_addr=0xBADu;port_active_fp_samples=0; }
        ID3D12PipelineState* selected=port_find_fp_pso(&key);
        u32 want=i<3?expected[i]:all,mask=port_texture_prepass_mask(selected);
        port_check_texture_resolved=port_check_texture_null=0;
        for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++) dr.tex_rt[u]=777;
        port_prepare_texture_slots(&dr,9,mask,port_check_texture_resolve,port_check_texture_null_view);
        dispatch=dispatch && port_check_texture_resolved==want && port_check_texture_null==(all^want);
        if(i==3) fallback=fallback && !selected && mask==all;
        unchanged=unchanged && !memcmp(dr.tex,original.tex,sizeof(dr.tex));
        for(unsigned u=0;u<PORT_TEXTURE_UNITS;u++)
            unchanged=unchanged && dr.tex_rt[u]==((want&(1u<<u))?100+(int)u:-1);
    }
    /* Mock identities are not COM objects; remove them before ownership cleanup. */
    memset(s_d3d.vp_fp,0,(size_t)s_d3d.vp_fp_n*sizeof(*s_d3d.vp_fp));
    port_release_fp_cache();
    int ok=parser&&dispatch&&unchanged&&fallback&&port_check_texture_payload;
    fprintf(stderr,"[compat-check] texture prepass parser=%d active0/7/15-and-zero=%d state=%d fallback=%d payload=%d %s\n",
            parser,dispatch,unchanged,fallback,port_check_texture_payload,ok?"PASS":"FAIL");
    return ok?0:1;
}

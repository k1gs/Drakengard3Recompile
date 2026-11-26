static int port_check_fp_texcoords(char* hlsl)
{
    unsigned passed=0;
    for(unsigned unit=7;unit<=9;unit++) {
        u8 program[16]={0};
        /* MOV r0, TEXn with an identity swizzle and unconditional execution. */
        port_fp_word(program,(1u<<24)|((4u+unit)<<13)|(15u<<9)|1u);
        port_fp_word(program+4,1u|(1u<<11)|(2u<<13)|(3u<<15)|(7u<<18));
        char operand[32];
        snprintf(operand,sizeof(operand),"(input.tc%u).xyzw",unit);
        int decoded=port_decompile_guest_fp(program,sizeof(program),1,0,hlsl,RSX_FP_HLSL_BUFFER_SIZE);
        ID3DBlob* shader=NULL;
        HRESULT hr=decoded==1 && strstr(hlsl,operand)
            ? D3DCompile(hlsl,strlen(hlsl),"fp-texcoord",NULL,NULL,"main","ps_5_0",0,0,&shader,NULL)
            : E_FAIL;
        if(SUCCEEDED(hr) && shader) passed++;
        if(shader) shader->lpVtbl->Release(shader);
    }
    fprintf(stderr,"[compat-check] fragment TEX7..9 forwarded and compiled=%u/3\n",passed);
    return passed==3;
}

static int port_check_fp_predicated_selection(char* hlsl)
{
    u8 program[112]={0};
    const u32 xyzw=(1u<<11)|(2u<<13)|(3u<<15);
    const u32 cc_xyzw=(1u<<23)|(2u<<25)|(3u<<27);
    const u32 always=(7u<<18)|cc_xyzw;
    /* Read the destination, predicate and alternative value independently
     * from b1 so the compiler must preserve the per-lane runtime selection. */
    port_fp_word(program,(1u<<24)|(15u<<9));
    port_fp_word(program+4,2u|xyzw|always);
    port_fp_word(program+32,(1u<<24)|(1u<<30)|(1u<<8)|(15u<<9));
    port_fp_word(program+36,2u|xyzw|always);
    port_fp_word(program+64,(1u<<24)|(5u<<9));
    port_fp_word(program+68,2u|xyzw|cc_xyzw|(4u<<18));
    port_fp_word(program+96,1u);

    ID3DBlob* shader=NULL;
    ID3DBlob* assembly=NULL;
    int decoded=port_decompile_guest_fp(program,sizeof(program),1,0,hlsl,RSX_FP_HLSL_BUFFER_SIZE);
    HRESULT hr=decoded>0 ? D3DCompile(hlsl,strlen(hlsl),"fp-predicate-check",NULL,NULL,
        "main","ps_5_0",0,0,&shader,NULL) : E_FAIL;
    if(SUCCEEDED(hr) && shader)
        hr=D3DDisassemble(shader->lpVtbl->GetBufferPointer(shader),
            shader->lpVtbl->GetBufferSize(shader),0,NULL,&assembly);
    const char* instructions=assembly?(const char*)assembly->lpVtbl->GetBufferPointer(assembly):"";
    int conditional_move=SUCCEEDED(hr) && strstr(instructions,"movc ")!=NULL;
    int ok=conditional_move &&
        strstr(hlsl,"r[0].xz = _p.xz ? _v.xz : r[0].xz;");
    if(assembly) assembly->lpVtbl->Release(assembly);
    if(shader) shader->lpVtbl->Release(shader);
    fprintf(stderr,"[compat-check] fragment predicate masked conditional move=%d %s\n",
        conditional_move,ok?"PASS":"FAIL");
    return ok;
}

/* Compile the actual decompiler output for both six-bit temporary files.
 * The first instruction writes a constant into the selected register. The
 * second reads it into the selected colour export. Predicated writes also
 * read the old destination, so every addressable lane must be initialized. */
static int port_check_fp_registers(void)
{
    char* hlsl=(char*)malloc(RSX_FP_HLSL_BUFFER_SIZE);
    if(!hlsl) return 0;
    unsigned compiled=0;
    for(unsigned half=0;half<2;half++) {
        for(unsigned predicated=0;predicated<2;predicated++) {
            for(unsigned reg=0;reg<64;reg++) {
                u8 program[48]={0};
                const u32 source_swizzle=(1u<<11)|(2u<<13)|(3u<<15);
                const u32 condition_swizzle=(1u<<23)|(2u<<25)|(3u<<27);
                const u32 always=(7u<<18)|condition_swizzle;
                const u32 condition=((predicated?2u:7u)<<18)|condition_swizzle;
                port_fp_word(program,(1u<<24)|(reg<<1)|(half<<7)|(15u<<9));
                port_fp_word(program+4,2u|source_swizzle|condition);
                port_fp_word(program+16,0x3E800000u);
                port_fp_word(program+20,0x3F000000u);
                port_fp_word(program+24,0x3F400000u);
                port_fp_word(program+28,0x3F800000u);
                port_fp_word(program+32,(1u<<24)|(half<<7)|(15u<<9)|1u);
                port_fp_word(program+36,(reg<<2)|(half<<8)|source_swizzle|always);

                int instructions=port_decompile_guest_fp(program,sizeof(program),!half,0,
                                                        hlsl,RSX_FP_HLSL_BUFFER_SIZE);
                char register_name[24];
                snprintf(register_name,sizeof(register_name),"%c[%u]",half?'h':'r',reg);
                ID3DBlob* blob=NULL;
                ID3DBlob* error=NULL;
                HRESULT hr=instructions>0 && strstr(hlsl,register_name)
                    ? D3DCompile(hlsl,strlen(hlsl),"fp-register-check",NULL,NULL,"main","ps_5_0",0,0,&blob,&error)
                    : E_FAIL;
                if(FAILED(hr) || !blob) {
                    fprintf(stderr,"[compat-check] fragment register %s predicated=%u FAIL hr=%08lX %s\n",
                        register_name,predicated,(unsigned long)hr,
                        error?(const char*)error->lpVtbl->GetBufferPointer(error):"");
                    if(error) error->lpVtbl->Release(error);
                    if(blob) blob->lpVtbl->Release(blob);
                    free(hlsl);
                    return 0;
                }
                if(error) error->lpVtbl->Release(error);
                blob->lpVtbl->Release(blob);
                compiled++;
            }
        }
    }
    const int selection=port_check_fp_predicated_selection(hlsl) && port_check_fp_texcoords(hlsl);
    free(hlsl);
    fprintf(stderr,"[compat-check] fragment registers r0..63/h0..63 direct/predicated compiled=%u/256 PASS\n",compiled);
    return compiled==256 && selection;
}

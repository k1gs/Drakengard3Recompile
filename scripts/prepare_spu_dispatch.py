"""Make unresolved SPU dispatch and interpreter gaps fail with a local LS capture."""
import re


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Pinned SPU dispatch source changed: ' + old[:80])
    return text.replace(old, new)


def prepare(sdk, output):
    for name in ('spu_channels', 'spu_interp', 'spu_dispatch_mt'):
        source = sdk / 'runtime/spu' / (name + '.c')
        text = source.read_text(encoding='utf-8')
        if name == 'spu_channels':
            anchor='    if (channel_is_mfc(channel)) {\n        /* Atomic line ops'
            text = replace_once(text, anchor,
                '    if (channel_is_mfc(channel)) {\n'
                '        if(channel==MFC_Cmd) port_task_dma_context(ctx);\n        /* Atomic line ops')
            text = 'void port_task_dma_context(spu_context*);\n' + text
            text = replace_once(text,
                '    uint32_t lsa = (uint32_t)(ls - ctx->ls);',
                '    port_task_dma_trace(ctx,ea,ls,size);\n'
                '    uint32_t lsa = (uint32_t)(ls - ctx->ls);')
            text = 'void port_task_dma_trace(spu_context*,uint32_t,const uint8_t*,uint32_t);\n' + text
            text = replace_once(text,
                '        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */',
                '        memcpy(ls, mem, MFC_ATOMIC_LINE);              /* line -> local store */\n'
                '        port_task_dma_trace(ctx,ea,ls,MFC_ATOMIC_LINE);')
            text = replace_once(text, '    ctx->pc &= SPU_LS_MASK;',
                '    ctx->pc &= SPU_LS_MASK;\n'
                '    /* Image 55 text starts at 0x3000; zero is missing policy memory. */\n'
                '    if(ctx->image_id==55 && !ctx->pc) port_spu_branch_missing(ctx);')
            text = replace_once(text, 'static uint64_t spu_host_ns(void)\n{', r'''/* Differential fixtures control time on their own thread only. Normal
 * execution never installs this callback and keeps the actual host clock. */
static _Thread_local uint64_t (*port_test_clock)(void);
void port_spu_set_test_clock(uint64_t (*clock_fn)(void)) { port_test_clock=clock_fn; }
static uint64_t spu_host_ns(void)
{
    if(port_test_clock) return port_test_clock();''')
            text = replace_once(text,
                '    return (spu_lifted_fn)spu_lookup(lsa, ctx ? ctx->image_id : 0);',
                '    spu_lifted_fn dsp=(spu_lifted_fn)port_msdsp_lookup(ctx,lsa);\n'
                '    if(dsp) return dsp;\n'
                '    return (spu_lifted_fn)spu_lookup(lsa, ctx ? ctx->image_id : 0);')
            text = replace_once(text, '    spu_fn fn = NULL;\n    int code_owner = 0;',
                '    spu_fn fn = port_msdsp_lookup(ctx,ctx->pc);\n'
                '    if(fn) { fn(ctx); return; }\n    int code_owner = 0;')
            text = replace_once(text,
                '            memcpy(mem, ls, MFC_ATOMIC_LINE);          /* commit local store */',
                '            memcpy(mem, ls, MFC_ATOMIC_LINE);          /* commit local store */\n'
                '            port_lf_atomic_trace(ctx, ea, ls);')
            text = 'void port_lf_atomic_trace(spu_context*,uint32_t,const uint8_t*);\n' + text
            text = replace_once(text, 'void spu_halt(spu_context* ctx)\n{',
                'void spu_halt(spu_context* ctx)\n{\n'
                '    if (ctx->status == SPU_STATUS_STOPPED_BY_HALT) port_spu_branch_missing(ctx);')
            text = replace_once(text,
                's_iu = e ? 1 : 0;', 's_iu = e ? atoi(e) : 1;')
            text = text.replace('-- ending the job\\n', '-- unresolved dispatch\\n')
            text = replace_once(text, '              spu_interp_run(ctx, p);',
                '              /* Return to the host caller before interpreting its continuation. */\n'
                '              spu_interp_run_until(ctx, p, ctx->gpr[0]._u32[0] & 0x3FFFCu);')
            old = '        spu_halt(ctx);\n        return;\n    }\n    /* WWS job-code overlay match:'
            text = replace_once(text, old, old.replace('spu_halt(ctx);', 'port_spu_branch_missing(ctx);'))
            old = '    ctx->status = SPU_STATUS_STOPPED_BY_HALT;\n}\n\n/* ===========================================================================\n * Execution trace'
            text = replace_once(text, old, old.replace('ctx->status = SPU_STATUS_STOPPED_BY_HALT;', 'port_spu_branch_missing(ctx);'))
        elif name == 'spu_dispatch_mt':
            text = replace_once(text, '    uint32_t pc = ctx->pc & SPU_LS_MASK;', r'''    uint32_t pc = ctx->pc & SPU_LS_MASK;
    if(ctx->image_id==55) {
        static _Thread_local unsigned n;
        static _Thread_local uint32_t recent[32][3];
        static _Thread_local int trace=-1;
        if(trace<0) trace=getenv("PORT_TASK_RESULT_TRACE")!=NULL;
        if(trace) {
            recent[n&31][0]=pc;recent[n&31][1]=ctx->gpr[0]._u32[0];recent[n&31][2]=ctx->gpr[1]._u32[0];n++;
            if(!pc) {
                fprintf(stderr,"[task-null-branch] recent target/lr/sp:");
                unsigned count=n<32?n:32;
                for(unsigned i=n-count;i<n;i++) fprintf(stderr," %05X/%05X/%05X",recent[i&31][0],recent[i&31][1],recent[i&31][2]);
                fprintf(stderr,"\n");
            }
        }
        if(!pc) port_spu_branch_missing(ctx);
    }''')
            text = replace_once(text, '    if (!special) {',
                '    if (!special) {\n'
                '        spu_dispatch_fn dsp=port_msdsp_lookup(ctx,pc);\n'
                '        if(dsp) { ctx->pc=pc; __attribute__((musttail)) return dsp(ctx); }')
        else:
            # Capture the actual inputs of the observed decoder before stepping.
            # The local fixture can then compare native execution with this interpreter.
            text = replace_once(text, '    uint64_t steps = 0;', r'''    uint64_t steps = 0;
    { static _Thread_local int captured, capture_enabled=-1;
      if(capture_enabled<0) capture_enabled=getenv("PORT_SPU_ENTRY_CAPTURE")!=NULL;
      if(!captured && capture_enabled && ctx->image_id==41 && start_lsa==0x21D20u) {
          captured=1;
          FILE* f=fopen("audio-entry-21D20.bin","wb");
          if(f) {
              uint32_t header[4]={0x41554431u,(uint32_t)ctx->image_id,start_lsa,0};
              fwrite(header,sizeof(header),1,f);fwrite(ctx->gpr,sizeof(ctx->gpr),1,f);
              fwrite(ctx->ls,1,0x40000,f);fclose(f);
              fprintf(stderr,"[spu-entry] captured real decoder inputs image=41 pc=21D20\n");
          }
      }
    }''')
            text = replace_once(text, '        steps++;', r'''        steps++;
        { static int profile=-1;static _Thread_local unsigned samples;
          if(profile<0) profile=getenv("PORT_SPU_PROFILE")!=NULL;
          if(profile && !(steps&65535u) && samples<16) {
              fprintf(stderr,"[spu-perf] image=%d entry=%05X pc=%05X steps=%llu\n",ctx->image_id,start_lsa,ctx->pc,(unsigned long long)steps);
              if(!samples) {
                  char path[128];snprintf(path,sizeof(path),"interp-%p-image%d-entry%05X.bin",(void*)ctx,ctx->image_id,start_lsa);
                  FILE* f=fopen(path,"wb");if(f){fwrite(ctx->ls,1,0x40000,f);fclose(f);}
              }
              samples++;
          }
        }''')
            old = 'ctx->pc = pc; ctx->stop_code = 0x1000; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return 1;'
            text = replace_once(text, old, old.replace('return 1;', 'port_spu_branch_missing(ctx); return 1;'))
            old = '        ctx->pc = pc; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return 1;'
            text = replace_once(text, old, '        ctx->pc = pc; port_spu_branch_missing(ctx); return 1;')
        def include(match):
            path = source.parent / match[1]
            return '#include "' + path.resolve().as_posix() + '"' if path.is_file() else match[0]
        text = re.sub(r'#include "([^"]+)"', include, text)
        text = ('#include "spu_context.h"\nvoid port_spu_branch_missing(spu_context*);\n'
                'typedef void (*port_msdsp_fn)(spu_context*);\n'
                'extern port_msdsp_fn port_msdsp_lookup(const spu_context*,uint32_t);\n' + text)
        target = output / (name + '_compat.c')
        if not target.exists() or target.read_text(encoding='utf-8') != text:
            target.write_text(text, encoding='utf-8')

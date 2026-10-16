"""Make unresolved SPU dispatch and interpreter gaps fail with a local LS capture."""
import re


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Pinned SPU dispatch source changed: ' + old[:80])
    return text.replace(old, new)


def prepare(sdk, output):
    for name in ('spu_channels', 'spu_interp'):
        source = sdk / 'runtime/spu' / (name + '.c')
        text = source.read_text(encoding='utf-8')
        if name == 'spu_channels':
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
        else:
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
        text = '#include "spu_context.h"\nvoid port_spu_branch_missing(spu_context*);\n' + text
        target = output / (name + '_compat.c')
        if not target.exists() or target.read_text(encoding='utf-8') != text:
            target.write_text(text, encoding='utf-8')

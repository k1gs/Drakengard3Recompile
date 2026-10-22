"""Pin asynchronous task identity and arguments before starting a host worker."""
import re
from prepare_jobchains import prepare as prepare_jobchains


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Pinned task dispatch source changed: ' + old[:90])
    return text.replace(old, new)


def workload(text):
    text = once(text, '} spu_async_job;', '    uint8_t taskinfo[48];\n} spu_async_job;')
    text = once(text, 'static void ts_unlock(void) { InterlockedExchange(&s_ts_lock, 0); }',
        'static void ts_unlock(void) { InterlockedExchange(&s_ts_lock, 0); }\n'
        'void port_taskset_lock(void) { ts_lock(); }\nvoid port_taskset_unlock(void) { ts_unlock(); }')
    text = once(text, 'spu_workload_dispatch_async(vm_base + elf, (uint32_t)sz, ctx);',
        'port_dispatch_task_locked(vm_base + elf, (uint32_t)sz, ctx, taskset_ea, t);')
    text = once(text, 'int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,\n                                uint32_t args_ea)',
        'static int dispatch_async(const uint8_t* image, uint32_t image_size,\n                                uint32_t args_ea, uint32_t taskset_ea, uint32_t taskid)')
    old = '''    { extern uint32_t g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
      j->taskset_ea = g_ydkj_real_taskset_ea; j->taskid = g_ydkj_real_taskid; }'''
    new = '''    j->taskset_ea = taskset_ea; j->taskid = taskid;
    if (taskset_ea) {
        extern uint8_t* vm_base;
        if (taskid >= 128) port_task_dispatch_blocker("invalid task slot");
        memcpy(j->taskinfo, vm_base + taskset_ea + 0x80 + 0x30 * taskid, 48);
        uint32_t elf = ((uint32_t)j->taskinfo[0x14]<<24) | ((uint32_t)j->taskinfo[0x15]<<16)
                     | ((uint32_t)j->taskinfo[0x16]<<8) | j->taskinfo[0x17];
        if ((elf & ~7u) != (uint32_t)(image-vm_base)) port_task_dispatch_blocker("ELF/TaskInfo mismatch at enqueue");
        uint8_t* run = ts_running(taskset_ea);
        uint8_t mask = (uint8_t)(0x80u >> (taskid & 7));
        if (!run) port_task_dispatch_blocker("taskset running table exhausted");
        if (run[taskid/8] & mask) { free(j); return 1; }
        if (j->image_id == 42) {
            uint32_t queue = ((uint32_t)j->taskinfo[0]<<24) | ((uint32_t)j->taskinfo[1]<<16)
                           | ((uint32_t)j->taskinfo[2]<<8) | j->taskinfo[3];
            port_lf_register_consumer(queue, taskset_ea, taskid);
        }
        run[taskid/8] |= mask;
        vm_base[taskset_ea + 0x20 + taskid/8] &= (uint8_t)~mask;
        static unsigned trace_count;
        if (trace_count++ < 16) {
            uint32_t a[4];
            for (unsigned k=0;k<4;k++) a[k]=((uint32_t)j->taskinfo[4*k]<<24)
                |((uint32_t)j->taskinfo[4*k+1]<<16)|((uint32_t)j->taskinfo[4*k+2]<<8)|j->taskinfo[4*k+3];
            fprintf(stderr,"[task-enqueue] image=%d taskset=%08X slot=%u elf=%08X args=%08X %08X %08X %08X\\n",
                j->image_id,taskset_ea,taskid,elf,a[0],a[1],a[2],a[3]);
        }
    }'''
    text = once(text, old, new)
    anchor='            int32_t rc = spu_run_lifted_job_abi(j->fn, ls, j->args_ea, j->image_id,'
    text = once(text,anchor,
        '            port_task_result_trace(0,j->image_id,j->taskset_ea,j->taskid,j->taskinfo,ls,0);\n'+anchor)
    anchor='            fprintf(stderr, "[spu_workload] async image=%d RETURNED rc=%d "'
    text = once(text,anchor,
        '            port_task_result_trace(1,j->image_id,j->taskset_ea,j->taskid,j->taskinfo,ls,rc);\n'+anchor)
    # Auto-scheduled tasks use the same claim operation as CreateTask.
    text = once(text, '        run[t / 8] |= m;\n        fprintf(stderr, "[taskset] start task',
        '        fprintf(stderr, "[taskset] start task')
    text = once(text, '                    spurs_pm_build_context(ls, j->taskset_ea, j->taskid, 0, 0);',
        '                    spurs_pm_build_context(ls, j->taskset_ea, j->taskid, 0, 0);\n'
        '                    /* Preserve enqueue-time args/ELF/context; keep the builder full LS pattern. */\n'
        '                    memcpy(ls + 0x2780, j->taskinfo, 32);')
    text = once(text, '    if (!th) { free(j); return 0; }',
        '    if (!th) { if (taskset_ea) port_task_dispatch_blocker("CreateThread failed"); free(j); return 0; }')
    text += '''
/* Task callers hold the taskset lock through slot allocation and enqueue. */
int port_dispatch_task_locked(const uint8_t* image, uint32_t size, uint32_t args,
                              uint32_t taskset, uint32_t taskid) {
    int result=dispatch_async(image,size,args,taskset,taskid);
    if (!result) port_task_dispatch_blocker("task enqueue failed");
    return result;
}
int spu_workload_dispatch_async(const uint8_t* image, uint32_t size, uint32_t args) {
    return dispatch_async(image,size,args,0,0);
}
'''
    return ('#include <stdint.h>\n'
        'void port_task_result_trace(int,int,uint32_t,uint32_t,const uint8_t*,const uint8_t*,int);\n'
        'void port_lf_register_consumer(uint32_t,uint32_t,uint32_t);\n'
        'void port_task_dispatch_blocker(const char*);\n'
        'int port_dispatch_task_locked(const uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t);\n') + text


def prepare_spurs(sdk, output):
    source = sdk / 'libs/spurs/cellSpurs.c'
    text = source.read_text(encoding='utf-8')
    # The taskset argument is a guest BE doubleword, not a host struct field.
    # The SDK stubs discarded it twice, so task CRTs read their allocator at EA 0.
    text = once(text, '    (void)attr;\n    return cellSpursCreateTaskset(spurs, taskset, 0, NULL, 0);', r'''    uint32_t ea=(uint32_t)(uintptr_t)attr;
    if(!ea) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if(ea & 7) return CELL_SPURS_TASK_ERROR_ALIGN;
    if(vm_read32(ea)!=1) return CELL_SPURS_TASK_ERROR_INVAL;
    uint64_t args=vm_read64(ea+8);
    if(getenv("PORT_TASK_RESULT_TRACE"))
        fprintf(stderr,"[taskset-args] taskset=%08X args=%016llX\n",
            (uint32_t)(uintptr_t)taskset,(unsigned long long)args);
    return cellSpursCreateTaskset(spurs,taskset,args,
        (const u8*)(uintptr_t)(ea+16),vm_read32(ea+24));''')
    begin=text.index('    (void)sdkVersion; (void)argTaskset; (void)priority; (void)maxContention;',
                     text.index('s32 _cellSpursTasksetAttributeInitialize('))
    end=text.index('\n}',begin)
    text=text[:begin]+r'''    uint32_t ea=(uint32_t)(uintptr_t)attr;
    uint32_t priorities=(uint32_t)priority;
    if(!ea || !priorities) return CELL_SPURS_TASK_ERROR_NULL_POINTER;
    if(ea & 7) return CELL_SPURS_TASK_ERROR_ALIGN;
    for(unsigned i=0;i<8;i++)
        if(vm_base[priorities+i]>15) return CELL_SPURS_TASK_ERROR_INVAL;
    uint8_t saved_priority[8];
    memcpy(saved_priority,vm_base+priorities,8);
    memset(vm_base+ea,0,512); // CellSpursTasksetAttribute, including reserved bytes.
    vm_write32(ea,revision);
    vm_write32(ea+4,sdkVersion);
    vm_write64(ea+8,argTaskset);
    memcpy(vm_base+ea+16,saved_priority,8);
    vm_write32(ea+24,maxContention);
    vm_write32(ea+32,6400);
    return CELL_OK;'''+text[end:]
    # Job return is not an application user-event. The upstream broadcast of
    # monotonically increasing job indices into every attached queue corrupts
    # Drakengard's event-helper slot array (job 321 clears resource table slot 0).
    # Real SPU send_event already routes through the bound port in lv2_register.
    start=text.index('/* SPURS_EVENT_D3=<n>: probe.')
    end=text.index('static void jc_execute(',start)
    removed=text[start:end]
    if removed.count('static void jc_signal_done(')!=1 or 's_spurs_event_queue[i]' not in removed:
        raise ValueError('Pinned synthetic job-completion body changed')
    text=text[:start]+text[end:]
    text=once(text,'static void jc_execute(',
        'void (*port_jc_end_hook)(u32,u32)=0; /* Isolated native regression seam. */\nstatic void jc_execute(')
    for old in ('            jc_signal_done(jc_ea, idx);\n',
                '                if (jobs) jc_signal_done(jc_ea, jobs - 1);\n',
                '    jc_signal_done(s_jobchains[slot].jc_ea, -1);\n'):
        text=once(text,old,'')
    start=text.index('            /* Signal per JOB, not per lap.')
    end=text.index('            pc += 8; continue;',start)
    text=text[:start]+text[end:]
    start=text.index('    /* Tell the application the chain is done.')
    end=text.index('    return 0;',start)
    text=text[:start]+text[end:]
    text = once(text, '    u32 free_slot = CELL_SPURS_MAX_TASK;',
        '    port_taskset_lock();\n    u32 free_slot = CELL_SPURS_MAX_TASK;')
    text = once(text, '        s_jobchains[i].run_count++;',
        '        s_jobchains[i].run_count++;\n'
        '        if(getenv("PORT_JC_TRACE") && s_jobchains[i].run_count<=512)\n'
        '            fprintf(stderr,"[JC] %s chain=%08X run=%d running=%ld entry=%08X cmd=%016llX\\n",\n'
        '                who,(u32)jc_ea,s_jobchains[i].run_count,(long)s_jobchains[i].running,\n'
        '                s_jobchains[i].entry_ea,(unsigned long long)vm_read64(s_jobchains[i].entry_ea));')
    text = once(text, '    s_jobchains[slot].running = 0;',
        '    if(getenv("PORT_JC_TRACE")) fprintf(stderr,"[JC] worker end chain=%08X runs=%d\\n",\n'
        '        s_jobchains[slot].jc_ea,s_jobchains[slot].run_count);\n'
        '    s_jobchains[slot].running = 0;')
    text = once(text, '                printf("[cellSpurs] chain 0x%08X: END after %d job(s)\\n", jc_ea, jobs);',
        '                if(getenv("PORT_JC_TRACE")) fprintf(stderr,"[JC] END chain=%08X pc=%08X jobs=%d reread=%016llX\\n",\n'
        '                    jc_ea,pc,jobs,(unsigned long long)vm_read64(pc));\n'
        '                if(port_jc_end_hook) port_jc_end_hook(jc_ea,pc);\n'
        '                printf("[cellSpurs] chain 0x%08X: END after %d job(s)\\n", jc_ea, jobs);')
    text=prepare_jobchains(text)
    text = once(text, '                    spu_workload_dispatch_async(host_elf, (uint32_t)sz,\n                                                (uint32_t)(uintptr_t)context);',
        '                    if (!port_dispatch_task_locked(host_elf, (uint32_t)sz,\n'
        '                                                (uint32_t)(uintptr_t)context, taskset_ea, i))\n'
        '                        port_task_dispatch_blocker("task image not dispatched");')
    text = once(text, '            return CELL_OK;\n        }\n    }\n\n    fprintf(stderr, "[cellSpurs] CreateTask:',
        '            port_taskset_unlock();\n            return CELL_OK;\n        }\n    }\n\n    port_taskset_unlock();\n    fprintf(stderr, "[cellSpurs] CreateTask:')
    def include(m):
        p = source.parent / m[1]
        return '#include "' + p.resolve().as_posix() + '"' if p.is_file() else m[0]
    text = re.sub(r'#include "([^"]+)"', include, text)
    text = ('#include <stdint.h>\nvoid port_taskset_lock(void);\nvoid port_taskset_unlock(void);\n'
        'void port_task_dispatch_blocker(const char*);\n'
        'int port_dispatch_task_locked(const uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t);\n') + text
    target = output / 'cellSpurs_compat.c'
    if not target.exists() or target.read_text(encoding='utf-8') != text:
        target.write_text(text, encoding='utf-8')

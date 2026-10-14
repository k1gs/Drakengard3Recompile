"""Pin asynchronous task identity and arguments before starting a host worker."""
import re


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
        'void port_task_dispatch_blocker(const char*);\n'
        'int port_dispatch_task_locked(const uint8_t*,uint32_t,uint32_t,uint32_t,uint32_t);\n') + text


def prepare_spurs(sdk, output):
    source = sdk / 'libs/spurs/cellSpurs.c'
    text = source.read_text(encoding='utf-8')
    text = once(text, '    u32 free_slot = CELL_SPURS_MAX_TASK;',
        '    port_taskset_lock();\n    u32 free_slot = CELL_SPURS_MAX_TASK;')
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

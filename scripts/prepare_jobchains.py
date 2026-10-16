"""Preserve a registered SPURS command stream across workload wake signals."""


def prepare(text):
    def once(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise ValueError('Pinned jobchain source changed: ' + old[:90])
        text = text.replace(old, new)

    once('} s_jobchains[MAX_JOBCHAINS];', r'''} s_jobchains[MAX_JOBCHAINS];
static u32 s_jc_cursor[MAX_JOBCHAINS],s_jc_return[MAX_JOBCHAINS];
static int s_jc_pending[MAX_JOBCHAINS];
static volatile long s_jc_state_lock;
static void jc_lock_state(void) {
    while(_InterlockedCompareExchange(&s_jc_state_lock,1,0)) Sleep(0);
}
static void jc_unlock_state(void) { _InterlockedExchange(&s_jc_state_lock,0); }
''')
    once('            s_jobchains[i].run_count = 0;',
         '            s_jobchains[i].run_count = 0;\n'
         '            s_jc_cursor[i]=entry_ea;s_jc_return[i]=0;s_jc_pending[i]=0;')
    start = text.index('static void jc_execute(')
    end = text.index('static DWORD WINAPI jc_thread(', start)
    body = text[start:end]
    body = body.replace('static void jc_execute(', 'static int jc_execute(', 1)
    old = '    u32 pc = entry_ea, ret_pc = 0;'
    if body.count(old) != 1:
        raise ValueError('Pinned jobchain entry changed')
    body = body.replace(old, '''    int slot=-1;
    for(int i=0;i<MAX_JOBCHAINS;i++) if(s_jobchains[i].jc_ea==jc_ea) { slot=i;break; }
    if(slot<0) abort();
    u32 pc=s_jc_cursor[slot],ret_pc=s_jc_return[slot];
    (void)entry_ea;''')
    body = body.replace('return;', 'return 0;')
    old = '''                if(port_jc_end_hook) port_jc_end_hook(jc_ea,pc);'''
    if body.count(old) != 1:
        raise ValueError('Pinned jobchain END hook changed')
    body = body.replace(old, '''                s_jc_cursor[slot]=pc;s_jc_return[slot]=ret_pc;
                if(port_jc_end_hook) port_jc_end_hook(jc_ea,pc);''')
    old = '                return 0;\n            }\n            if (ext == (7 | (14 << 3)))'
    if body.count(old) != 1:
        raise ValueError('Pinned jobchain END return changed')
    body = body.replace(old, '                return 1;\n            }\n            if (ext == (7 | (14 << 3)))')
    old='           jc_ea, jobs);\n}'
    if body.count(old)!=1:
        raise ValueError('Pinned jobchain malformed-stream exit changed')
    body=body.replace(old,'           jc_ea, jobs);\n    return 0;\n}')
    text = text[:start] + body + text[end:]
    start = text.index('static DWORD WINAPI jc_thread(')
    end = text.index('/* cellSpursRunJobChain --', start)
    text = text[:start] + r'''static DWORD WINAPI jc_thread(LPVOID p)
{
    int slot=(int)(intptr_t)p;
    { const char* delay=getenv("SPURS_JC_DELAY");
      if(delay && *delay) Sleep((unsigned)atoi(delay)); }
    for(;;) {
        jc_lock_state();s_jc_pending[slot]=0;jc_unlock_state();
        int ended=jc_execute(s_jobchains[slot].entry_ea,s_jobchains[slot].jc_ea,
                             s_jobchains[slot].size_desc);
        jc_lock_state();
        /* An END is a published idle tail, not permission to replay the
         * already consumed prefix. A signal received while retiring must
         * re-read that very tail, which the PPU may now have replaced. */
        if(ended && s_jobchains[slot].running && s_jc_pending[slot] &&
           vm_read64(s_jc_cursor[slot])!=127ull) {
            jc_unlock_state();continue;
        }
        if(getenv("PORT_JC_TRACE")) fprintf(stderr,"[JC] worker end chain=%08X runs=%d cursor=%08X\n",
            s_jobchains[slot].jc_ea,s_jobchains[slot].run_count,s_jc_cursor[slot]);
        s_jc_pending[slot]=0;s_jobchains[slot].running=0;
        jc_unlock_state();return 0;
    }
}

''' + text[end:]
    old = '''        /* Coalesce: a chain already being walked must not start twice. */
        if (_InterlockedCompareExchange(&s_jobchains[i].running, 1, 0) == 0) {
            HANDLE th = CreateThread(NULL, 1u << 20, jc_thread, (LPVOID)(intptr_t)i, 0, NULL);
            if (th) CloseHandle(th);
            else s_jobchains[i].running = 0;
        }'''
    once(old, '''        /* Serialize signal publication with worker retirement. */
        jc_lock_state();
        s_jc_pending[i]=1;
        int launch=!s_jobchains[i].running;
        if(launch) s_jobchains[i].running=1;
        jc_unlock_state();
        if(launch) {
            HANDLE th=CreateThread(NULL,1u<<20,jc_thread,(LPVOID)(intptr_t)i,0,NULL);
            if(th) CloseHandle(th);
            else {
                jc_lock_state();s_jobchains[i].running=0;jc_unlock_state();
                return CELL_SPURS_CORE_ERROR_AGAIN;
            }
        }''')
    return text

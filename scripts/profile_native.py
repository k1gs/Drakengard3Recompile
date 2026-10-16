"""Bounded Windows x64 RIP sampler; briefly suspends then always resumes threads.

Run outside the process sandbox so the target's threads are visible. This is
wall-time sampling, not a CPU flame graph; thread CPU deltas are reported too.
"""
import argparse
import collections
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid', type=int, required=True)
    p.add_argument('--seconds', type=int, default=20, choices=range(1, 61))
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--stack-candidates', action='store_true', help='Also collect raw stack words pointing into the EXE (not unwound frames)')
    p.add_argument('--tid', type=int, action='append', help='Restrict sampling to these host thread IDs')
    a = p.parse_args()
    if a.out.exists():
        p.error('Output already exists')
    k = C.WinDLL('kernel32', use_last_error=True)
    ps = C.WinDLL('psapi', use_last_error=True)
    def api(lib, name, restype, *args):
        f = getattr(lib, name)
        f.restype, f.argtypes = restype, args
        return f
    close = api(k, 'CloseHandle', W.BOOL, W.HANDLE)
    op = api(k, 'OpenProcess', W.HANDLE, W.DWORD, W.BOOL, W.DWORD)
    ot = api(k, 'OpenThread', W.HANDLE, W.DWORD, W.BOOL, W.DWORD)
    suspend = api(k, 'SuspendThread', W.DWORD, W.HANDLE)
    resume = api(k, 'ResumeThread', W.DWORD, W.HANDLE)
    context = api(k, 'GetThreadContext', W.BOOL, W.HANDLE, C.c_void_p)
    times = api(k, 'GetThreadTimes', W.BOOL, W.HANDLE, *([C.POINTER(W.FILETIME)] * 4))
    snap = api(k, 'CreateToolhelp32Snapshot', W.HANDLE, W.DWORD, W.DWORD)
    class Thread(C.Structure):
        _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('tid', W.DWORD),
                    ('pid', W.DWORD), ('priority', W.LONG), ('delta', W.LONG), ('flags', W.DWORD)]
    first = api(k, 'Thread32First', W.BOOL, W.HANDLE, C.POINTER(Thread))
    nxt = api(k, 'Thread32Next', W.BOOL, W.HANDLE, C.POINTER(Thread))
    enum = api(ps, 'EnumProcessModules', W.BOOL, W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD))
    name = api(ps, 'GetModuleFileNameExW', W.DWORD, W.HANDLE, W.HMODULE, W.LPWSTR, W.DWORD)
    class Module(C.Structure):
        _fields_ = [('base', C.c_void_p), ('size', W.DWORD), ('entry', C.c_void_p)]
    info = api(ps, 'GetModuleInformation', W.BOOL, W.HANDLE, W.HMODULE, C.POINTER(Module), W.DWORD)
    process = op(0x410, False, a.pid)
    if not process:
        raise C.WinError(C.get_last_error())
    modules = []
    handles, start, counts = {}, {}, collections.Counter()
    failures = 0
    stack_counts = collections.Counter()
    read = api(k, 'ReadProcessMemory', W.BOOL, W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t))
    def cpu(h):
        ts = [W.FILETIME() for _ in range(4)]
        if not times(h, *[C.byref(t) for t in ts]):
            return None
        return sum((t.dwHighDateTime << 32) | t.dwLowDateTime for t in ts[2:]) / 1e7
    started = time.monotonic()
    try:
        ms = (W.HMODULE * 1024)()
        needed = W.DWORD()
        if not enum(process, ms, C.sizeof(ms), C.byref(needed)):
            raise C.WinError(C.get_last_error())
        for m in ms[:needed.value // C.sizeof(W.HMODULE)]:
            path, mi = C.create_unicode_buffer(32768), Module()
            if name(process, m, path, len(path)) and info(process, m, C.byref(mi), C.sizeof(mi)):
                modules.append(dict(path=path.value, base=mi.base, size=mi.size))
        while time.monotonic() - started < a.seconds:
            shot = snap(4, 0)
            try:
                t = Thread()
                t.size = C.sizeof(t)
                ok = first(shot, C.byref(t))
                while ok:
                    if t.pid == a.pid and t.tid not in handles and (not a.tid or t.tid in a.tid):
                        h = ot(0x4A, False, t.tid)
                        if h:
                            handles[t.tid], start[t.tid] = h, cpu(h)
                    ok = nxt(shot, C.byref(t))
            finally:
                close(shot)
            for tid, h in handles.items():
                # AMD64 CONTEXT must be aligned to 16 bytes, flags at 48, RIP at 248.
                buf = C.create_string_buffer(1248)
                ptr = (C.addressof(buf) + 15) & ~15
                C.c_uint32.from_address(ptr + 48).value = 0x100001
                if suspend(h) == 0xFFFFFFFF:
                    continue
                try:
                    if context(h, ptr):
                        rip = C.c_uint64.from_address(ptr + 248).value
                        counts[tid, rip] += 1
                        if a.stack_candidates:
                            rsp=C.c_uint64.from_address(ptr+152).value
                            words=(C.c_uint64*128)();got=C.c_size_t()
                            if read(process,rsp,words,C.sizeof(words),C.byref(got)):
                                exe=modules[0]
                                # These are diagnostic candidates, including stale
                                # stack slots; never present them as an unwind.
                                for word in set(words):
                                    if exe['base']<=word<exe['base']+exe['size']:
                                        stack_counts[tid,word]+=1
                    else:
                        failures += 1
                finally:
                    resume(h)
            time.sleep(.01)
        cpus = {tid: (cpu(h) or 0) - (start[tid] or 0) for tid, h in handles.items()}
    finally:
        for h in handles.values():
            close(h)
        close(process)
    result = dict(pid=a.pid, seconds=time.monotonic()-started, modules=modules,
                  cpu_seconds=cpus, failed_contexts=failures,
                  samples=[dict(tid=t, rip=r, count=n) for (t, r), n in counts.most_common()])
    result['stack_candidates']=[dict(tid=t,rip=r,count=n) for (t,r),n in stack_counts.most_common()]
    with a.out.open('x', encoding='utf-8') as f:
        json.dump(result, f, indent=2)
    print(f'{sum(counts.values())} samples, {failures} failures -> {a.out}')


if __name__ == '__main__':
    main()

# Native memory reuse — 2026-10-15

Publication checkpoint 2026-10-15: runtime changes are committed as `328e3e9`. Local/uncommitted status statements below describe the original capture time. Current blocker and visual limitations are recorded in `LFQUEUE_PROGRESS.md`.

The port still has no validated menu or playable frame. This stage fixes a
reproduced allocator failure; it does not establish rendering correctness.
All changes since `87255ed` remain local and uncommitted.

## Confirmed failure and correction

Run 038 / build 043 exhausted the fixed 0x40000000..0x50000000 guest window on
a 64-KiB kernel request. The failure recorded `bump=50000000`,
`live=0A190000` (161.5625 MiB), and 73 successful frees. Freed allocations were
removed from the record table but their address space was never reused.
The title reported `FMallocPS3DL.cpp OOM 32900`.

The native `--check-memory` regression first holds 1025 distinct live pages,
frees them, then performs 4097 allocate/free cycles with only one page live.
Build 044 fails at cycle 3071 with **zero live bytes**, 4096 frees, and the bump
pointer at the end of the window (`reports/memory-reuse-before.log`).

Build 045 passes that same test (`reports/memory-reuse-after.log`). It also
checks zeroed returned pages, double-free rejection, and 1-MiB alignment.
Portable tests pass 21/21 (`reports/portable-tests-memory-stage.log`).

`scripts/prepare_memory.py` applies checked changes to a build-local SDK copy.
A bitmap marks only pages explicitly released through `sys_memory_free`.
Allocation searches aligned contiguous freed pages before extending the bump
pointer. Virgin gaps and mmapper reservations are not treated as free space.
Allocation, release, bookkeeping, commit failure rollback and zeroing share
the allocator lock. The existing 256-MiB guest window is unchanged.

## Boot continuation

Run 039 / build 045 passes the earlier small-request failure, loads TITLE and
TEXTURES.TFC, and creates additional guest fragment pipelines. It later requests
`0x31790000` kernel bytes; the title reports an original request of 829997056
bytes (791.546875 MiB). Failure state is `bump=4EF80000`, `live=0EF80000`,
73 frees. This is a distinct blocker, not evidence that the allocator needs a
larger window. Trace the requesting PPU stack and its source size next.
Early renderer readbacks are still entirely zero. Pipeline creation and FIFO
progress do not demonstrate a correct screen.

There is also an independent SDK guard-worker issue: a job guard that remains
closed for 20 seconds terminates its chain. Its relation to the current failure
has not been proved, and its behavior has not been changed in this stage.

## GPU writer evidence

`PORT_GPU_WRITERS=1` enables bounded diagnostic tracing of PPU 32/64-bit writes
and SPU DMA over recorded FIFO self-JUMPs (`port/gpu_writer_trace.cpp`).
No guest command, label or completion is substituted by this trace.
Run 036 shows 228 unique traced parks released by real SPU DMA writes of NOP.
Their PPU producer resolves to `func_0140DB10`, LR `0140DF64`.
Its final stalled address was absent from the traced creation records; that
absence alone does not identify a faulty producer.

Run 037, with 64-bit stores also traced, reaches GET==PUT and shader-chain END
after 229 jobs, then the small-request OOM. Adding diagnostics changes host
scheduling: this run is **not** proof of a GPU synchronization fix. The strict
FIFO fix and synthetic contract remain described in `GPU_FIFO_PROGRESS.md`.

Game assets, generated sources, logs, frame dumps and executables remain under
ignored directories and must not be added to Git.

## Final checkpoint

Build 046 adds a bounded guest-stack diagnostic on window exhaustion; it does
not change the allocation decision. Native memory/FIFO/task-snapshot/interpreter
return checks all pass again. Run 039 stopped at its 180-second limit
(180.12s); run 040 at its 120-second limit (120.15s). No background run remains.
Both runs captured two entirely zero BMP pixel buffers.

Run 040 fails on a different large request: `0x0AE00000` (174 MiB), with
`bump=4E0D0000`, `live=0E0D0000` (224.8125 MiB), 73 frees. Caller is guest
thread 21, LR `008C2C54`. The saved stack chain leads through allocator routines
`008C2B90`, `008C3538`, `008CC9C0`, `0141A210`, then `012EFED8`, `00E062F0`,
`00C04B0C+5F0`, `00C08414+D8`, `00C08C80+494`, `00B16A64+1C`, `00C960FC+28`.
These are saved return addresses; indirect calls can leave LR stale, and stack
scans can include non-address data. Do not assign source function names from
these addresses alone. The variation in request size between runs is evidence
to investigate, not proof of corruption or a reason to clamp the request.

Next: capture the allocation size and source object before the indirect calls
around `00C04B0C+5F0`, then follow the size-producing path and check it against
the relevant resource bytes. Keep allocator capacity and real completion
signals unchanged while diagnosing this separate failure.

Final executable SHA-256:
`65d97f76f4cd6d305a3749c5e4195e3e56531f928a0ebb40e098fae20817ab72`.

# SPURS task identity — 2026-10-15

Publication checkpoint 2026-10-15: runtime changes are committed as `328e3e9`. Local/uncommitted status statements below describe the original capture time. Current blocker and visual limitations are recorded in `LFQUEUE_PROGRESS.md`.

The Havok alignment halt was caused by executing one SPU image with another
task's descriptor. Fixing task identity removes that observed halt; it does not
yet produce a usable screen. Previous stage: `SPU_OVERLAY_PROGRESS.md`.

## Confirmed mismatch

Run 026's capture is image 54 (ELF guest EA 0x01831B00). Its CreateTask arguments
are `01AB4880 01AC3C00 01AC3980 00000000`. But its LS TaskInfo at 0x2780 contains
`01AAFC80 00000001 01AC3480 01AC3500`, with ELF EA **0x018F4600** at +0x14.
Those belong to image 63. The LS/128-GPR capture and CreateTask logs independently
establish the mismatch. Consequently, the misaligned pointer 0x47254508 was
read from the wrong job descriptor; changing alignment would conceal the cause.

The pinned SDK passes taskset/slot through process-wide globals and snapshots
only those numbers at dispatch. The async thread later builds its context from
the live guest TaskInfo. Also, the running bit was set only after the worker
started, allowing the exit scheduler to see an enqueued task as still pending.

## Change

`scripts/prepare_spu_tasks.py` prepares build-local copies of `cellSpurs.c` and
`spu_workload.c`. Original SDK files and game instructions are untouched.

- Task dispatch receives taskset and slot explicitly; generic workload dispatch
  does not inherit an unrelated task's globals.
- The 48-byte TaskInfo is captured synchronously at enqueue and checked against
  the dispatched ELF. The worker uses the captured args/ELF/context fields when
  constructing its LS context. The existing full-LS pattern remains as before.
- CreateTask slot selection, enqueue, exit scheduling and the running-bit claim
  share the taskset lock. A duplicate claim does not spawn another worker.
- Enqueue failure, an ELF/TaskInfo mismatch or exhausted host tracking stops
  with exit 9 instead of reporting a successful task creation.

The legacy global counters and joins elsewhere in the SDK are not a complete
SPURS model. SPU-side task creation, JoinTask2 across multiple tasksets, host
thread failures and non-Windows execution remain separately unverified.

## Validation

Native `--check-spu-task-snapshot` enqueues two tasks in separate tasksets,
re-enqueues one duplicate, then overwrites their guest TaskInfo args and ELF
before releasing workers. Both execute with their original arguments and ELF,
and exactly two callbacks complete. PASS: `spu-task-snapshot-check-001.log` and
`spu-task-snapshot-check-002.log`. These use synthetic ELF metadata/native
callbacks; they test the actual runtime enqueue/context path, not game logic.

| Run | Build | Result |
| --- | --- | --- |
| 027 | 030 | 90s bounded run; no previous HALT/assert or task invariant stop; stopped by runner |
| 028 | 031 | 120s bounded run; same result; trace shows image 54 enqueued with its own ELF and args |
| 029 | 032 | 120s, PPU samples identify RenderingThread 20 at LR 0x008B0D18 |
| 030 | 033 | 95s requested / 96.43s wall time, stable GPU label one behind target |
| 031 | 033 | 95s, same one-behind state with FIFO GET==PUT; GCM_DRAINDBG and PPU samples recorded |

Run 028 trace includes:
`image=54 taskset=01AA7800 slot=0 elf=01831B00 args=01AB4880 01AC3C00 01AC3980 00000000`.
Run 028 executable SHA-256:
`677256ef31cbf25f2da7377b730bac8273558f0552b3931c5836605ee70371e2`.
Portable tests: 21/21 (`portable-tests-task-snapshot.log`).
Runs 027/028 each emitted two renderer BMPs. All four have zero pixel data
(`frame-stats-task-snapshot.json`); no menu or playable screen is established.

## Rendering investigation

RenderingThread 15 exits during initialization, and RenderingThread 20 is then
created on the same guest argument. Main later waits on queue 11 through the
two-slot event structure discussed in the previous report. This is not yet proof
that the restart is wrong or that a queue event is lost.

`PORT_PPU_SAMPLE=1` takes three best-effort snapshots of the runtime's registered
PPU contexts at 30/60/90 seconds. It reports last direct-call sites using the
SDK's `ppu_report_guest_lrs`; a stable LR does not prove a thread is idle because
indirect calls do not update it. The option is recorded in the run manifest.

Build-local `ppu_loader_compat.cpp` adds a read-only callback to those samples.
Run 030: RenderingThread 20, object 0x403E01E0, label 0x20000FF0, value/last
0x465, target 0x466, wrap threshold 0x40000000 (same in both late samples).
The direct call site is in `func_008B0BB0`; it flushes via `func_00E1BAEC` and
sleeps 200us while polling. The label is emitted via `func_00E1BE5C`
(methods 0x1D6C/0x1D70).

Run 031: value 0x4CB vs target 0x4CC. Final FIFO drain reports `caught-up`,
GET=PUT=0x000E9048, `ctx.current=0x400E9048`. No backlog remains at that sampled
endpoint, but this does not distinguish an unpublished packet, skipped/corrupt
packet or wrong semaphore handling. Inspect raw FIFO words around PUT and the
producer's publication order before changing GPU completion behavior. Do not
force the label to the expected value.

Final build 033 SHA-256:
`2c47be3a6bb720e41f5f85bcbcf1880ab89a8d88be26a56522eafd3d2dee9883`.
Return-boundary and queue native checks PASS (`spu-return-task-stage.log`,
`queue-task-stage.log`). All bounded processes are stopped. No menu/gameplay is
validated and no background continuation is active.

```powershell
.\scripts\build-game.cmd
.\build\game\DrakengardRecompile.exe --check-spu-task-snapshot
$env:PORT_PPU_SAMPLE='1'
.\scripts\run-game.ps1 -Seconds 120 -Run FRESH_ID -CaptureFrames
Remove-Item Env:PORT_PPU_SAMPLE
```

Latest published commit remains `87255ed`; this stage is local and uncommitted.

# Synthetic job events corrupted the resource table — 2026-10-15

Local continuation after `LFQUEUE_PUSH_PROGRESS.md`. No commits or pushes.
The LF queue implementation remains in place. Menu/gameplay are not validated.

## Confirmed cause

The pinned SDK's `jc_signal_done` broadcasts an application event into **every**
queue attached to any SPURS instance after each job, on END, and on chain-thread
exit. Its `data2` is the monotonically increasing job index. These events are
not emitted by the guest SPU program.

Drakengard's event-helper thread `00BA69CC` receives queue 5, takes low 24 bits
of data2 as a slot index and computes `base + index*16`. It allocates helper
records at `+200/+204` and clears the slot's pending word with stwcx at `+208`.
Its base is `01AC1C00`. This protocol has a bounded application slot array;
it does not accept arbitrary increasing job numbers.

At synthetic job index `0x141` (321), its pending-word address is:

```
01AC1C00 + 0x141*16 + 0x208 = 01AC3218
```

That address is resource-table slot zero. The real constructor had previously
written a resource object there. The synthetic event caused the helper to clear
that object pointer, so handle `00100000` later resolved to NULL. The size
outputs remained untouched and the subsequent stack-derived allocation followed.

Evidence:

- Run 050/build 059 records constructor writes to `01AC3218/01AC321C` from
  `00CB5624`, then unrelated event-helper writes into the resource table.
- Run 051's page-watch was diagnostic only, affected timing and did not capture
  the critical clear within its bound. Its missing clear is not negative proof.
- Run 052/build 060: `reports/game-run-052-errors.log:33016` logs the atomic
  attempt `41B96800 -> 0` from `00BA69CC`; the live context has helper base
  `r28=01AC1C00`, index displacement `r29=1410`, slot base `r31=01AC3010`.
  The subsequent resource query at line 33103 sees handle `00100000`, table
  `01AC3218`, slot zero = NULL, followed by allocation `0CF7D300`.
- The final atomic diagnostic logs successful stwcx commits, not attempts.

## Fix

`scripts/prepare_spu_tasks.py` removes the synthetic job-completion generator
and its three call sites from the build-local `cellSpurs_compat.c`. It checks
the pinned source anchors. The SDK checkout is not edited.

Actual SPU user events still use `SPU_WrOutMbox` + `SPU_WrOutIntrMbox` and
`spu_deliver_user_event` in `lv2_register.c`: the selected port resolves to one
bound queue, and the original payload is preserved. Job return, chain END and
chain-thread return do not generate extra application events.

No resource-table entry is restored manually, no sizes are zeroed/clamped, and
no label or EventFlag completion is fabricated. The genuine guest constructor
is responsible for creating its resource.

## Regression

`build\game\DrakengardRecompile.exe --check-spurs-events`

The harness attaches two queues, creates and executes real native job chains
with registered test SPU entries, and inspects the actual LV2 queues:

1. A job that emits no event must leave both queues empty, including after END
   and Join.
2. A job that sends a real user event to port 4 must deliver exactly one event
   into that port's queue, with original data2/data3; the second queue stays empty.

Before fix/build 061: FAIL, silent job produces 2–3 unsolicited events per queue.
After fix/build 062 and 063: PASS, silent job 0/0, actual-event job 1/0,
payload preserved and both entries actually executed.

Portable tests: 21/21. Build 062: LFQueue test and six prior native checks pass;
interpreter HALT exits 8 as expected.

## Bounded game validation

Run 053/build 062: 120 seconds, stopped at time limit. LF publication runs;
no old NULL-call chain, resource-table atomic clear, or large stack-derived
allocation appears. This run used the prior NULL-only resource-query diagnostic,
so successful method dispatch requires the following run's positive trace.
GPU FIFO progresses past earlier offsets, but two saved renderer pixel buffers
are still all zero. No menu or controls claim.

`PORT_RESOURCE_TRACE=1` records bounded table writes and successful slot-zero
atomic clears. `PORT_ALLOC_TRACE=1` records bounded resource-query receiver,
method target, handle and table entry as well as large allocator requests.
All captures/logs/generated sources remain ignored.

Final run 054/build 063: 120 seconds, stopped at time limit. Positive traces
at `reports/game-run-054-errors.log:9497` (also 9581/9908/9967) show handle
`00100000` resolving to receiver/table entry `41B48700`, method `00D570C0`.
There are no subsequent NULL-call chains or old large stack-derived allocations
in this bounded run. Resource outputs are obtained through the original game
method; the table was not repaired manually.

The two saved renderer pixel buffers remain zero; frame 001 was visually
inspected and is black. This still does not validate a menu or controls.
RenderingThread samples show label progression `4BD -> 513`, each one behind its
current target, and GPU GET/PUT progress from `EEAE8/108D8C` to
`17ABE4/18061C`. A one-behind sample by itself is not proof of a stuck fence.
Main tid 1 repeatedly waits on queue 11 with 100000-us timeout, LR `008EFE08`.
Next: identify that wait's predicate/producer and the lack of further useful
rendered frames, correlating real shader/DMA work rather than advancing labels.
The previously observed separate job-guard issue is not claimed fixed.

Final build 063: portable 21/21, eight native checks PASS (including the two
new LFQueue/SPURS-event regressions), interpreter HALT expected exit 8.
Executable SHA-256:
`4c42fb58d2fd090cb018b90063dd39a88b54491bf3ba12b09aecb55a7f3aa91f`.
All game processes are stopped. Changes remain local and uncommitted.

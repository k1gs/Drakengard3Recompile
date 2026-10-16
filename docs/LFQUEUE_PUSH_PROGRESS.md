Latest continuation: `SPURS_EVENT_CORRUPTION.md` confirms and fixes resource-table corruption from synthetic SDK job events. Previous NULL-resource blocker below is historical. Menu remains unvalidated.

# LFQueue publication and real image-42 completion — 2026-10-15

The former `_cellSpursLFQueuePushBody` exit-6 barrier is replaced by an
implementation of the observed mode-3 ANY2ANY path. Changes are local;
no commit or push was made. Menu, controls and gameplay remain unvalidated.
The NULL resource receiver persists after real LF completion.

## Protocol derived from the local game consumer

Image 42, fingerprint `00E729804474449A`, guest ELF `017A9780`, lift
`work/full-spu-001/spu_0001_at_01799780/spu_recomp.c`. Directory suffix is
file offset, not guest VA. No GPL implementation was copied.

| Queue offset | Observed ANY2ANY field |
| --- | --- |
| `+0/+2/+4/+6` | BE16 completed pop, completion bitmap, waiter control, reserved pop |
| `+8/+A/+C/+E` | BE16 completed push, completion bitmap, waiter control, reserved push |
| `+10/+14` | BE32 element size/depth |
| `+18` | BE64 buffer, low bit tagged |
| `+30..+4F` | Pop waiter tokens |
| `+50..+6F` | Push waiter tokens |

Pop entry `09CB0` calls reservation `0A238`, using GETLLAR/PUTLLC and `+6`.
`0ACC8` DMA-reads the selected slot. `09AD8` waits for DMA and completes pop
via `0A6C8`. Completed pop at `+0` advances before record processing; pop
completion is not proof of work completion.

Waiter control contains three 5-bit counters, wrapping at 30: queued in bits
0..4, acknowledged in 5..9, issued in 10..14. Its 15 reusable token slots are
at `+32 + 2*(counter % 15)`. On empty, the consumer registers its token and
calls WAIT_SIGNAL(2) through `09C30`, link `09C80`, work object `queue|5`.
After wake it acknowledges the signal and retries reservation.

PPU holds the shared lock-line across reservation, slot copy and BE16
publication, and calls `spu_coh_notify_write` for payload/queue lines. It issues
a signal for a token actually registered in the guest ring after unlocking.
It never sets work-completion EventFlags or creates resource results.

## Explicit scope and identity bridge

Supported: observed mode 3, ANY2ANY, serialized PPU producers and one image-42
consumer per queue. Full producers block until actual SPU pop completion.
Other modes/directions, outstanding split-phase SPU producers, ambiguous
consumer identity, multiple/unknown waiter tokens stop explicitly.

The pinned runtime assigns workload ID 0 to all tasksets. The token alone
therefore cannot resolve its taskset. `prepare_spu_tasks.py` binds image 42's
queue argument, taskset and slot from its enqueue-time TaskInfo snapshot.
Publication resolves the guest token through that association, never through
the global last-created taskset. This is a scoped bridge, not general SPURS
workload-ID allocation. Service teardown/rebinding and multiple consumers are
not implemented. SDK sources were not edited; changes use build-local copies.

## Verification

Run `build\game\DrakengardRecompile.exe --check-lfqueue` from repository root.
The check loads image-42 constants from local EBOOT file offset `01799780`,
verifies the fingerprint, then executes its actual lifted reserve/DMA/complete
functions. It covers 65 sequential publications and cursor wrap; depth-16 full
queue and blocked seventeenth producer released by real SPU pop; exact FIFO
payloads; 31 blocking consumer wakes and waiter-ring wrap, including publication
racing the host park. Builds 057/058 pass all coverage. Portable tests: 21/21.
Six existing native checks pass on build 057; interpreter HALT exits 8 as expected.

Run 047/build 054: 90 seconds, previous push barrier passed, SPU frees slots and
re-enters waits. NULL resource query remains; two black renderer readbacks.

Run 048/build 056: 120 seconds, `PORT_LF_TRACE=1 PORT_ALLOC_TRACE=1`.
Successful image-42 PUTLLC records prove reservation/acknowledgement, pop
completion, then actual EventFlag `01A54E80` writes with word `00010000` from
LS link `03364`. Example: `reports/game-run-048-errors.log` lines 14930–14949.
A full 30-position waiter ring cycle is visible. These are SPU writes, not
PPU-generated completions. Correct decompression of every output is not proven.

Despite those completions, tid 21 still reaches NULL receiver in `00CB5B80`
(run 048 line 30569), then requests `0D07D180` bytes from stack-derived counts.
Both captured pixel buffers are all zero; the second was also visually inspected.
Runs 047/048 stopped at their time limits. LF publication alone does not solve
resource construction/lookup or rendering.

`PORT_LF_TRACE=1` logs at most 128 successful image-42 atomic commits.
`PORT_ALLOC_TRACE=1` also captures bounded NULL-query resource/handle/table
snapshots. Neither diagnostic changes guest results.

## Next step

Trace the handle consumed by `00CB5528`, its producer and lifetime. The function
requires `(handle >> 20) & 63 == 1`, then reads
`table + ((handle << 2) & 003FFFFC)`; table pointer is at TOC-`3F7C`.
The failing `00C04B0C` path loads the handle from
`*( *(object+3B8) + BC )`, object in r22. Verify the handle type and table entry
before changing resource logic. Repeat renderer readback after a real fix.
Do not clamp counts, grow memory, fabricate resources or advance GPU labels.
The separate 20-second job guard remains unresolved.

Final bounded run 049 / build 058, 90 seconds, confirms actual LF/EventFlag
commits again. The new NULL-query snapshot identifies a valid type-1 handle
`00100000`, table `01AC3218`, offset 0, **entry 0 = NULL**. Object `46489300`,
resource descriptor `40F89CD0`, tid 21. The handle-type check is not the failure
in this run; trace creation/population/lifetime of slot zero next. Snapshot is
at `reports/game-run-049-errors.log:34529`. Captures remain black. Process was
stopped at the time limit; no game process remains.

Final executable SHA-256:
`f2fdf4c61684ffa4648cb150f1d9f70b33353199c8973a6fbb017d7cb3a1d318`.

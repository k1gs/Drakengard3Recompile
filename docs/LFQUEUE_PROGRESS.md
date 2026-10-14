# LF queue blocker and large-allocation diagnosis вЂ” 2026-10-15

Publication checkpoint 2026-10-15: runtime changes are committed as `328e3e9`. Local/uncommitted status statements below describe the original capture time. Current blocker and visual limitations are recorded in `LFQUEUE_PROGRESS.md`.

There is still no validated menu or playable frame. This stage identifies the
large-allocation path, corrects fresh LF queue initialization, and stops at the
missing producer API before later failures obscure it. It does **not** implement
LF queue publication or establish that the resource failure is entirely caused
by that missing API. Changes remain local since published commit `87255ed`.

## Large allocation is downstream of a null resource query

Runs 041вЂ“043 / builds 047вЂ“049 trace the requested size before kernel allocation.
The large size is already present entering `0141A210` through `000107DC` and
`012EFED8`; it is not introduced by the kernel allocator's rounding.

Run 043's host backtrace was symbolized against that build's exe/PDB before the
next rebuild, using LLVM 20.1.8:

```
llvm-symbolizer --obj=build/game/DrakengardRecompile.exe --relative-address \
  0xA1DE511 0x6404315 0x84ACD5F 0x9C92AB8
```

The call sites resolve to generated files `ppu_recomp_110.cpp:13537`
(`00C04B0C`), `ppu_recomp_064.cpp:51104` (`00E062F0`),
`ppu_recomp_088.cpp:14667` (`012E5C98`) and `ppu_recomp_106.cpp:8222`
(`00C08414`). These RVAs are specific to build 049; do not symbolize them
against the current rebuilt executable.

The allocating caller uses the output word at frame+0x74, shifted left three
bits, as the array byte size. Immediately before this, `00CB5B80` looks up the
resource and invokes its method with output pointers frame+0x70/+0x74. The log
records a null receiver/target at that query. The SDK returns from the null call
without filling outputs, and the caller subsequently uses the stack words.
This explains why large requests differ between runs. Do not clamp the counts,
zero these outputs to fake a successful resource query, or increase the window
to absorb them. The resource-producing path must be completed.

`PORT_ALLOC_TRACE=1` remains an optional, bounded entry/host-stack diagnostic.
The experiment's unused outer-object sampling was removed. Saved LR alone was
insufficient to identify this allocation site because indirect calls do not
reliably update it in the lift.

## Confirmed earlier missing API

The earlier unresolved NID `8A85674D` is `_cellSpursLFQueuePushBody`.
Caller `00AC7BD8` passes a 32-byte stack record and mode 3. The caller logs an
error and continues; an error return does not safely terminate this path.
The new handler records the queue and input record and exits 6 explicitly.
It publishes no bytes, sends no signal and claims no successful task execution.

Run 044 / build 050 stops there after 61.90s:

- PPU thread 4, queue `4042AF00`, input `D0012CC0`, mode 3.
- Element size 32, depth 16, backing buffer `4042AF80`.
- Input words include completion event flag `01A54E80`.
- The second queue is `4043EE00`, depth 12, buffer `4043EE80`.

## Fresh initialization correction

The previous SDK initializer wrote the plain Spurs queue layout into the LF
queue: BE32 depth at +0x0C, untagged buffer, zero direction/sentinel fields.
The LF ABI instead has BE16 cursor groups and separate completion fields.
`port/queue_api.cpp` now implements only the observed **fresh ANY2ANY** init,
with range/alignment checks and rejection of nonzero queue memory. Other
directions stop explicitly rather than pretend that their protocol is covered.
Backing-buffer contents are preserved.

The field layout and initial sentinels were checked against pinned RPCS3
revision `6d7020d5d40bc3e0f1227f2eebb6a312d953c286`:
[cellSync.h](https://github.com/RPCS3/rpcs3/blob/6d7020d5d40bc3e0f1227f2eebb6a312d953c286/rpcs3/Emu/Cell/Modules/cellSync.h),
[cellSync.cpp](https://github.com/RPCS3/rpcs3/blob/6d7020d5d40bc3e0f1227f2eebb6a312d953c286/rpcs3/Emu/Cell/Modules/cellSync.cpp).
These GPL reference files remain ignored; no implementation was copied.
Their SHA-256 values are respectively
`d6e0e7ff6502d46efafe02db9fe3276bbd4fa8688cd83c697ab4d27f97b69dd5` and
`b3d02e46282852626520b41f28e02c4bdedf3a93d2170540dceeb3edea74932e`.
The ANY2ANY push-pointer function in this reference is itself TODO; its success
stub is not a working producer implementation to reuse.

Native `--check-queue` validates the entire fresh 128-byte LF line, tagged
buffer, adjacent canaries and preservation of backing bytes, alongside the
existing plain queue FIFO/wrap/blocking checks. Builds 051 and 052 pass.
Memory/task-snapshot/FIFO regression checks also pass; portable tests 21/21.

Run 045 / build 051 stops at the real missing push after 60.82s, exit 6.
Its LF snapshot has the corrected fields; an SPU changed a cursor halfword
at +0x04 before the first PPU push. This proves activity on the line, not that
publication or resource creation works. Early frame buffers remain zero.

## Next implementation

The consumer is image **42**, fingerprint `00E729804474449A`, ELF guest address
`017A9780`, local lift `work/full-spu-001/spu_0001_at_01799780` (filename uses
file offset, not guest VA). The two tasksets are `4048D380` and `4043D480`, each
slot 0, with its queue in argument word 0. The consumer reports SPURS syscall 2,
link LS `09C80`, with work object `queue|5`.

Follow image 42's reservation, slot copy, completion and wakeup protocol before
implementing the PPU publication path for mode 3. Use the shared lock-line and
coherence mechanisms; do not publish a plain BE32 queue index into this layout.
Verify PPU publish -> actual SPU consumption -> real event flag `01A54E80`
completion, then repeat the resource query and inspect frames. The unrelated
20-second job-guard timeout remains unfixed and is not established as this cause.

Game data, generated code, captures, logs and executables remain ignored.

Final build 052 / run 046: exit 6 at the missing LF push after 59.39s, no time-limit kill. Both captured pixel buffers are zero. Executable SHA-256 `81dd6bf88f61c0f305d9b102f87ff5e2a5478c6448888aad0a785714e9eac23e`. All bounded runs are stopped.

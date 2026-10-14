# SPU overlay execution — 2026-10-15

Publication checkpoint 2026-10-15: runtime changes are committed as `328e3e9`. Local/uncommitted status statements below describe the original capture time. Current blocker and visual limitations are recorded in `LFQUEUE_PROGRESS.md`.

The audio task now executes its relocated SPU code and repeatedly completes the
real audio EventFlag handshake. A usable screen or gameplay is not verified.
This stage extends `SPURS_QUEUE_PROGRESS.md`; all game/generated files remain
local and ignored.

## Two confirmed runtime problems

1. In run 021, image 41 branched to unlifted LS `0x37018`. The SDK halted the
   task and the job wrapper reported `RETURNED rc=0`. That was incomplete
   execution, not successful task completion. Build 024 / run 023 instead
   stopped with exit 8 and captured the full 256-KiB LS at that point.
2. The opt-in high-LS interpreter fallback used `spu_interp_run` without the
   guest return address. A codec returning to the middle of a lifted function
   could interpret the caller's continuation rather than returning to its host
   frame. A native reproduction sets r3=7 in a tiny callee, returns to an
   unregistered caller continuation which would set r3=99 and stop. Before the
   fix: FAIL, r3=99, pc=0x1000C, stopped. After: PASS, r3=7, pc=0x10004, running.

The build-local channels copy now calls `spu_interp_run_until` with the current
link register as the return boundary. Runtime-loaded high-LS code uses the
interpreter by default in this title build; `SPU_INTERP_UNLIFTED=0` disables it
for diagnosis. Unknown dispatch/interpreter instructions stop with exit 8 and
capture LS plus registers. Original SDK sources are unchanged.

This is a hybrid native/interpreter path, not a static lift of the relocated
codec. It does not establish complete SPU ISA accuracy. Interpreted code rejoins
existing lifted code through the runtime dispatcher.

## Origin of the code

The entire 8064-byte LS range `0x37000..0x38F80` in run 023 matches the executable
segment at file offset 0x80 of `spu_0027_at_018F8680.elf`, byte for byte. Its
original LS base is 0x80 and entry is 0x98; the loaded entry is 0x37018, relocation
delta 0x36F80. Segment SHA-256:
`79f61e2f4fa33e8d2a9ce1507894732ae6ba6d6727f413949f7ccce779f4bc14`.
Evidence: `reports/audio-overlay-origin-002.json`. A search in firmware `.pic`
files did not identify it; the embedded ELF comparison did. No new download or
firmware extraction is required. The existing original-base lift does not serve
these relocated addresses.

## Controlled runs

| Run | Behavior |
| --- | --- |
| 022, build 023 | Old interpreter fallback enabled explicitly; 90s timeout, zero audio flag WAKE lines, later audio EventFlag blocked. This extra inherited flag was not recorded by the old runner; command was `SPU_INTERP_UNLIFTED=1`. |
| 023, build 024 | Fallback disabled by default at that stage; exit 8 after 43.90s, exact missing-code LS captured. |
| 024, build 026 | Corrected return boundary, explicit fallback enabled; 90s timeout, 40 real EventFlag WAKE lines at 0x01B0EF80, no image-41 false completion. |
| 025, build 027 | Fallback enabled by default; 90s timeout, audio continues. Captured main-thread event wait and another Havok HALT condition. |
| 026, build 028 | Default fallback plus fatal HALT guard; exit 8 after 56.82s on image-54 assertion, LS and all 128 GPRs captured. |

Build 026 exe SHA-256:
`01882b3057577b83fc2323609b4840622a286dffa55a5d5568d140bdbfc706ff`.
Build 027 exe SHA-256:
`9c1dbfbc81f9e94fa4e1ea5002a9bfd58398187814c1cebd84a3899d157f3b16`.
Runs 024 and 025 each produced two renderer BMPs. All four have entirely zero
pixel data (`reports/frame-stats-overlay-001.json`): no usable image was captured.

Native regression: `--check-spu-return`, before/after logs in
`reports/spu-return-before.log` and `reports/spu-return-after.log`. Queue checks
still PASS (`queue-native-check-004.log`), portable tests 21/21
(`portable-tests-spu-return.log`).

## Next investigation

Event trace identifies queue 11 as a guest event object at 0x479E9390, held in
the two-slot structure 0x01A7A898. Its vtable is 0x016FAFD8. Poll OPD 0x0195ECC8
points to 0x008D9C94 (queue receive); signal OPD 0x0195ECB0 points to 0x008D9BE4
(event-port send). Main stack contains `008EF7A8 → 008C4378 → 008C5F10`.
This identifies the waiting object but does not yet prove its producer lost an
event. `PORT_EVENT_TRACE=1` logs four observations; `PS3_WAITBT=1` gives stacks.
The runner now records these inherited diagnostic options.

Image 54 also emits `HALT-ASSERT heqi` at LS 0x05098, then the old wrapper reports
rc=0. This was already present in run 021, so it is not established as a new
regression. Build 028 makes STOPPED_BY_HALT fatal, while normal SPURS exit uses
STOPPED_BY_STOP and remains separate. Generated PC can lag the exact instruction;
use the HALT-ASSERT log for the halt address and the captured registers/LS to
analyze its alignment checks. Do not bypass the halt or fabricate completion.

Run 026 captures local control block r84=0x1D780. At the assertion, r3=0,
r18 preferred word=0x47254508 (low nibble 8), r16=8 and r11=0. The code checks
both the allocation return and that control-block pointer for 16-byte alignment;
the second check fails. Other control-block words include 0x4816D5E4. These are
observed values; whether the producer wrote the wrong field, the job read the
wrong layout, or an allocator contract is wrong remains unresolved. Follow the
DMA origin of this block before changing alignment or forcing the assertion.

Build 028 SHA-256:
`f2a823889c3560f0de87f3142770e78e207a39045ea355849fc8ee3fee964a52`.
The native return regression still passes (`spu-return-final-002.log`). All
bounded processes are stopped; no background run is left active.

Final build 029 also traps interpreted HALT conditions, not just lifted ones.
Synthetic `--check-spu-interpreter-halt` reaches a real `heqi r11,0` and exits 8
as required (`spu-interpreter-halt-check.log`); the return check still PASS
(`spu-return-final-003.log`). Final exe SHA-256:
`0b4aaf75103681308aff13f80f55fb0616d604af0993a9b5f4db3b86982a7ef8`.
The full game trace remains run 026 / build 028; build 029 was checked with
focused native probes and does not have a separate full-game run.

```powershell
.\scripts\build-game.cmd
.\build\game\DrakengardRecompile.exe --check-spu-return
.\build\game\DrakengardRecompile.exe --check-queue
.\scripts\run-game.ps1 -Seconds 90 -Run FRESH_ID -CaptureFrames
```

The changes are local; latest published commit is still `87255ed`.

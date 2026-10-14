# GPU FIFO preservation — 2026-10-15

Publication checkpoint 2026-10-15: runtime changes are committed as `328e3e9`. Local/uncommitted status statements below describe the original capture time. Current blocker and visual limitations are recorded in `LFQUEUE_PROGRESS.md`.

The runtime discarded pending GPU commands when it encountered a self-JUMP.
This explains the misleading GET==PUT endpoint and a missing label release.
The title build now preserves the stream and waits for the actual producer
patch. A usable screen is still not verified.

## Evidence

Run 033 captures the actual IO mapping: IO 0x000E5FF4 → EA 0x400E5FF4.
The last submitted words before PUT 0xE61F4 are:

| IO | Word | Meaning |
| --- | --- | --- |
| 0xE61E4 | 0x00041D6C | semaphore offset, one argument |
| 0xE61E8 | 0x00000FF0 | label 255 |
| 0xE61EC | 0x00041D70 | backend semaphore release |
| 0xE61F0 | 0x00840400 | wire value, decodes to label 0x484 |

RenderingThread waits for 0x483 while the label remains 0x482. The newer 0x484
release would satisfy the game's comparison. The same trace records
`FIFO resync (jump-park) 0x000E28B4 -> put 0x000E61F4`: the runtime skipped the
range containing that release. `gcm_fifo_resync_why` explicitly moved GET to
the write head, optionally rescuing NV406E references but not the complete
command stream. Replaying only a fence would still discard rendering work.

Run 032's FIFO captures are **invalid for packet analysis**: the guest offset
table had not been populated, so the diagnostic read PPU code. Register/label
observations from that run remain separate. Build 035 fixes the snapshot to
use the runtime's active `gcm_io2ea` mapping, exposed by a read-only helper.
Original captures are preserved; do not treat run 032 as a valid GPU stream.

## Implementation and native reproduction

`scripts/prepare_gcm.py` creates a build-local `cellGcmSys_compat.c`. It removes
GET-to-PUT resync from park/unmapped/circling recovery and preserves GET when
the current IO offset has no mapping. The original SDK remains unchanged.
Other FIFO behavior, including existing ring recycling, is not a complete RSX
implementation and remains independently unverified.

`--check-gcm-park` uses the actual decoder on a synthetic stream: self-JUMP,
semaphore offset, backend release. It performs 12 parked passes (including the
stuck watchdog threshold), confirms GET and label do not advance, patches the
self-JUMP to NOP as a producer would, then requires the decoder to write label
0x466 and consume the stream. It never forces a completed label in game runs.

- Build 036, old decoder: FAIL, GET skipped to PUT, release lost
  (`gcm-park-before.log`).
- Build 037, preserved stream: PASS, held through all passes and executed the
  packet after the real fixture patch (`gcm-park-after.log`).
- Run 034: 95s bounded run. GET is now retained at a self-JUMP (late IO
  0xE5CA8, word 0x200E5CA8) while PUT advances to 0xEB6E0. No fake completion
  is published. The next task is identifying who should patch that park.

Run 034 exe SHA-256:
`2883280a050d4b0956f904bcf1ca6414ffc810ea660d9e38cbdf62042575a96f`.

## Diagnostic capture and continuation

`PORT_PPU_SAMPLE=1` samples the known RenderingThread and dumps 576 bytes around
PUT and at GET using the real mapping. Files and offsets are logged under
`work/native-runs/RUN/gpu-fifo-dumps/`; original game data is untouched.
The first region begins 512 bytes before PUT; the second begins exactly at GET.
These are best-effort concurrent snapshots, not an atomic GPU/PPU savestate.

Inspect the self-JUMP's source EA and follow its real PPU/SPU writer. A new PUT
does not prove the parked producer has finished. Do not skip the stream or set
the expected label manually.

```powershell
.\scripts\build-game.cmd
.\build\game\DrakengardRecompile.exe --check-gcm-park
$env:PORT_PPU_SAMPLE='1'
$env:GCM_DRAINDBG='1'
.\scripts\run-game.ps1 -Seconds 95 -Run FRESH_ID -CaptureFrames
Remove-Item Env:PORT_PPU_SAMPLE,Env:GCM_DRAINDBG
```

Builds 038/039 failed to replace the existing exe with a Windows sharing
violation (0x80070020). No Drakengard process was present; the lock holder was
not identified. Moving the exact old file to ignored
`build/game/DrakengardRecompile-build037.exe` preserved it and allowed build 040
to create the canonical executable. No unrelated process was killed.

Run 035 / build 040 confirms the preserved head: GET 0xE9690 maps to EA
0x400E9690, whose first word is the self-JUMP 0x200E9690. PUT is 0xEF0C8.
The same head is present in both late snapshots. Following words are genuine
GPU commands (0x000408E4, 0x00041FF4, 0x00041D60, then draw packets), not PPU
code. The producer still has not released this segment at the sampled times.
Next: trace writes/DMAs to the self-JUMP EA and correlate them with shader-job
completion. Lack of a patch alone does not yet identify a faulty producer.

Run 035 requested 95s, stopped by runner after 95.18s; no playable image was
established. Final executable SHA-256:
`455fa276bba711d71a8b9a87cdb033e4deb2e61a9d93b9d2f5d7eb9f884e8c1a`.
Native FIFO contract, task snapshot/dedup and interpreter return checks PASS
(`gcm-park-final.log`, `spu-task-gpu-stage.log`, `spu-return-gpu-stage.log`).
Portable tests remain 21/21 (`portable-tests-gpu-stage.log`). All bounded child
processes are stopped; no background continuation is active.

The stage remains local and uncommitted. Latest published commit: `87255ed`.

# SPURS queue stage — 2026-10-14

Publication checkpoint 2026-10-15: runtime changes are committed as `328e3e9`. Local/uncommitted status statements below describe the original capture time. Current blocker and visual limitations are recorded in `LFQUEUE_PROGRESS.md`.

`cellSpursQueuePopBody` now consumes real records published by the lifted SPU
producer. This advances the Havok startup path; a playable menu is not verified.

## Evidence and implementation

The PPU caller at `0xAD213C..0xAD21E0` passes queue, output, 0, 1 and treats any
nonzero return as fatal. The queue initialized by the pinned SDK has BE32 read
and write indices at +0/+4, element size at +0x10, depth at +0x14 and a BE64
buffer address at +0x18. The observed queue uses 16-byte entries and depth 128.
The disassembly stays in ignored `work/ppu-stage1/queue-pop-caller-001.txt`.

`port/queue_api.cpp` implements this observed path: shared SPU line lock, actual
payload copy, read-index advance modulo twice the depth and coherence write
notification. Empty nonblocking reads return task AGAIN without touching output;
blocking reads wait for a real producer update. Null, alignment, address bounds
and inconsistent ring state return task error codes. No synthetic completion
record or unrelated event is injected.

Limitations: r6 is inferred to select waiting from this caller; r5=1 is not
verified and stops if reached with data. Negative reservation indices retry.
The full waiter/task wake protocol remains unknown. This is an observed-path
implementation, not a complete SPURS queue replacement. RPCS3's pinned queue
pop implementation is itself a stub, so it did not validate this contract.

## Title-specific SDK adapters

The pinned runtime selects some legacy game adapters by numeric image IDs,
including 22 (CRI context/argument handling), 3 and 1/2. Generated Drakengard
ELF IDs 1..40 collided with that range. The build now creates a local registration
copy with IDs 41..80. Function tables and workload registrations move together;
fingerprints, function bodies, the closing image-0 sentinel and raw jobs 100/101
stay unchanged. The SDK and original generated registry are not edited.

`scripts/namespace_spu_registry.py` rejects changed/missing declarations and ID
collisions. This avoids known legacy branches; it does not prove all generic
runtime behavior is correct.

## Validation

- Run 016 recorded the original empty queue and caller arguments.
- Run 017 observed an actual producer advancing write index 0→1; the initial
  diagnostic implementation stopped on the first available record.
- Run 018 consumed records from five queues, with read indices 0→1→2→3→4.
- Build 021 failed configuration because the namespace validator omitted the
  image-0 sentinel. Run 019 consequently used build 020's old exe and is **not**
  evidence for namespaced IDs. Its manifest hash identifies that exe.
- Build 022 / run 020 uses IDs 41..80: five queues again consume real records.
  Bounded 90-second run, stopped by runner, exit -1; no OOM or missing-job stop.
  Later event receives and SPU WAIT_SIGNAL are observations, not a proven cause
  of a hang. Exe SHA-256:
  `7c2071cd84c065901151f5e4e6d8e95294775869e424b23a3d96d5437bfebe74`.
- Native `--check-queue`: empty-output canary, errors, FIFO, wrap and a delayed
  host producer PASS, including after the final address-bounds correction
  (`reports/queue-native-check-003.log`, build 023).
- Portable tests: 21/21 (`reports/portable-tests-queue.log`).
- Build 023 / run 021: another bounded 90-second run, forced exit -1. Renderer
  readback produced two 1280×720 BMPs. Both have all-zero pixel data; frame 001
  was also inspected visually and is black. No usable game screen was observed.
  Exe SHA-256:
  `549e26b70ff524947e3ae8404da2a0e5d2e3edbee999c7e0520722ad964351a7`.
  Late event receives include queues 9/10 at LR `0xEB3EA8` and queue 11 with
  timeout 100000 at LR `0x8EFE08`. Trace producers before changing these waits.

All bounded child processes are stopped. The queue stage is local, uncommitted;
the latest published resource-stage commit remains `87255ed`.

The increasing `CreateTask(id=...)` log value is a global diagnostic counter.
The actual returned ID is allocated per taskset slot; values above 100 alone
are not evidence of task leakage.

## Reproduce and continue

```powershell
.\scripts\build-game.cmd
# Continue only if the build exits successfully.
.\build\game\DrakengardRecompile.exe --check-queue
.\scripts\run-game.ps1 -Seconds 90 -Run FRESH_ID -CaptureFrames
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

`-CaptureFrames` enables the existing renderer's GPU backbuffer readback: at most
12 BMPs, every 120 eligible presents with at least one draw, inside the ignored
run directory. The manifest records these settings and the exe hash before
launch. This sampling can miss content; draw calls and black/empty samples do
not establish menu behavior. Captures and generated/game files stay local.

Next: inspect actual renderer output, correlate PPU event waits with their real
producers, and check input only once a usable screen exists. Do not bypass waits
by returning fabricated success.

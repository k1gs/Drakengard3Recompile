---
name: dod3-native-research
description: Investigate Drakengard 3 native recompilation failures in guest code, SPURS, FIFO, loading and timing using exact runtime witnesses. Use for this project, not PS2 recompilation or general GPU profiling.
---

# Native compatibility research

Read the current checkpoint and root `AGENTS.md`, then inspect Git status and
the relevant preparation script. Historical results cannot overrule newer ones.

Find the earliest observed divergence, not only the last blocked consumer.
For a queue stall, correlate its producer, ownership, arguments, counters and
publication order. For a guest fault, retain the image identity, PC/LR, relevant
registers, memory and input payloads. An LS dump alone is not a complete replay.
Do not reopen a resolved historical fault without an observed regression.

State a narrow hypothesis and its distinguishing observation before changing
code. Prefer a bounded trace or fixture reproducing the actual path over large
unfiltered logs. Keep each event tied to a task/draw/submission identity; wall
time or presented frame number alone may join unrelated work.

Repair the source of bad state. Do not synthesize task completion, replace guest
pointers, clamp away work or return success for unsupported behavior. Keep useful
unsupported-state failures observable. Changes to third-party code belong in
checked preparation scripts and build-local copies.

Validate with the relevant native fixture, portable checks and one bounded
runtime reproduction when needed. Record which levels actually passed: CPU
contract, real GPU, menu transition, story scene or interactive gameplay. Heavy
tracing and GPU capture timings are not clean FPS benchmarks.

Before running a game, use the runner lock and verify real processes. Respect
the hidden 300-second limit and audio setting in `AGENTS.md`. Confirm shutdown
before replacing the executable. Keep raw evidence ignored and summarize only
the finding, limitations and next test in the local checkpoint.

For rendered-frame problems, switch to `../dod3-frame-analysis/SKILL.md` rather
than extending a CPU-only hypothesis past the GPU boundary.

---
name: dod3-frame-analysis
description: Investigate Drakengard 3 missing geometry, material errors, D3D12 resource hazards and slow frames using GPU captures, exact draw inputs and producer-consumer traces. Prefer PIX for the verified D3D12 backend; this is not a PS2 rendering workflow.
---

# Frame investigation

Read `AGENTS.md` and the newest checkpoint. Run
`scripts/doctor-gpu-tools.ps1 -Json` to detect tools; do not assume installation,
successful capture or replay. For PIX, read `references/pix.md` only when needed.
Use an available equivalent only after verifying support for the active API.

Choose the measurement for the uncertainty:

- Missing draw: follow the guest command through recording, batching, PSO and
  actual GPU submission. Preserve topology, input offsets and shader identity.
- Draw submitted but invisible: inspect transformed positions, clipping, culling,
  scissor, depth/stencil, PS invocations and passed samples. Occlusion samples
  do not prove a correct material or final visible output.
- Draw passes but image is wrong: compare raw GPU target values immediately
  before/after that draw; separate RGB, alpha, finite values and row padding.
  Follow the first shader that actually samples that resource and its output.
- Slow frame: separate clean CPU/GPU timing from capture/validation overhead.
  Inspect compilation, resource churn, submissions, waits and uploads before
  calling the GPU the bottleneck.

Capture an event identity and its inputs together. A presented frame may contain
many submissions with the same frame number and reused draw indices. Join by a
unique capture tag or verified complete tuple, not by frame/index alone. Record
VS/FP code hash, constants, VB range, target and actual SRV/RTV resource identity,
format, dimensions, subresource, mapping and state. A CPU texture address or
nonzero descriptor is not evidence of what the GPU read.

When a resource is read and written, inspect channel usage, write masks and
ordering. A snapshot must preserve real GPU contents and the required boundary;
do not substitute stale CPU memory or disable game logic. Track ownership until
GPU completion and ensure descriptors reference the intended snapshot.

Treat approximate CPU HLSL evaluation as a diagnostic aid, not an original ISA
or GPU oracle. Likewise, capture shader edits are experiments, not shipped fixes.
After a supported change, verify the narrow invariant and adjacent behavior,
then reproduce through the normal renderer. Do not claim all artifacts fixed
because one draw works. Keep captures private and never automatically display
bright screenshots. Use the bounded hidden runner rules in `AGENTS.md`.

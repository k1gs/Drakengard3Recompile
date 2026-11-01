# PIX on Windows

PIX is the preferred capture tool for this project's native D3D12 backend.
Run `scripts/doctor-gpu-tools.ps1 -Json` first and inspect the installed
`pixtool.exe --help`; CLI syntax and export capabilities depend on the version.
Do not invent commands from an older documentation example.

GPU replay may require Windows Developer Mode. The doctor reports its state;
never enable it silently as a capture side effect. Obtain explicit permission
before changing that system setting. A capture can be created even when replay
is unavailable, so record those outcomes separately.

## Capture lifecycle

Use the project's bounded runner for the game, not an untracked PIX launcher
that bypasses the lock, hidden-window policy or watchdog. Capture is opt-in.
An already-running D3D12 process cannot simply load the GPU capturer late: load
the matching installed `WinPixGpuCapturer.dll` before any D3D12 initialization,
or launch with the supported PIX injection mechanism under runner supervision.
Never copy an arbitrary DLL beside the game or introduce a release dependency
on an absolute machine path. No silent fallback should be reported as capture.

This checkout supports `scripts/run-game.ps1 -PixCapture -Run <fresh-id>
-Seconds 300 -SkipMovies`. It detects the installed capturer, loads it before
D3D12 initialization and waits for `work/rsx-scene-<fresh-id>.trigger`. The usual
local scene controller can create that trigger after normal input, but inspect
its current scenario and timeout first; it is not a distributed dependency.
Otherwise create the trigger only after the scene is reached in an explicitly
authorized interactive test. The one-frame capture is written as
`work/native-runs/<fresh-id>/frame.wpix`. The ordinary runner process owns
shutdown; do not start another game from `pixtool launch` alongside it.

Request one Present-to-Present frame after the ordinary menu/loading transition
has reached the relevant scene. Partial offscreen submissions alone may omit
the consumer or final composition. Store captures under the ignored run folder.
Record executable hash, tool version, capture trigger and actual output file.
Verify a nonempty capture can be replayed before calling capture successful.
Capturer loading, capture creation and replay validation are separate milestones.

After the game is closed, use `scripts/analyze-pix-capture.ps1 -CapturePath
<file.wpix>` for a bounded hidden replay and event-list export. It writes logs,
`events.csv` and `analysis.json` under ignored `work/`; it does not open the PIX
UI or display a screenshot. Nonzero exit or an absent event list is a failed
analysis, not a successful replay.

## Inspect the captured frame

Start with the first incorrect target value. Use the event list to move from the
draw that produces it to the shader that consumes it. Inspect mesh/VS output,
pipeline state, depth, SRVs, RTVs, component mapping and the exact resource
subresource/version. Use pixel history or shader debugging where supported.
Apply GPU validation to the saved frame when live validation is too expensive.
Export only the needed event table, shader or raw resource. Screenshots alone
cannot establish draw ordering or correct resource bindings.

Separate correctness captures from timing captures. A capture replay or edited
shader can diagnose a problem but does not validate the normal native run.
If PIX cannot capture this path, report its exact error and use the project's
query/readback probes; never label a BMP or log as a PIX capture.

Official references, verify against the installed version:

- https://devblogs.microsoft.com/pix/programmatic-capture/
- https://devblogs.microsoft.com/pix/pixtool/
- https://devblogs.microsoft.com/pix/taking-a-capture/
- https://devblogs.microsoft.com/pix/gpu-captures/
- https://devblogs.microsoft.com/pix/download/

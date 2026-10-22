# Drakengard 3 — Native Windows Recompilation

An experimental native Windows port of Drakengard 3, based on static recompilation of the PS3 executable. The goal is to run as much game code natively as possible, with minimal PS3 compatibility machinery, using game files supplied by the user. The currently analyzed release is NPUB31251 v1.01.

## Current status

The native executable renders the Unreal intro and plays audio. The project includes a full PPU lift, 40 embedded SPU images, raw SPURS jobs, and 71 additional native functions for the streamed MultiStream DSP plugins. The DSP plugins are selected only when their runtime code matches the verified modules from the user's EBOOT.

**This is not yet a playable port.** Movie playback runs at approximately 30 FPS on the tested machine. The title screen has been visually confirmed by the user, and automated keyboard input reaches the main menu. After presentation fixes, a hidden test recorded approximately 27–30 FPS and 170 menu frames without the previously observed blue artifact or black flashes. Frame updates later stopped; prolonged stability, gameplay, and physical controller operation remain unverified.

The latest local build passed 25 portable tests and 23 native checks, including ordered clears, FIFO flip boundaries, deferred flip callbacks, and native/interpreter DSP comparisons. The experimental audio decoder remains disabled by default. These checks do not establish full game compatibility.

## Rendering progress

Actual captures from the experimental native Windows build. Performance numbers describe these tested intro sections, not gameplay.

### Native movie playback

![Native movie playback after fixing feedback from the previous framebuffer](assets/screenshots/native-movie-frame.png)

The movie now uses its actual YUV video textures. A screen-size heuristic previously replaced the luma texture with the previous rendered frame, causing persistent silhouettes and overlapping subtitles.

### Title screen

![Drakengard 3 title screen with the Start prompt](assets/screenshots/native-title-screen.png)

User-provided capture at approximately 29 FPS. The title screen is visible, but overlapping images and flickering in the menu remain unresolved.

## Test controls

The executable keeps its render window hidden by default. Visible tests require the explicit `-Interactive` option; F12 closes an interactive test, and the runner enforces the requested time limit. Hidden captures remain the default while rendering stability is investigated.

Focus the game window before pressing keys. The keyboard is used when no gamepad occupies port 0. Pass `-KeyboardOnly` to the runner to select it explicitly, even when a controller is connected. Keyboard bindings and fallback behavior are unchanged.

| Keyboard | Game action |
| --- | --- |
| Enter / Esc | Start; request a skip in skippable cutscenes |
| Space / Z | Cross / confirm |
| Backspace / X | Circle / back |
| Arrow keys | D-pad and left stick |
| A / S | Square / Triangle |
| Q / W | L1 / R1 |
| 1 / 2 | L2 / R2 |
| Tab | Select |

```powershell
.\scripts\run-game.ps1 -Run keyboard-test-001 -Seconds 300 -SkipIntro -CaptureFrames -KeyboardOnly
```

Choose a fresh Run ID each time. The test runner stops the game at the specified time limit. Cutscene skipping remains under the game's control; these keys send the real Start button, without bypassing game logic.

Add `-SkipIntro` (alias: `-SkipMovies`) to request automatic skipping. While the renderer detects a full-screen YUV movie, it sends short Start presses through the normal pad API. It releases Start between presses and stops when video frames stop arriving. This also applies to later prerecorded movies; engine-rendered cutscenes still require manual input. Menus receive no automatic confirmation presses.

## Gamepads

The Windows port now uses SDL2 GameController instead of an XInput-only backend.
SDL2 2.32.8 is pinned by commit and linked statically during the CMake build;
no separate SDL DLL or controller translation application is required.

Supported device families through SDL's native drivers and mappings:

| Family | Connection / behavior |
| --- | --- |
| Sony DualSense / DualSense Edge | USB and Bluetooth; touchpad click also sends Select |
| Nintendo Switch Pro, paired left/right Joy-Cons | USB / Bluetooth as supported by the device; Joy-Cons combine into one controller |
| 8BitDo and compatible Switch-mode controllers | SDL's Switch driver or built-in controller mappings |
| Xbox 360, One, Series and compatible XInput controllers | USB, wireless adapter or Bluetooth as supported by the device |

All standard PS3 controls are mapped: both sticks and stick clicks, D-pad,
four face buttons, shoulders, triggers, Start and Select. Sticks have a deadzone
and an exact neutral value of 128. Analog triggers retain their pressure;
Nintendo's digital ZL/ZR report released or fully pressed. Face-button pressure
is digital on these devices, as their hardware does not provide DS3 pressure.

| PS3 | DualSense | Xbox | Nintendo (physical position) |
| --- | --- | --- | --- |
| Cross (bottom) | Cross | A | B |
| Circle (right) | Circle | B | A |
| Square (left) | Square | X | Y |
| Triangle (top) | Triangle | Y | X |
| Start / Select | Options / Create or touchpad click | Menu / View (Start / Back) | + / − |

Up to four gamepads can connect while the game is running. Connected devices
retain their port when another device disconnects; new devices fill free ports.
Disconnecting releases buttons, centres sticks and clears the old rumble request.
The PS3 small/large motors map to SDL's high/low frequency rumble, with renewal
while the game polls and automatic stop if polling stops or the pad API shuts
down. Accelerometer and yaw gyro data are translated to PS3 sensor values when
SDL exposes those sensors; other devices keep neutral sensor values. The PS3
game supplies conventional rumble, not adaptive-trigger or HD-haptics effects.
In-game button art remains the original PlayStation art.

SDL's `SDL_GAMECONTROLLERCONFIG` and `SDL_GAMECONTROLLERCONFIG_FILE` overrides
can add mappings for unrecognized third-party pads. Using a translation layer
such as Steam Input may expose a virtual Xbox pad instead of the native device.

Validation: the standalone native test uses real SDL virtual joysticks to check
buttons, axes, pressure, rumble, unplug/replug, stable ports and reinitialization.
It does not emulate the USB/Bluetooth HID protocols. Physical Windows testing
of each device/transport, motion calibration and in-game play remain pending.

Build and run this contract without generated game code or game files:

```powershell
cmake -S tests -B build/input-tests -DPS3RECOMP_DIR="$PWD/third_party/ps3recomp"
cmake --build build/input-tests --config Debug
ctest --test-dir build/input-tests -C Debug --output-on-failure
```

For an offline build, also set `-DFETCHCONTENT_SOURCE_DIR_PORT_SDL2=PATH_TO_SDL2`
to an existing SDL2 2.32.8 source checkout.

## Quick start

Run from PowerShell in the project root:

```powershell
.\scripts\doctor.ps1
.\scripts\start-ghidra.ps1
```

Tool preparation and checks:

```powershell
.\scripts\bootstrap.ps1
.\scripts\build-spu-extension.ps1
.\scripts\build-runtime.cmd
.\scripts\check-toolchain.cmd
.\scripts\check-ghidra.ps1
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

Bootstrap downloads official archives, records revisions and SHA-256 hashes in `reports/`, and checks published digests for binary releases. LLVM is extracted with 7-Zip without running its installer. Global PATH and Java settings are not modified.

Create the Python environment with `python -m venv .venv`, then install the toolkit requirements with `.\.venv\Scripts\python.exe -m pip install -r third_party\ps3recomp\tools\requirements.txt`. Installed versions are recorded in `reports/python-lock.txt`; that file can be passed to `pip install -r` when recreating the environment.

RPCS3 is available locally at `tools/rpcs3/rpcs3.exe` for reference runs. Firmware, accounts, licenses, and game files are not included. Install firmware according to RPCS3's official instructions and provide your own prepared game files before a reference run.

Building the game executable also requires the locally generated PPU and SPU sources. These are intentionally excluded from Git; a fresh clone alone is not sufficient to build or run the game.

## Directories

| Path | Purpose |
| --- | --- |
| `input/` | User-supplied files, read-only during analysis |
| `work/` | Working copies, Ghidra projects, loader/lifter output, and generated code |
| `reports/` | Tool versions, hashes, build logs, and check results |
| `third_party/` | External tool source snapshots and their licenses |
| `tools/` | Portable tools, archives, and Gradle cache |
| `build/` | Local build output |
| `docs/` | Local research notes and handoffs (not tracked in Git) |

The primary repository is [k1gs/Drakengard3Recompile](https://github.com/k1gs/Drakengard3Recompile), on branch `main`. The original [GitLab repository](https://gitlab.thebakerc.at/k1gs/drakengard-recompile) is retained as a separate remote.

`.gitignore` excludes local research notes, `CURRENT_STATE.md`, local tools, working data, generated game code, crash dumps, and build output. `.gitattributes` defines line endings. The existing GitLab CI configuration runs portable intake/parser tests and requires a runner with container image support. The Windows runtime, Ghidra, and game are checked separately on the local machine.

Do not publish game images, DLC, licenses, keys, memory dumps, or generated game code. Review the files being published explicitly: ignore rules do not protect files already tracked by Git.

## Importing user-supplied game files

First confirm that the download has completed and been verified in the download client. Then run a one-time intake with a fresh report name:

```powershell
.\.venv\Scripts\python.exe scripts\inspect_input.py "PATH_TO_COMPLETED_IMAGE_OR_DIRECTORY" --download-complete --hash-all --out reports\intake-001.json
```

The script reads files, computes streaming SHA-256 hashes, identifies ELF/SELF/PKG files, and parses PARAM.SFO. It does not extract ISO images; a full file listing and metadata require a separate extraction into `work/`. It refuses to overwrite a report and detects size or modification-time changes while reading. Its output does not prove download completion or image authenticity.

Recompilation requires a valid plain ELF of the main executable and the game's file tree. For user-owned SELF files, use RPCS3's documented extraction/decryption workflow. Before lifting, inspect the executable's architecture, endianness, TOC, OPD, imports, PRX/SPRX libraries, and embedded SPU modules.

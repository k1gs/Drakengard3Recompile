<div align="center">

# Drakengard 3

### Native Windows port through static recompilation

A source-only project building toward a complete, playable PC version of **Drakengard 3**.

![Source: PlayStation 3](https://img.shields.io/badge/source-PlayStation_3-003791)
![Target: Windows x64](https://img.shields.io/badge/target-Windows_x64-0078D4)
![Game: NPUB31251 v1.01](https://img.shields.io/badge/game-NPUB31251_v1.01-6750A4)
![Stage: Active development](https://img.shields.io/badge/stage-active_development-D97706)

[Progress](#progress) &middot; [Screenshots](#rendering-progress) &middot; [Getting started](#getting-started) &middot; [Controls](#test-controls) &middot; [Contributing](#contributing)

</div>

> **Native port in development. Gameplay with working controls is not yet confirmed.**
> The Windows executable renders the intro, plays audio and reaches the main menu.
> New game loading now passes the previous SPURS initialization failure and reaches
> a story scene with advancing subtitles. Rendering stability and performance beyond
> menus remain active work.

## The project

The goal is a complete, playable native Windows recompilation of **Drakengard 3**.
The project translates the PS3 executable into native code and replaces platform
services where needed, with as much native execution and as little PS3 compatibility
machinery as possible. **Windows x64 is the target platform.**

Development currently targets **NPUB31251, version 1.01**. You supply the original game
files; this repository contains the port's source, preparation tools and checks.
Generated game code and local research notes stay outside version control.

## Progress

| Area | Verified milestone | Remaining work |
| --- | --- | --- |
| Recompilation & runtime | PPU code and embedded SPU modules execute natively on Windows | Broader runtime and game compatibility |
| Intro & audio | Rendering and playback confirmed; tested movie sections reach about 30 FPS | Validate later playback and transitions |
| Title & menus | Title screen and main menu reached through normal keyboard input | Rendering stability and extended interaction |
| New game loading | Corrected taskset initialization; reached a story scene with advancing subtitles | Validate subsequent transitions and loading times |
| Input | Keyboard test controls and an XInput backend | Physical controller validation and Switch-mode 8BitDo support |
| Gameplay & performance | A later scene continues rendering after a Start request | Confirm gameplay with control and improve performance beyond menus |

The latest loading fix preserves the shared taskset argument during SPURS initialization.
Previously it was discarded, leaving a task's memory allocator with a zero base.
A bounded, hidden five-minute test continued past that failure after the correction.

The latest verified checkpoint passed **26 native checks and 25 portable tests**.
These validate specific runtime contracts and tooling; they do not establish complete
game compatibility. A later scene measured roughly **5-6 FPS with diagnostic tracing
enabled**, which is not a normal-build gameplay benchmark. The optional native audio
decoder remains disabled by default.

## Rendering progress

Captures from the native Windows build. These show rendering milestones; they do not establish gameplay readiness.

**Title screen**

![Drakengard 3 title screen](assets/screenshots/native-title-screen.png)

*The title screen, captured at approximately 29 FPS during an earlier test.*

<details>
<summary>Movie playback - native YUV rendering</summary>

![Native movie rendering](assets/screenshots/native-movie-frame.png)

*Video rendered from its YUV textures. Correcting framebuffer feedback removed persistent silhouettes and overlapping subtitles from this path.*

</details>

## Getting started

**A fresh clone alone cannot build or run the game.** Building also requires your own game files and locally generated PPU/SPU sources, which are excluded from Git.

Run the environment check from PowerShell in the project root:

```powershell
.\scripts\doctor.ps1
```

Once the local tools, game tree, and generated sources are prepared:

```powershell
.\scripts\build-game.cmd
.\scripts\check-native.ps1
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

<details>
<summary><strong>Tool preparation and executable intake</strong></summary>

```powershell
.\scripts\bootstrap.ps1
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r third_party\ps3recomp\tools\requirements.txt
.\scripts\build-spu-extension.ps1
.\scripts\build-runtime.cmd
.\scripts\check-toolchain.cmd
.\scripts\check-ghidra.ps1
.\scripts\start-ghidra.ps1
```

Bootstrap downloads official archives, records revisions and SHA-256 hashes locally, and checks published binary digests. Portable tools do not change global PATH or Java settings.

For a completed, verified game image or directory, create a fresh intake report:

```powershell
.\.venv\Scripts\python.exe scripts\inspect_input.py "PATH_TO_COMPLETED_IMAGE_OR_DIRECTORY" --download-complete --hash-all --out reports\intake-001.json
```

Intake reads metadata and hashes files. It does not extract ISO images or prove image authenticity. Recompilation needs a valid plain main ELF and the game's file tree. For your own SELF files, use RPCS3's documented extraction/decryption workflow before analyzing and lifting the executable.

RPCS3 is used locally for reference runs. Firmware, accounts, licenses, and game files are not included.

</details>

## Test controls

Tests run **hidden by default** and close automatically at the requested time limit. Use a fresh Run ID for each test:

```powershell
.\scripts\run-game.ps1 -Run keyboard-test-001 -Seconds 300 -SkipIntro -CaptureFrames -KeyboardOnly
```

Add `-Interactive` to open a visible window. Focus it before using the keyboard; **F12** closes an interactive test. Rendering stability is still being investigated, so hidden captures remain the default.

| Keyboard | Game action |
| --- | --- |
| Enter / Esc | Start; request a cutscene skip |
| Space / Z | Cross / confirm |
| Backspace / X | Circle / back |
| Arrow keys | D-pad and left stick |
| A / S | Square / Triangle |
| Q / W | L1 / R1 |
| 1 / 2 | L2 / R2 |
| Tab | Select |

`-KeyboardOnly` selects keyboard input even when an XInput controller is connected. An 8BitDo in Switch mode is not supported by the current backend.

`-SkipIntro` (alias `-SkipMovies`) requests skips through ordinary Start presses while full-screen YUV movies are detected. It releases Start between presses and stops when movie frames stop arriving. The game decides whether a scene is skippable; menus receive no automatic confirmation presses. Engine-rendered scenes require manual input.

## Project layout

| Directory | Purpose |
| --- | --- |
| `port/` | Windows runtime, platform compatibility, rendering integration, and native checks |
| `scripts/` | Preparation, build, analysis, and bounded test runners |
| `tests/` | Portable tooling tests |
| `assets/` | Public screenshots |
| `third_party/` | External source snapshots and their licenses |

Local `input/`, `work/`, `tools/`, `build/`, `reports/`, and research notes stay outside version control. Game images, DLC, licenses, keys, memory dumps, and generated game code must remain local.

## Contributing

Focused work on native execution, rendering, platform services, input and regression
checks is welcome. For runtime reports, include the commit, game revision, reproduction
steps, first failure and relevant performance measurements. Distinguish intro, menu,
loading and gameplay results.

Changes should implement the real operation and demonstrate its failure cases.
Unsupported behavior must remain visible rather than being reported as successful
completion. Keep game files, generated game code, saves, memory dumps and raw captures
out of issues and patches.

## Source-only distribution

This repository distributes project source, tooling, tests and selected screenshots.
It includes **no game files, original executables, game licenses, keys or generated game
code**. Supply your own local inputs and keep them in ignored directories.

External source components retain their respective licenses. See the notices alongside
them, including [the ps3recomp runtime notice](port/LICENSE.ps3recomp.txt).

[GitHub repository](https://github.com/k1gs/Drakengard3Recompile) &middot; [Original GitLab repository](https://gitlab.thebakerc.at/k1gs/drakengard-recompile)

*Independent, unofficial project. Not affiliated with the game's rights holders.*

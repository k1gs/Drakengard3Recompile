<div align="center">

# Drakengard 3 Recompile

**Bringing Drakengard 3 to Windows through native static recompilation.**

![Platform: Windows x64](https://img.shields.io/badge/platform-Windows_x64-0078D4?style=flat-square)
![Approach: Static recompilation](https://img.shields.io/badge/approach-static_recompilation-8B1E2D?style=flat-square)
![Status: In development](https://img.shields.io/badge/status-in_development-D99A24?style=flat-square)

[Progress](#current-status) В· [Screenshots](#rendering-progress) В· [Build](#building) В· [Controls](#test-controls)

</div>

## The goal

A complete, playable native Windows recompilation of **Drakengard 3**. The project translates the PS3 executable into native code and replaces platform services where needed, with as much native execution and as little PS3 compatibility machinery as possible.

Development currently targets **NPUB31251 v1.01**. You supply the original game files; this repository contains the port's source, tools, and checks.

## Current status

| Milestone | Progress |
| --- | --- |
| Native executable | PPU recompilation and embedded SPU modules run on Windows |
| Intro and audio | Rendering and playback confirmed; tested movie sections reach about 30 FPS |
| Title and main menu | Reached through normal keyboard input |
| New game loading | Passed the previous task initialization failure and reached a story scene with advancing subtitles |
| Playable gameplay | Not yet confirmed |
| Controllers | XInput backend; Switch-mode 8BitDo support is pending |

The latest loading fix preserves the shared taskset argument during SPURS initialization. Previously it was discarded, leaving a task's memory allocator with a zero base. The corrected run continued past that failure during a bounded, hidden five-minute test.

**Work continues on rendering stability, loading times, performance beyond menus, and gameplay controls.** A later scene measured roughly 5вЂ“6 FPS with diagnostic tracing enabled; this is not a normal-build gameplay benchmark. The latest verified checkpoint passed **26 native checks and 25 portable tests**. The optional native audio decoder remains disabled by default.

## Rendering progress

Captures from the native Windows build. These show rendering milestones; they do not establish gameplay readiness.

### Title screen

![Drakengard 3 title screen](assets/screenshots/native-title-screen.png)

*The title screen, captured at approximately 29 FPS during an earlier test.*

### Movie playback

![Native movie rendering](assets/screenshots/native-movie-frame.png)

*Video rendered from its YUV textures. Correcting framebuffer feedback removed persistent silhouettes and overlapping subtitles from this path.*

## Building

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
.\scripts
un-game.ps1 -Run keyboard-test-001 -Seconds 300 -SkipIntro -CaptureFrames -KeyboardOnly
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

## Repository layout

| Directory | Purpose |
| --- | --- |
| `port/` | Windows runtime, platform compatibility, rendering integration, and native checks |
| `scripts/` | Preparation, build, analysis, and bounded test runners |
| `tests/` | Portable tooling tests |
| `assets/` | Public screenshots |
| `third_party/` | External source snapshots and their licenses |

Local `input/`, `work/`, `tools/`, `build/`, `reports/`, and research notes stay outside version control. Game images, DLC, licenses, keys, memory dumps, and generated game code must remain local.

[GitHub repository](https://github.com/k1gs/Drakengard3Recompile) В· [Original GitLab repository](https://gitlab.thebakerc.at/k1gs/drakengard-recompile)

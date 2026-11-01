# Drakengard 3 Recompile

Read the newest sections of `docs/NEXT_CHAT_PROMPT.md` before using historical
checkpoints, then inspect Git status and recent commits. Check current files and
tool availability rather than assuming an earlier experiment was completed.

## Research skills

- For guest-code, SPURS, FIFO, loading, timing or native compatibility failures,
  read `.agents/skills/dod3-native-research/SKILL.md` and follow its evidence workflow.
- For missing geometry, incorrect materials, render targets, GPU state or frame
  performance, read `.agents/skills/dod3-frame-analysis/SKILL.md`. Prefer an actual
  GPU capture when it can answer the question; use its PIX reference for D3D12.
- These skills apply automatically to matching work. Do not invoke unrelated
  skills merely because a tool or keyword appears in a log.

## Project boundaries

The immediate milestone is playable gameplay. The eventual goal is a complete
native PC port matching the original game, with minimal transitional PS3
compatibility code. Windows x64 is the current implementation target; keep
platform boundaries clean for future Linux work without implementing it now.
Future work includes 60 FPS, 1920x1080 and PC configuration. Defer comparison
against an untranslated game until the current build is substantially usable.

- Automated game tests are hidden, at most 300 seconds, with guaranteed cleanup.
  Use `scripts/run-game.ps1`, its exclusive `work/native-run.lock`, and verify
  real process termination outside a process-restricted sandbox before rebuilding.
  Visible gameplay or bright frame previews require the user's explicit request.
- Set `PORT_AUDIO_DECODER_NATIVE=0`. Use ordinary input to skip available movies
  and navigate menus. Never substitute success, guest pointers or skipped logic.
- Preserve unrelated dirty work, including `port/main.cpp` and local artwork.
  Do not reset, stash, stage or commit it with a focused fix.
- Do not edit `third_party/ps3recomp` directly. Change checked preparation scripts
  and generated build-local copies; test the actual generated implementation.
- Keep game assets, generated lifts, raw captures, dumps and research reports in
  ignored local directories. Publish only reviewed source changes. No force push.
- Distinguish intro, menu, loading, story scene and input-verified gameplay.
  FPS, sound, one image or successful unit checks do not establish playability.
- If delegating, use only the user-authorized Luna model for economical routine
  tasks. Do not silently substitute another model if it is unavailable.
- Keep progress concise. A short local checkpoint is enough; do not accumulate
  permanent investigation reports or edit README without a concrete need.

# Промпт для следующего чата

Скопируй текст ниже в новый чат, открытый в этой же папке проекта.

---

Продолжаем перенос Drakengard 3 (NPUB31251) с PS3 на Windows через статическую перекомпиляцию. Рабочая папка `D:\proj\drakengard 3 recompile`, PowerShell, Windows. Продолжай реализацию и проверку автономно. Цель — нормальное видимое intro, настоящее меню и управление, затем gameplay. Не останавливаться на плане или исследовании.

Сначала прочитай `docs/FIRST_NATIVE_IMAGE.md`, актуальные верхние секции `CURRENT_STATE.md`, `docs/HANDOFF.md` и этот файл. Проверь git status/log, существующие reports и build. Исторические секции ниже описывают уже пройденные blockers; они не являются текущим состоянием. Текущий checkpoint — коммит с сообщением `Render first native intro with real queue, jobchain and shader state` (найди hash через git log).

**Реально достигнуто:** build088/run073 дал узнаваемую красную заставку Unreal Engine в native GPU readback `work/native-runs/073/frame_002.bmp`, 1280x720. Run075 повторил её непосредственно в видимом окне, и пользователь сам подтвердил: «я это вижу». Пользователь слышит озвучку. В заголовке живого окна FPS около0.3, draws43; playback очень медленный и часто чёрный. Это intro, НЕ подтверждённые title/menu/gameplay. Не заявлять, что порт уже playable.

**Текущие файлы и результаты:**

- EXE `build/game/DrakengardRecompile.exe`, SHA256 `aaef3ac3ca2d0b58723605f68379e249deea9aa8e2defa0820a9394475a3c13a`.
- ELF `work/extracted/PS3_GAME/USRDIR/EBOOT.elf`, SHA256 `454fb591e9192cf9d4a5c65912eb4cd8c982ba52e26474bcfc9dc6ee1dc12acc`. Исходные игровые данные не менять.
- Build088 успешно слинкован, лог `reports/game-build-088.log`. После него изменялись только документация и runner для видимого окна, native tests перепроверены перед коммитом.
- Run073240s завершён по лимиту, frame002 узнаваемый, frame000/001 чёрные. Run074 завершился сам через250.98s с exit1, причина пока неизвестна; не считать его crash без доказательства (закрытие окна тоже возможно).
- Run075300s завершён по лимиту, exit-1, `interactive_window=true`; видимый логотип подтверждён через Computer Use. Автоматическая JSON строка validation универсальная и не учитывает отдельную визуальную проверку.
- Следующие свободные номера: build089, run076; перед использованием проверь, что файлов с этим номером ещё нет. Не перезаписывать reports/captures.
- Все игровые прогоны этого этапа остановлены. Проверь живые процессы перед новым запуском; не запускай несколько копий одновременно.

**Что уже исправлено (не возвращать старые обходы):**

1. LFQueue mode3 ANY2ANY: реальное reserve/copy/publication под общим SPU lock-line, coherence notifications и wake зарегистрированного image42 consumer из enqueue-time TaskInfo. Native fixture выполняет настоящие lifted reserve/DMA/complete:65 записей/wrap, full blocking producer,31 consumer wakes/ring wrap. Охват ограничен одним наблюдаемым consumer; неизвестные режимы/owners/split-phase producer fail-closed. `port/queue_api.cpp`, `docs/LFQUEUE_PUSH_PROGRESS.md`.
2. Удалены синтетические SDK job-completion events во все SPURS queues. Они очищали resource table slot0 при job321, порождали NULL receiver и вторичный огромный allocation. Настоящие SPU events по связанному порту сохранены. `scripts/prepare_spu_tasks.py`, `docs/SPURS_EVENT_CORRUPTION.md`.
3. Renderer использует build-local `rsx_d3d12_backend_compat.c`. Валидные VP constants больше не подменяются «garbage projection» матрицей; depth enable/write/compare записаны per draw и входят в PSO key. VP cache slots стабильны, pending draw references не сдвигаются memmove eviction. `scripts/prepare_renderer.py`, `docs/RENDERER_STATE_PROGRESS.md`, `docs/VERTEX_BINDING_PROGRESS.md`.
4. FP code-only hash теперь согласован с buffered decompiler/per-draw constants и export control. Реальная программа run071 длиной4816 bytes (200 instructions,101 constants,END4816) была усечена старым4096-byte окном. Decode/hash/collect теперь16KiB, bank512, stride8448. Fixtures72/151 проверяют последний slot. Не сокращать/клампить counts. `docs/FRAGMENT_CONSTANTS_PROGRESS.md`.
5. JobChain сохраняет cursor/return PC на END, не переигрывает обработанный префикс при Run, не теряет wake во время retirement. Реальные native append/branch и END/publication race fixtures PASS. `scripts/prepare_jobchains.py`, `docs/JOBCHAIN_RESUME_PROGRESS.md`.
6. PPU vpkswss: signed word→signed half saturation, canonical BE lanes, alias-safe dst, sticky VSCR.SAT.11 guarded sites в lift units103/109 заменяются воспроизводимым скриптом `scripts/prepare_ppu_vmx.py`; исходники сохраняются в ignored `work/full-ppu-001/compat-before-vpkswss`. `port/ppu_vmx_checks.cpp` + portable patch test.

**Проверки перед checkpoint:** portable22/22; vpkswss, jobchain-resume, fragment-constants, vertex-cache, vertex-constants, depth-state, lfqueue, spurs-events, queue, memory, spu-task-snapshot, spu-return, gcm-park, lwcond — все exit0. SPU interpreter HALT — ожидаемый exit8. Свежие логи `reports/checkpoint-*-088.log`, `reports/portable-tests-checkpoint-088.log`. Это не доказательство полного SPURS/render coverage или gameplay.

**Команды из корня проекта:**

```powershell
# Сборка: использовать новый номер, не затирать088.
cmd /c scripts\build-game.cmd > reports/game-build-089.log 2>&1

# Portable tests.
.\.venv\Scripts\python.exe -m unittest discover -s tests -v

# Native checks: каждый check запускает отдельный процесс.
$checks=@('vpkswss','jobchain-resume','fragment-constants','vertex-cache','vertex-constants','depth-state','lfqueue','spurs-events','queue','memory','spu-task-snapshot','spu-return','gcm-park','lwcond')
foreach($check in $checks) {
    & .\build\game\DrakengardRecompile.exe "--check-$check"
    if($LASTEXITCODE -ne 0) { throw "Failed: $check" }
}
& .\build\game\DrakengardRecompile.exe --check-spu-interpreter-halt
# Ожидаемый LASTEXITCODE=8.

# Видимое окно! Без -Interactive runner скрывает окно.
$env:PORT_PPU_SAMPLE='1'
$env:RSX_CAP_OFFSCREEN='1'
.\scripts\run-game.ps1 -Seconds 300 -Run 076 -CaptureFrames -CaptureRsx -Interactive
```

Runner всегда ограничивает запуск временем (максимум300s), затем останавливает child и сохраняет JSON/hash/env. -Interactive только делает окно видимым. Первые~минуты могут быть чёрными; в run075 логотип появился поздно. Не выдавать отсутствие изображения в первых sparse dumps за доказательство отсутствия кадра вообще. CaptureFrames сохраняет12 readbacks с шагом120 qualifying frames, поэтому при0.3FPS их мало. CaptureRsx даёт original VS/FP HLSL+ucode, inputs CB/VB/FPCB и raw textures (первые64 уникальных). RSX_CAP_OFFSCREEN фильтрует capture, не меняет rendering.

**Следующая работа:**

Сначала сохрани baseline и выясни, почему видимое intro идёт около0.3FPS: раздельно измерь PPU/SPU host work, JobChain worker creation, GPU submission/present/fences, shader D3DCompile/PSO compilation и runtime logging. В build есть bounded PORT_RENDER_TRACE и PORT_JC_TRACE, но они меняют timing; коррелируй с control run. Затем минимальное исправление подтверждённой причины и новый visible run, дойти до настоящего меню. Причина низкого FPS ещё НЕ установлена. Ненулевая геометрия/текстуры/слышимый звук сами по себе не доказывают корректный final frame.

Известные непроверенные области: общая SPURS workload-ID allocation, LFQueue teardown/rebinding/multiple consumers, JobChain shutdown/recreation и nested CALL beyond old single-return slot, отдельный20s jobguard, stencil/shared single depth target, alpha/unnormalized coordinates в старых backend helpers. Не чинить всё сразу без воспроизводимой связи с текущей проблемой.

**Инструкции работы:**

- Не подделывать HLE success, events, resources, GPU labels, completion, count clamps, shader colours/w; не обходить guest waits ради картинки. ISA saturation vpkswss — настоящая семантика, разрешена.
- SDK в `third_party/ps3recomp` не редактировать напрямую; checked compatibility scripts создают build-local copies. Эта папка может быть без собственного .git: `git -C` тогда покажет parent repo, не принять его hash за SDK revision. Зафиксированная версия SDK указана в существующих setup/handoff файлах.
- Игровые ISO/ELF/EDAT, lifts, SDK, EXE, PDB, captures, reports остаются ignored и не включаются в Git. Коммитить только проверенные source/scripts/tests/docs по явному запросу пользователя. В этом checkpoint push не запрошен и не выполнялся.
- Сохранять чужие dirty changes; не reset/stash/discard, не менять историю. Перед новым изменением проверить status и действующие AGENTS.md/skills.
- Для native UI использовать навык computer-use и @oai/sky через node_repl. Выбрать единственное реально возвращённое игровое окно, activate, screenshot; не автоматизировать консоль/Codex UI и не писать свой PowerShell SendInput/UIA. Клавиатурный fallback SDK: Enter=Start, Z=Cross, X=Circle, стрелки=d-pad/stick; требует foreground игры, XInput имеет приоритет. Меню/controls ещё не протестированы.
- Для хвоста растущих логов использовать Python binary seek/read[-N:], не Get-Content -Tail (здесь он однажды выдавал огромный поток). Для lifted chunks искать узко, Python mmap при необходимости.
- Общаться по-русски, коротко сообщать результат и следующий проверяемый шаг. Не просить повторных разрешений на уже разрешённую разработку. Не создавать другие чаты или subagents без явного запроса.

Начинай с проверки checkpoint и baseline производительности. Пользователь уже увидел первый настоящий кадр; сохрани этот прогресс.

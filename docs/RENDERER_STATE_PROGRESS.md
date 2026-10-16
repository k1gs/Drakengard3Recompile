# Renderer state, 2026-10-15

Меню и игровой кадр пока не подтверждены. После исправления SPURS resource-table corruption работа продолжается в renderer. SDK остаётся неизменным: `scripts/prepare_renderer.py` создаёт build-local `rsx_d3d12_backend_compat.c`, подключённый через `port/CMakeLists.txt`.

## Подтверждённые исправления

1. `vp_record_cb` подменял первые четыре vertex constants и viewport depth, если c00 не попадал в (0,8). У игры нормальная строка камеры начинается с -0.831469. Удалена эвристика, upload сохраняет весь guest bank и viewport depth. `--check-vertex-constants` вызывает настоящий upload; build064 до исправления FAIL для c00=-0.831469,0,16, build065 и069 после PASS: exact=1/depth=1.
2. Guest fragment PSO всегда использовал depth enabled/write ALL/LESS_EQUAL. Теперь enable, write mask и comparison фиксируются при записи каждого draw и входят в ключ PSO cache. RSX comparison 0x200..0x207 переводится в D3D12 1..8. `--check-depth-state` проверяет все32 комбинации, включая сохранение draw state после изменения текущего RSX state; build069 PASS.

Охват второго исправления — guest-FP pipelines. Fallback shaders и stencil state отдельно не исправлены. Backend пока использует общий depth buffer и очищает его при смене color RT; это остаётся ограничением и не считается корректным моделированием guest depth surfaces.

## Проверка изображения

- Run055/build063: main queue11 получает13 настоящих событий port8. `008D9C94` — polling receive(timeout100ms), `008D9BE4` — send по полю buffer+8. WAIT с LR008EFE08 сам по себе не означает потерянное событие.
- Run056/build063: diagnostic VP_NOFIXPROJ=1, итоговый composite чёрный. Эта переменная ещё не записывалась в run JSON; её установка зафиксирована здесь.
- Run057/build065: сохранённые camera constants уже не подменены; frame410 содержит172 ops, включая scene geometry. Main offscreen RTs и backbuffer нулевые; LUT0C190000 (256x16) содержит11520 ненулевых pixel bytes. Это не изображение игры.
- Run058/build065: контрольный DEPTH_OFF=1,120s. Геометрия и backbuffer всё ещё нулевые, ненулевая только та же LUT. Отключение глубины не устраняет симптом. DEPTH_OFF и RSX_CAP_OFFSCREEN в этом запуске унаследованы; старый runner их не записывал в JSON.
- Run059/build069:120s, без DEPTH_OFF, с правильным per-draw state. Frame373/172ops:80 draws depth enabled/write1/LEQUAL,18 enabled/write0/LEQUAL,74 disabled/write0/ALWAYS. Backbuffer/scene RTs чёрные, LUT ненулевая. Нельзя объявлять depth исправление причиной устранения чёрного кадра.
- Run060/build071:120s,27 source VP/ucode сохранены;22 BMP, ненулевые только две LUT copies. Scene VP56 instructions заканчивается MOV полного r3 в o0; старый SDK-комментарий о принудительном w=-1 к нему не относится. Build071 SHA256 `6e3c3919494b6cdf71fd22a43abedac7cffa45d1756d2d64508231902161782d`.

`-CaptureRsx` в `scripts/run-game.ps1` включает RSX_CAP=200:3:120 и отдельный каталог внутри ignored run. RSX_CAP_OFFSCREEN=1 выбирает batches с offscreen draws, не меняя рендеринг. Capture manifest содержит per-draw depth state. При RSX_CAP_DIR сохраняются исходные HLSL и ucode уникальных VP до диагностических shader rewrites. Runner теперь записывает VP_NOFIXPROJ, RSX_CAP_OFFSCREEN, DEPTH_OFF, CULL_OFF/CULL_FLIP в inherited_diagnostics.

Build072 дополняет capture: первые четыре offscreen geometry draws сохраняют CB bank+viewport epilogue и первые три uploaded vertices (все16 атрибутов). Используется настоящий cb_slot и vb_byte_offset с post-flip parity, а не предполагаемое равенство slot/draw index. Run061/frame00 op020..023 использует VP hash784000D6 (53 instructions). Ограниченный CPU evaluator source HLSL (`work/inspect_uploaded_vp.py`, результат `reports/vp-input-evaluation-061.log`) для этих12 вершин получает finite coordinates, но w примерно -2..-10 и y в диапазоне -143..114: эти образцы вне clip volume. Это не GPU shader trace и не доказательство неправильной ISA/камеры для всей сцены. Следующий эксперимент — проверить охват всех vertices/draws, происхождение camera/model matrices и затем FP/RT chain, не подменять w.

Build069: portable21/21; native LFQueue, SPURS events, queue, memory, task snapshot, SPU return, GCM park, lwcond PASS; interpreter HALT ожидаемый exit8. Vertex/depth fixtures PASS. Build066 остановился на проверке неоднозначного patch anchor;067/068 не перелинковались, пока шёл058. Build070: диагностический mkdir заменён на существующий Windows API;071 собран успешно.

Следующий разбор — реальные source VP/attributes и clip coordinates сцены, затем FP/RT цепочка. Не форсировать w/цвет/labels ради видимого результата. Shader dumps, screenshots, binaries и logs остаются ignored; commit/push не выполнялись.

Финальный checkpoint: build072 SHA256 `86aac60af9d2b20130143dea4c71b3687657f48d9338b66793fab8879fb48301`; новые vertex/depth fixtures PASS. Run061 остановлен по лимиту120s, процесс17736 отсутствует.22 BMP, ненулевые только две LUT copies, backbuffer/scene RTs чёрные. Все запущенные игровые прогоны завершены.

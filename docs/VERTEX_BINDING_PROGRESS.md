# Vertex shader binding, 2026-10-15

Найдено подтверждённое нарушение lifetime в renderer cache. SDK хранит16 VP entries; при переполнении освобождает entry0 и memmove сдвигает остальные entries. Pending D3D12DrawRecord содержит только vs_idx, поэтому этот индекс начинает указывать на другую программу ещё до submission.

Run061: при записи VS hash1AE388AA (56 instructions) помещён в slot5, draw op020 содержит vs=5. При позднем capture inputs этот же slot5 содержит hash784000D6 (53 instructions). Предыдущие результаты CPU evaluator для op020..023 относятся к подменённому VS, а не обязательно к программе, назначенной игрой. Вывод об ошибке камеры или ISA из отрицательного w отозван; источник matrices пока не установлен.

## Исправление

В `scripts/prepare_renderer.py` генерируется build-local cache со стабильными индексами:

- Pending non-clear VP draws закрепляют используемые slots до submission/reset batch.
- Вытеснение заменяет только незакреплённый slot, не сдвигая остальные.
- Если все slots закреплены, массив расширяется на16 entries. После завершения batch entries можно повторно использовать; размер определяется пиком одновременно используемых программ, а не произвольным увеличением фиксированного лимита.
- Record сохраняет vs_hash при назначении. Capture выводит recorded_hash и текущий hash, позволяя проверить identity.
- PSO cache уже включает slot и hash VS, поэтому повторное использование свободного slot не переиспользует pipeline другой программы.

Native `--check-vertex-cache` создаёт35 pending bindings (больше исходного cache), проверяет их сохранность после каждого добавления и reuse свободного slot при сохранении закреплённого. Build074 с исходным memmove-алгоритмом (только build-local helper заменён для контрольной проверки) FAIL/exit1; build075 с исправлением PASS/exit0. Build073 и076 также PASS. SDK, игровые shaders и guest matrices напрямую не изменены.

Capture первых четырёх offscreen geometry draws теперь сохраняет все их uploaded vertices, CB bank и viewport epilogue. Это диагностические ignored файлы, не изменение геометрии.

## Проверка в игре пока не завершена

Run062/build075120s: два начальных BMP чёрные, geometry capture не получен. Run063/build076180s с PORT_RENDER_TRACE и PPU snapshots: первые12 FP compile/PSO операций завершены успешно за0..31ms. Поздние samples показывают GET=DFCD0, PUT=E55A4, word в mapped EA400DFCD0 — self-JUMP200DFCD0. RenderingThread ждёт label45F/target460. Это отдельная точка ожидания реального производителя; нельзя пропускать команды или подставлять label. Пока нет основания утверждать, что stable VP bindings устранили чёрный кадр.

Следующее — корреляция park/patch с реальными SPU DMAs и затем захват geometry с recorded_hash==bound hash. CPU evaluator на uploaded vertices остаётся ограниченным анализом сгенерированного HLSL, не hardware/ISA conformance.

Run064/build076180s с GPU writer trace: GETE5F28/PUTEB7FC, ожидаемый patch400E5F28 не наблюдался, geometry capture не получен. Run065/build077120s с дополнительной JC trace продвинул FIFO и дал geometry frame369/158ops. У op020..023 recorded_hash и bound hash совпадают (1AE388AA,56 instructions). Это подтверждает shader identity в реальном batch. Backbuffer/scene BMPs остаются чёрными; ненулевая только LUT.

Ограниченный HLSL evaluator проверил все22974 uploaded vertices этих четырёх draws. Op020:96 внутри D3D clip volume,72 потенциально видимых треугольника; op021..023:0 таких треугольников. Все координаты finite. Полное отсечение геометрии не объясняет чёрный кадр. Результат `reports/vp-triangle-evaluation-065.log`; evaluator в ignored work/ не является GPU/ISA conformance.

JC trace показывает repeated Run while running и повторные проходы от entry. Это пока гипотеза scheduler gap, не реализованное исправление. В качестве независимого API reference просмотрен [RPCS3 cellSpurs.cpp](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSpurs.cpp): RunJobChain сигнализирует workload и будит SPURS. Само по себе это не задаёт END/resume semantics job-manager policy, поэтому дополнительный replay цепочки не добавлен без проверки.

Run066/build078120s: контрольный NO_ALPHATEST=1, captured geometry всё ещё чёрная. В выбранных scene draws alpha test и так выключен (alpha_ctl=00000700), поэтому этот переключатель не объясняет их чёрный цвет. Сохранены source FP HLSL и per-draw FP constants.

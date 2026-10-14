# Состояние на 2026-10-15

Код текущего этапа зафиксирован коммитом `328e3e9`. Проверки перед публикацией: portable 21/21, шесть native checks PASS, interpreter HALT — ожидаемый exit 8. Следующий blocker — `_cellSpursLFQueuePushBody`; игрового кадра нет. Упоминания local/uncommitted ниже относятся к истории предыдущих прогонов.

Последний этап: `docs/LFQUEUE_PROGRESS.md`. Большие allocations идут из stack counts после NULL resource query (00CB5B80 → 00C04B0C); точный host call site найден через PDB. Раньше в прогоне отсутствует _cellSpursLFQueuePushBody (8A85674D), mode 3 / q4042AF00 / tid4. Исправлена свежая ANY2ANY LFQueue init; native ABI/FIFO/memory/task checks PASS, portable 21/21. Push пока явно останавливает exit 6 с capture, а не продолжает к вторичному OOM. Следующее — publication/consumption protocol в SPU image42 (017A9780). Меню/кадр не получены; всё локально.

Текущий этап: `docs/MEMORY_REUSE_PROGRESS.md`. Подтверждён и исправлен OOM из-за отсутствия reuse освобождённых kernel pages: run 038 live 161.56 MiB при исчерпанном 256-MiB окне. Native regression до FAIL при zero live bytes / после PASS (4097 циклов, zeroing, double-free, 1-MiB alignment); portable 21/21. Run 039 проходит прежний малый запрос, затем получает отдельный запрос ~792 MiB. Рабочего изображения ещё нет; следующий разбор — происхождение большого размера. Изменения локальные, опубликован `87255ed`.

Финальный checkpoint: build 046, runs 039/040 завершены по лимиту 180/120s; кадры нулевые. Run 040: большой запрос уже 174 MiB, tid 21 / LR 008C2C54, saved stack через 00C04B0C+5F0. Размер между прогонами различается; причина ещё не установлена. Подробности и следующий эксперимент — в MEMORY_REUSE_PROGRESS.md. Все четыре native regression checks PASS.

Последний этап: `docs/GPU_FIFO_PROGRESS.md`. Подтверждён и исправлен пропуск pending GPU commands при self-JUMP/resync. Native FIFO contract: до FAIL, после PASS, включая 12 parked passes и продолжение после producer patch. Runs 034/035 сохраняют GET на настоящем self-JUMP вместо прыжка к PUT. Следующее — реальный writer/DMA, который должен исправить parked word (run 035: EA 0x400E9690). Картинка пока чёрная. Build 040, native task/return checks PASS, portable 21/21, все процессы остановлены. Изменения локальные.

Точная следующая точка (runs 029–031): RenderingThread 20, LR 0x008B0D18, GPU label 0x20000FF0 на один шаг позади target. FIFO в конце сообщает GET==PUT (run 031: 0xE9048). Проверить raw packet и publication/processing fence; label вручную не двигать. Build 033, все процессы остановлены. Подтверждённое исправление Havok и проверки — в `docs/SPURS_TASK_IDENTITY.md`.

Текущий этап: `docs/SPURS_TASK_IDENTITY.md`. Причина Havok HALT подтверждена: image 54 получил TaskInfo/args image 63. Исправлены явная передача taskset/slot, enqueue-time snapshot и running claim под общим lock. Native проверка двух tasksets + перезапись TaskInfo + duplicate enqueue PASS. Runs 027/028 на 90/120 секунд без прежнего HALT; кадры чёрные. Следующее — RenderingThread и main event wait. Более ранние сводки ниже описывают историю.

Обновление 2026-10-15: `docs/SPU_OVERLAY_PROGRESS.md`. Исправлен возврат из interpreter fallback для загруженного по новому адресу SPU кода. Audio EventFlag теперь реально просыпается (40 WAKE в run 024). Native regression до FAIL/после PASS; queue PASS, portable 21/21. Четыре новых renderer samples чёрные. Run 026 останавливается exit 8 на Havok image 54, HALT-ASSERT LS 0x05098: контрольный указатель 0x47254508 не выровнен на 16 байт. LS/GPR сохранены; причина происхождения указателя ещё не найдена. Штатный SPURS exit отделён от аварийного HALT. Изменения локальные, последний опубликованный commit `87255ed`.

Последний этап: `docs/SPURS_QUEUE_PROGRESS.md`. Реализован наблюдаемый путь `cellSpursQueuePopBody`: пять очередей получают реальные записи SPU, FIFO/wrap/blocking producer проверены native handler. ELF image IDs 41..80 исключают коллизии с известными адаптерами SDK; raw jobs 100/101 сохранены. Run 020: 90s без OOM/MISS, завершён по времени; меню не подтверждено. Portable tests 21/21. Следующие проверки — изображение и источники event waits. Resource stage опубликован как `87255ed`; queue stage пока локальный. Абзацы ниже описывают предыдущий этап.

Собран полный Windows x64 PPU lift Drakengard 3: 51 057 функций, 113 chunks, 40 встроенных SPU ELF и raw SPURS jobs. Native запуск читает EDAT/config, основные cooked packages и animation data, исполняет shader job и создаёт audio/physics tasks. D3D12 draw calls есть, меню/корректный игровой кадр/управление/прохождение пока не подтверждены. Актуальный отчёт: `docs/RESOURCE_API_PROGRESS.md`; предыдущий этап — `docs/FULL_NATIVE_BOOT.md`.

Исправлены link `dxguid`, title ID, lwcond layout, trophy BE32, нормальная передача аргумента `sceNpDrmIsAvailable2` в verified license-3 EDAT decoder, OVIS size/error ABI для resident-only/invalid images и лимит kernel allocation records 1024→4096. В штатном resource API проверяются dev_hash/block MAC; ключи не логируются/не сохраняются. Plaintext cache и raw captures только в ignored work/. Coalesced config: 14 файлов, 188 секций, 3595 пар. Raw jobs: 469 и 13 lifted functions. Последний run 015, 90s: 469+3 HIT, rc=0, OOM нет; завершён по времени. Следующий HLE gap — `cellSpursQueuePopBody` 0x91066667, SPU event wait 0x01B0EF80 и task lifecycle. Реальный кадр/меню ещё нужны.

PPU: 3 309 unsupported/non-code sites guarded при исполнении; SPU unsupported и raw-job MISS тоже переводятся в stop. Это не доказательство корректности остальных ISA/HLE путей. lwcond wait остаётся poll-style, allocator — bump без reuse. Portable tests 19/19; native EDAT 6/6, SPU ELF 7/7 + actual image, memory reproduction before FAIL/after PASS. Полная сборка/boot проверяются отдельно.

Полное дерево извлечено в `work/extracted/`; 8 783 исходных пути и размера совпали с ISO listing, plain ELF добавляет один рабочий файл. DLC не устанавливались. Firmware 4.93 установлена локально. RPCS3 headless static-interpreter reference дошёл до GCM/RHI, SPU и FIOS, остановлен через 60 секунд; GUI reference ещё нужен.

Первая инфраструктура опубликована как `3ea74f0`, полный native boot — `e84f735`; новые resource/SPU/memory fixes подготовлены к публикации по запросу пользователя. Игра, firmware, отчёты, инструменты, generated PPU/SPU, plaintext, captures и executable в Git не включаются. Нижние разделы подготовки — исторические.

## Установлено локально

- Ghidra 12.1.4, Java 21.0.9 используется из существующей установки.
- GhidraSPU `c158884b3a9ac1607b9ee053ba43aeada3f33bed`, собран Gradle 9.4.1 и добавлен в переносимую Ghidra.
- LLVM/clang-cl 20.1.8, распакован без системной установки.
- RPCS3 0.0.43-20298 / `6d7020d5d40bc3e0f1227f2eebb6a312d953c286`, firmware 4.93 установлена; выполнен ограниченный headless reference run.
- ps3recomp `a2c20108af45174dbb7d3e2f87f6acaaef09c6b4`, runtime v0.11.0.
- Изолированное Python 3.10 окружение `.venv`; фактические зависимости в `reports/python-lock.txt`.
- Используются существующие MSVC 14.41.34120, Windows SDK, CMake и Ninja.

Официальные бинарные архивы Ghidra/LLVM/RPCS3 проверены по SHA-256 из GitHub release metadata. Ревизии и хеши источников записаны в `reports/bootstrap-*.json`.

## Выполненные проверки

| Проверка | Результат | Доказательство |
| --- | --- | --- |
| Сборка runtime clang-cl/Ninja, RelWithDebInfo | Успех, `build/runtime/ps3recomp_runtime.lib` | `reports/runtime-build.log` |
| CTest runtime | 6/6: sync_stress, cond_owner, cond_recursive, cond_spurious, fiber_switch, vdec_stub | `reports/runtime-build.log` |
| PPU conformance | 1533 passed, 0 failed, 0 skipped | `reports/toolchain-check.log` |
| PPU branches, SPU RI10, custom header | Успех | `reports/toolchain-check.log` |
| Сборка GhidraSPU | Успех | `reports/spu-extension-build.log` |
| Ghidra headless import/disassembly | PPU `li r3,0x1` и SPU `stop 0x0`, искусственные raw-байты | `reports/ghidra-smoke.log`, маркеры LAB_SMOKE_OK |
| Intake parser | 3 теста: big-endian PPU ELF, SFO, повреждённые заголовки/границы | `reports/intake-tests.log` |

Проверка Ghidra не проверяет качество декомпиляции реальных PS3-функций. `vdec_stub` проверяет stub, а не декодирование роликов. Conformance имеет ограниченный охват: например, журнал явно отмечает немоделируемый FPSCR. Сборка upstream содержит предупреждения; они сохранены в логе. Полный upstream torture suite требует отдельный PPU toolchain/fixtures и здесь не выполнялся. Интерактивная GUI-проверка RPCS3/Ghidra и запуск игры не выполнены.

## Следующее действие после загрузки

1. Зафиксировать образ новым intake-отчётом и SHA-256, извлечь в рабочую область, подтвердить PARAM.SFO и редакцию.
2. Подготовить plain ELF и эталонный запуск RPCS3.
3. Построить карту PPU/SPU/импортов и оценить пробелы runtime до полного lift.

План и критерии: `docs/PORTING_PLAN.md`. Около 46 GiB свободно на D: после первоначальной подготовки; перед извлечением и крупными дампами проверить запас снова. На этом этапе коммитов и публикаций ещё не было.

## Подключение GitLab

Локальная папка подключена к `https://gitlab.thebakerc.at/k1gs/drakengard-recompile.git`, ветка `main` отслеживает `origin/main`. История начинается с `429bf83` (`Initial commit`, шаблон README). Подготовлены новый README, `.gitattributes` и `.gitlab-ci.yml` для stdlib-тестов intake. Наличие runner и серверное выполнение CI не проверены; локальные тесты проходят. Публикуемый состав ограничен нашими скриптами, тестами и документацией.

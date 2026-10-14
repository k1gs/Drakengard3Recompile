# Состояние на 2026-10-14

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

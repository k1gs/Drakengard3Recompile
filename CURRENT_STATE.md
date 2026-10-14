# Состояние на 2026-10-14

Подготовка инфраструктуры и первый статический разбор завершённой загрузки выполнены. Получен ELF, построены начальные карты PPU/SPU/импортов, разобран entry и скомпилирована ограниченная проба C++ в object-файл. Самостоятельного PC executable пока нет. Подробности и доказательства: `docs/FIRST_ANALYSIS.md`. Нижние разделы подготовки описывают состояние до первого разбора.

Полное базовое дерево извлечено в `work/extracted/`; все 8 783 исходных пути и размера совпали с ISO listing. Полученный ELF добавляет один рабочий файл. Последние проверки собственных парсеров: 7/7 (`reports/analysis-tests-002.log`). DLC не устанавливались. Firmware 4.93 присутствует; отдельный headless Interpreter (static) прогон прошёл до GCM/RHI, SPU и FIOS init и остановлен нами через 60 секунд. GUI/меню/игровой процесс не проверены. Установщик firmware отдельно дал assertion при teardown.

**Остановка исследовательской работы по просьбе пользователя из-за лимитов.** Тяжёлые задачи завершены или остановлены, автоматического продолжения нет. Для следующей сессии читать `docs/HANDOFF.md`, затем `docs/FIRST_ANALYSIS.md`. По отдельному запросу пользователя скрипты, тесты и документация подготовлены к публикации в GitLab; main отслеживает origin/main. Файлы игры, firmware, инструменты, отчёты и сгенерированный код остаются локально.

## Установлено локально

- Ghidra 12.1.4, Java 21.0.9 используется из существующей установки.
- GhidraSPU `c158884b3a9ac1607b9ee053ba43aeada3f33bed`, собран Gradle 9.4.1 и добавлен в переносимую Ghidra.
- LLVM/clang-cl 20.1.8, распакован без системной установки.
- RPCS3 0.0.43-20298 / `6d7020d5d40bc3e0f1227f2eebb6a312d953c286`, firmware не устанавливалась, игра не запускалась.
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

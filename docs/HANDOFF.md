# Передача работы — Drakengard 3, 2026-10-14

Пользователь попросил закруглиться из-за исчерпания лимитов и уделить внимание документации. Работа остановлена на первом исследовательском этапе; фоновые продолжения и автоматизации не создавались.

## Что читать сначала

1. `CURRENT_STATE.md` — актуальная сводка; нижние разделы подготовки исторические.
2. `docs/FIRST_ANALYSIS.md` — факты, адреса, хеши, эксперименты и ограничения.
3. `docs/PORTING_PLAN.md` — общий маршрут до самостоятельного PC executable.

## Подготовленный вход

- Оригинал ISO и пять DLC PKG: `NPUB31251-[Drakengard 3]/`. Не изменять, не публиковать.
- Полностью извлечённая базовая игра: `work/extracted/PS3_GAME/`.
- Plain PPU ELF: `work/extracted/PS3_GAME/USRDIR/EBOOT.elf`, SHA-256 `454fb591e9192cf9d4a5c65912eb4cd8c982ba52e26474bcfc9dc6ee1dc12acc`.
- Build: NPUB31251, APP_VER 01.01; SFO CATEGORY DG. Происхождение/оригинальность редакции не подтверждены.
- Firmware 4.93 уже есть в локальном RPCS3. Повторно устанавливать без причины не нужно: установщик имеет отдельный teardown assertion, но execution trace прошёл инициализацию.

## Карта для продолжения

| Артефакт | Смысл |
| --- | --- |
| `work/ppu-stage1/EBOOT.loader.json` | Entry OPD `0x196a428`, code `0xa8a020`, TOC `0x1981090` |
| `work/ppu-stage1/EBOOT.functions.json` | 30 142 входа из OPD, неполный список |
| `work/ppu-stage1/functions-expanded.json` | 45 796 эвристических диапазонов |
| `work/ppu-section-filtered-001/functions.json` | 45 778 диапазонов внутри SHF_EXECINSTR |
| `work/ppu-section-filtered-001/rejected.json` | 18 диапазонов вне code sections, проверить отдельно |
| `work/ppu-stage1/imports_named.json` | 251 импорт из 17 библиотек, 249 имён |
| `reports/title-audit-002.json` | 40 SPU ELF-кандидатов, 218 NID с кандидатами регистраций, 33 без найденной регистрации |
| `work/ppu-stage1/entry-ghidra-raw-001.txt` | 19 инструкций entry и гипотеза декомпилятора |
| `work/ppu-stage1/startup-a8a128-001.txt` | CRT-кандидат: 117 инструкций, TLS/atexit/PRX и вызов main-кандидата |
| `work/ppu-stage1/main-candidate-8c5f10-001.txt` | Main-кандидат: 45 инструкций, прямые и косвенные вызовы |
| `work/entry-lift-001/entry.obj` | Только compile probe, не executable и не проверенная семантика |

Не запускать entry compile probe: lifter пропустил вызов `0xa8a128`, находящийся вне 76-байтового среза. «253 functions lifted» включает 251 HLE-stub и 2 фрагмента entry, а не 253 восстановленные функции игры.

## Что делать следующим

1. **Сверить состояние и вход.** `git status --short`, `scripts/doctor.ps1`, проверить SHA-256 ELF и свободное место. Повторно распаковывать ISO или хешировать все десятки ГБ без изменения входа не нужно.
2. **Сформировать замкнутую стартовую цепочку.** Entry `0xa8a020` → CRT `0xa8a128` → main-кандидат `0x8c5f10`. Проверить прямые цели, косвенный вызов, OPD/TOC и функции initialization/finalization. Границы брать из filtered map и сверять дизассемблированием. Не пропускать неизвестные вызовы.
3. **Разобрать HLE-кандидатов вручную.** Проверить сначала SPURS queues/LFQueues, Ovis overlays, PRX loading, FIOS/FS. `33 без регистрации` — результат ограниченного статического сканера, а не доказательство отсутствия реализаций. Учитывать явные/условные регистрации и link-time inclusion. Для будущего ps3recomp execution использовать `PS3_HLE_UNRESOLVED=fail`, чтобы неизвестный вызов не подменялся успехом.
4. **Получить GUI-эталон RPCS3.** Нужны реальный кадр/меню, управление и чтение ресурсов. Headless run не заменяет это. Проверить, являются ли FIOS-UNREALENGINE3 cache misses обычными probes с fallback или блокируют загрузку. Не менять SFO, TOC, пути и проверки игры без подтверждённой причины.
5. **Только затем расширять lift.** Не включать весь RX PT_LOAD как PPU-код: там лежат данные и SPU. Опираться на исполняемые секции, отдельно исследовать SPU/overlays, контролировать unsupported/non-code diagnostics и выходы за диапазон.

## Повторяемые дешёвые команды

```powershell
.\scripts\doctor.ps1
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
.\.venv\Scripts\python.exe scripts\disassemble_function.py work\extracted\PS3_GAME\USRDIR\EBOOT.elf --sdk third_party\ps3recomp --functions work\ppu-section-filtered-001\functions.json --imports work\ppu-stage1\imports_named.json --address 0x8c5f10 --out work\ppu-stage1\main-NEXT.txt
```

Новые отчёты писать под новыми именами. Скрипты защищают существующие результаты от перезаписи. `analyze-entry.ps1` теперь использует проверенный raw-срез; его существующий report/project сначала сохранять, а для нового запуска задавать свежие имена в скрипте.

## Эталонные прогоны и известные ошибки

- `reference-smoke-001`: PPU LLVM, 120 секунд, остановлен во время компиляции.
- `reference-smoke-002`: невалидная попытка смены decoder (`Interpreter (fast)` не поддерживается), effective decoder остался LLVM.
- `reference-smoke-003`: валидный PPU `Interpreter (static)`, 60 секунд, Null video/audio; TTY подтверждает GCM/RHI, SPU и FIOS init. Процесс остановлен лимитом. Показ меню не подтверждён.
- Полный Ghidra ELF import остановлен после затянувшегося создания >100 address segments; причина неизвестна. Raw entry import/decompile успешно выполнен. Не повторять тяжёлый импорт без изменения подхода.
- UModel binary не скачался с официального сервера. UE3 prefix собственного ENGINE.XXX прочитан нашим скриптом: version 860, licensee 0; полная ресурсная совместимость не проверена.
- Установщик firmware: `Verification failed`, `fixed_typemap.hpp:220`, `manual_typemap` destructor, `process teardown`, код `0xC0000409`. Это отдельная ошибка завершения; не приписывать её игре.

## Проверки и репозиторий

Собственные парсеры: 7/7 unit tests. Runtime: ранее 6/6 CTest и 1533 PPU conformance checks; это не end-to-end проверка игры. PowerShell syntax и `git diff --check` проходили.

Origin: `https://gitlab.thebakerc.at/k1gs/drakengard-recompile.git`, main → origin/main; исходный коммит `429bf83`. После остановки исследования пользователь отдельно запросил публикацию подготовленной работы в GitLab. Публикуемый состав: наши скрипты, тесты и документация. Рабочие файлы, инструменты, firmware, отчёты, образы, лицензии и сгенерированный код игры исключены через `.gitignore`. Перед последующими публикациями проверять фактический staged diff и состав файлов.

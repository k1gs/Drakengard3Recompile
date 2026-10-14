# Первый разбор NPUB31251, 2026-10-14

## Зафиксированные входы

- ISO: 20 444 741 632 байт; SHA-256 `cf630b39d2fb84d3c63f91cc7181bb24c9b46f321c37403ba38c7e4f2d5a7f2e`.
- PARAM.SFO: TITLE_ID `NPUB31251`, TITLE `Drakengard 3`, APP_VER/VERSION `01.01`, CATEGORY `DG`, PS3_SYSTEM_VER `03.4000`.
- EBOOT.BIN: 10 173 008 байт; SHA-256 `93c01857566819de862451b7a78d6004a6e8d0dbac320ce684d1b01685d68258`.
- EBOOT.elf, получен RPCS3 `--headless --decrypt` на рабочей копии: 26 872 656 байт; SHA-256 `454fb591e9192cf9d4a5c65912eb4cd8c982ba52e26474bcfc9dc6ee1dc12acc`.
- Пять DLC PKG, включая три части японской озвучки, учтены и захешированы в `reports/intake-001.json`; не устанавливались.

Название папки и SFO не подтверждают оригинальность или происхождение сборки. Сочетание NPUB title ID с disc-категорией DG и ISO зафиксировано как свойство предоставленных файлов. Оригиналы не изменялись.

## Контейнер и ресурсы

ISO прочитан 7-Zip. Таблица содержит 8 783 файла, суммарно 20 433 131 090 байт: 8 663 `.XXX`, 3 `.TFC`, 95 `.BIK`, один `.UPK` и служебные файлы. Ролики занимают 10 161 934 128 байт; `.XXX` — 9 219 198 979 байт; `.TFC` — 986 565 669 байт. Внешних `.SPRX`/`.SELF` в перечисленном дереве нет; это не исключает динамической загрузки firmware-модулей.

Базовое дерево полностью извлечено в `work/extracted/`. 7-Zip завершился успешно; intake содержит 8 784 файла (включая отдельно созданный ELF), без ошибок. Пути и размеры всех 8 783 ISO-entries совпали с извлечёнными файлами, результат сравнения `reports/extraction-compare-001.json`. Это проверка структуры/размеров, не независимая проверка SHA-256 каждого ресурса. Оригиналы и DLC не изменялись.

`ENGINE.XXX` имеет стандартный Unreal package tag, big-endian prefix, file version 860 и licensee version 0. Общий prefix сообщает 19 645 names, 30 521 exports и 180 imports; их содержимое и объектная сериализация пока не разобраны. SHA-256 пакета `346e957c136d56fa15f3cb07e52f4ab023a1c4cea1cf51b50eb972887cd1aceb`.

Таблица PS3TOC содержит строки с путями к SHIPPING/TEST/symbols ELF, но сами эти файлы отсутствуют в ISO listing. Эти строки не означают, что нам доступны debug-символы. В ELF таблица имён секций заполнена нулями; символы исходной игры не восстановлены.

## PPU и SPU

PPU: ELF64 big-endian, EM_PPC64, EXEC. Два PT_LOAD:

| VA | Filesz | Memsz |
| --- | --- | --- |
| `0x10000` | `0x19099e8` | `0x19099e8` |
| `0x1920000` | `0x8d500` | `0x230698` |

Entry OPD `0x196a428` → code `0xa8a020`, TOC/r2 `0x1981090`. OPD extent `0x192d238..0x1979090`: 38 859 дескрипторов, 30 142 уникальных входа по loader.

Расширенный поиск `find_functions.py` дал **45 796 кандидатов функций**. Это эвристический список, не подтверждённое количество функций: RX-сегмент включает данные и SPU-образы, а границы требуют проверки. Уже найден важный конкретный пример неполноты OPD: entry вызывает `0xa8a128`, отсутствующий в списке loader, но найденный расширенным поиском.

Фильтрация по SHF_EXECINSTR оставила 45 778 диапазонов, полностью лежащих в исполняемых секциях; 18 вынесены в `work/ppu-section-filtered-001/rejected.json`. Четыре code ranges: `0x10200..0x1022c`, `0x10230..0x157b92c`, `0x157b92c..0x157b950`, `0x157b950..0x157f038`. Ни один из 40 SPU ELF-кандидатов не находится в этих секциях. Это ограничивает область PPU-lift, но не подтверждает семантику функций.

Найдено 40 встроенных ELF-кандидатов с machine SPU и допустимыми границами program segments. В отчёте сохранены file offsets, соответствующие PPU VA, размеры и хеши. Полная реконструкция, overlays, назначение каждого модуля и SPU-перекомпиляция пока не выполнены.

## Стартовый путь

В срезе `0xa8a020..0xa8a06c` Ghidra независимо прочитала 19 инструкций: восстановление TOC из OPD, стек, вызов `0xa8a128` и дальнейший вызов stub `0x157e890`, который NID-база именует `sys_prx_exitspawn_with_level`.

`0xa8a128..0xa8a2fc`: 117 инструкций по эвристическим границам. Видны обращения к `sys_initialize_tls`, `sys_time_get_system_time`, регистрациям PRX/atexit и вызов `0x8c5f10` с подготовленными аргументами. `0x8c5f10` — **кандидат main**, пока без восстановленного имени: его срез содержит косвенный вызов и несколько прямых вызовов, включая `sys_ppu_thread_exit`.

Все адреса выше — VA для указанного SHA-256 ELF. Тексты дизассемблирования и гипотезы декомпилятора лежат в игнорируемом `work/ppu-stage1/`.

## Проба перекомпиляции

76-байтовый entry-срез переведён lifter в C++ и **скомпилирован clang-cl в object-файл**, без линковки и исполнения. Вывод включает 2 фрагмента entry и 251 HLE-stub, поэтому число «253 functions lifted» не означает перекомпиляцию 253 функций игры.

Существенное ограничение: вызов `0xa8a128` вне среза отмечен lifter как `bl -> non-code` и пропущен. Такой результат нельзя запускать или представлять как работоспособный startup. Проба проверяет только обработку среза и компилируемость. Для следующего шага требуется замкнутое покрытие вызываемых функций и проверка каждого пропущенного/неподдержанного пути.

## HLE — кандидаты, не доказанная поддержка

251 импорт, 17 библиотек; NID-база дала 249 имён. Первый проход только по generated-table обнаружил 188 кандидатов. После включения явных регистраций в scaffold/runtime получено **218/251**; для **33 NID** сканер не нашёл регистрации. Отчёт `reports/title-audit-002.json` заменяет первоначальную неполную оценку `title-audit-001.json`.

Статические вхождения регистраций не доказывают подключение к конкретной host-сборке, порядок вызовов, ABI или семантику. Макросы и косвенная регистрация могут не распознаться. В частности, сетевые offline-handlers не обеспечивают полноценную сеть.

Приоритет проверки: SPURS queue/LFQueue, Ovis overlays, PRX loading и корректная файловая модель для SdataOpenByFd. Не подменять отсутствие обработчиков безусловным успехом. В выбранном upstream runtime unresolved HLE по умолчанию возвращает CELL_OK; при будущих диагностических запусках явно задавать `PS3_HLE_UNRESOLVED=fail` и сохранять first-failure trace. Это не относится к эталонному RPCS3.

## Ограничения и препятствия

- Полный импорт ELF в стандартный Ghidra ELF loader создал более 100 address segments и затянулся; процесс остановлен. Причина не установлена. Отдельный импорт небольшого raw-среза по исходному VA успешно завершён. PS3 ABI/TOC в Ghidra не считается автоматически восстановленным.
- Официальный UModel binary с gildor.org не удалось скачать из этого окружения; ресурсы через UModel не проверялись. [Таблица разработчика](https://www.gildor.org/projects/umodel/compat) перечисляет Drakengard 3, но это не заменяет проверку наших файлов.
- Firmware скачана с фактической HTTP-ссылки на [официальной HTTPS-странице Sony](https://www.playstation.com/en-us/support/hardware/ps3/system-software/). HTTPS на CDN дал hostname mismatch; TLS-проверка не отключалась. MD5 совпал с digest в официальном URL, SHA-256 PUP `158471fd834f8ea8036136b6aab43cd86c7ba73d79ca30e0af3c0fe0001cf365`.
- Установщик RPCS3 записал успех firmware 4.93, файлы `dev_flash` и `version.txt` присутствуют, но процесс затем завершился `0xC0000409`. Отдельный interpreter-прогон позже прошёл до инициализации GCM/RHI, SPU и FIOS; ошибка завершения установщика при этом остаётся неустранённой.
- Stderr уточнил симптом: `Verification failed` в `fixed_typemap.hpp:220`, destructor `manual_typemap`, `Emulation object is unavailable (process teardown)`. Это подтверждает assertion при завершении установщика; причина assertion пока не установлена. Наличие 144 external firmware-модулей и version 04.9300 проверено отдельно.
- GUI, графика, звук, управление, меню и игровой процесс пока не проверены. Самостоятельного PC executable нет.

## Доказательства и воспроизведение

Первый headless trace `reference-smoke-001` использовал штатный PPU LLVM, Null video/audio; за 120 секунд процесс ещё выполнял компиляцию и был остановлен нашим лимитом. RPCS3 распознал firmware 4.93, title NPUB31251 и импортировал executable. Это не проверка выполнения игры.

`reference-smoke-002` сохраняется как невалидный эксперимент смены decoder: RPCS3 отверг старое имя `Interpreter (fast)` и оставил LLVM. В исходниках установленной ревизии `6d7020d5` подтверждено имя `Interpreter (static)`; wrapper исправлен и теперь отдельно проверяет `invalid enum or integer`. Ошибка в настройке не скрыта и не считается подтверждением interpreter execution.

**`reference-smoke-003` — валидный ограниченный execution trace.** Effective config: `PPU Decoder: Interpreter (static)`, Null video/audio, ошибки enum отсутствуют. За 60 секунд TTY зафиксировал завершение GCM/RHI init, начало SPU init и FIOS 1.3.5 initialization. В логе есть файловые обращения, синхронизация потоков, память и RSX context calls. Процесс остановлен нами по лимиту; `exit_code=-1` относится к принудительной остановке, не к самостоятельному crash.

При этом есть `CELL_ENOENT` для путей кеша `/dev_hdd0/game/NPUB31251/USRDIR/FIOS-UNREALENGINE3/...`, включая STARTUP/CORE/ENGINE.XXX, и `RSX: Display queue was discarded while not empty`. Это наблюдения headless-прогона. Они не доказывают потерю исходных ресурсов или причину сбоя: все файлы базового дерева сверены, а fallback/cache-поведение ещё не прослежено. Настройки путей и проверки игры не патчились. Меню, кадр и игровой процесс не подтверждены.

Артефакты: `reports/reference-smoke-003.json`, `reports/reference-smoke-003-config.yml`, `reports/reference-smoke-003.log`, `reports/reference-smoke-003-errors.txt`, `reports/reference-smoke-003-tty.log`.

Локальные отчёты: `intake-001.json`, `extracted-intake-002.json`, `iso-listing.txt`, `ppu-loader-001.log`, `elf-layout-001.txt`, `title-audit-002.json`, `ue3-engine-header-001.json`, `function-discovery-001.log`, `ghidra-entry-raw-001.log`, `entry-lift-001.log`, `entry-compile-001.log`, `firmware-install-001.log`. Они не публикуются автоматически.

```powershell
.\.venv\Scripts\python.exe scripts\audit_title.py work\extracted\PS3_GAME\USRDIR\EBOOT.elf --sdk third_party\ps3recomp --imports work\ppu-stage1\imports_named.json --iso-listing reports\iso-listing.txt --out reports\title-audit-NEXT.json
.\.venv\Scripts\python.exe scripts\disassemble_function.py work\extracted\PS3_GAME\USRDIR\EBOOT.elf --sdk third_party\ps3recomp --functions work\ppu-stage1\functions-expanded.json --imports work\ppu-stage1\imports_named.json --address 0xa8a128 --out work\ppu-stage1\startup-NEXT.txt
```

Базовое дерево, начальная фильтрация PPU-кода и ограниченный execution trace получены. Следующая цель: замкнутое покрытие CRT→main, проверка 33 NID без найденной регистрации и полноценный GUI-эталон с контролем FIOS/cache-путей. До проверки этой цепочки массовая генерация C++ не считается полезным продвижением к запускаемому порту. Порядок продолжения — `docs/HANDOFF.md`.

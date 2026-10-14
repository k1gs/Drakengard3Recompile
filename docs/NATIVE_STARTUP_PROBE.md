# Native startup probe — 2026-10-14

Получен отдельный Windows `.exe`, исполняющий статически перекомпилированный стартовый префикс предоставленной сборки Drakengard 3. Он останавливается **до выполнения первого HLE**, а отдельный режим проверяет самодостаточный CRT-helper. Код игры и полученный `.exe` остаются в игнорируемом `work/`.

## Что реально выполнено

В `work/startup-probe-005/startup_probe.exe` включены четыре гостевых диапазона:

| VA | Инструкций | Роль в эксперименте |
| --- | --- | --- |
| `0xa8a020..0xa8a044` | 9 | Entry, восстановление TOC и вызов CRT |
| `0xa8a044..0xa8a06c` | 10 | Продолжение entry/exit; включено для сохранения fallthrough |
| `0xa8a128..0xa8a2fc` | 117 | CRT, остановка на первом импорте |
| `0xea2ee4..0xea2f48` | 25 | Отдельный leaf-helper инициализации таблиц |

Lifter — API локального ps3recomp, upstream source не менялся. Все прямые вызовы за пределы включённых диапазонов направлены в явные stop-barriers; импортные stub-функции тоже останавливают выполнение. Нет фиктивных `CELL_OK`. Косвенный вызов или LV2 syscall также останавливаются. Генератор отказывается принимать `TODO`, `non-code`, `undecoded`, `unmodeled` в lowering и потерянные continuation. Это строгий gate для выбранного подмножества, а не доказательство корректности всего lifter.

Entry→CRT прошёл до `sys_initialize_tls`, NID `0x744680a2`, LR `0xa8a180`. На четырёх синтетических наборах boot-регистров проверены:

- восстановленный TOC/r2 `0x1981090`;
- смещение SP на `0x110`, back chain и сохранённый LR `0xa8a040`;
- аргументы TLS: r7→r3, low32(r8)→r4, low32(r9)→r5, low32(r10)→r6;
- используемые сохранённые регистры CRT;
- global store из исходного r12 с независимой проверкой big-endian байтов.

Входные регистры основной серии намеренно синтетические. Дополнительный режим `reference-boot` сверяет scalar-параметры loader с pinned RPCS3 и PT_TLS; stack/argv/envp addresses, r13 и полная process-модель остаются синтетическими. Полный ELF загружается как данные в проверяемые PT_LOAD-диапазоны; чужие машинные инструкции не исполняются на host. Перед загрузкой проверяются точный размер и SHA-256 ELF.

Helper `0xea2ee4` заполняет две последовательные таблицы по адресу, который берёт из `TOC-0x1838`. В нашем ELF base = `0x1ad3838`. Первая таблица: `[0,1,2,0xffffffff,...]`, вторая — 256 нулей; loop выполняет 253 итерации, CTR становится нулём. Проверены все 512 слов и byte order первых ID на двух разных исходных заполнениях памяти. Предположение о назначении как CRT/descriptor state не считается восстановленным исходным именем.

## Проверки

`reports/startup-probe-check-006.json`: 11/11 случаев прошли — четыре entry-варианта, один reference-shaped scalar-вариант, два helper-варианта и четыре отрицательных проверки. Wrong-size ELF, same-size ELF с изменённым байтом, unmapped TOC и неправильный аргумент дают ожидаемую ошибку, а не crash или фиктивный успех. Отрицательные fixture-файлы создаются самим runner в проверенном временном каталоге, удаляются после теста и не требуют отдельного Ghidra smoke test; оригинал не меняется.

SHA-256 проверенного `.exe`: `337f77a158d13db5a33ba29c64eed160809b8cab4f730068aa127df10d3a739f`. Лог сборки: `reports/startup-probe-build-006.log`. Старые probe-001..004 сохранены как предыдущие попытки; актуальный — 005.

Собственные unit tests: 10/10, включая порядок constructor-table dispatch, отсутствие sentinel и null OPD. Проверки native-probe требуют локальную Windows-сборку и не входят в portable GitLab CI.

Сборка использует `/EHs`, чтобы stop-исключения могли проходить через extern-C memory/HLE helpers. `/EHsc` для этой диагностической модели не подходит. В логе остаются предупреждения о неиспользуемых общих helper-функциях upstream; предупреждения о non-throwing boundary исправлены до запуска.

## Таблица инициализации до main

Диспетчер `0xa8a408` загружает указатель из `TOC-0x7fb0`, затем идёт назад по 32-битным указателям OPD до `0xffffffff`.

- TOC slot: `0x19790e0`, table end: `0x19202c4`.
- Sentinel: `0x1920000`, 176 записей в порядке обратного обхода.
- 176 уникальных code targets, все внутри SHF_EXECINSTR и с известной границей функции.
- У всех записей TOC `0x1981090`.
- Сумма эвристических диапазонов — 216 204 байта; это не размер замкнутого call graph.
- Первый target: OPD `0x1979088` → code `0xeb8e48`; последние targets включают `0x3902d8`, `0x25c190`, `0x161ec0`.

Данные в `reports/constructor-audit-001.json`. Это статическая карта; constructor bodies и их зависимости не исполнялись. Нельзя пропустить таблицу и считать достижение main корректным стартом.

## Уточнённые границы CRT

| Цель CRT | Наблюдение |
| --- | --- |
| `0xea34e8` | Загружает r3 из TOC и вызывает `0xea337c`; семантика ещё не восстановлена |
| `0xea330c` | Вызывает `sys_lwmutex_create`, отдельная ветка ошибки на `0xea3550` |
| `0xe9cc5c` | Инициализирует структуры через `0xea32b8`, требуется отдельный разбор |
| `0xea2ee4` | Leaf-helper проверен отдельно native-исполнением |
| `0x10200` | `.init`-кандидат: вызывает `0xa8a398`, затем constructor-dispatcher `0xa8a408` |
| `0x8c5f10` | Main-кандидат, тело ещё не выполнялось native |
| `0xe957e4` | Путь после возврата main: LV2 syscall с r11=988 и вызовы exit/finalization |

Дизассемблирование сохранено в `work/ppu-stage1/crt-*-001.txt`. Для `0xea330c` эвристический диапазон заканчивается после вызова error-handler: продолжение надо проверять, нельзя предполагать обычный return. Точно так же native-probe не утверждает закрытость всей цепочки после TLS.

## Воспроизведение

Уже собранный probe:

```powershell
.\work\startup-probe-005\startup_probe.exe .\work\extracted\PS3_GAME\USRDIR\EBOOT.elf 0
.\work\startup-probe-005\startup_probe.exe .\work\extracted\PS3_GAME\USRDIR\EBOOT.elf 0 table-helper
.\work\startup-probe-005\startup_probe.exe .\work\extracted\PS3_GAME\USRDIR\EBOOT.elf 0 reference-boot
.\.venv\Scripts\python.exe scripts\check_startup_probe.py --out reports\startup-probe-check-NEXT.json
```

Свежая генерация без перезаписи предыдущих артефактов:

```powershell
.\.venv\Scripts\python.exe scripts\generate_startup_probe.py --out work\startup-probe-NEXT
.\scripts\build-startup-probe.cmd work\startup-probe-NEXT
.\.venv\Scripts\python.exe scripts\check_startup_probe.py --probe work\startup-probe-NEXT\startup_probe.exe --out reports\startup-probe-check-NEXT.json
.\.venv\Scripts\python.exe scripts\audit_startup_tables.py --out reports\constructor-audit-NEXT.json
```

Probe привязан к ELF SHA-256 `454fb591e9192cf9d4a5c65912eb4cd8c982ba52e26474bcfc9dc6ee1dc12acc`. Generated code нельзя переносить на другой EBOOT без новой проверки. Размеры/границы секций используются из этого ELF; memory helpers отвергают доступ за пределами mapped ranges. В helper-mode есть прямой VM access только в заранее проверенном 2 KiB диапазоне.

## Сверка boot/TLS-контракта

В официальных исходниках RPCS3 ревизии `6d7020d5` loader задаёт r3..r10 по порядку: argc, argv, envp, envc, main_thread_id, TLS VA, TLS filesz, TLS memsz. r11 получает entry OPD, r12 — malloc_pagesize. Обработчик `ppu_cmd::set_args` явно копирует i-й аргумент в `gpr[i+3]`. Это согласуется с наблюдаемой перестановкой регистров в CRT.

Для нашего ELF PT_TLS: VA `0x197fc7c`, filesz `0x2e8`, memsz `0x2e8`; в эталонном `reference-smoke-003.log` те же значения и malloc_pagesize `0x100000`. Режим `reference-boot` проверяет именно эту перестановку и global store page-size до stop-barrier; он не реализует TLS-handler.

В source RPCS3 `sys_initialize_tls` имеет ранний выход при уже установленном r13, выделяет TLS system area и инициализирует exit/PRX mutex state. В текущем ps3recomp handler выполняет bump allocation и устанавливает r13 без такого guard; loader ps3recomp также заранее копирует TLS и устанавливает r13. Это **различие моделей, требующее проверки**, а не уже доказанная причина сбоя Drakengard 3. Обработчики и upstream runtime не менялись.

Первичные источники: [PPUModule.cpp](https://github.com/RPCS3/rpcs3/blob/6d7020d5/rpcs3/Emu/Cell/PPUModule.cpp#L2927), [PPUThread.cpp](https://github.com/RPCS3/rpcs3/blob/6d7020d5/rpcs3/Emu/Cell/PPUThread.cpp#L2524), [sys_ppu_thread_.cpp](https://github.com/RPCS3/rpcs3/blob/6d7020d5/rpcs3/Emu/Cell/Modules/sys_ppu_thread_.cpp#L70). Локальные копии — `tools/reference-source/`, только справка; не включены в Git.

## Следующая цель

Проверить TLS idempotence/side effects и callback-пути выбранного runtime на контролируемых тестах до интеграции HLE. До native-продвижения к main построить зависимости `.init`, 176 initializers и error/finalization paths; сохранять stop-barriers на каждом неизвестном переходе. Отдельно всё ещё нужен GUI-эталон RPCS3.

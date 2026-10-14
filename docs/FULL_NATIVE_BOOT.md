# Полный native boot: 2026-10-14

## Результат и границы

Собран Windows x64 executable **полного PPU lift**, исполняющий стартовый код Drakengard 3 и читающий её локальное дерево ресурсов. Это уже не ограниченный startup probe. Прохождение игры, меню, отображение игрового кадра и управление **не подтверждены**.

Вход: NPUB31251 v1.01, plain ELF SHA-256 `454fb591e9192cf9d4a5c65912eb4cd8c982ba52e26474bcfc9dc6ee1dc12acc`. SDK ps3recomp `a2c20108af45174dbb7d3e2f87f6acaaef09c6b4`; LLVM 20.1.8, MSVC 14.41, Windows SDK. Исходные ISO/SELF не менялись, DLC не устанавливались.

## Полный lift

- PPU: 51 057 определений функций, 113 C++ chunks, 481 869 710 байт до instrumentation. Статический аудит: неопределённых function symbols нет. Это не доказательство полноты CFG или корректности перевода.
- Диапазон ограничен исполняемыми секциями, `--code-end 0x157f038`; используется `work/ppu-section-filtered-001/functions.json`, а не весь RX PT_LOAD с данными/SPU.
- SPU: извлечены 40 embedded ELF, суммарно 1 596 904 байта; каждый получил отдельный symbol prefix и fingerprint registration. Первый runtime MISS `0x00E729804474449A` соответствует извлечённому `spu_0001_at_01799780`.
- PPU audit обнаружил 3 257 `.word`, 47 прочих unsupported операций и 5 отброшенных/non-code branch sites. 3 309 sites в 31 chunk заменены на явный stop при **исполнении**. Исходные chunks сохранены в `original-unguarded/`. Неизвестная операция больше не становится молчаливым NOP; conditional branch сохраняет условие.
- SPU lifter тоже содержит unsupported `.word`; `port/spu_guard.h` переводит вызываемый `spu_unsupported` из log-and-continue в stop. Само наличие данных в heuristic code ranges не доказывает достижимость.

## Наблюдения запусков

| Прогон | Наблюдение |
| --- | --- |
| `game-run-001`, 30 секунд | Реальные CRT/init, GCM/RHI, SPU, FIOS; D3D12 backend initialized на RTX 4060. Повторяющиеся `wait for invalid cond`. Остановлен по лимиту времени. |
| `game-run-002`, 20 секунд | После ABI correction ошибки `invalid cond` исчезли; title ID NPUB31251, cache mount, дополнительные SPURS tasksets, открытие COALESCED_INT.BIN.EDAT. Остановлен по лимиту времени. |
| `game-run-003`, 20 секунд | Полный guarded PPU/SPU: две async задачи дают registry HIT и исполняют image 2. Trophy GetGameInfo → OOM примерно 2,5 ГБ. Остановлен по времени. |
| `game-run-004`, 20 секунд | С trophy endian wrapper, но прогон дошёл только до FIOS/дополнительного taskset за это время. Нельзя использовать его для подтверждения trophy fix. |
| `game-run-005`, 15,65 секунды | Trophy GetGameInfo пройден без прежнего OOM; две SPU async HIT. Завершился сам на resource-blocker для COALESCED_INT.BIN.EDAT, без timeout. |
| `game-run-006`, 13,67 секунды | Повторил тот же ресурсный blocker, две SPU HIT; исправленный runner сохранил exit code **5**, timeout=false. |

Логи находятся только локально в `reports/game-run-NNN{,-errors}.log` и JSON manifests. `SetPrepareFlip`, clear surface и backend ready **не являются доказательством игрового изображения**. Первые пять manifest имеют `exit_code=null` из-за PowerShell Process handle lifetime, включая самостоятельно завершившийся run 005; эти данные не переписаны. Runner исправлен: удерживает отдельный process handle до окончания.

## Исправления scaffolding

1. Windows link: добавлена `dxguid` для `IID_IDXGIFactory6`.
2. PARAM.SFO читается до guest execution: шаблон использовал BLES00000, что давало чужие пути cache/save. Теперь ID/версия берутся из предоставленного дерева игры.
3. `sys_lwcond_t` — два BE32 поля: pointer на lwmutex по +0 и queue ID по +4, всего 8 байт. Upstream записывал BE64 pointer и ID по +8, повреждая соседние данные. Исправление применяется к build-local копии с проверкой ожидаемых строк, upstream checkout не изменяется. Раскладка сверена с [RPCS3 sys_lwcond.h, pinned revision](https://github.com/RPCS3/rpcs3/blob/6d7020d5d40bc3e0f1227f2eebb6a312d953c286/rpcs3/Emu/Cell/lv2/sys_lwcond.h#L20).
4. Добавлены crash filter, guest register dump, небольшой minidump, явные PPU/SPU/resource stop reasons.
5. `sceNpTrophyGetGameInfo`: backend записывал native little-endian u32 прямо в guest memory. PPU воспринимал 32 trophies как `0x20000000`, затем возникал allocation около 2,5 ГБ. Context-aware wrapper сериализует пять scalar fields в BE32 и reserved bytes; strings остаются byte arrays. Раскладка сверена с [pinned RPCS3 trophy header](https://github.com/RPCS3/rpcs3/blob/6d7020d5d40bc3e0f1227f2eebb6a312d953c286/rpcs3/Emu/Cell/Modules/sceNpTrophy.h). Это не реализация TRP: upstream placeholder trophy counts и остальные trophy APIs ещё требуют проверки.

Исправление lwcond **не реализует полноценные condition variables**: upstream wait пока poll-style, signal handlers остаются no-op. Исчезновение прежней ошибки подтверждает исправление layout на этом пути, но не семантику синхронизации во всех сценариях.

## Следующий блокер: доступ к ресурсам

`sceNpDrmIsAvailable2`, NID `0xF042B14F`, не зарегистрирован. Запуск использует `PS3_HLE_UNRESOLVED=fail`, поэтому вызов возвращает ошибку, не fake success. Затем FS открывает `COALESCED_INT.BIN.EDAT`; decoder отказывает, и предыдущий runtime отдаёт игре ciphertext с magic `4E504400`. После этого лог содержит запрос allocation `0x24D90000`. Связь неверного ресурса и allocation — рабочая гипотеза, не доказанная instruction-level трассой.

Заголовок EDAT: version 3, license 3, flags `0x3C`, block `0x4000`, plaintext size 310 640. Сообщение upstream «license type 3 needs a RIF key» нельзя считать доказательством необходимости пользовательской RIF/RAP: сам комментарий decoder различает free klicensee для типа 3 и RIF для типов 1/2. Следующий инженерный шаг — сверить нормальный контракт NPDRM API/поддержку формата и авторизованный способ получения plaintext. Лицензионные проверки не подменяются успехом, ключи не ищутся и не публикуются.

Build-local FS теперь явно останавливается, если после штатного resolver остаётся NPD, вместо передачи контейнера UE3-парсеру. Это диагностика причины, не исправление загрузки ресурса. Оригинальный ресурс сохраняется.

Дополнительно остаётся незарегистрированным `_sys_spu_printf_initialize` (`0x45FE2FCE`). Другие фактические blockers надо устанавливать после корректного чтения ресурсов, не угадывать из списка импортов.

## Воспроизведение локально

Существующий lift уже выполнен. Повторно не запускать guard поверх изменённых chunks; для нового lift использовать новую папку/новый audit и согласовать RECOMP_DIR в CMake.

```powershell
.\scripts\build-game.cmd
.\build\game\DrakengardRecompile.exe --check-lwcond
.\scripts\run-game.ps1 -Seconds 30 -Run NEXT
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

Runner проверяет SHA-256 ELF, задаёт VFS/firmware roots и отдельный `work/native-state/dev_hdd0`, ограничивает время, завершает только запущенный им процесс и сохраняет exe hash. Нельзя указывать `reports`/generated code/ресурсы игры для Git staging. `port/` содержит MIT template scaffolding и наши compatibility/diagnostic additions; лицензия шаблона сохранена в `LICENSE.ps3recomp.txt`.

Команды исходного lift (нужны существующие loader/functions/import reports):

```powershell
.\.venv\Scripts\python.exe third_party\ps3recomp\tools\ppu_lifter.py work\extracted\PS3_GAME\USRDIR\EBOOT.elf --functions work\ppu-section-filtered-001\functions.json --hle-stubs work\ppu-stage1\EBOOT.imports.json --code-end 0x157f038 --jobs 4 --chunk-lines 60000 -o work\full-ppu-001
.\.venv\Scripts\python.exe scripts\audit_generated_lift.py work\full-ppu-001 --out reports\full-ppu-audit-001.json
.\.venv\Scripts\python.exe scripts\guard_generated_lift.py work\full-ppu-001 --audit reports\full-ppu-audit-001.json
.\.venv\Scripts\python.exe third_party\ps3recomp\tools\extract_spu_images.py work\extracted\PS3_GAME\USRDIR\EBOOT.elf -o work\spu-images-001
.\.venv\Scripts\python.exe third_party\ps3recomp\tools\build_spu_workloads.py --images work\spu-images-001 --lifted work\full-spu-001 --out work\full-spu-001\spu_workloads.c --register-fn drakengard_spu_register_all --title Drakengard3
```

## Проверки

Portable unittest: 12/12 (`analysis-tests-004.log`), включая hash mismatch rejection до изменения chunks, conditional stop edge и сохранение legacy bytes/backup. Старые 11/11 startup-probe checks относятся только к малому probe. Полная сборка и реальные boot traces проверяются отдельно от unit tests.

Native `--check-lwcond`: PASS, exit 0 (`lwcond-native-check-001.log`), реальный зарегистрированный handler пишет BE32 pointer/queue и сохраняет canaries по обеим сторонам. Full PPU/SPU со strict guards собраны (`game-build-005.log`); trophy wrapper добавлен incremental build (`game-build-006.log`). Exe, проверенный boot runs 005/006: 215 803 904 байта, SHA-256 `1d877c4b632b86f5bea9b1c1679647be5bd4cf60a2110ea3c1cddb10c5ef765e`.

Последняя конфигурационная правка поддерживает PORT_ENABLE_SPU=OFF без link error (пустая регистрация, не имитация исполнения). Default build со SPU повторно собран в game-build-007.log; полного повторного boot после этого relink не делали, native lwcond check повторён: PASS.

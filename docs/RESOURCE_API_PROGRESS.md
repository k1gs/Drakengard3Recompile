# Native resource boot: 2026-10-14

Продолжение после `e84f735` и `FULL_NATIVE_BOOT.md`. Игрового меню/прохождения пока нет. Native запуск уже читает coalesced config, проходит подготовку SPU-программ, исполняет raw SPURS job и открывает основные cooked packages. Исходники, тесты и отчёты об изменениях публикуются отдельно от игровых данных.

## EDAT: нормальный API вместо потери аргумента

Подключены context-aware `sceNpDrmIsAvailable` и `sceNpDrmIsAvailable2`. Первый аргумент — guest pointer на klicensee, второй — путь. Для license type 3 этот аргумент является штатным ключом RAP-free ресурса: см. [pinned RPCS3 sceNp.cpp](https://github.com/RPCS3/rpcs3/blob/6d7020d5d40bc3e0f1227f2eebb6a312d953c286/rpcs3/Emu/Cell/Modules/sceNp.cpp#L633).

Build-local EDAT adapter принимает аргумент API, проверяет `dev_hash` и MAC каждого блока через существующие криптографические primitives SDK. Type 1/2 этим аргументом не расшифровываются и не получают fake success. Содержимое ключа не логируется, не записывается в sidecar, не включается в repository; temporary copy очищается после вызова. Decoder и исходный SDK checkout не редактируются на месте: checked transformations создают копии в `build/game/`.

Полная NPDRM validation не заявляется: adapter сохраняет возможности/ограничения upstream decoder, включая ограниченный набор EDAT flags и отсутствие полной проверки всех header/title/signature structures. Неверный `dev_hash` и повреждённые блоки отвергаются. Никакая license conversion/поиск чужих ключей не выполняется.

`PS3_EDAT_CACHE` задаётся runner на `work/native-runs/RUN/edat-cache`. Registry связывает исходный host path с **успешно проверенным** plaintext path; `cellFsOpen/SdataOpen` используют его. Неуспешные outputs удаляются. Исходный EDAT не переписывается. FS guard для нерасшифрованного NPD сохраняется.

Проверено на предоставленной сборке:

- `COALESCED_INT.BIN.EDAT`, license 3, flags `0x3C`: `dev_hash` и block MAC checks прошли с аргументом, переданным самой игрой.
- Получено 310 640 plaintext bytes, SHA-256 `5df74694b5e3182e6e4c8a1668f8aa6c00fc677be35db42acfdcde18ce02b704`.
- Read-only parser разобрал 14 config files, 188 sections, 3 595 key/value pairs; значения не экспортируются. Счётчики BE, UTF-16 strings **LE**. Maps занимают 310 628 bytes; оставшиеся 12 нулевых bytes согласуются с 16-byte padding, которое parser принимает только по явному флагу. Первоначальное предположение UTF-16BE дало неверные имена, исправлено по фактическим байтам; исходный report сохранён как неактуальная интерпретация.

## OVIS: реальный return contract

Upstream stub имел two-argument signature и требовал несуществующий на этом пути `cellOvisInit`. Игра передаёт **один ELF pointer** и использует r3 как table size, либо распознаёт `CELL_OVIS_ERROR_INVAL` (`0x80410402`). Ошибка `0x80410701` от старой заглушки превращалась в огромный allocation.

Обоснование по caller `0xAD2690..0xAD2FB4`:

- `0xAD27DC`: загрузка ELF pointer в r3;
- `0xAD27E0`: `cellOvisGetOverlayTableSize`;
- `0xAD27E8..0xAD27F8`: нулевой размер или `0x80410402` ведут к следующему entry;
- `0xAD2800`: любой другой результат увеличивается на 132 и идёт в allocator;
- `0xAD2860..0xAD286C`: цикл по 23 entries, шаг записи 12 bytes.

Поддержан ограниченный корректный случай: ELF32-BE EM_SPU, entry в executable LOAD, LOAD ranges в 256-KiB LS без пересечений, без overlay sections/unknown flags → empty overlay table, size 0. Настоящие overlays вызывают explicit blocker, не fake zero. Невалидный ELF → штатный INVAL, который обрабатывает сама игра. [RPCS3 OVIS signatures/error enum](https://github.com/RPCS3/rpcs3/blob/6d7020d5d40bc3e0f1227f2eebb6a312d953c286/rpcs3/Emu/Cell/Modules/cellOvis.cpp).

Первый проверенный ELF: guest VA `0x18F4600`, file offset `0x18E4600`, extracted image `spu_0022_at_018E4600.elf`. Два LOAD, 14 sections, overlays нет. Entry 5 в static program table (`0x19ACA70`, 12-byte records) указывает на 8-byte zero marker `0x176DB60`, рядом с именем `SPU_HeightFieldRelease.elf`. Возврат INVAL здесь подтверждён caller branch и позволяет штатно продолжить цикл; никаких PPU branches не патчилось.

## Raw SPURS job

После OVIS обнаружился raw job `27E63EA06003D416`, 34 688 bytes, descriptor `0x1A2B980`. Это не один из 40 embedded SPU ELF. Upstream runtime при MISS возвращал 0, позволяя цепочке идти без исполнения job: draw calls из такого run не доказывают корректный render.

Добавлен build-local workload guard: сначала `SPU_DUMP_MISS` сохраняет blob в приватный run directory, затем процесс завершает explicit exit 7. Captured blob SHA-256: `e3d00177058fd2e00f25bd7af0962262d1cc69d5f8eb0ce52ec045a6d622e181`.

`lift_raw_spurs_job.py` требует явные LS base/entry. Здесь оба равны 0, согласно staging в pinned SDK `runtime/spu/spurs_job.c`; WWS defaults 0x4000 не применяются. Получены 469 functions, 25 call targets, 23 unsupported `.word` sites guarded при исполнении. Image ID 100, отдельный symbol prefix. Runtime registration использует **нестандартный FNV offset basis SDK** `1469598103934665603`; generator дополнительно проверяет fingerprint/size из имени capture до создания файлов. Ранняя ошибочная registration с обычным FNV basis сохранена локально как rejected attempt и не включена в build.

Run 013 подтвердил HIT и `RETURNED rc=0` для 11 job descriptors. Это проверка исполнения CRT/job, не полная проверка корректности shader patch output. Source/raw blob/registry/manifests остаются в игнорируемом `work/`.

## Memory allocation table

Run 013 открыл CORE, ENGINE, GFXUI, IPDRV, GAMEFRAMEWORK, ONLINESUBSYSTEMPC, SQEXSEAD, SQEX03GAME, STARTUP, GUIDCACHE, START и animation packages. Позже появился небольшой OOM и незавершённое ожидание SPU event flag `0x01B0EF80`.

Отдельный native check воспроизвёл отказ **ровно на allocation 1025**, при 1024 live allocations по 64 KiB (64 MiB), далеко до исчерпания guest window. Причина — `SYS_MEMORY_ALLOC_MAX=1024`. В build-local header число записей увеличено до 4096: по одной на каждую 64-KiB page в существующем 256-MiB allocation window. Размер guest memory/window не увеличивался. Новый native check: 1025 уникальных диапазонов allocated/freed, PASS. C consumers массива проверены: только sys_memory.c.

Это не полноценная замена allocator: upstream bump allocation не переиспользует освобождённые адреса, а дальнейшие memory/accounting/concurrency paths ещё требуют проверки. Влияние именно table fix на последующий boot проверяется отдельно от native reproduction; небольшой OOM и event wait не объявляются одной и той же причиной.

Run 014: 1354 successful allocations, прежний небольшой OOM не повторился до остановки. Дополнительные SPU tasks созданы; exit 7 через 52,98s на следующем unregistered raw job `D6E964B601AB14C0`, 528 bytes. Он поднят отдельно: 13 functions, image ID 101, explicit LS base/entry 0. Реальное исполнение проверяется в следующем прогоне.

Run 015 (`game-build-017`): 90-second bounded run, завершён по time limit (91,38s wall-clock, forced exit -1). **OOM не было**. Первый raw job дал 469 HIT, второй 3 HIT, в логе есть соответствующие `RETURNED rc=0`. Открыто 45 файлов; выполнение дошло до дальнейшей SPU task initialization. Последний executable SHA-256 `1502b3ac1e8320cf5d6ae0c0aa10dfa6c3def78832cf71111b7f6958df32ab3c`.

Подтверждённый следующий HLE gap: `cellSpursQueuePopBody` NID `0x91066667` (stub `0x157ED98`). Встречается вместе с незавершённым `EventFlagWait` на `0x01B0EF80`, pattern 1, и job-chain guard `0x01AAD200`. Причинная связь требует ABI/queue-layout и thread trace, **сигналы не подделывать**. Task creation повторяется, IDs превышают 100; проверить per-taskset allocation/lifecycle, прежде чем считать это нормальным поведением. Не делать новый полный lift PPU/40 ELF для этого HLE blocker.

Все native процессы завершены; фонового продолжения/автоматизации нет. Следующий этап — реализовать нормальный queue API, проверить event delivery и task lifecycle, затем получить реальный кадр, меню и input. Draw calls, audio PortStart и успешное завершение raw jobs не подтверждают картинку/звук/геймплей.

## Проверки и команды

| Проверка | Результат |
| --- | --- |
| Portable unittest | 19/19, `analysis-tests-008.log` |
| Synthetic EDAT | 6/6: CMAC, wrapped HMAC flags 0x3C, wrong key, corruption/cleanup, license boundary, absent key |
| SPU ELF classification | 7/7 synthetic + actual first OVIS image |
| Native memory reproduction | Before: failure at 1025; after: 1025 allocate/free PASS |
| Native jobs | Run 013: 11 HIT/RETURNED rc=0; timeout 60s |
| Full native with two raw jobs + memory fix | Run 015: 469 + 3 HIT, no OOM, timeout 90s; QueuePopBody/event wait остаются |

```powershell
.\scripts\build-game.cmd
.\scripts\check-edat.cmd
# Run edat_contract.exe inside a fresh ignored work directory (it writes synthetic fixtures).
.\scripts\check-overlays.cmd
.\build\game\overlay_contract.exe work\spu-images-001\spu_0022_at_018E4600.elf
.\build\game\DrakengardRecompile.exe --check-memory
.\scripts\run-game.ps1 -Seconds 60 -Run NEXT
.\.venv\Scripts\python.exe -m unittest discover -s tests -v
```

```powershell
.\.venv\Scripts\python.exe scripts\lift_raw_spurs_job.py work\native-runs\012\spu-job-dumps\spujob_27E63EA06003D416_34688.bin --sdk third_party\ps3recomp --out work\full-spu-001 --base 0 --entry 0 --image-id 100
```

Последнюю команду не повторять поверх существующего lift: generator защищает manifest/ID от перезаписи. Следующий captured job получает свой image ID и тот же explicit ABI audit. Не публиковать raw/generated files, plaintext cache или игровые ресурсы.

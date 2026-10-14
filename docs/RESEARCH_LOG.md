# Журнал исследования

Правило: факт, гипотеза и следующий эксперимент записываются отдельно. Адрес без SHA-256 executable, типа адреса (VA/file offset) и редакции игры не считать устойчивой ссылкой.

## Шаблон записи

```text
ID / UTC:
Цель:
Вход: title ID, APP_VER, SHA-256, base/update/DLC
Инструменты: версии / ревизии
Команда / настройки:
Наблюдение: адрес VA / file offset, API/NID, лог/кадр
Вывод: подтверждённый факт
Гипотеза: что ещё не установлено
Следующая проверка и критерий опровержения:
Артефакты: пути и SHA-256
```

## 2026-10-14 — подготовка

Пользователь сообщил, что образ скачивается. После этого файлы игры не читались. Анализ кода игры, запуск игры и проверка ресурсов не выполнены. Подготовлены переносимые инструменты, окружение Python, intake и синтетические проверки. Фактические результаты сборки/проверок записать в CURRENT_STATE.md; логи хранятся в reports/.

## 2026-10-14 — загрузка завершена, первый разбор

По сообщению пользователя начат анализ завершённых файлов. Зафиксированы ISO и пять DLC PKG; оригиналы сохранены. Из рабочей копии SELF штатным RPCS3 получен PPU ELF. Loader, эвристический поиск, SPU-signature scan, HLE-registration audit и UE3 header inspection дали воспроизводимые результаты. Стартовый срез независимо дизассемблирован Ghidra и скомпилирован после lift, но не исполнялся: внешний вызов пропущен из-за границ среза. Все выводы и ограничения собраны в `FIRST_ANALYSIS.md`.

Подтверждённый пробел OPD: вызов startup `0xa8a128` есть в entry-коде, но этой функции нет среди 30 142 входов loader. Расширенный поиск её нашёл. Первоначальная оценка HLE 188/251 была неполной: явные регистрации в scaffold увеличили число кандидатов до 218/251. Это всё ещё не проверка поведения.

## 2026-10-14 — продолжение: native startup probe

После запроса продолжить проверен исходный ELF SHA-256, рабочее дерево на старте было чистым после опубликованного `3ea74f0`. Добавлен собственный Windows diagnostic harness, который загружает pinned ELF как данные и исполняет статически поднятый entry→CRT до stop на `sys_initialize_tls`. Переходы за frontier не пропускаются, HLE success не имитируется. Отдельный leaf-helper выполняет 253 итерации и проверяется по всем 512 словам результата.

Ранний gate поймал отсутствовавшее fallthrough-продолжение entry `0xa8a044`; этот диапазон включён явно. Для C-linkage stop-исключений выбран `/EHs`. После исправлений: 11/11 native cases и 10/10 unit tests. Актуальный exe — `work/startup-probe-005/startup_probe.exe`; отчёт — `docs/NATIVE_STARTUP_PROBE.md`, проверка — `reports/startup-probe-check-006.json`.

Статически восстановлен обратный обход 176 initialization entries через OPD, все с primary TOC. Boot/TLS scalar-контракт сверён с исходниками RPCS3 `6d7020d5` и предыдущим execution trace. Обнаружено различие моделей TLS idempotence/side effects между RPCS3 и ps3recomp; причинная связь с поведением игры пока не проверена. Upstream runtime не менялся, новый HLE не внедрялся. Новые исходники/документация пока локальны; игровые бинарники и generated code остаются игнорируемыми.


## 2026-10-14 — полный PPU/SPU lift и native boot

Полный PPU lift: 113 chunks, 51 057 definitions, 0 undefined function symbols. 40 embedded SPU images извлечены и подняты с отдельными registry fingerprints. Сборка clang-cl потребовала dxguid. Первый реальный native run дошёл до FIOS, но повторял invalid cond. Проверка pinned RPCS3 sys_lwcond.h показала upstream ABI bug: структура 8 байт, не 12. Build-local correction и PARAM.SFO init позволили пройти дальше, к cache/resources; invalid cond исчезли.

Новый блокер: unregistered sceNpDrmIsAvailable2 и нерасшифрованный COALESCED_INT.BIN.EDAT. Предыдущий FS возвращал NPD ciphertext, после чтения появился большой allocation. Причинная связь с allocation пока гипотеза; невозможность использовать ciphertext как UE3 data подтверждена. Добавлены явные stops для encrypted resources и исполняемых unsupported PPU/SPU operations, без изменения оригинальных игровых файлов. Детали и воспроизведение — FULL_NATIVE_BOOT.md. Unit tests 12/12. GUI/gameplay пока не проверены; новые изменения локальны.

Trophy GetGameInfo дополнительно исправлен context-aware BE32 output wrapper: legacy little-endian 32 воспринималось как 0x20000000 trophies и приводило к OOM ~2,5 ГБ. Full native runs 005/006 прошли этот участок и завершились на encrypted resource guard; run 006 exit 5, 13,67s, no timeout. Две SPU tasks дают fingerprint HIT. lwcond native canary check PASS.

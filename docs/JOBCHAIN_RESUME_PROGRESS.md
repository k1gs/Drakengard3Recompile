# ShaderPatching stream resume, 2026-10-16

Наблюдаемый producer публикует descriptors в command stream01A2A900 и вызывает RunJobChain серией сигналов. Guest code0140DB10/008B0608 содержит lwsync перед publication; repeated calls0140E840..0140E888 не обозначают десять отдельных jobs. Run068 trace показывает END после11, затем повторные обходы от entry с62/117/.../487 jobs. Старые descriptors оставались в префиксе, который host worker исполнял повторно.

## Исправление

`scripts/prepare_jobchains.py`, вызываемый из prepare_spu_tasks, сохраняет PC/return PC зарегистрированного command stream на END. Следующий workload signal читает тот же tail, который producer может заменить новой JOB/NEXT/RESET_PC командой. Обработанные префиксы не replay-ятся. Pending wake и claim/retirement worker защищены общим коротким lock; повторный Run остаётся сигналом и не создаёт второго worker. Если END заменён во время retirement и поступил сигнал, тот же worker продолжает от сохранённого PC. CreateThread failure теперь возвращает error вместо success.

Охват — наблюдаемый append/branch stream и handoff на END. Shutdown/recreation активного chain и расширенная вложенность CALL не заявлены проверенными. Старый отдельный20s job-guard timeout не объявляется исправленным. Синтетические SPURS events не возвращены; реально выполняются зарегистрированные SPU jobs.

## Native regression

`--check-jobchain-resume` запускает реальные host jobchain API с зарегистрированными SPU test jobs и считает их исполнения:

- Idle append и переход RESET_PC в другой segment: build082 до4/3/2/1 FAIL, build083/084 после1/1/1/1 PASS.
- Вторая JOB публикуется после чтения END, но до освобождения worker: до1/0 FAIL, после1/1 PASS. Narrow END hook используется только этим isolated fixture; обычный runtime оставляет его null.
- SPURS events regression после изменения PASS; synthetic event broadcast отсутствует.

Run069/build084:150s запрошены, игра дошла до HUD_TITLE_SF. END traces показывают новые пакеты16 jobs вместо replay растущих префиксов. FIFO EC814/EC814 и17DB80/17DB80 реально drained в samples. Затем явная остановка на FP constant bank72 при capacity64; это следующий реальный renderer limit, не основание пропускать команды/подставлять completion. Scene/backbuffer captures всё ещё чёрные.

## HUD bank

Build085 расширяет b1 до128 CONST slots: поддерживаемые4096 bytecode bytes физически вмещают не больше128 пар instruction16+payload16; 5 metadata rows плюс128 constants дают2128 bytes, stride2304 после alignment256. Native fixture собирает72 constants, проверяет slot71=72 и generated fp_constants[71]; PASS. Используется modern constant collector без молчаливого truncation.

Run070/build085 остановлен через55.34s на строгом collect failure(-1). Следующий bounded diagnostic086 сохраняет failing bytecode и проверяет END в расширенном окне. Причина пока не установлена: проверить program boundary, branch/CONST decode и согласованность публикации shader data. Старый literal path мог молча принимать усечённую программу, поэтому восстановление старого поведения не считается исправлением.
Продолжение build087/088: boundary failure оказался усечением валидного4816-byte FP старым4096-byte окном; затем реализован PPU vpkswss. Run073 впервые показал Unreal Engine intro. Подробности и ограничения в FIRST_NATIVE_IMAGE.md; старые состояния ниже исторические.

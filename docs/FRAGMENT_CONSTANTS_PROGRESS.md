# Fragment constant buffer, 2026-10-15

В pinned SDK backend и decompiler оказались рассогласованы. Backend исключает inline-CONST payload из pipeline hash и загружает64 float4 constants на каждый draw, полагая, что шейдер читает этот buffer. Фактически `rsx_fp_decompile(..., RSX_FP_CTRL_AUTO, ...)` встраивает literal values в HLSL. Cached pipeline сохраняет старые uniforms даже после guest patch. Дополнительно AUTO игнорирует уже сохранённый SET_SHADER_CONTROL export-width bit.

## Реализация

Build-local `port_decompile_guest_fp` вызывает современный `rsx_fp_decompile_buffered_ex` с реальными exp32 и cube_mask. Сгенерированный b1 layout адаптирован к существующему per-draw upload: texture-scale rows0..3, alpha row4, fp_constants[64] начиная с row5. Таким образом code-only hash и constant upload снова соответствуют shader source. Более64 constants приводят к явной диагностической остановке; значения и counts не ограничиваются молча.

Гостевые shader programs, payload и matrices не менялись. SDK напрямую не редактировался. Передача alpha/unnormalized coordinates через старые backend helpers остаётся отдельным участком проверки; новая buffered API сама по себе не заявлена полным исправлением renderer.

## Regression

`--check-fragment-constants` вызывает тот же decompile helper, который строит guest-FP pipelines. Синтетический MOV CONST меняет inline2.0 на9.0 при неизменных instruction bytes:

- Build079 до исправления: source-stable=0, buffered=0, values=1, exports=0; FAIL/exit1.
- Build080 после: source-stable=1, buffered=1, values=1, exports=1; PASS/exit0.

Проверяются неизменность shader source/cache hash, чтение fp_constants[0], точный физический layout buffer, обновление извлечённых runtime values и выбор r0/h0 по export control. Это CPU-side integration contract, не GPU pixel readback.

Предыдущий этап и source evidence shader identity — `VERTEX_BINDING_PROGRESS.md`. Next: real geometry capture после buffered fix, actual FP outputs и texture/RT chain. Не включать диагностические forced colours или w в обычной сборке; не пропускать FIFO park ради кадра.

Продолжение2026-10-16 — `JOBCHAIN_RESUME_PROGRESS.md`: после stream-resume fix игра дошла до HUD bank72. Build085 использует128 CONST slots и stride2304;72-slot fixture PASS. Упоминание capacity64 выше историческое. Strict collector выявил следующую malformed/truncated program; boundary и bytes исследуются отдельно, лимит не обходится.
Актуально2026-10-16/build087: failing shader в run071 оказался валидной программой4816 bytes с101 CONST и END4816. Decode/hash/collect согласованы в16-KiB окне, bank512/stride8448; fixtures72 и151 constants PASS. Capacity64/128 ниже — история. См. FIRST_NATIVE_IMAGE.md.

# Первый native кадр, 2026-10-16

Пользователь непосредственно подтвердил видимое изображение run075: «я это вижу». Run075 завершён по300s лимиту (300.17s, exit-1), не crash. Перед checkpoint повторно прошли portable22/22, все14 native checks и ожидаемый HALTexit8; свежие логи reports/checkpoint-*-088.log. Для нового чата — NEXT_CHAT_PROMPT.md.

Build088, SHA256 `aaef3ac3ca2d0b58723605f68379e249deea9aa8e2defa0820a9394475a3c13a`.
Run073 (240s) сохранил `work/native-runs/073/frame_002.bmp`: 1280x720, узнаваемая красно-чёрная заставка Unreal Engine, проверенная визуально. Первые два readbacks чёрные. Forced shader colours/w, DEPTH_OFF, CULL_OFF и NO_ALPHATEST не включены. Это первый подтверждённый intro frame; title/menu/gameplay и управление ещё не подтверждены.

Run071 обнаружил настоящую FP длиной4816 bytes (200 instructions,101 constants, END4816), без branch. Старое4096-byte окно усекало её. Build087 согласует decode/hash/collect в16-KiB окне: максимум512 instruction+CONST пар, b1 stride8448 после alignment256. Fixtures72 и151 constants проверяют последние значения и стабильность buffered source.

Run072 затем остановился на неподдерживаемой PPU `vpkswss` в0151A624. Build088 добавляет signed word → signed halfword saturation с canonical BE lanes, alias-safe результатом и sticky VSCR.SAT. Build-local patch заменяет11 guarded sites в двух lift units и хранит исходники; SDK не редактируется. --check-vpkswss:12 cases PASS, portable patch fixture проверяет backup/idempotence/unrelated guards.

Проверки088: portable22/22; vpkswss, jobchain-resume, fragment-constants, vertex-cache, vertex-constants, depth-state, lfqueue, spurs-events, queue, memory, spu-task-snapshot, spu-return, gcm-park, lwcond — PASS. Interpreter HALT — ожидаемый exit8. Логи reports/*-check-088.log и portable-tests-088.log.

Run074 завершился через250.98s с exit1, до300s лимита; причина завершения пока не установлена. Его sparse readbacks чёрные. Run075 запущен с -Interactive: окно выбрано и выведено на передний план через Computer Use. После чёрного старта непосредственно в живом окне подтверждён красный Unreal logo, title FPS0.31/draws43. Таким образом intro image повторён в отдельном visible run, но playback очень медленный. Menu/gameplay не подтверждены. Не выдавать слышимую озвучку за доказательство корректного video/render/menu. Run075 завершён runner по300s лимиту.

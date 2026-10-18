// Opt-in pad input for the observed YUV movie path. Never bypasses guest logic.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>

extern "C" int port_check_movie_detection();

namespace
{
std::atomic<ULONGLONG> movie_since{0}, last_movie_frame{0}, last_pulse{0};

bool enabled()
{
    static const bool value = std::getenv("PORT_SKIP_MOVIES") != nullptr;
    return value;
}

unsigned start_button(bool active, ULONGLONG now, ULONGLONG since, ULONGLONG last)
{
    if (!active || !since || !last || now < since || now < last || now - last > 500)
        return 0;
    const auto age = now - since;
    return age >= 300 && (age - 300) % 1500 < 150 ? 0x08u : 0u;
}
} // namespace

extern "C" void port_note_video_frame(unsigned active)
{
    if (!enabled())
        return;
    if (!active)
    {
        last_movie_frame.store(0);
        movie_since.store(0);
        return;
    }
    const auto now = GetTickCount64();
    ULONGLONG empty = 0;
    movie_since.compare_exchange_strong(empty, now);
    last_movie_frame.store(now);
}

extern "C" unsigned port_movie_skip_button()
{
    if (!enabled())
        return 0;
    const auto now = GetTickCount64(), since = movie_since.load();
    const auto button = start_button(true, now, since, last_movie_frame.load());
    if (button)
    {
        const auto pulse = since + 300 + ((now - since - 300) / 1500) * 1500;
        if (last_pulse.exchange(pulse) != pulse)
            std::fprintf(stderr, "[movie-skip] sending Start during YUV video\n");
    }
    return button;
}

extern "C" int port_check_movie_skip()
{
    const bool ok =
        start_button(false, 1400, 1000, 1400) == 0 && start_button(true, 1400, 0, 1400) == 0 &&
        start_button(true, 1299, 1000, 1299) == 0 && start_button(true, 1300, 1000, 1300) == 8 &&
        start_button(true, 1450, 1000, 1450) == 0 && start_button(true, 2800, 1000, 2800) == 8 &&
        start_button(true, 2800, 1000, 2000) == 0 && start_button(true, 1400, 1000, 0) == 0;
    std::fprintf(stderr, "[compat-check] movie skip: opt-in, release, cadence, stalled video: %s\n",
                 ok ? "PASS" : "FAIL");
    return ok && port_check_movie_detection() == 0 ? 0 : 1;
}

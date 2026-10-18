// Owns D3D12 presentation, FIFO draining and guest vblank/flip pacing.
#include "host.h"
#include <cstdio>
#include <cstdlib>

extern "C"
{
    int rsx_d3d12_backend_init(uint32_t, uint32_t, const char *);
    void rsx_d3d12_backend_present(void);
    int rsx_d3d12_backend_pump_messages(void);
    void cellGcmTickVBlank(void);
    void cellGcmTickFlip(void);
    int cellGcm_take_flip_pending(void);
    void cellGcm_rsx_process_fifo(void);
    void cellGcm_fifo_kick_wait(unsigned);
    unsigned cellGcm_flip_request_count(void);
}

static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_d3d12_backend_present();
    cellGcmTickFlip();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which is not an atomic increment. */
    InterlockedIncrement(&g_frames_presented);
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char *title = getenv("PS3_TITLE");
    if (!title || !*title)
        title = "ps3recomp";

    int rsx_ok = (rsx_d3d12_backend_init(1280, 720, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", "D3D12", rsx_ok ? "OK -- window open" : "FAILED");

    unsigned last_flip = 0;
    ULONGLONG next_tick = GetTickCount64();
    const bool profile = getenv("PORT_CLOCK_PROFILE") != nullptr;
    auto seconds = []()
    {
        LARGE_INTEGER q, f;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&f);
        return (double)q.QuadPart / f.QuadPart;
    };
    double window = seconds(), idle = 0, drain = 0, present = 0;
    unsigned drains = 0, presents = 0;
    auto walk = [&]()
    {
        double t = profile ? seconds() : 0;
        cellGcm_rsx_process_fifo();
        if (profile)
        {
            drain += seconds() - t;
            drains++;
        }
    };
    auto show = [&](bool guest)
    {
        double t = profile ? seconds() : 0;
        if (guest)
            present_guest_frame();
        else
            rsx_d3d12_backend_present();
        if (profile)
        {
            present += seconds() - t;
            presents++;
        }
    };

    for (;;)
    {
        double wait_start = profile ? seconds() : 0;
        cellGcm_fifo_kick_wait(4);
        if (profile)
            idle += seconds() - wait_start;
        ULONGLONG now = GetTickCount64();

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240)
        {
            cellGcmTickVBlank();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending())
            {
                show(true);
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok)
                walk();
            next_tick += 16; /* ~60 Hz */
            fired++;
        }
        if (fired >= 240)
            next_tick = now; /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok)
        {
            if (cellGcm_take_flip_pending())
            {
                show(true);
                last_flip = cellGcm_flip_request_count();
            }
            walk();

            if (rsx_d3d12_backend_pump_messages() != 0)
            {
                rsx_ok = 0; /* window closed */
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip)
            {
                /* The FIFO drain also publishes a pending notification. Claim
                 * it here, otherwise the next iteration presents this flip a
                 * second time, potentially after next-frame writes. */
                cellGcm_take_flip_pending();
                show(true);
                last_flip = fc;
            }
            else if (fc == 0)
            {
                show(false);
            }
        }
        if (profile && seconds() - window >= 3)
        {
            double elapsed = seconds() - window;
            fprintf(stderr,
                    "[clock-perf] wall=%.3f idle=%.3f drain=%.3f/%u present=%.3f/%u other=%.3f\n",
                    elapsed, idle, drain, drains, present, presents,
                    elapsed - idle - drain - present);
            window = seconds();
            idle = drain = present = 0;
            drains = presents = 0;
        }
    }
    return 0;
}

void port_start_frame_clock()
{
    CreateThread(nullptr, 4u * 1024 * 1024, frame_clock, nullptr, 0, nullptr);
}

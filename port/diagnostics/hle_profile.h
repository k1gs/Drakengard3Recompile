#pragma once
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <atomic>
#include <cstring>

extern "C" int ppu_prof_snapshot(int, unsigned *, unsigned *, const char **);

// Opt-in inclusive timing of outermost HLE calls on the selected PPU thread.
// Guest gaps include direct syscalls, lifted code and host scheduling delays.
struct PortHleProfile
{
    using Clock = std::chrono::steady_clock;
    struct Entry
    {
        unsigned nid, lr, count, a3;
        unsigned long long a4;
        double seconds;
    };
    inline static std::array<Entry, 512> entries{};
    inline static std::mutex lock;
    inline static Clock::time_point window{}, last_end{};
    inline static double hle = 0, gaps = 0;
    inline static thread_local unsigned depth = 0;
    inline static std::atomic<unsigned> waited_cond{0}, signal_lines{0};
    inline static std::atomic<long long> waited_at{0};
    bool entered = false, outer = false;
    unsigned nid = 0, lr = 0, tid = 0;
    unsigned a3 = 0;
    unsigned long long a4 = 0;
    Clock::time_point start{};
    PortHleProfile(unsigned id, ppu_context *ctx)
    {
        static const bool enabled = std::getenv("PORT_HLE_PROFILE") != nullptr;
        if (!enabled || !ctx)
            return;
        if (id == 0x8000006Bu && ctx->thread_id == 1)
        {
            waited_cond.store((unsigned)ctx->gpr[3]);
            waited_at.store(Clock::now().time_since_epoch().count());
        }
        if (id >= 0x8000006Cu && id <= 0x8000006Eu && ctx->gpr[3] == waited_cond.load() &&
            signal_lines.fetch_add(1) < 32)
        {
            double elapsed =
                std::chrono::duration<double>(
                    Clock::duration(Clock::now().time_since_epoch().count() - waited_at.load()))
                    .count();
            fprintf(stderr, "[cond-signal] cond=%llu tid=%llu lr=%08X since-wait=%.3f\n",
                    (unsigned long long)ctx->gpr[3], (unsigned long long)ctx->thread_id,
                    (unsigned)ctx->lr, elapsed);
        }
        static const unsigned target = []()
        {
            const char *v = std::getenv("PORT_HLE_PROFILE_TID");
            return v ? (unsigned)std::strtoul(v, nullptr, 10) : 1u;
        }();
        static const char *target_name = std::getenv("PORT_HLE_PROFILE_NAME");
        if (target_name && *target_name)
        {
            unsigned named_tid = 0, pc = 0;
            const char *name = nullptr;
            if (!ppu_prof_snapshot((int)ctx->thread_id - 1, &named_tid, &pc, &name) ||
                named_tid != ctx->thread_id || !name || std::strcmp(name, target_name) != 0)
                return;
        }
        else if (ctx->thread_id != target)
            return;
        entered = true;
        outer = depth++ == 0;
        if (!outer)
            return;
        nid = id;
        lr = (unsigned)ctx->lr;
        tid = (unsigned)ctx->thread_id;
        start = Clock::now();
        a3 = (unsigned)ctx->gpr[3];
        a4 = ctx->gpr[4];
        std::lock_guard<std::mutex> guard(lock);
        if (window == Clock::time_point{})
            window = start;
        if (last_end != Clock::time_point{})
            gaps += std::chrono::duration<double>(start - last_end).count();
    }
    ~PortHleProfile()
    {
        if (!entered)
            return;
        --depth;
        if (!outer)
            return;
        auto end = Clock::now();
        double dt = std::chrono::duration<double>(end - start).count();
        std::lock_guard<std::mutex> guard(lock);
        hle += dt;
        last_end = end;
        for (auto &e : entries)
            if (!e.count || (e.nid == nid && e.lr == lr && (nid != 0x8000006Bu || e.a3 == a3)))
            {
                if (!e.count)
                {
                    e.nid = nid;
                    e.lr = lr;
                    e.a3 = a3;
                    e.a4 = a4;
                }
                e.count++;
                e.seconds += dt;
                break;
            }
        double wall = std::chrono::duration<double>(end - window).count();
        if (wall < 3)
            return;
        fprintf(stderr, "[ppu-perf] tid=%u wall=%.3f hle=%.3f guest-gap=%.3f\n", tid, wall, hle,
                gaps);
        for (unsigned rank = 0; rank < 4; rank++)
        {
            Entry *best = nullptr;
            for (auto &e : entries)
                if (e.count && (!best || e.seconds > best->seconds))
                    best = &e;
            if (!best)
                break;
            fprintf(stderr,
                    "[ppu-perf] tid=%u nid=%08X lr=%08X calls=%u seconds=%.3f a3=%08X a4=%llX\n",
                    tid, best->nid, best->lr, best->count, best->seconds, best->a3, best->a4);
            best->count = 0;
        }
        for (auto &e : entries)
            e = Entry{};
        window = end;
        hle = gaps = 0;
    }
};

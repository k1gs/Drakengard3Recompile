/* sysPrxForUser condition variables use an eight-byte guest object. Waiters
 * belong to the current queue ID, so recreating an address cannot wake old
 * waiters. The same host lock serializes registration and notification. */
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "ps3emu/error_codes.h"

struct PortLwcondWaiter {
    uint32_t condition, queue, tid;
    bool signalled = false;
    std::condition_variable wake;
    PortLwcondWaiter* next = nullptr;
};

static std::mutex port_lwcond_lock;
static PortLwcondWaiter* port_lwcond_first;
static uint32_t port_lwcond_next_id = 0x4C000000u;

static bool port_lwcond_valid(uint32_t ea)
{
    return ea >= 0x10000 && ea <= UINT32_MAX - 7 && (ea & 3u) == 0;
}

static void port_lwcond_result(ppu_context* ctx, uint32_t value)
{
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)value;
}

static void sys_lwcond_create(ppu_context* ctx)
{
    const uint32_t condition = (uint32_t)ctx->gpr[3];
    const uint32_t mutex = (uint32_t)ctx->gpr[4];
    if (!port_lwcond_valid(condition) || !port_lwcond_valid(mutex))
        return port_lwcond_result(ctx, CELL_EINVAL);
    const std::lock_guard<std::mutex> lock(port_lwcond_lock);
    for (auto* waiter = port_lwcond_first; waiter; waiter = waiter->next)
        if (waiter->condition == condition)
            return port_lwcond_result(ctx, CELL_EBUSY);
    if (++port_lwcond_next_id == 0)
        ++port_lwcond_next_id;
    vm_write32(condition, mutex);
    vm_write32(condition + 4, port_lwcond_next_id);
    port_lwcond_result(ctx, CELL_OK);
}

static void sys_lwcond_destroy(ppu_context* ctx)
{
    const uint32_t condition = (uint32_t)ctx->gpr[3];
    if (!port_lwcond_valid(condition))
        return port_lwcond_result(ctx, CELL_EINVAL);
    const std::lock_guard<std::mutex> lock(port_lwcond_lock);
    if (!vm_read32(condition + 4))
        return port_lwcond_result(ctx, CELL_ESRCH);
    for (auto* waiter = port_lwcond_first; waiter; waiter = waiter->next)
        if (waiter->condition == condition)
            return port_lwcond_result(ctx, CELL_EBUSY);
    vm_write32(condition, 0);
    vm_write32(condition + 4, 0);
    port_lwcond_result(ctx, CELL_OK);
}

static void port_lwcond_signal(ppu_context* ctx, bool all, bool targeted)
{
    const uint32_t condition = (uint32_t)ctx->gpr[3];
    const uint64_t target = ctx->gpr[4];
    if (!port_lwcond_valid(condition))
        return port_lwcond_result(ctx, CELL_EINVAL);
    const std::lock_guard<std::mutex> lock(port_lwcond_lock);
    const uint32_t queue = vm_read32(condition + 4);
    if (!queue)
        return port_lwcond_result(ctx, CELL_ESRCH);
    for (auto* waiter = port_lwcond_first; waiter; waiter = waiter->next) {
        if (waiter->condition != condition || waiter->queue != queue ||
            waiter->signalled || (targeted && waiter->tid != target))
            continue;
        waiter->signalled = true;
        waiter->wake.notify_one();
        if (!all)
            return port_lwcond_result(ctx, CELL_OK);
    }
    port_lwcond_result(ctx, targeted ? CELL_EPERM : CELL_OK);
}

static void sys_lwcond_signal(ppu_context* ctx) { port_lwcond_signal(ctx, false, false); }
static void sys_lwcond_signal_all(ppu_context* ctx) { port_lwcond_signal(ctx, true, false); }
static void sys_lwcond_signal_to(ppu_context* ctx) { port_lwcond_signal(ctx, false, true); }

static void sys_lwcond_wait(ppu_context* ctx)
{
    using Clock = std::chrono::steady_clock;
    const uint32_t condition = (uint32_t)ctx->gpr[3];
    const uint64_t usec = ctx->gpr[4];
    if (!port_lwcond_valid(condition))
        return port_lwcond_result(ctx, CELL_EINVAL);

    std::unique_lock<std::mutex> lock(port_lwcond_lock);
    const uint32_t mutex = vm_read32(condition);
    const uint32_t queue = vm_read32(condition + 4);
    if (!queue || !port_lwcond_valid(mutex))
        return port_lwcond_result(ctx, CELL_ESRCH);
    HANDLE sem = lwm_sem(mutex);
    if (!sem)
        return port_lwcond_result(ctx, CELL_ESRCH);
    const uint32_t owner = vm_read32(mutex + LWM_OWNER);
    const uint32_t recursion = vm_read32(mutex + LWM_RECUR);
    if (owner != LWM_SELF(ctx) || !recursion)
        return port_lwcond_result(ctx, CELL_EPERM);

    PortLwcondWaiter waiter{condition, queue, LWM_SELF(ctx)};
    auto** tail = &port_lwcond_first;
    while (*tail)
        tail = &(*tail)->next;
    *tail = &waiter;
    vm_write32(mutex + LWM_RECUR, 0);
    vm_write32(mutex + LWM_OWNER, 0);
    const bool released = ReleaseSemaphore(sem, 1, nullptr) != 0;
    if (released) {
        if (usec) {
            const auto now = Clock::now();
            const auto limit = (Clock::time_point::max)();
            const auto max_wait = std::chrono::duration_cast<std::chrono::microseconds>(limit - now);
            const auto wait = usec <= static_cast<uint64_t>(max_wait.count())
                ? now + std::chrono::microseconds(usec) : limit;
            waiter.wake.wait_until(lock, wait, [&] { return waiter.signalled; });
        } else {
            waiter.wake.wait(lock, [&] { return waiter.signalled; });
        }
    }
    auto** entry = &port_lwcond_first;
    while (*entry != &waiter)
        entry = &(*entry)->next;
    *entry = waiter.next;
    lock.unlock();
    if (released && WaitForSingleObject(sem, INFINITE) != WAIT_OBJECT_0)
        return port_lwcond_result(ctx, CELL_ESRCH);
    vm_write32(mutex + LWM_OWNER, owner);
    vm_write32(mutex + LWM_RECUR, recursion);
    port_lwcond_result(ctx, !released ? CELL_EPERM : waiter.signalled ? CELL_OK : CELL_ETIMEDOUT);
}

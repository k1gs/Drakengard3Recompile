#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <vector>

extern "C" uint64_t spu_workload_fingerprint(const void* data, size_t size);

// These counters are intentionally exposed for read-only diagnostics. A reader
// may observe them between updates.
extern "C" {
uint64_t port_spu_image_cache_calls;
uint64_t port_spu_image_cache_hits;
uint64_t port_spu_image_cache_hashes;
uint64_t port_spu_image_cache_hash_bytes;
}

namespace {

constexpr size_t kMaxImageSize = 1u << 20;
constexpr size_t kCacheCapacity = 8;

struct CachedImage {
    uintptr_t source_address = 0;
    std::vector<uint8_t> bytes;
    uint64_t fingerprint = 0;
};

std::array<CachedImage, kCacheCapacity> gCache;
std::shared_mutex gCacheMutex;
size_t gNextVictim = 0;

void increment_counter(uint64_t& counter, uint64_t amount = 1)
{
    std::atomic_ref<uint64_t>(counter).fetch_add(amount, std::memory_order_relaxed);
}

uint64_t hash_image(const void* data, size_t size)
{
    increment_counter(port_spu_image_cache_hashes);
    increment_counter(port_spu_image_cache_hash_bytes, size);
    return spu_workload_fingerprint(data, size);
}

} // namespace

extern "C" uint64_t port_spu_image_fingerprint(const void* data, size_t size)
{
    increment_counter(port_spu_image_cache_calls);

    if (!data || !size || size > kMaxImageSize)
        return hash_image(data, size);

    const auto address = reinterpret_cast<uintptr_t>(data);
    {
        std::shared_lock lock(gCacheMutex);
        for (const CachedImage& cached : gCache) {
            if (cached.source_address == address && cached.bytes.size() == size &&
                std::memcmp(cached.bytes.data(), data, size) == 0) {
                increment_counter(port_spu_image_cache_hits);
                return cached.fingerprint;
            }
        }
    }

    // Keep an owned copy to detect writes to the same guest address.
    // As with the underlying hash, the caller must keep the input stable for
    // the duration of this call.
    std::vector<uint8_t> snapshot;
    try {
        const auto* first = static_cast<const uint8_t*>(data);
        snapshot.assign(first, first + size);
    } catch (const std::bad_alloc&) {
        return hash_image(data, size);
    }

    const uint64_t image_fingerprint = hash_image(snapshot.data(), size);

    std::unique_lock lock(gCacheMutex);
    CachedImage* replacement = nullptr;
    for (CachedImage& cached : gCache) {
        if (cached.source_address == address && cached.bytes.size() == size) {
            replacement = &cached;
            break;
        }
    }

    if (!replacement)
        replacement = &gCache[gNextVictim++ % gCache.size()];

    replacement->bytes.swap(snapshot);
    replacement->source_address = address;
    replacement->fingerprint = image_fingerprint;
    return image_fingerprint;
}

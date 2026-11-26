#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" uint64_t spu_workload_fingerprint(const void*, size_t);
extern "C" uint64_t port_spu_async_contract_fingerprint(const void*, size_t);
extern "C" uint64_t port_spu_image_cache_hits;

namespace {

using TestImage = std::array<uint8_t, 32 * 1024>;

bool matches_reference(const void* data, size_t size)
{
    return port_spu_async_contract_fingerprint(data, size) == spu_workload_fingerprint(data, size);
}

bool check_mutations(TestImage& image)
{
    bool passed = true;
    for (size_t offset : {size_t{0}, image.size() / 2, image.size() - 1}) {
        image[offset] ^= 0x80;
        const bool changed = matches_reference(image.data(), image.size());
        image[offset] ^= 0x80;
        const bool restored = matches_reference(image.data(), image.size());
        passed = passed && changed && restored;
    }
    return passed;
}

bool check_cache_limits(const TestImage& original)
{
    const std::vector<uint8_t> oversized((1u << 20) + 1, 0x53);
    bool passed = matches_reference(oversized.data(), oversized.size());

    // Keep twelve distinct addresses live to fill the eight slots and trigger eviction.
    std::array<std::array<uint8_t, 64>, 12> images{};
    for (size_t i = 0; i < images.size(); ++i) {
        auto& image = images[i];
        image.fill(static_cast<uint8_t>(i));
        if (!matches_reference(image.data(), image.size())) {
            passed = false;
        }
    }
    return matches_reference(original.data(), original.size()) && passed;
}

bool check_parallel_lookups()
{
    std::atomic<bool> passed{true};
    std::array<std::thread, 4> workers;
    for (size_t i = 0; i < workers.size(); ++i) {
        workers[i] = std::thread([i, &passed] {
            std::array<uint8_t, 8192> image{};
            image.fill(static_cast<uint8_t>(i + 1));
            const auto expected = spu_workload_fingerprint(image.data(), image.size());
            for (unsigned lookup = 0; lookup < 200; ++lookup) {
                if (port_spu_async_contract_fingerprint(image.data(), image.size()) != expected) {
                    passed.store(false);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return passed.load();
}

double time_1000_lookups(const TestImage& image, uint64_t (*fingerprint)(const void*, size_t),
                         uint64_t& checksum)
{
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 1000; ++i) {
        checksum ^= fingerprint(image.data(), image.size());
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

extern "C" int port_check_spu_image_cache()
{
    TestImage image{};
    for (size_t i = 0; i < image.size(); ++i) {
        image[i] = static_cast<uint8_t>((i * 37) ^ (i >> 7));
    }

    const bool first_lookup = matches_reference(image.data(), image.size());
    std::atomic_ref<uint64_t> hits(port_spu_image_cache_hits);
    const auto hits_before = hits.load(std::memory_order_relaxed);
    const bool repeat_lookup = matches_reference(image.data(), image.size());
    const bool warm = hits.load(std::memory_order_relaxed) > hits_before;
    const bool values = first_lookup && repeat_lookup;
    const bool mutations = check_mutations(image);

    const auto copy = image;
    const bool sizes = matches_reference(copy.data(), copy.size()) &&
                       matches_reference(image.data(), image.size() - 1) &&
                       matches_reference(image.data(), 0) && matches_reference(nullptr, 0);
    const bool bounded = check_cache_limits(image);
    const bool parallel = check_parallel_lookups();

    uint64_t checksum = 0;
    const double original_seconds = time_1000_lookups(image, spu_workload_fingerprint, checksum);
    const double cached_seconds = time_1000_lookups(image, port_spu_async_contract_fingerprint, checksum);
    const bool passed = values && warm && mutations && sizes && bounded && parallel && checksum == 0;
    std::fprintf(stderr,
                 "[compat-check] SPU image cache values=%d warm=%d mutations=%d sizes/moved=%d "
                 "bounded=%d parallel=%d %s; 1000x32KiB original=%.6fs cached=%.6fs\n",
                 values, warm, mutations, sizes, bounded, parallel, passed ? "PASS" : "FAIL",
                 original_seconds, cached_seconds);
    return passed ? 0 : 1;
}

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <thread>
#include <vector>

extern "C" uint64_t spu_workload_fingerprint(const void*,size_t);
extern "C" uint64_t port_spu_async_contract_fingerprint(const void*,size_t);

// Read-only diagnostic snapshots may observe these counters between updates.
extern "C" {
uint64_t port_spu_image_cache_calls,port_spu_image_cache_hits;
uint64_t port_spu_image_cache_hashes,port_spu_image_cache_hash_bytes;
}
namespace {
constexpr size_t max_image_size=1u<<20;
struct Image {
    uintptr_t address=0;
    std::vector<uint8_t> bytes;
    uint64_t fingerprint=0;
};
std::array<Image,8> images;
std::shared_mutex image_lock;
size_t victim=0;
void increment(uint64_t& counter,uint64_t n=1) {
    std::atomic_ref<uint64_t>(counter).fetch_add(n,std::memory_order_relaxed);
}
uint64_t fingerprint(const void* data,size_t size) {
    increment(port_spu_image_cache_hashes);
    increment(port_spu_image_cache_hash_bytes,size);
    return spu_workload_fingerprint(data,size);
}
}

extern "C" uint64_t port_spu_image_fingerprint(const void* data,size_t size)
{
    increment(port_spu_image_cache_calls);
    if(!data || !size || size>max_image_size) return fingerprint(data,size);
    {
        std::shared_lock guard(image_lock);
        for(const auto& image:images)
            if(image.address==reinterpret_cast<uintptr_t>(data) && image.bytes.size()==size &&
               !std::memcmp(image.bytes.data(),data,size)) {
                increment(port_spu_image_cache_hits);
                return image.fingerprint;
            }
    }
    // Snapshot all bytes, rather than trusting a guest address or a prefix.
    // As with the original hash, callers must own stable input during the call.
    std::vector<uint8_t> snapshot;
    try { snapshot.assign(static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+size); }
    catch(const std::bad_alloc&) { return fingerprint(data,size); }
    uint64_t hash=fingerprint(snapshot.data(),size);
    std::unique_lock guard(image_lock);
    Image* replace=nullptr;
    for(auto& image:images)
        if(image.address==reinterpret_cast<uintptr_t>(data) && image.bytes.size()==size) {
            replace=&image;
            break;
        }
    if(!replace) replace=&images[victim++%images.size()];
    replace->bytes.swap(snapshot);
    replace->address=reinterpret_cast<uintptr_t>(data);
    replace->fingerprint=hash;
    return hash;
}

extern "C" int port_check_spu_image_cache()
{
    std::array<uint8_t,32768> bytes{};
    for(size_t i=0;i<bytes.size();i++)bytes[i]=uint8_t((i*37)^(i>>7));
    auto actual=[](const void* p,size_t n) {return port_spu_async_contract_fingerprint(p,n);};
    auto correct=[&](const void* p,size_t n) {return actual(p,n)==spu_workload_fingerprint(p,n);};
    bool values=correct(bytes.data(),bytes.size());
    uint64_t hits=port_spu_image_cache_hits;
    values=correct(bytes.data(),bytes.size())&&values;
    bool warm=port_spu_image_cache_hits>hits;
    bool mutations=true;
    for(size_t pos: {size_t(0),bytes.size()/2,bytes.size()-1}) {
        bytes[pos]^=0x80;mutations=correct(bytes.data(),bytes.size())&&mutations;
        bytes[pos]^=0x80;mutations=correct(bytes.data(),bytes.size())&&mutations;
    }
    auto moved=bytes;
    bool sizes=correct(moved.data(),moved.size())&&correct(bytes.data(),bytes.size()-1)&&
        correct(bytes.data(),0)&&correct(nullptr,0);
    std::vector<uint8_t> large(max_image_size+1,0x53);
    bool bounded=correct(large.data(),large.size());
    std::array<std::array<uint8_t,64>,12> eviction{};
    for(size_t i=0;i<eviction.size();i++) {
        eviction[i].fill(uint8_t(i));bounded=correct(eviction[i].data(),64)&&bounded;
    }
    bounded=correct(bytes.data(),bytes.size())&&bounded;
    std::atomic<bool> parallel{true};std::array<std::thread,4> threads;
    for(unsigned t=0;t<threads.size();t++)threads[t]=std::thread([&,t] {
        std::array<uint8_t,8192> data{};data.fill(uint8_t(t+1));
        uint64_t expected=spu_workload_fingerprint(data.data(),data.size());
        for(unsigned i=0;i<200;i++)if(actual(data.data(),data.size())!=expected)parallel=false;
    });
    for(auto& thread:threads)thread.join();
    auto begin=std::chrono::steady_clock::now();uint64_t result=0;
    for(unsigned i=0;i<1000;i++)result^=spu_workload_fingerprint(bytes.data(),bytes.size());
    double original=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    begin=std::chrono::steady_clock::now();
    for(unsigned i=0;i<1000;i++)result^=actual(bytes.data(),bytes.size());
    double cached=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    bool ok=values&&warm&&mutations&&sizes&&bounded&&parallel.load()&&result==0;
    fprintf(stderr,"[compat-check] SPU image cache values=%d warm=%d mutations=%d sizes/moved=%d bounded=%d parallel=%d %s; 1000x32KiB original=%.6fs cached=%.6fs\n",
        values,warm,mutations,sizes,bounded,parallel.load(),ok?"PASS":"FAIL",original,cached);
    return ok?0:1;
}

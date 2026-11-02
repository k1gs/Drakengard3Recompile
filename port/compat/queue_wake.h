#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// A notification only invalidates a wait ticket. The caller must check the
// guest queue again under its lock; a notification never supplies queue data.
class PortQueueWakeRegistry {
public:
    class Wait {
        std::mutex mutex;
        std::condition_variable changed;
        uint64_t generation=0;
    public:
        uint64_t snapshot() {
            std::lock_guard lock(mutex);
            return generation;
        }
        void publish() {
            { std::lock_guard lock(mutex); ++generation; }
            changed.notify_all();
        }
        bool wait(uint64_t ticket, std::chrono::milliseconds limit=std::chrono::milliseconds(1)) {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock,limit,[&] { return generation!=ticket; });
        }
    };
private:
    struct Entry { uint32_t line=0; Wait wait; };
    std::array<Entry,128> entries{};
    std::mutex directory;
    std::atomic<bool> registered{false};
public:
    Wait* get(uint32_t line) {
        if(!line || (line&127)) return nullptr;
        std::lock_guard lock(directory);
        for(auto& entry:entries) if(entry.line==line) return &entry.wait;
        for(auto& entry:entries) if(!entry.line) {
            entry.line=line;
            registered.store(true,std::memory_order_release);
            return &entry.wait;
        }
        // Retain bounded polling if the optional host directory is full.
        return nullptr;
    }
    bool publish(uint32_t line) {
        if(!line || (line&127)) return false;
        if(!registered.load(std::memory_order_acquire)) return false;
        Wait* waiter=nullptr;
        {
            std::lock_guard lock(directory);
            for(auto& entry:entries) if(entry.line==line) { waiter=&entry.wait; break; }
        }
        if(!waiter) return false;
        waiter->publish();
        return true;
    }
};

PortQueueWakeRegistry& port_queue_wakes();

#pragma once
#include <cstdint>

// Process-launch diagnostics. The task-pointer watch also needs stores when
// the general writer diagnostics are disabled.
extern "C" int port_gpu_ppu_write_active;
inline constexpr uint64_t port_task_pointer_address = 0x01AC39B8;

inline bool port_gpu_ppu_write32_needed(uint64_t address)
{
    return port_gpu_ppu_write_active || address == port_task_pointer_address;
}

inline bool port_gpu_ppu_write64_needed(uint64_t address)
{
    return port_gpu_ppu_write_active || address == port_task_pointer_address ||
           address + 4 == port_task_pointer_address;
}

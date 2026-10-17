#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

inline constexpr uint64_t kGuestVmSize = 0x100010000ull;
inline constexpr uint32_t kGuestStackTop = 0x0FF00000u;

bool port_allocate_guest_vm();
void port_start_frame_clock();

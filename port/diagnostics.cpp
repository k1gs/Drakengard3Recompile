#include "ppu_recomp.h"
#include "win32_compat.h"
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <dbghelp.h>
#endif

extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;

static void guest_context()
{
    auto* ctx = g_active_ctx;
    if (!ctx) return;
    fprintf(stderr, "[guest] cia=%08llX lr=%08llX ctr=%08llX tid=%llu\n",
            (unsigned long long)ctx->cia, (unsigned long long)ctx->lr,
            (unsigned long long)ctx->ctr, (unsigned long long)ctx->thread_id);
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "[guest] r%d=%016llX r%d=%016llX r%d=%016llX r%d=%016llX\n",
                i, (unsigned long long)ctx->gpr[i], i+1, (unsigned long long)ctx->gpr[i+1],
                i+2, (unsigned long long)ctx->gpr[i+2], i+3, (unsigned long long)ctx->gpr[i+3]);
}

extern "C" [[noreturn]] void port_unimplemented_instruction(unsigned long long function, const char* detail)
{
    fprintf(stderr, "[lift-blocker] function=%08llX instruction=%s\n", function, detail);
    guest_context();
    fflush(stderr);
    std::exit(3);
}

extern "C" void port_spu_unsupported(uint32_t pc, const char* mnemonic)
{
    fprintf(stderr, "[spu-lift-blocker] ls_pc=%05X instruction=%s\n", pc, mnemonic);
    fflush(stderr);
    std::exit(4);
}

extern "C" [[noreturn]] void port_resource_blocker(const char* path)
{
    fprintf(stderr, "[resource-blocker] encrypted NPD data is not usable as plaintext: %s\n", path);
    fprintf(stderr, "[resource-blocker] A supported, authorized plaintext resource is required; original data was not modified.\n");
    guest_context();
    fflush(stderr);
    std::exit(5);
}

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    fprintf(stderr, "[host-crash] exception=%08lX address=%p rva=%llX\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
            (unsigned long long)((uintptr_t)ep->ExceptionRecord->ExceptionAddress-base));
    guest_context();
    auto file = CreateFileW(L"native-crash.dmp", GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ep, FALSE};
        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &info, nullptr, nullptr);
        fprintf(stderr, "[host-crash] minidump=%s\n", ok ? "native-crash.dmp" : "failed");
        CloseHandle(file);
    }
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

extern "C" void port_install_diagnostics()
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash_filter);
#endif
}

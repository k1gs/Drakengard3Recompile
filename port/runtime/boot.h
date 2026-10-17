#pragma once
#include <cstdint>

extern "C"
{
    uint32_t ppu_load_elf(const char *path); /* ELF -> guest RAM, returns entry OPD */
    void ppu_recomp_register(void);          /* generated: lifted table -> address map */
    void ppu_hle_init(void);                 /* firmware import NID -> HLE handler */
    void ppu_sysprx_register(void);          /* boot-critical CRT (sys_initialize_tls, ...) */
    void ppu_fs_register(void);              /* cellFs over the real game directory */
    void lv2_init_syscalls(void);            /* the lv2 syscall table */
    int ppu_run(uint32_t entry_opd, uint32_t stack_top);

    /* This project's own: load whatever real system PRX modules the title needs
     * into guest RAM and register their exports. stubs.cpp has the empty version
     * a game with no lifted PRX wants. */
    void ps3_load_prx_modules(void);

    extern const char *ppu_vfs_root; /* host dir the PS3 mount points map into */

    /* The status the guest handed to sys_process_exit. A title that exits that way
     * never returns through ppu_run, so this is only read on the path where the
     * entry function unwound instead. */
    extern int g_sys_process_exit_called;
    extern int32_t g_sys_process_exit_code;

    /* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
     * calls back into recompiled code through -- cellSysutil events and the GCM
     * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
    typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                        uint64_t, uint64_t, uint64_t);
    extern ps3_guest_caller_fn g_ps3_guest_caller;
    uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                            uint64_t, uint64_t);
}

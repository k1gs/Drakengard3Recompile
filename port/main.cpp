// Drakengard 3 native Windows boot: load, register, then dispatch the title.
#include "runtime/boot.h"
#include "runtime/host.h"
#include "checks/registry.h"
#include <timeapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C"
{
    void port_install_diagnostics();
    void cellGame_init_from_paramsfo(const char *);
    void drakengard_spu_register_all();
    void drakengard_raw_jobs_register_all();
    void port_msdsp_register();
    void port_register_compatibility();
}

static char s_vfs_root[1024];

static void derive_vfs_root(const char *eboot)
{
    const char *env = getenv("PS3_VFS_ROOT");
    if (env && *env)
    {
        ppu_vfs_root = env;
        return;
    }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char *p = s_vfs_root; *p; p++)
        if (*p == '\\')
            *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++)
    {
        char *s = strrchr(s_vfs_root, '/');
        if (s)
            *s = 0;
    }
    if (!s_vfs_root[0])
        strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

int main(int argc, char **argv)
{
    port_install_diagnostics();
    if (argc < 2)
    {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

    // Preserve sub-millisecond guest waits on Windows.
    timeBeginPeriod(1);
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("=== Drakengard 3 experimental native build ===\n");

    if (!port_allocate_guest_vm())
    {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }
    int check_result = 0;
    if (port_run_check(argv[1], check_result))
        return check_result;

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry)
    {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    char sfo[1200];
    snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
    cellGame_init_from_paramsfo(sfo);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register(); /* lifted function table -> address map */
    drakengard_spu_register_all();
    drakengard_raw_jobs_register_all();
    port_msdsp_register();
    ps3_load_prx_modules(); /* this game's lifted system PRX, if it has any */
    ppu_hle_init();         /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();  /* boot-critical CRT */
    ppu_fs_register();      /* cellFs over the game directory */
    port_register_compatibility();
    lv2_init_syscalls(); /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    port_start_frame_clock();

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, kGuestStackTop);

    int rc = ppu_run(entry, kGuestStackTop);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}

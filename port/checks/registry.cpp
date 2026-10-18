#include "registry.h"
#include <cstdio>
#include <cstring>

extern "C"
{
    int port_check_present_boundary();
    int port_check_lwcond_layout();
    int port_check_memory_table();
    int port_check_queue();
    int port_check_lfqueue();
    int port_check_spurs_events();
    int port_check_vertex_constants();
    int port_check_depth_state();
    int port_check_vertex_cache();
    int port_check_pipeline_cache();
    int port_check_texture_units();
    int port_check_msdsp();
    int port_check_fragment_constants();
    int port_check_jobchain_resume();
    int port_check_vpkswss();
    int port_check_spu_return();
    int port_check_spu_task_snapshot();
    int port_check_gcm_park();
    int port_check_spu_interpreter_halt();
}

namespace
{
struct Check
{
    const char *argument;
    int (*run)();
};
const Check checks[] = {
    {"--check-present-boundary", port_check_present_boundary},
    {"--check-lwcond", port_check_lwcond_layout},
    {"--check-memory", port_check_memory_table},
    {"--check-queue", port_check_queue},
    {"--check-lfqueue", port_check_lfqueue},
    {"--check-spurs-events", port_check_spurs_events},
    {"--check-vertex-constants", port_check_vertex_constants},
    {"--check-depth-state", port_check_depth_state},
    {"--check-vertex-cache", port_check_vertex_cache},
    {"--check-pipeline-cache", port_check_pipeline_cache},
    {"--check-texture-units", port_check_texture_units},
    {"--check-msdsp", port_check_msdsp},
    {"--check-fragment-constants", port_check_fragment_constants},
    {"--check-jobchain-resume", port_check_jobchain_resume},
    {"--check-vpkswss", port_check_vpkswss},
    {"--check-spu-return", port_check_spu_return},
    {"--check-spu-task-snapshot", port_check_spu_task_snapshot},
    {"--check-gcm-park", port_check_gcm_park},
    {"--check-spu-interpreter-halt", port_check_spu_interpreter_halt},
};
} // namespace

bool port_run_check(const char *argument, int &result)
{
    if (std::strcmp(argument, "--list-checks") == 0)
    {
        for (const auto &check : checks)
            std::puts(check.argument);
        result = 0;
        return true;
    }
    for (const auto &check : checks)
    {
        if (std::strcmp(argument, check.argument) == 0)
        {
            result = check.run();
            return true;
        }
    }
    return false;
}

#include "registry.h"
#include <cstdio>
#include <cstring>

extern "C"
{
    int port_check_movie_skip();
    int port_check_present_boundary();
    int port_check_lwcond_layout();
    int port_check_memory_table();
    int port_check_queue();
    int port_check_lfqueue();
    int port_check_spurs_events();
    int port_check_vertex_constants();
    int port_check_depth_state();
    int port_check_depth_surfaces();
    int port_check_depth_surfaces_gpu();
    int port_check_vertex_cache();
    int port_check_pipeline_cache();
    int port_check_texture_units();
    int port_check_texture_cache();
    int port_check_gamepad();
    int port_check_compressed_vertices();
    int port_probe_gamepad();
    int port_check_msdsp();
    int port_check_audio_decoder();
    int port_check_fragment_constants();
    int port_check_jobchain_resume();
    int port_check_vpkswss();
    int port_check_spu_return();
    int port_check_spu_task_snapshot();
    int port_check_gcm_park();
    int port_check_fifo_flip_boundary();
    int port_check_prepare_flip();
    int port_check_spu_interpreter_halt();
    int port_check_spu_task_null_branch();
    int port_check_spu_task_replay();
    int port_check_task_write_watch();
    int port_check_taskset_arguments();
}

namespace
{
struct Check
{
    const char *argument;
    int (*run)();
};
const Check checks[] = {
    {"--check-compressed-vertices", port_check_compressed_vertices},
    {"--check-gamepad", port_check_gamepad},
    {"--check-taskset-arguments", port_check_taskset_arguments},
    {"--check-task-write-watch", port_check_task_write_watch},
    {"--check-movie-skip", port_check_movie_skip},
    {"--check-present-boundary", port_check_present_boundary},
    {"--check-lwcond", port_check_lwcond_layout},
    {"--check-memory", port_check_memory_table},
    {"--check-queue", port_check_queue},
    {"--check-lfqueue", port_check_lfqueue},
    {"--check-spurs-events", port_check_spurs_events},
    {"--check-vertex-constants", port_check_vertex_constants},
    {"--check-depth-state", port_check_depth_state},
    {"--check-depth-surfaces", port_check_depth_surfaces},
    {"--check-depth-surfaces-gpu", port_check_depth_surfaces_gpu},
    {"--check-vertex-cache", port_check_vertex_cache},
    {"--check-pipeline-cache", port_check_pipeline_cache},
    {"--check-texture-units", port_check_texture_units},
    {"--check-texture-cache", port_check_texture_cache},
    {"--check-msdsp", port_check_msdsp},
    {"--check-audio-decoder", port_check_audio_decoder},
    {"--check-fragment-constants", port_check_fragment_constants},
    {"--check-jobchain-resume", port_check_jobchain_resume},
    {"--check-vpkswss", port_check_vpkswss},
    {"--check-spu-return", port_check_spu_return},
    {"--check-spu-task-snapshot", port_check_spu_task_snapshot},
    {"--check-gcm-park", port_check_gcm_park},
    {"--check-fifo-flip-boundary", port_check_fifo_flip_boundary},
    {"--check-prepare-flip", port_check_prepare_flip},
    {"--check-spu-interpreter-halt", port_check_spu_interpreter_halt},
    {"--check-spu-task-null-branch", port_check_spu_task_null_branch},
};
} // namespace

bool port_run_check(const char *argument, int &result)
{
    if (std::strcmp(argument, "--probe-gamepad") == 0)
    {
        result = port_probe_gamepad();
        return true;
    }
    // This diagnostic needs private captures and is not an asset-free check.
    if (std::strcmp(argument, "--replay-spu-task") == 0)
    {
        result = port_check_spu_task_replay();
        return true;
    }
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

cmake_minimum_required(VERSION 3.20)
project(DrakengardRecompile
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Recompiled PS3 game project (ps3recomp)"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "This port targets Windows with Direct3D 12.")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: this project is still sitting inside ps3recomp/templates/project/
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ps3recomp" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../work/full-ppu-001" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${CMAKE_CURRENT_BINARY_DIR}/ppu_loader_compat.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_compat.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/ppu_fs.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/ppu_sysprx.cpp"
)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare_ppu_vmx.py"
    --lift "${RECOMP_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/prepare_runtime.py"
    --sdk "${PS3RECOMP_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/ppu_sysprx.cpp"
    COMMAND_ERROR_IS_FATAL ANY)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../work/full-spu-001" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(SPU_SOURCES "")
if(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${CMAKE_CURRENT_BINARY_DIR}/spu_dispatch_mt_compat.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c raw_jobs_register.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                if(gen STREQUAL "spu_workloads.c")
                    execute_process(COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/namespace_spu_registry.py"
                        "${SPU_DIR}/${gen}" --out "${CMAKE_CURRENT_BINARY_DIR}/spu_workloads_compat.c"
                        COMMAND_ERROR_IS_FATAL ANY)
                    list(APPEND SPU_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/spu_workloads_compat.c")
                else()
                    list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
                endif()
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

# Handwritten code is grouped by responsibility; generated code stays local.
set(PORT_RUNTIME_SOURCES
    runtime/frame_clock.cpp
    runtime/movie_skip.cpp
    runtime/guest_memory.cpp
    runtime/ppu_vmx.cpp
    runtime/msdsp_native.c
    runtime/audio_decoder_native.c
)
set(PORT_COMPAT_SOURCES
    compat/trophy_abi.cpp
    compat/resource_api.cpp
    compat/overlay_api.cpp
    compat/queue_api.cpp
)
set(PORT_DIAGNOSTIC_SOURCES
    diagnostics/diagnostics.cpp
    diagnostics/gpu_writer_trace.cpp
)
set(PORT_CHECK_SOURCES
    checks/registry.cpp
    checks/compat_checks.cpp
    checks/ppu_vmx_checks.cpp
    checks/spu_dispatch_checks.cpp
    checks/spu_task_checks.cpp
    checks/gcm_checks.cpp
    checks/msdsp_checks.c
    checks/audio_decoder_checks.c
)

add_executable(${PROJECT_NAME}
    main.cpp
    ${PORT_RUNTIME_SOURCES}
    ${PORT_COMPAT_SOURCES}
    ${PORT_DIAGNOSTIC_SOURCES}
    ${PORT_CHECK_SOURCES}
    "${CMAKE_CURRENT_BINARY_DIR}/edat_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/spu_workload_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/spu_channels_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/spu_interp_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/sys_memory_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/sys_event_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/cellSpurs_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/cellGcmSys_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/cellPad_compat.c"
    "${CMAKE_CURRENT_BINARY_DIR}/rsx_d3d12_backend_compat.c"
    stubs.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)
set(MSDSP_SOURCES runtime/msdsp_native.c checks/msdsp_checks.c)
set_source_files_properties(${MSDSP_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")
if(PORT_ENABLE_SPU AND EXISTS "${SPU_DIR}/msdsp_native/msdsp_bytes.h")
    set_property(SOURCE ${MSDSP_SOURCES} APPEND PROPERTY
        INCLUDE_DIRECTORIES "${SPU_DIR}/msdsp_native")
    set_property(SOURCE ${MSDSP_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS PORT_WITH_MSDSP)
endif()
set(AUDIO_DECODER_SOURCES runtime/audio_decoder_native.c checks/audio_decoder_checks.c)
set_source_files_properties(${AUDIO_DECODER_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu")
if(PORT_ENABLE_SPU AND EXISTS "${SPU_DIR}/audio_decoder_native/audio_decoder_bytes.h")
    set_property(SOURCE ${AUDIO_DECODER_SOURCES} APPEND PROPERTY
        INCLUDE_DIRECTORIES "${SPU_DIR}/audio_decoder_native")
    set_property(SOURCE ${AUDIO_DECODER_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS PORT_WITH_AUDIO_DECODER)
endif()
if(NOT SPU_SOURCES)
    set_source_files_properties(stubs.cpp PROPERTIES COMPILE_DEFINITIONS PORT_WITHOUT_SPU)
endif()
if(NOT PORT_ENABLE_SPU OR NOT EXISTS "${SPU_DIR}/raw_jobs_register.c")
    set_property(SOURCE stubs.cpp APPEND PROPERTY COMPILE_DEFINITIONS PORT_WITHOUT_RAW_JOBS)
endif()

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(MSVC)
            set_source_files_properties(${SPU_SOURCES} PROPERTIES
                COMPILE_OPTIONS "-O2;-w;/FI${CMAKE_CURRENT_SOURCE_DIR}/spu_guard.h")
        else()
            set_source_files_properties(${SPU_SOURCES} PROPERTIES
                COMPILE_OPTIONS "-O2;-w;-include;${CMAKE_CURRENT_SOURCE_DIR}/spu_guard.h")
        endif()
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
)

# Link the C++ runtime even when the generated entry units are C.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        dxguid
        user32
        gdi32
        winmm
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - preserve explicit floating-point operations in generated code.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
if(MSVC)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/rsx_d3d12_backend_compat.c" PROPERTIES COMPILE_OPTIONS "/w;-O2;-fno-strict-aliasing;-ffp-contract=off")
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

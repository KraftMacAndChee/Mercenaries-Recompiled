cmake_minimum_required(VERSION 3.20)

# Mercenaries - Static Recompilation

project(mercenaries_recomp C CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    # Do not let the host Windows locale change how source bytes are parsed.
    # This applies to the port and runtime subdirectories added below.
    add_compile_options(/utf-8)
endif()

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Use the shared runtime from this source tree.
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get recomp_0000.c, recomp_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
set(RECOMP_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH
    "Directory containing generated Mercenaries recomp C sources")
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${RECOMP_GEN_DIR}/*.c")

include(scripts/Dev-Vehicle-Catalog.cmake)
include(scripts/Shader-Warmup.cmake)
include(scripts/Mission-Script-Fixes.cmake)

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/mod_compatibility.c
    src/recomp_world_properties.c
    src/shop_config.c
    src/audio_bank_fixes.c
    src/ps2_upgrades.c
    src/recomp_options.c
    src/recomp_original_bugs.c
    src/boids.c
    src/mission_script_fixes.c
    src/recomp_ui.c
    src/recomp_controls.c
    src/chapter_return.c
    src/recomp_prompts.c
    src/recomp_options_menu.c
    src/dev_menu.c
    src/dev_missions.c
    src/dev_overlay.c
    src/free_cam.c
    src/shader_warmup.c
    ${SHADER_WARMUP_HEADER}
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME}
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE resources/app.rc resources/ps2_upgrades.rc)
    target_include_directories(${PROJECT_NAME} PRIVATE resources)
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${RECOMP_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    dxgi
    dxguid
    xinput
    winmm
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    # Keep a linker map beside the executable so native crash RVAs can be
    # resolved even in optimized builds without a PDB.
    # Translated x86 calls use native C frames in addition to the simulated
    # Xbox stack. Complex title initialization exhausts Windows' 1 MiB default;
    # reserve 64 MiB of virtual address space while committing only 64 KiB up
    # front so the stack can grow on demand.
    # /Brepro removes wall-clock linker timestamps so identical inputs produce
    # byte-identical executables. This is required for ISO-to-port provenance:
    # a clean machine must be able to reproduce and verify the same artifact.
    target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE /MAP /STACK:67108864,65536 /Brepro)
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj /MP)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
    # Full guest function tracing is intentionally limited to diagnostic builds.
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:ENABLE_RECOMP_FUNC_TRACE=1>
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)


# End-user first-launch setup
#
# Keep setup and ISO extraction outside the recompiled game process. The
# launcher validates/extracts the user's retail disc assets and then starts the
# same game executable used by automated tests. It has no dependency on
# engineering reference material.
add_executable(mercenaries_launcher WIN32
    src/setup_launcher.c
    src/setup_art.cpp
)
if(WIN32)
    target_sources(mercenaries_launcher PRIVATE resources/app.rc resources/launcher.rc)
    target_include_directories(mercenaries_launcher PRIVATE resources)
endif()
target_link_libraries(mercenaries_launcher PRIVATE
    gdiplus
    ole32
    bcrypt
    comctl32
    comdlg32
    shell32
)
target_compile_definitions(mercenaries_launcher PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
if(MSVC)
    target_link_options(mercenaries_launcher PRIVATE /Brepro /STACK:8388608,65536)
endif()
set_target_properties(mercenaries_launcher PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    CXX_STANDARD 11
    OUTPUT_NAME "Mercenaries Recompiled"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

set(MERCENARIES_XDVDFS_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xdvdfs")
set(MERCENARIES_SDL2_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2")
add_custom_command(TARGET mercenaries_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:mercenaries_launcher>/tools"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MERCENARIES_XDVDFS_ROOT}/xdvdfs.exe"
        "$<TARGET_FILE_DIR:mercenaries_launcher>/tools/xdvdfs.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/launcher/AgencyFB-COPYRIGHT.txt"
        "$<TARGET_FILE_DIR:mercenaries_launcher>/tools/AgencyFB-COPYRIGHT.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MERCENARIES_XDVDFS_ROOT}/LICENSE"
        "$<TARGET_FILE_DIR:mercenaries_launcher>/tools/xdvdfs-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MERCENARIES_SDL2_ROOT}/SDL2.dll"
        "$<TARGET_FILE_DIR:mercenaries_launcher>/SDL2.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MERCENARIES_SDL2_ROOT}/LICENSE.txt"
        "$<TARGET_FILE_DIR:mercenaries_launcher>/SDL2-LICENSE.txt"
    VERBATIM
)

# Wine-only compiler dependency, kept off the native Windows DLL search path.
add_custom_command(TARGET mercenaries_recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/d3dcompiler"
        "$<TARGET_FILE_DIR:mercenaries_recomp>/compat"
)

# Copy the template only on a fresh build; never reset a developer's opt-in.
add_custom_command(TARGET mercenaries_recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/resources/developer.ini
        -DDESTINATION=$<TARGET_FILE_DIR:mercenaries_recomp>/developer.ini
        -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/Copy-Developer-Config.cmake
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/resources/modcompatibility.ini
        -DDESTINATION=$<TARGET_FILE_DIR:mercenaries_recomp>/modcompatibility.ini
        -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/Copy-Developer-Config.cmake
)

# Runtime attribution accompanies both local builds and player packages.
add_custom_command(TARGET mercenaries_recomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:mercenaries_recomp>/licenses"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XBOXRECOMP_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:mercenaries_recomp>/licenses/xboxrecomp-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XBOXRECOMP_DIR}/third_party/dsp56300/msvc-x64-v0.1.3/LICENSE"
        "$<TARGET_FILE_DIR:mercenaries_recomp>/licenses/DSP56300-LICENSE.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XBOXRECOMP_DIR}/src/licenses/xemu-LGPL-2.1.txt"
        "$<TARGET_FILE_DIR:mercenaries_recomp>/licenses/xemu-LGPL-2.1.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XBOXRECOMP_DIR}/src/licenses/xemu-GPL-2.0.txt"
        "$<TARGET_FILE_DIR:mercenaries_recomp>/licenses/xemu-GPL-2.0.txt"
    VERBATIM
)

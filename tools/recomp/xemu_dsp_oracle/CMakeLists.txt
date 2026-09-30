cmake_minimum_required(VERSION 3.20)
project(xemu_dsp_oracle C)

# Development-only differential diagnostic.  This target is deliberately
# separate from, and is never installed or linked into, Mercenaries Recompiled.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(XEMU_DSP "${REPO_ROOT}/references/xemu/hw/xbox/mcpx/apu/dsp")
set(DSP56300 "${REPO_ROOT}/third_party/dsp56300/msvc-x64-v0.1.3")

add_executable(dsp_diff
    dsp_diff.c
    "${XEMU_DSP}/interp/dsp_cpu.c"
)
set_property(TARGET dsp_diff PROPERTY C_STANDARD 11)
target_compile_definitions(dsp_diff PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(dsp_diff PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${XEMU_DSP}"
    "${XEMU_DSP}/interp"
    "${DSP56300}/include"
)
target_link_libraries(dsp_diff PRIVATE
    "${DSP56300}/lib/dsp56300_emu_ffi.lib"
    ws2_32 ntdll userenv bcrypt advapi32
)

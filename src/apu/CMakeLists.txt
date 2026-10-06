# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP) and Encode Processor (EP): DSP56300 JIT

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    apu_mmio_hook.c
    apu_xaudio2.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

# The bundled DSP56300 runtime is an MSVC-ABI Rust archive that requires the
# MSVC C++ runtime (__chkstk, _fltused, type_info RTTI), so it is only linked
# for the supported MSVC build. The MinGW cross build uses the portable
# fallback in apu_dsp.c, which still drives the VP audio path.
if(MSVC)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The bundled DSP56300 runtime currently requires an x64 build")
    endif()

    set(DSP56300_ROOT
        "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dsp56300/msvc-x64-v0.1.3")
    set(DSP56300_LIBRARY "${DSP56300_ROOT}/lib/dsp56300_emu_ffi.lib")
    if(NOT EXISTS "${DSP56300_ROOT}/include/dsp56300.h" OR
       NOT EXISTS "${DSP56300_LIBRARY}")
        message(FATAL_ERROR
            "Missing pinned DSP56300 v0.1.3 MSVC artifacts under ${DSP56300_ROOT}")
    endif()
    target_include_directories(xbox_apu PRIVATE "${DSP56300_ROOT}/include")
    target_compile_definitions(xbox_apu PRIVATE XBOX_APU_DSP56300=1)
endif()

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32)
    target_link_libraries(xbox_apu PRIVATE
        platform kernel32 winmm
        "${DSP56300_LIBRARY}"
        ws2_32 ntdll userenv bcrypt advapi32
        # XAudio2 runtime backend (apu_xaudio2.c). The import library is
        # xaudio2_9.lib on modern SDKs; the MSVC build pulls it in via
        # #pragma comment, which MinGW's lld honours only if libxaudio2.a
        # exists, so link the versioned library explicitly for MinGW.
        xaudio2_9
    )
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
endif()
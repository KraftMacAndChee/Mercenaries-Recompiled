# Toolchain file for cross-building the Windows x64 port from Linux with
# LLVM-MinGW. Pass with:
#   cmake -DCMAKE_TOOLCHAIN_FILE=<this file> \
#         -DLLVM_MINGW=/path/to/llvm-mingw ...
#
# Get LLVM-MinGW from https://github.com/mstorsjo/llvm-mingw/releases
# (llvm-mingw-*-ucrt-ubuntu-*-x86_64.tar.xz).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT LLVM_MINGW)
    set(LLVM_MINGW "$ENV{LLVM_MINGW}")
endif()
if(NOT LLVM_MINGW)
    message(FATAL_ERROR "Set -DLLVM_MINGW=/path/to/llvm-mingw or $LLVM_MINGW")
endif()

set(CMAKE_C_COMPILER   "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR           "${LLVM_MINGW}/bin/x86_64-w64-mingw32-ar")
set(CMAKE_RANLIB       "${LLVM_MINGW}/bin/x86_64-w64-mingw32-ranlib")
set(CMAKE_STRIP        "${LLVM_MINGW}/bin/x86_64-w64-mingw32-strip")

set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

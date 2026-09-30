"""Locate the retail HumanPlayer allocation in one selected diagnostic run."""

import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[2]
SIGNATURE = struct.pack("<II", 0x002E32B8, 0x660E4490)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--offset", type=lambda value: int(value, 0), required=True)
    args = parser.parse_args()

    executable = args.executable.resolve(strict=True)
    release = (ROOT / "build/mercenaries/bin/Release").resolve()
    if executable.parent != release or not executable.name.startswith("mercenaries_recomp_run"):
        raise ValueError("Only a named workspace diagnostic executable may be read")
    if not 0 <= args.offset <= 0x10000000:
        raise ValueError("Invalid guest-memory offset")

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.ReadProcessMemory.argtypes = [
        wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    kernel.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]

    process = kernel.OpenProcess(0x1000 | 0x10, False, args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        name = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(name))
        if (not kernel.QueryFullProcessImageNameW(process, 0, name, ctypes.byref(length))
                or Path(name.value).resolve() != executable):
            raise RuntimeError("Diagnostic process image mismatch")

        start, end, block_size = 0x00800000, 0x04000000, 0x00100000
        matches: list[int] = []
        overlap = len(SIGNATURE) - 1
        previous = b""
        for guest in range(start, end, block_size):
            size = min(block_size, end - guest)
            output = ctypes.create_string_buffer(size)
            copied = ctypes.c_size_t()
            if not kernel.ReadProcessMemory(
                    process, guest + args.offset, output, size, ctypes.byref(copied)):
                raise ctypes.WinError(ctypes.get_last_error())
            data = previous + output.raw[:copied.value]
            cursor = 0
            while True:
                found = data.find(SIGNATURE, cursor)
                if found < 0:
                    break
                address = guest - len(previous) + found
                if address % 16 == 0:
                    matches.append(address)
                cursor = found + 1
            previous = data[-overlap:]

        for address in matches:
            print(f"0x{address:08X}")
        if len(matches) != 1:
            raise RuntimeError(f"Expected one live HumanPlayer, found {len(matches)}")
    finally:
        kernel.CloseHandle(process)


if __name__ == "__main__":
    main()

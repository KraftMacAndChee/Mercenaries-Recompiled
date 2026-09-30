"""Hold one isolated test stick through a jump without a neutral frame.

Targets one explicitly named hidden diagnostic process. Each command expires
within 1.5 seconds if this helper stops; the helper always ends with neutral.
"""

import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import tempfile
import time

from walk_retail_diagnostic import ROOT, capture_acknowledged


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--command", type=Path, required=True)
    parser.add_argument("--ly", type=int, choices=(-32768, 32767), default=32767)
    parser.add_argument("--landing-ms", type=int, choices=range(1500, 5001), default=3000)
    args = parser.parse_args()

    executable = args.executable.resolve(strict=True)
    command = args.command.resolve(strict=True)
    if (executable.parent != (ROOT / "build/mercenaries/bin/Release").resolve()
            or not executable.name.startswith("mercenaries_recomp_run")
            or executable.suffix != ".exe"):
        raise ValueError("Explicit workspace diagnostic executable required")
    if (not command.is_relative_to((ROOT / "artifacts/test-runs").resolve())
            or command.name != "gamepad-command.txt" or command.stat().st_size > 1024):
        raise ValueError("Scoped diagnostic command file required")

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    process = kernel.OpenProcess(0x1000, False, args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())

    current = command.read_text(encoding="utf-8").strip()
    sequence = int(current.split()[0])

    def issue(hold_ms: int, analog: int = 0, ly: int = 0) -> int:
        nonlocal current, sequence
        if command.read_text(encoding="utf-8").strip() != current:
            raise RuntimeError("Another controller took ownership; yielding")
        sequence += 1
        updated = f"{sequence} 1 {hold_ms} 0 {analog:X} 0 {ly} 0 0 1"
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=command.name + ".", suffix=".tmp", dir=command.parent
        )
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as stream:
                stream.write(updated)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary_name, command)
        except BaseException:
            try:
                os.unlink(temporary_name)
            except FileNotFoundError:
                pass
            raise
        current = updated
        return sequence

    verified = False
    try:
        name = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(name))
        if (not kernel.QueryFullProcessImageNameW(
                process, 0, name, ctypes.byref(length))
                or Path(name.value).resolve() != executable):
            raise RuntimeError("Diagnostic process image mismatch")
        verified = True

        issue(1500, ly=args.ly)
        time.sleep(0.6)
        # Retail Xbox layout: B jumps; A reloads.
        issue(350, analog=0x02, ly=args.ly)
        time.sleep(0.25)
        deadline = time.monotonic() + args.landing_ms / 1000.0
        while time.monotonic() < deadline:
            issue(1500, ly=args.ly)
            time.sleep(0.5)
        final_sequence = issue(80)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if capture_acknowledged(command.parent, final_sequence):
                print(
                    f"Completed continuous run/jump/landing sequence; "
                    f"capture gamepad-{final_sequence:06d}.bmp"
                )
                return
            time.sleep(0.05)
        raise RuntimeError("Final neutral command was not acknowledged")
    finally:
        if verified:
            try:
                if command.read_text(encoding="utf-8").strip() == current:
                    issue(1)
            except BaseException:
                pass
        kernel.CloseHandle(process)


if __name__ == "__main__":
    main()
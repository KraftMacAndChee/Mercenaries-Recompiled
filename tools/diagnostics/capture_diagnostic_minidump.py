"""Capture thread stacks from an explicitly selected workspace test process.

No debugger injection, memory writes, termination, or global input. Full guest
RAM is deliberately separate (inspect_live_named_actors.py --dump-guest).
"""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--full-memory', action='store_true',
                        help='Include readable process pages for bounded native-state inspection')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    expected = args.executable.resolve(strict=True)
    destination = args.output.resolve()
    release = (root / 'build/mercenaries/bin/Release').resolve()
    public_preview = (root / 'artifacts/user-preview/mercenaries_recomp_preview.exe').resolve()
    private_test = (expected.is_relative_to((root / 'artifacts/test-runs').resolve())
                    and expected.parent.name == 'runtime'
                    and expected.name.startswith('mercenaries_recomp_run'))
    if (expected.parent != release and expected != public_preview and not private_test) or expected.suffix.lower() != '.exe':
        parser.error('Executable must be a workspace diagnostic or the exact public preview')
    if not destination.is_relative_to((root / 'artifacts').resolve()) or destination.suffix.lower() != '.dmp':
        parser.error('Output must be a new .dmp inside workspace artifacts')
    if destination.exists():
        parser.error('Refusing to overwrite an existing dump')
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    d = ctypes.WinDLL('dbghelp', use_last_error=True)
    k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k.OpenProcess.restype = wintypes.HANDLE
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    k.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k.CreateFileW.restype = wintypes.HANDLE
    d.MiniDumpWriteDump.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    d.MiniDumpWriteDump.restype = wintypes.BOOL
    process = k.OpenProcess(0x0400 | 0x0010, False, args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    output = None
    try:
        path = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(path))
        if not k.QueryFullProcessImageNameW(process, 0, path, ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        if Path(path.value).resolve() != expected:
            raise RuntimeError(f'Refusing unrelated process: {path.value}')
        output = k.CreateFileW(str(destination), 0x40000000, 0, None, 1, 0x80, None)
        if output == ctypes.c_void_p(-1).value:
            output = None
            raise ctypes.WinError(ctypes.get_last_error())
        dump_type = 0x1802 if args.full_memory else 0x1800
        if not d.MiniDumpWriteDump(process, args.pid, output, dump_type, None, None, None):
            raise ctypes.WinError(ctypes.get_last_error())
        print(f'Captured PID {args.pid}: {expected}\nThread dump: {destination}')
    finally:
        if output:
            k.CloseHandle(output)
        k.CloseHandle(process)


if __name__ == '__main__':
    main()

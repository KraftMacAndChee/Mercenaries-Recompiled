"""Exercise direct launch through the production entry point with synthetic files.

Run from an x64 MSVC developer shell; no retail input or generated code required.
"""
from pathlib import Path
import ctypes
import os
import shutil
import subprocess
import struct
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#define UNICODE
#define _UNICODE
#include "mod_loader.cpp"
namespace mercmods {
BOOL selector(HWND, const fs::path &installation, const fs::path &, Selection) {
    std::ofstream(installation / "selector.txt") << "selector";
    return TRUE;
}
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 4) return 3;
    if (!wcscmp(argv[3], L"select")) {
        mercmods::Selection s;
        s.mods = {{L"A", mercmods::fs::path(argv[1]) / L"mods/A", true},
                  {L"B", mercmods::fs::path(argv[1]) / L"mods/B", false}};
        mercmods::save_selection(argv[1], s);
        return 0;
    }
    return mercenaries_mod_launch(nullptr, argv[1], argv[2]) ? 0 : 2;
}
'''
CHILD = r'''
#include <windows.h>
#include <fstream>
int wmain() {
    std::wofstream out("child.tmp");
    for (const wchar_t *name : {L"MERCENARIES_GAME_DIR", L"MERCENARIES_SAVE_DIR",
         L"MERCENARIES_MOD_OVERLAY", L"MERCENARIES_MOD_CACHE", L"MERCENARIES_CONFIG_ROOT"}) {
        wchar_t value[32768] = {};
        GetEnvironmentVariableW(name, value, 32768);
        out << value << L'\n';
    }
    out.close();
    MoveFileExW(L"child.tmp", L"child.txt", MOVEFILE_REPLACE_EXISTING);
    for (int i = 0; i < 500; ++i) {
        if (GetFileAttributesW(L"release") != INVALID_FILE_ATTRIBUTES) return 0;
        Sleep(20);
    }
    return 4;
}
'''


def wait_for(predicate, label):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(label)


def main():
    compiler = shutil.which('cl')
    if not compiler:
        raise RuntimeError('Run from an x64 MSVC developer shell with cl on PATH')
    output = ROOT / 'artifacts/mod-autolaunch'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output, ignore_cleanup_errors=True) as tmp:
        case = Path(tmp)
        harness_cpp = case / 'launcher.cpp'
        child_cpp = case / 'child.cpp'
        harness_cpp.write_text(HARNESS, encoding='utf-8')
        child_cpp.write_text(CHILD, encoding='utf-8')
        harness = case / 'launcher.exe'
        child = case / 'child.exe'
        for source, exe in [(harness_cpp, harness), (child_cpp, child)]:
            subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/O2',
                            '/I' + str(ROOT / 'ports/mercenaries/src'), str(source),
                            '/Fo' + str(source.with_suffix('.obj')), '/Fe' + str(exe),
                            '/link', 'bcrypt.lib', 'user32.lib', '/STACK:8388608'],
                           check=True)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_ulong, ctypes.c_ulong,
                                     ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong,
                                     ctypes.c_void_p]
        kernel.CreateFileW.restype = ctypes.c_void_p
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        invalid = ctypes.c_void_p(-1).value
        modes = [None, '', 'unknown', 'x' * 16, 'x' * 200, 'modded', 'MoDdEd',
                 'vanilla', 'VANILLA']
        for number, mode in enumerate(modes):
            root = case / str(number)
            game = root / 'game_files'
            game.mkdir(parents=True)
            shutil.copy2(child, root / 'mercenaries_recomp.exe')
            (game / 'read-only.bin').write_bytes(b'original')
            for name in ['A', 'B']:
                asset = root / 'mods' / name / 'game_files/read-only.bin'
                asset.parent.mkdir(parents=True)
                asset.write_bytes(name.encode())
            subprocess.run([str(harness), str(root), str(game), 'select'], check=True)
            selection = (root / 'mod-loader.ini').read_bytes()
            env = dict(os.environ)
            env.pop('MERCENARIES_AUTOLAUNCH_MODE', None)
            # Stale overlay variables must not survive vanilla preparation.
            env.update(MERCENARIES_MOD_OVERLAY='stale', MERCENARIES_MOD_CACHE='stale')
            if mode is not None:
                env['MERCENARIES_AUTOLAUNCH_MODE'] = mode
            subprocess.run([str(harness), str(root), str(game), 'launch'], env=env,
                           check=True, timeout=10)
            direct = mode is not None and mode.lower() in ('modded', 'vanilla')
            if not direct:
                assert (root / 'selector.txt').is_file(), mode
                assert not list(root.rglob('child.txt')), mode
            else:
                wait_for(lambda: bool(list(root.rglob('child.txt'))), 'child did not start')
                capture = next(root.rglob('child.txt'))
                values = capture.read_text().splitlines()
                try:
                    assert Path(values[0]) == game, values
                    if mode.lower() == 'modded':
                        assert Path(values[1]) == root / 'mod-saves/mercenaries', values
                        assert Path(values[2]).is_file(), values
                        assert Path(values[3]) == Path(values[2]).parent / 'storage', values
                        # Decode the real overlay to ensure disabled B did not
                        # replace the saved enabled A selection.
                        data = Path(values[2]).read_bytes()
                        magic, count = struct.unpack_from('<II', data)
                        assert magic == 0x31444f4d and count == 1
                        a, b = struct.unpack_from('<II', data, 8)
                        name = data[16:16 + a * 2].decode('utf-16le')
                        target = Path(data[16 + a * 2:16 + (a + b) * 2].decode('utf-16le'))
                        assert name == 'read-only.bin' and target.read_bytes() == b'A'
                        assert capture.parent != root
                        assert (capture.parent / 'read-only.bin').exists() is False
                    else:
                        assert capture.parent == root
                        assert Path(values[1]) == root / 'saves/mercenaries', values
                        assert values[2:4] == ['', ''], values
                    # Child inherits the lease after the launcher has exited.
                    lease = root / 'mod-loader.lock'
                    handle = kernel.CreateFileW(str(lease), 0x80000000, 0, None, 3, 0, None)
                    if handle != invalid:
                        kernel.CloseHandle(handle)
                        raise AssertionError('child did not retain the cache lease')
                finally:
                    (capture.parent / 'release').write_text('1')
                    def released():
                        handle = kernel.CreateFileW(str(root / 'mod-loader.lock'),
                                                   0x80000000, 0, None, 3, 0, None)
                        if handle == invalid:
                            return False
                        kernel.CloseHandle(handle)
                        return True

                    wait_for(released, 'child did not release lease')
            assert (root / 'mod-loader.ini').read_bytes() == selection
            assert (game / 'read-only.bin').read_bytes() == b'original'
    print('PASS: selector fallback, case-insensitive modded/vanilla direct launch, '
          'saved selection, save isolation, stale overlay clearing, inherited lease, '
          'unchanged retail input')


if __name__ == '__main__':
    main()

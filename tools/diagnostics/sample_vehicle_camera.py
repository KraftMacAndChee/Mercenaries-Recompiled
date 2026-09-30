"""Read-only, bounded sampling of the retail Mario64 vehicle camera."""
import argparse
import ctypes
import json
import struct
import time
from ctypes import wintypes
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('pid', type=int)
p.add_argument('expected_exe', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--seconds', type=float, default=30)
a = p.parse_args()
if not 0 < a.seconds <= 60:
    p.error('--seconds must be in (0, 60]')
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k.OpenProcess.restype = wintypes.HANDLE
k.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                              ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                       wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
k.CloseHandle.argtypes = [wintypes.HANDLE]
h = k.OpenProcess(0x1010, False, a.pid)
if not h:
    raise ctypes.WinError(ctypes.get_last_error())
try:
    name = ctypes.create_unicode_buffer(32768)
    size = wintypes.DWORD(len(name))
    if not k.QueryFullProcessImageNameW(h, 0, name, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    if Path(name.value).resolve() != a.expected_exe.resolve():
        raise RuntimeError('Process identity mismatch')

    def read(address, count):
        if not 0x10000 <= address <= 0x4000000 - count:
            raise ValueError('Address outside retail guest memory')
        data = ctypes.create_string_buffer(count)
        got = ctypes.c_size_t()
        if not k.ReadProcessMemory(h, address + 0x10000, data, count, ctypes.byref(got)) or got.value != count:
            raise ctypes.WinError(ctypes.get_last_error())
        return data.raw

    samples = []
    start = time.perf_counter()
    while time.perf_counter() - start < a.seconds:
        camera = read(0x4140e8, 0x3c)
        state = struct.unpack_from('<I', camera, 0x38)[0]
        active = struct.unpack_from('<I', camera, 0x24)[0]
        if state and state == active:
            s = read(state, 0x1c0)
            if struct.unpack_from('<I', s)[0] == 0x2e6fd4:
                samples.append({
                    'seconds': round(time.perf_counter() - start, 4),
                    'state': hex(state),
                    'mode': hex(struct.unpack_from('<I', s, 0x158)[0]),
                    'chase_guid': hex(struct.unpack_from('<I', s, 0x60)[0]),
                    'winch_t': struct.unpack_from('<f', s, 0x180)[0],
                    'stick_length': struct.unpack_from('<f', s, 0xf8)[0],
                    'stick_min': struct.unpack_from('<f', s, 0xfc)[0],
                    'tilt': struct.unpack_from('<f', s, 0x100)[0],
                    'yaw': struct.unpack_from('<f', s, 0x10c)[0],
                    'position': struct.unpack_from('<3f', s, 0x40),
                    'old_position': struct.unpack_from('<3f', s, 0x11c),
                })
        time.sleep(.05)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps({'pid': a.pid, 'exe': name.value,
                                   'sampling_ms': 50, 'samples': samples}, indent=2))
    print(json.dumps({'samples': len(samples), 'winch_t_range':
        [min(s['winch_t'] for s in samples), max(s['winch_t'] for s in samples)] if samples else [],
        'output': str(a.output)}))
finally:
    k.CloseHandle(h)

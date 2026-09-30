"""Read-only, bounded sampling of the active retail camera shake state."""
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
p.add_argument('--vehicle', type=lambda value: int(value, 0), help='Optional verified vehicle actor guest address; read-only hull correlation')
a = p.parse_args()
if a.vehicle is not None and not 0x10000 <= a.vehicle <= 0x4000000-0xF0:
    p.error('--vehicle is outside guest memory')
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

    if not a.expected_exe.resolve().is_relative_to(Path('artifacts/test-runs').resolve()):
        raise RuntimeError('Only an isolated test-runs executable may be sampled')
    samples = []
    start = time.perf_counter()
    while time.perf_counter() - start < a.seconds:
        game_dt = struct.unpack('<f', read(0x413FA0, 4))[0]
        game_ticks = struct.unpack('<I', read(0x4140CC, 4))[0]
        camera = read(0x4140e8, 0x28)
        active = struct.unpack_from('<I', camera, 0x24)[0]
        if 0x10000 <= active <= 0x4000000-0x60:
            state = read(active, 0x60)
            samples.append({'seconds': time.perf_counter()-start,
                'game_dt': game_dt, 'game_ticks': game_ticks,
                'active': hex(active), 'vtable': hex(struct.unpack_from('<I',state)[0]),
                'delay': struct.unpack_from('<f',state,0x54)[0],
                'duration': struct.unpack_from('<f',state,0x58)[0],
                'amplitude': struct.unpack_from('<f',state,0x5c)[0],
                'matrix': struct.unpack_from('<16f',state,0x10)})
            if struct.unpack_from('<I', state)[0] == 0x2E70E4 and active <= 0x4000000-0x100:
                # Original StateTurret fields; read only, no gameplay mutation.
                turret = read(active + 0x60, 0xA0)
                samples[-1].update(turret_inertia=struct.unpack_from('<f', turret)[0],
                    turret_old_barrel_matrix=struct.unpack_from('<16f', turret, 0x50),
                    turret_stick_length=struct.unpack_from('<f', turret, 0x90)[0],
                    turret_noise_phase=struct.unpack_from('<f', turret, 0x94)[0],
                    turret_owner_guid=hex(struct.unpack_from('<I', turret, 0x98)[0]),
                    turret_index=struct.unpack_from('<i', turret, 0x9C)[0],
                    camera_stable_across_turret_read=read(active+0x10,64)==state[0x10:0x50])
            if a.vehicle is not None:
                hull = read(a.vehicle + 0xB0, 64)
                stable = read(active + 0x10, 64) == state[0x10:0x50]
                samples[-1].update(vehicle=hex(a.vehicle),
                    vehicle_matrix=struct.unpack('<16f', hull), camera_stable_across_hull_read=stable)
        time.sleep(.002)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(json.dumps({'pid': a.pid, 'exe': name.value,
                                   'requested_sampling_ms': 2, 'samples': samples}, indent=2))
    changes=[]
    for previous,current in zip(samples,samples[1:]):
        if previous['active']==current['active'] and previous['duration']>current['duration']>0:
            changes.append((current['seconds']-previous['seconds'],previous['duration']-current['duration'],previous['amplitude']*current['amplitude']<0))
    print(json.dumps({'samples':len(samples),'shake_samples':sum(s['duration']>0 for s in samples),
        'decrement_histogram':{str(d):sum(round(c[1],6)==d for c in changes) for d in sorted(set(round(c[1],6) for c in changes))},
        'observed_sign_flips':sum(c[2] for c in changes),'output':str(a.output)}))
finally:
    k.CloseHandle(h)

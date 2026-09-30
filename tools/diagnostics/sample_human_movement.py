"""Bounded read-only movement sampling in an explicitly selected retail test.

Samples are NOT atomic simulation snapshots: input may be cleared/repopulated
between game updates. Correlate sustained values and local command lifetimes;
one zero input or speed is not proof of the reported forced-walk bug.
No input delivery, process suspension, memory writes or preview access.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import struct
import time

ROOT = Path(__file__).resolve().parents[2]


def decode_player(data):
    if len(data) != 0x400 or struct.unpack_from('<II', data) != (0x2E32B8, 0x660E4490):
        raise ValueError('Selected object is no longer the retail player')
    # Retail vtable slot 238: LEA EAX,[ECX+280]; RET. Do not use the
    # vehicle base at +1D8 as the human physics object.
    p = 0x280
    return dict(position=struct.unpack_from('<3f', data, 0xE0),
                health=struct.unpack_from('<f', data, 0x98)[0],
                move_mode=struct.unpack_from('<I', data, p+8)[0],
                move_speed=struct.unpack_from('<I', data, p+12)[0],
                run_stick=struct.unpack_from('<2f', data, p+0x30),
                authored_speeds=struct.unpack_from('<2f', data, p+0x44),
                run_enabled=data[p+0x65], human_controlled=data[p+0xA4],
                # Retail Update 13F833..13F88E and 13F7A0 pin these fields.
                physics_accumulated_dt=struct.unpack_from('<f',data,p+0xB4)[0],
                physics_min_update_dt=struct.unpack_from('<f',data,p+0xB8)[0],
                physics_last_accumulated_dt=struct.unpack_from('<f',data,p+0xBC)[0],
                physics_velocity=struct.unpack_from('<3f',data,p+0xE4))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--player', type=lambda v:int(v,0), required=True)
    parser.add_argument('--offset', type=lambda v:int(v,0), required=True)
    parser.add_argument('--seconds', type=int, choices=range(1,61), default=15)
    args = parser.parse_args()
    exe = args.executable.resolve(strict=True)
    if (exe.parent != (ROOT/'build/mercenaries/bin/Release').resolve()
            or not exe.name.startswith('mercenaries_recomp_run') or exe.suffix != '.exe'):
        raise ValueError('Only workspace diagnostic executables may be sampled')
    if not 0x10000 <= args.player <= 0x4000000-0x400 or not 0 <= args.offset <= 0x10000000000:
        raise ValueError('Invalid bounded guest address')
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    k.OpenProcess.restype = wintypes.HANDLE
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    k.ReadProcessMemory.argtypes = [wintypes.HANDLE,ctypes.c_void_p,ctypes.c_void_p,
                                   ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
    k.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE,wintypes.DWORD,
                                           wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
    process = k.OpenProcess(0x1010, False, args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    def read(guest, size):
        if not 0 <= guest <= 0x4000000-size:
            raise ValueError('Read outside guest RAM')
        b = ctypes.create_string_buffer(size); n = ctypes.c_size_t()
        if not k.ReadProcessMemory(process,args.offset+guest,b,size,ctypes.byref(n)) or n.value != size:
            raise ctypes.WinError(ctypes.get_last_error())
        return b.raw
    try:
        name = ctypes.create_unicode_buffer(32768); n = wintypes.DWORD(len(name))
        if not k.QueryFullProcessImageNameW(process,0,name,ctypes.byref(n)) or Path(name.value).resolve() != exe:
            raise RuntimeError('Diagnostic process image mismatch')
        start = time.monotonic()
        while time.monotonic()-start < args.seconds:
            sample = decode_player(read(args.player,0x400))
            sample['wall_seconds'] = round(time.monotonic()-start,4)
            sample['game_state'] = f'{struct.unpack("<I",read(0x413F68,4))[0]:08X}'
            sample['simulation_dt'] = struct.unpack('<f',read(0x413FA0,4))[0]
            # Retail PblTimeBase::SetRate (1FFBB0) and MissionWon enter
            # (182D2F) identify game/no-pause bases. These are independent
            # read-only observations, not one atomic frame snapshot.
            sample['game_time_ticks'] = struct.unpack('<I',read(0x4140CC,4))[0]
            sample['unpaused_time_ticks'] = struct.unpack('<I',read(0x4140D8,4))[0]
            sample['game_time_rate'] = struct.unpack('<f',read(0x4140D4,4))[0]
            sample['seconds_per_tick'] = struct.unpack('<f',read(0x6921A0,4))[0]
            print(json.dumps(sample),flush=True)
            time.sleep(.1)
    finally:
        k.CloseHandle(process)


if __name__ == '__main__':
    main()

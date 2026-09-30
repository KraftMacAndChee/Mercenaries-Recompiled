"""Bounded read-only airstrike observations in one explicitly selected test PID.

No input, suspension or guest writes. One actor census per second can add
diagnostic overhead; this is not a frame-rate benchmark or an atomic snapshot.
Actor absence does not prove destruction or explain why an actor disappeared.
"""
import argparse
import ctypes
from ctypes import wintypes
import json
import math
from pathlib import Path
import struct
import time

ROOT = Path(__file__).resolve().parents[2]
# Retail 00043923 installs the derived airplane vtable after calling the
# base airplane constructor. Keep the vtable in each result: a census entry
# is not proof that this particular aircraft belongs to the requested strike.
VTABLES = {0x2DD068: 'beacon', 0x2E1050: 'airplane',
           0x2E12E0: 'airplane', 0x2E1C20: 'emplaced',
           0x2E21A8: 'helicopter'}
ARTILLERY_HASHES = {0x86294500: 'artillery1', 0x892949B9: 'artillery2'}


def decode_actors(memory, base):
    """Census aligned actor records with matching self-pointers, not raw hits."""
    rows = []
    for vtable, kind in VTABLES.items():
        needle = struct.pack('<I', vtable)
        index = 0
        while True:
            index = memory.find(needle, index)
            if index < 0:
                break
            address = base + index
            if address % 16 == 0 and index + 0x250 <= len(memory):
                self_pointer = struct.unpack_from('<I', memory, index+0x34)[0]
                name = struct.unpack_from('<I', memory, index+4)[0]
                pos = struct.unpack_from('<3f', memory, index+0xE0)
                if (self_pointer == address and all(math.isfinite(v) and abs(v)<100000 for v in pos)
                        and (kind != 'emplaced' or name in ARTILLERY_HASHES)):
                    row = dict(kind=ARTILLERY_HASHES.get(name,kind), address=f'{address:08X}',
                               vtable=f'{vtable:08X}', name_hash=f'{name:08X}', position=pos)
                    if kind == 'beacon':
                        row.update(aircraft_guid=f'{struct.unpack_from("<I",memory,index+0x204)[0]:08X}',
                                   lifestyle=struct.unpack_from('<I',memory,index+0x208)[0],
                                   death_age=struct.unpack_from('<f',memory,index+0x20C)[0],
                                   fire_permission=memory[index+0x210],
                                   tracking_laser=memory[index+0x212],
                                   drift=struct.unpack_from('<f',memory,index+0x214)[0],
                                   laser_position=struct.unpack_from('<3f',memory,index+0x234))
                    else:
                        row['health'] = struct.unpack_from('<f',memory,index+0x98)[0]
                    rows.append(row)
            index += 4
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--executable',type=Path,required=True)
    p.add_argument('--offset',type=lambda v:int(v,0),required=True)
    p.add_argument('--seconds',type=int,choices=range(1,46),default=30)
    args=p.parse_args()
    exe=args.executable.resolve(strict=True)
    if (exe.parent != (ROOT/'build/mercenaries/bin/Release').resolve()
            or not exe.name.startswith('mercenaries_recomp_run') or exe.suffix != '.exe'):
        raise ValueError('Only an explicit workspace diagnostic may be sampled')
    if not 0 <= args.offset <= 0x10000000000:
        raise ValueError('Invalid guest mapping offset')
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    k.OpenProcess.restype=wintypes.HANDLE
    k.CloseHandle.argtypes=[wintypes.HANDLE]
    k.QueryFullProcessImageNameW.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
    k.ReadProcessMemory.argtypes=[wintypes.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
    h=k.OpenProcess(0x1010,False,args.pid)
    if not h:raise ctypes.WinError(ctypes.get_last_error())
    def read(address,count):
        if not 0<=address<=0x4000000-count:raise ValueError('Read outside guest RAM')
        b=ctypes.create_string_buffer(count);n=ctypes.c_size_t()
        if not k.ReadProcessMemory(h,args.offset+address,b,count,ctypes.byref(n)) or n.value!=count:
            raise ctypes.WinError(ctypes.get_last_error())
        return b.raw
    try:
        name=ctypes.create_unicode_buffer(32768);n=wintypes.DWORD(len(name))
        if not k.QueryFullProcessImageNameW(h,0,name,ctypes.byref(n)) or Path(name.value).resolve()!=exe:
            raise RuntimeError('Diagnostic process image mismatch')
        start=time.monotonic()
        while time.monotonic()-start<args.seconds:
            lap=time.monotonic()
            laser=read(0x3BE240,16)
            row=dict(wall_seconds=round(lap-start,3),
                     state=f'{struct.unpack("<I",read(0x413F68,4))[0]:08X}',
                     laser_active=laser[0],laser_hit=struct.unpack_from('<3f',laser,4),
                     actors=decode_actors(read(0x800000,0x3800000),0x800000))
            row['read_seconds']=round(time.monotonic()-lap,4)
            print(json.dumps(row),flush=True)
            time.sleep(max(0,1-(time.monotonic()-lap)))
    finally:
        k.CloseHandle(h)


if __name__=='__main__':main()

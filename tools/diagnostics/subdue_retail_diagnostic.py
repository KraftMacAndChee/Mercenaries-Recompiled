"""Bounded normal-stick/X/Y capture attempt in one private retail test process.

Reads only player/target/camera state; writes only the existing local gamepad
file. No health, collision, mission, guest memory, or global input writes.
"""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import math
import struct
import subprocess
import time
from walk_retail_diagnostic import ROOT, PATCH_EXE, stick_toward


def capture_buttons(distance, elapsed):
    """Source-aligned X stun then Y use; never swing outside use range."""
    if not math.isfinite(distance) or distance<0 or not math.isfinite(elapsed) or elapsed<0:
        raise ValueError('Invalid capture interval')
    if distance>=2:return 0
    cycle=elapsed%3
    return 4 if cycle<.3 else 8 if .65<=cycle<1.15 else 0


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--executable',type=Path,required=True)
    p.add_argument('--command',type=Path,required=True)
    p.add_argument('--player',type=lambda v:int(v,0),required=True)
    p.add_argument('--target',type=lambda v:int(v,0),required=True)
    p.add_argument('--offset',type=lambda v:int(v,0),required=True)
    p.add_argument('--seconds',type=int,choices=range(1,31),default=20)
    args=p.parse_args();exe=args.executable.resolve(strict=True);command=args.command.resolve(strict=True)
    if exe.parent!=(ROOT/'build/mercenaries/bin/Release').resolve() or not exe.name.startswith('mercenaries_recomp_run') or exe.suffix!='.exe':
        raise ValueError('Explicit diagnostic executable required')
    if command.parent.parent!=(ROOT/'artifacts/test-runs').resolve() or command.name!='gamepad-command.txt' or int((command.parent/'pid.txt').read_text())!=args.pid:
        raise ValueError('Explicit matching private diagnostic command/PID required')
    for address in (args.player,args.target):
        if not 0x10000<=address<=0x4000000-0x6BC:raise ValueError('Invalid actor address')
    if not 0<=args.offset<=0x10000000000:raise ValueError('Invalid guest offset')
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];k.OpenProcess.restype=wintypes.HANDLE
    k.CloseHandle.argtypes=[wintypes.HANDLE]
    k.QueryFullProcessImageNameW.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
    k.ReadProcessMemory.argtypes=[wintypes.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
    h=k.OpenProcess(0x1010,False,args.pid)
    if not h:raise ctypes.WinError(ctypes.get_last_error())
    def read(address,count):
        if not 0<=address<=0x4000000-count:raise ValueError('Read out of guest bounds')
        b=ctypes.create_string_buffer(count);n=ctypes.c_size_t()
        if not k.ReadProcessMemory(h,address+args.offset,b,count,ctypes.byref(n)) or n.value!=count:raise ctypes.WinError(ctypes.get_last_error())
        return b.raw
    def u32(address):return struct.unpack('<I',read(address,4))[0]
    def xz(address):
        x,_,z=struct.unpack('<3f',read(address,12))
        if not all(math.isfinite(v) and abs(v)<10000 for v in (x,z)):raise ValueError('Invalid position')
        return x,z
    old=command.read_text().strip();seq=int(old.split()[0]);verified=False
    def issue(x=0,y=0,analog=0,hold=300,capture=0):
        nonlocal old,seq
        if command.read_text().strip()!=old:raise RuntimeError('Other controller took ownership')
        seq+=1;new=f'{seq} 1 {hold} 0 {analog:X} {x} {y} 0 0 {capture}'
        patch=f'*** Begin Patch\n*** Update File: {command.as_posix()}\n@@\n-{old}\n+{new}\n*** End Patch'
        subprocess.run([str(PATCH_EXE),'--codex-run-as-apply-patch',patch],check=True,capture_output=True,text=True);old=new
    try:
        name=ctypes.create_unicode_buffer(32768);n=wintypes.DWORD(len(name))
        if not k.QueryFullProcessImageNameW(h,0,name,ctypes.byref(n)) or Path(name.value).resolve()!=exe:raise RuntimeError('Process image mismatch')
        verified=True;start=time.monotonic();bash_start=None;last_report=0
        while time.monotonic()-start<args.seconds:
            state=u32(0x413F68)
            if state!=0xC2CBD863:print(f'Stopped at game substate {state:08X}',flush=True);break
            if u32(args.player)!=0x2E32B8 or u32(args.player+4)!=0x660E4490:raise RuntimeError('Player changed')
            if u32(args.target)!=0x2E2CE0 or u32(args.target+4)!=0x3A69C438 or u32(args.target+0x34)!=args.target:raise RuntimeError('Target changed')
            if read(args.target+0x6B9,1)[0]:print('Observed retail subdued flag; no verification inferred',flush=True);break
            camera=u32(0x4140E8+0x1C)
            x,y,d=stick_toward(xz(args.player+0xE0),xz(camera+0x40),xz(args.target+0xE0))
            now=time.monotonic();analog=0
            if d<=1.5:x=y=0
            if d<2:
                if bash_start is None:bash_start=now
                analog=capture_buttons(d,now-bash_start)
            else:bash_start=None
            issue(x,y,analog)
            if now-last_report>=1:print(f'target_distance={d:.3f} input={analog:02X}',flush=True);last_report=now
            time.sleep(.10)
        else:print('Bounded capture attempt timed out',flush=True)
    finally:
        try:
            if verified and command.read_text().strip()==old:issue(hold=1,capture=1)
        finally:k.CloseHandle(h)


if __name__=='__main__':main()

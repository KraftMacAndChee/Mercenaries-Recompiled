"""One bounded, acknowledged local controller pulse in an explicit test PID.

No global keyboard/mouse input, focus changes, guest writes or preview access.
Uses the existing file protocol and yields if another controller takes over.
"""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import os
import tempfile
import time
from walk_retail_diagnostic import ROOT, capture_acknowledged


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--executable',type=Path,required=True)
    p.add_argument('--command',type=Path,required=True)
    p.add_argument('--analog',type=lambda s:int(s,16),default=0)
    p.add_argument('--digital',type=lambda s:int(s,16),default=0)
    p.add_argument('--lx',type=int,default=0)
    p.add_argument('--ly',type=int,default=0)
    p.add_argument('--rx',type=int,default=0)
    p.add_argument('--ry',type=int,default=0)
    p.add_argument('--hold-ms',type=int,default=180)
    p.add_argument('--resume-route',action='store_true',
                   help='After capture acknowledgment, return ownership to the existing automatic route')
    args=p.parse_args()
    exe=args.executable.resolve(strict=True)
    command=args.command.resolve(strict=True)
    private_runtime = (command.parent/'runtime').resolve()
    if (exe.parent not in ((ROOT/'build/mercenaries/bin/Release').resolve(),private_runtime)
            or not exe.name.startswith('mercenaries_recomp_run') or exe.suffix!='.exe'):
        raise ValueError('Explicit workspace diagnostic executable required')
    if (not command.is_relative_to((ROOT/'artifacts/test-runs').resolve())
            or command.name!='gamepad-command.txt' or command.stat().st_size>1024):
        raise ValueError('Only the scoped diagnostic command file is allowed')
    if (not 1<=args.hold_ms<=1000 or not 0<=args.analog<=255 or
            args.digital&~0x3F or not -32768<=args.lx<=32767 or
            not -32768<=args.ly<=32767 or not -32768<=args.rx<=32767 or
            not -32768<=args.ry<=32767):
        raise ValueError('Invalid bounded controller pulse')
    k=ctypes.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD]
    k.OpenProcess.restype=wintypes.HANDLE
    k.QueryFullProcessImageNameW.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
    k.CloseHandle.argtypes=[wintypes.HANDLE]
    h=k.OpenProcess(0x1000,False,args.pid)
    if not h: raise ctypes.WinError(ctypes.get_last_error())
    try:
        name=ctypes.create_unicode_buffer(32768); n=wintypes.DWORD(len(name))
        if not k.QueryFullProcessImageNameW(h,0,name,ctypes.byref(n)) or Path(name.value).resolve()!=exe:
            raise RuntimeError('Diagnostic process image mismatch')
        old=command.read_text(encoding='utf-8').strip()
        sequence=int(old.split()[0])+1
        new=(f'{sequence} 1 {args.hold_ms} {args.digital:X} '
             f'{args.analog:X} {args.lx} {args.ly} {args.rx} {args.ry} 1')
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=command.name + '.', suffix='.tmp', dir=command.parent
        )
        try:
            with os.fdopen(descriptor, 'w', encoding='utf-8', newline='') as temporary:
                temporary.write(new)
                temporary.flush()
                os.fsync(temporary.fileno())
            os.replace(temporary_name, command)
        except BaseException:
            try: os.unlink(temporary_name)
            except FileNotFoundError: pass
            raise
        deadline=time.monotonic()+5
        while time.monotonic()<deadline:
            if command.read_text(encoding='utf-8').strip()!=new:
                raise RuntimeError('Another controller took ownership; yielding')
            if capture_acknowledged(command.parent,sequence):
                print(f'Acknowledged local pulse {sequence}; capture gamepad-{sequence:06d}.bmp')
                if args.resume_route:
                    resume=f'{sequence+1} 0 1 0 0 0 0 0 0 0'
                    descriptor, temporary_name = tempfile.mkstemp(
                        prefix=command.name + '.', suffix='.tmp', dir=command.parent
                    )
                    try:
                        with os.fdopen(descriptor, 'w', encoding='utf-8', newline='') as temporary:
                            temporary.write(resume)
                            temporary.flush()
                            os.fsync(temporary.fileno())
                        os.replace(temporary_name, command)
                    except BaseException:
                        try: os.unlink(temporary_name)
                        except FileNotFoundError: pass
                        raise
                    print('Returned controller ownership to the existing diagnostic route')
                return
            time.sleep(.05)
        raise RuntimeError('No capture acknowledgment; pulse expires locally, do not infer game response')
    finally:
        k.CloseHandle(h)


if __name__=='__main__':main()

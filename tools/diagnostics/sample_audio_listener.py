"""Sample original listener position/velocity from one named private diagnostic.
ReadProcessMemory only; no writes, input, suspension, or audio changes.
"""
import argparse, ctypes, json, math, struct, time
from ctypes import wintypes as w
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('pid',type=int);p.add_argument('executable',type=Path);p.add_argument('output',type=Path)
p.add_argument('--seconds',type=int,default=45,choices=range(1,61))
a=p.parse_args();root=Path(__file__).resolve().parents[2]
exe=a.executable.resolve();out=a.output.resolve()
if not exe.name.startswith('mercenaries_recomp_run') or not exe.is_relative_to(root):p.error('Named workspace diagnostic required')
if not out.is_relative_to(root/'artifacts') or out.exists():p.error('Output must be a new artifact')
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.CloseHandle.argtypes=[w.HANDLE]
k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,ctypes.POINTER(w.DWORD)]
k.ReadProcessMemory.argtypes=[w.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
h=k.OpenProcess(0x1010,False,a.pid)
if not h:raise ctypes.WinError(ctypes.get_last_error())
try:
 name=ctypes.create_unicode_buffer(32768);n=w.DWORD(len(name))
 if not k.QueryFullProcessImageNameW(h,0,name,ctypes.byref(n)) or Path(name.value).resolve()!=exe:raise ValueError('Process identity mismatch')
 rows=[];start=time.monotonic();ticks=k.GetTickCount64;k.GetTickCount64.restype=ctypes.c_uint64
 while time.monotonic()-start<a.seconds:
  b=ctypes.create_string_buffer(24);got=ctypes.c_size_t()
  if not k.ReadProcessMemory(h,0x8423e0+0x10000,b,24,ctypes.byref(got)) or got.value!=24:raise ctypes.WinError(ctypes.get_last_error())
  vals=struct.unpack('<6f',b.raw)
  if not all(math.isfinite(v) for v in vals):raise ValueError('Nonfinite listener state')
  rows.append({'seconds':time.monotonic()-start,'tick_ms':ticks(),'position':vals[:3],'velocity':vals[3:]})
  time.sleep(.05)
 result={'pid':a.pid,'executable':str(exe),'guest_offset':'0x10000','listener_position':'0x8423e0','listener_velocity':'0x8423ec','samples':rows}
 out.parent.mkdir(parents=True,exist_ok=True)
 with out.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
 print(json.dumps({'samples':len(rows),'max_speed':max(math.dist(r['velocity'],(0,0,0)) for r in rows),'output':str(out)}))
finally:k.CloseHandle(h)

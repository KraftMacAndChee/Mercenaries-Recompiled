"""Bounded on-foot navigation using private test input; process memory is read-only. Obstacles stop the route; no teleporting."""
import argparse, ctypes as c, json, math, time
from ctypes import wintypes as w
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('run',type=Path);p.add_argument('x',type=float);p.add_argument('z',type=float);p.add_argument('--seconds',type=float,default=20);a=p.parse_args()
r=a.run.resolve();assert r.parent==Path('artifacts/test-runs').resolve();assert 0<a.seconds<=30
pid=int((r/'pid.txt').read_text());exe=next((r/'runtime').glob('mercenaries_recomp_run*.exe'))
k=c.WinDLL('kernel32',use_last_error=True);k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE;k.CloseHandle.argtypes=[w.HANDLE]
k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)];k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
h=k.OpenProcess(0x1010,False,pid);assert h
command=r/'gamepad-command.txt';seq=int(command.read_text().split()[0]);history=[]
def send(lx,ly):
 global seq
 seq+=1;command.write_text(f'{seq} 1 300 00 00 {lx} {ly} 0 0 0\n')
 history.append({'sequence':seq,'hold_ms':300,'sticks':[lx,ly,0,0],'route_target':[a.x,a.z]})
try:
 name=c.create_unicode_buffer(32768);n=w.DWORD(len(name));assert k.QueryFullProcessImageNameW(h,0,name,c.byref(n));assert Path(name.value).resolve()==exe
 import struct
 def read(addr,size):
  assert 0x10000<=addr<=0x4000000-size
  b=c.create_string_buffer(size);got=c.c_size_t();assert k.ReadProcessMemory(h,addr+0x10000,b,size,c.byref(got)) and got.value==size;return b.raw
 def u(addr):return struct.unpack('<I',read(addr,4))[0]
 actor=0x105e300;assert u(actor)==0x2e32b8
 started=time.monotonic();anchor=None;anchor_time=started;reason='duration'
 while time.monotonic()-started<a.seconds:
  assert u(actor)==0x2e32b8
  assert u(actor+0x768)==0, 'On-foot navigation only'
  x,y,z=struct.unpack('<3f',read(actor+0xe0,12));rc=u(0x414104);cx,cy,cz=struct.unpack('<3f',read(rc+0x40,12))
  assert all(math.isfinite(v) for v in [x,y,z,cx,cy,cz])
  dx,dz=a.x-x,a.z-z;distance=math.hypot(dx,dz);assert distance<2000
  if y < -1:reason='water';break
  if distance<1.1:reason='arrived';break
  now=time.monotonic()
  if anchor is None or math.hypot(x-anchor[0],z-anchor[1])>1:
   anchor=(x,z);anchor_time=now
  if now-anchor_time>3:reason='obstacle';break
  fx,fz=x-cx,z-cz;length=math.hypot(fx,fz);assert length>.01;fx/=length;fz/=length
  scale=30000*min(1,distance/3)
  send(int(scale*(-fz*dx+fx*dz)/distance),int(scale*(fx*dx+fz*dz)/distance));time.sleep(.15)
 print(json.dumps({'reason':reason,'position':[x,y,z],'distance':distance,'commands':len(history)}))
finally:
 send(0,0)
 with (r/'automation-history.jsonl').open('a') as out:
  for item in history:out.write(json.dumps(item)+'\n')
 k.CloseHandle(h)

"""Read-only comparison of verified retail time bases with host elapsed time."""
import argparse, ctypes as C, struct as S, json, time, hashlib
from ctypes import wintypes as W
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('run');p.add_argument('output',type=Path);p.add_argument('--seconds',type=float,default=30)
a=p.parse_args();root=Path('artifacts/test-runs').resolve();r=(root/a.run).resolve()
assert r.parent==root and 0<a.seconds<=60
pid=int((r/'pid.txt').read_text());k=C.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
k.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)]
k.QueryFullProcessImageNameW.argtypes=[W.HANDLE,W.DWORD,W.LPWSTR,C.POINTER(W.DWORD)]
k.CloseHandle.argtypes=[W.HANDLE]
h=k.OpenProcess(0x1010,False,pid);assert h
try:
 name=C.create_unicode_buffer(32768);n=W.DWORD(32768)
 assert k.QueryFullProcessImageNameW(h,0,name,C.byref(n))
 exe=Path(name.value).resolve();assert exe.parent==(r/'runtime').resolve() and exe.name.startswith('mercenaries_recomp_run')
 def read(address,count):
  assert 0x10000<=address<=0x4000000-count
  b=C.create_string_buffer(count);got=C.c_size_t()
  assert k.ReadProcessMemory(h,0x10000+address,b,count,C.byref(got)) and got.value==count
  return b.raw
 # Verified main loop 0x18047F..0x18052B: time bases and exported deltas.
 # PblTimeBase uses 3000 ticks/sec. These are cumulative counters, so missed
 # sampling polls do not lose elapsed simulation time.
 rows=[];start=time.perf_counter()
 while time.perf_counter()-start<a.seconds:
  t0=time.perf_counter();bases=read(0x4140cc,24);delta=read(0x413f98,20);state=S.unpack('<I',read(0x413f68,4))[0];t1=time.perf_counter()
  rows.append(dict(seconds=(t0+t1)/2-start,read_seconds=t1-t0,game_ticks=S.unpack_from('<I',bases)[0],unpaused_ticks=S.unpack_from('<I',bases,12)[0],game_dt=S.unpack_from('<f',delta,8)[0],unpaused_dt=S.unpack_from('<f',delta)[0],running_time=S.unpack_from('<f',delta,16)[0],state=hex(state)))
  time.sleep(.01)
 elapsed=rows[-1]['seconds']-rows[0]['seconds'];summary={'wall_seconds':elapsed,'samples':len(rows),'states':sorted(set(v['state'] for v in rows))}
 for field in ('game_ticks','unpaused_ticks'):
  seconds=((rows[-1][field]-rows[0][field])&0xffffffff)/3000
  summary[field]={'seconds':seconds,'ratio_to_wall':seconds/elapsed}
 summary['game_dt_range']=[min(v['game_dt']for v in rows),max(v['game_dt']for v in rows)]
 result={'pid':pid,'exe':str(exe),'sha256':hashlib.file_digest(exe.open('rb'),'sha256').hexdigest(),'summary':summary,'limitations':'One bounded private scene. Time-base speed does not prove individual subsystems use dt correctly. Sampling is read-only, not synchronized with every guest update.','samples':rows}
 a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(summary,indent=2))
finally:k.CloseHandle(h)

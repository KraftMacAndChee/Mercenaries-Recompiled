"""Read the opt-in completed-frame ring from an exact private test process."""
import argparse,ctypes as c,struct,re,json,gzip,hashlib
from ctypes import wintypes as w
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('run',type=Path);p.add_argument('output',type=Path);p.add_argument('--map',type=Path,default=Path('build/mercenaries-iso-run1721/bin/Release/mercenaries_recomp.map'));a=p.parse_args()
r=a.run.resolve();assert r.parent==Path('artifacts/test-runs').resolve()
pid=int((r/'pid.txt').read_text());exe=next((r/'runtime').glob('mercenaries_recomp_run*.exe'))
assert hashlib.sha256(exe.read_bytes()).digest()==hashlib.sha256(a.map.with_suffix('.exe').read_bytes()).digest(), 'Map executable mismatch'
k=c.WinDLL('kernel32',use_last_error=True);k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE;k.CloseHandle.argtypes=[w.HANDLE]
k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)];k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
h=k.OpenProcess(0x1010,False,pid);assert h
try:
 name=c.create_unicode_buffer(32768);n=w.DWORD(len(name));assert k.QueryFullProcessImageNameW(h,0,name,c.byref(n));assert Path(name.value).resolve()==exe
 ps=c.WinDLL('psapi');ps.EnumProcessModulesEx.argtypes=[w.HANDLE,c.POINTER(c.c_void_p),w.DWORD,c.POINTER(w.DWORD),w.DWORD]
 modules=(c.c_void_p*1024)();needed=w.DWORD();assert ps.EnumProcessModulesEx(h,modules,c.sizeof(modules),c.byref(needed),3);base=modules[0]
 def read(addr,size):
  b=c.create_string_buffer(size);got=c.c_size_t();assert k.ReadProcessMemory(h,addr,b,size,c.byref(got)) and got.value==size;return b.raw
 text=a.map.read_text();preferred=int(re.search(r'Preferred load address is ([0-9A-Fa-f]+)',text)[1],16)
 def sym(name):return base+int(re.search(r'\s'+name+r'\s+([0-9A-Fa-f]{16})\s',text)[1],16)-preferred
 mem=read(0x20000,0x3ff0000)
 found=[]
 start=0
 while True:
  off=mem.find(bytes.fromhex('90102f00'),start)
  if off<0: break
  start=off+4
  va=off+0x10000
  if off%16 or off+192>len(mem):continue
  refs=struct.unpack_from('<I',mem,off+4)[0]
  if refs&0xffff !=192:continue
  arrays=struct.unpack_from('<9I',mem,off+0x80)
  bad=0
  for i in range(0,9,3):
   ptr,count,cap=arrays[i:i+3]
   if count>(cap&0x7fffffff) or (count and not (0x10000<=ptr<=0x4000000-count*4)):bad|=1<<(i//3)
  found.append({'va':hex(va),'refs':hex(refs),'arrays':[hex(x) for x in arrays],'invalid':bad})
 target=0x1c3c450-0x10000
 result={'pid':pid,'count':len(found),'bad':[f for f in found if f['invalid']],'watched_body':mem[target:target+192].hex(),'entities':found}
 a.output.write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k!='entities'}))
finally:k.CloseHandle(h)


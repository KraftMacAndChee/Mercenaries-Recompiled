"""Read the retail C-17 turbulence flag from an explicitly named private process."""
import argparse,ctypes,json,struct,time
from ctypes import wintypes as w
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('pid',type=int);p.add_argument('expected_exe',type=Path);p.add_argument('output',type=Path);p.add_argument('--seconds',type=float,default=35);p.add_argument('--sample-noise',action='store_true');a=p.parse_args()
if not 0 < a.seconds <= 60:p.error('--seconds must be in (0, 60]')
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE
k.ReadProcessMemory.argtypes=[w.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,ctypes.POINTER(w.DWORD)];k.CloseHandle.argtypes=[w.HANDLE]
h=k.OpenProcess(0x1010,False,a.pid)
if not h:raise ctypes.WinError(ctypes.get_last_error())
try:
 name=ctypes.create_unicode_buffer(32768);n=w.DWORD(len(name))
 assert k.QueryFullProcessImageNameW(h,0,name,ctypes.byref(n))
 assert Path(name.value).resolve()==a.expected_exe.resolve(),'Process identity mismatch'
 def read(address,size):
  assert 0<=address<0x4000000-size
  b=ctypes.create_string_buffer(size);got=ctypes.c_size_t()
  if not k.ReadProcessMemory(h,address+0x10000,b,size,ctypes.byref(got)) or got.value!=size:raise ctypes.WinError(ctypes.get_last_error())
  return b.raw
 state=struct.unpack('<I',read(0x4140e8+0x38,4))[0];assert 0<state<0x4000000-0x180
 noise_samples=[];transitions=[];last=None;start=time.perf_counter();samples=0
 while time.perf_counter()-start<a.seconds:
  value=read(state+0x17c,1)[0];assert value in (0,1)
  t=time.perf_counter()-start;samples+=1
  if value!=last:transitions.append({'seconds':round(t,4),'enabled':bool(value)});last=value
  if a.sample_noise:
   data=read(state+0x128,0x28);noise_samples.append({'seconds':round(t,4),'enabled':bool(value),'noise':struct.unpack_from('<3f',data,0),'phase':struct.unpack_from('<f',data,0x24)[0]})
  time.sleep(.02)
 result={'pid':a.pid,'executable':name.value,'state':hex(state),'field_offset':'0x17c','samples':samples,'transitions':transitions}
 if a.sample_noise:result['noise_samples']=noise_samples
 a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k!='noise_samples'}))
finally:k.CloseHandle(h)

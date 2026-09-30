"""Read the opt-in completed-frame ring from an exact private test process."""
import argparse,ctypes as c,struct,re,json,gzip,hashlib
from ctypes import wintypes as w
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('run',type=Path);p.add_argument('output',type=Path);p.add_argument('--map',type=Path,default=Path('build/mercenaries-iso-run1721/bin/Release/mercenaries_recomp.map'));p.add_argument('--thread',type=int,required=True);p.add_argument('--guest',type=lambda x:int(x,0),required=True);a=p.parse_args()
r=a.run.resolve();assert r.parent==Path('artifacts/test-runs').resolve()
pid=int((r/'pid.txt').read_text());exe=next((r/'runtime').glob('mercenaries_recomp_run*.exe'))
assert hashlib.sha256(exe.read_bytes()).digest()==hashlib.sha256(a.map.with_suffix('.exe').read_bytes()).digest(), 'Map executable mismatch'
k=c.WinDLL('kernel32',use_last_error=True);k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenProcess.restype=w.HANDLE;k.CloseHandle.argtypes=[w.HANDLE]
k.QueryFullProcessImageNameW.argtypes=[w.HANDLE,w.DWORD,w.LPWSTR,c.POINTER(w.DWORD)];k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
h=k.OpenProcess(0x1038,False,pid);assert h
try:
 name=c.create_unicode_buffer(32768);n=w.DWORD(len(name));assert k.QueryFullProcessImageNameW(h,0,name,c.byref(n));assert Path(name.value).resolve()==exe
 ps=c.WinDLL('psapi');ps.EnumProcessModulesEx.argtypes=[w.HANDLE,c.POINTER(c.c_void_p),w.DWORD,c.POINTER(w.DWORD),w.DWORD]
 modules=(c.c_void_p*1024)();needed=w.DWORD();assert ps.EnumProcessModulesEx(h,modules,c.sizeof(modules),c.byref(needed),3);base=modules[0]
 def read(addr,size):
  b=c.create_string_buffer(size);got=c.c_size_t();assert k.ReadProcessMemory(h,addr,b,size,c.byref(got)) and got.value==size;return b.raw
 text=a.map.read_text();preferred=int(re.search(r'Preferred load address is ([0-9A-Fa-f]+)',text)[1],16)
 def sym(name):return base+int(re.search(r'\s'+name+r'\s+([0-9A-Fa-f]{16})\s',text)[1],16)-preferred
 assert c.sizeof(c.c_void_p)==8
 assert 0x10000<=a.guest<=0x3fffffc and a.guest%4==0
 k.OpenThread.argtypes=[w.DWORD,w.BOOL,w.DWORD];k.OpenThread.restype=w.HANDLE
 k.GetProcessIdOfThread.argtypes=[w.HANDLE];k.GetProcessIdOfThread.restype=w.DWORD
 for fn in ['SuspendThread','ResumeThread']:
  getattr(k,fn).argtypes=[w.HANDLE];getattr(k,fn).restype=w.DWORD
 for fn in ['GetThreadContext','SetThreadContext']:
  getattr(k,fn).argtypes=[w.HANDLE,c.c_void_p]
 k.WriteProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
 thread=k.OpenThread(0x005a,False,a.thread);assert thread
 assert k.GetProcessIdOfThread(thread)==pid
 suspended=False
 try:
  assert k.SuspendThread(thread)!=0xffffffff;suspended=True
  # AMD64 CONTEXT, 16-byte aligned; CONTEXT_DEBUG_REGISTERS only.
  storage=c.create_string_buffer(1232+15);address=(c.addressof(storage)+15)&~15
  c.c_uint32.from_address(address+48).value=0x00100010
  assert k.GetThreadContext(thread,address),c.get_last_error()
  old_dr0=c.c_uint64.from_address(address+72).value;old_dr7=c.c_uint64.from_address(address+112).value
  assert old_dr7&3==0, 'An existing DR0 watch is active'
  value=struct.unpack('<I',read(a.guest+0x10000,4))[0]
  for name,val in [('g_global_pointer_watchpoint_guest_va',a.guest),('g_global_pointer_watchpoint_old_value',value),('g_global_pointer_watchpoint_custom',1)]:
   v=c.c_uint32(val);done=c.c_size_t();assert k.WriteProcessMemory(h,sym(name),c.byref(v),4,c.byref(done)) and done.value==4
  c.c_uint64.from_address(address+72).value=a.guest+0x10000
  c.c_uint64.from_address(address+104).value=0
  c.c_uint64.from_address(address+112).value=(old_dr7&~0xf0003)|0xd0001
  assert k.SetThreadContext(thread,address),c.get_last_error()
  result={'pid':pid,'thread':a.thread,'guest':hex(a.guest),'value':hex(value),'old_dr0':hex(old_dr0),'old_dr7':hex(old_dr7),'guest_bytes_modified':False}
  a.output.write_text(json.dumps(result,indent=2));print(json.dumps(result))
 finally:
  if suspended:k.ResumeThread(thread)
  k.CloseHandle(thread)
finally:k.CloseHandle(h)

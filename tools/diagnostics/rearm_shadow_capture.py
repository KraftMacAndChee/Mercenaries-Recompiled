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
h=k.OpenProcess(0x1038,False,pid);assert h
try:
 name=c.create_unicode_buffer(32768);n=w.DWORD(len(name));assert k.QueryFullProcessImageNameW(h,0,name,c.byref(n));assert Path(name.value).resolve()==exe
 ps=c.WinDLL('psapi');ps.EnumProcessModulesEx.argtypes=[w.HANDLE,c.POINTER(c.c_void_p),w.DWORD,c.POINTER(w.DWORD),w.DWORD]
 modules=(c.c_void_p*1024)();needed=w.DWORD();assert ps.EnumProcessModulesEx(h,modules,c.sizeof(modules),c.byref(needed),3);base=modules[0]
 def read(addr,size):
  b=c.create_string_buffer(size);got=c.c_size_t();assert k.ReadProcessMemory(h,addr,b,size,c.byref(got)) and got.value==size;return b.raw
 text=a.map.read_text();preferred=int(re.search(r'Preferred load address is ([0-9A-Fa-f]+)',text)[1],16)
 def sym(name):return base+int(re.search(r'\s'+name+r'\s+([0-9A-Fa-f]{16})\s',text)[1],16)-preferred
 k.WriteProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
 # Only reset native diagnostic counters in a hash-matched private process.
 for name in ['?shadow_capture_done@?1??debug_capture_gameplay_draw_before@@9@9','?shadow_capture_seen@?BN@??debug_capture_gameplay_draw_before@@9@9']:
  address=base+int(re.search(r'\s'+re.escape(name)+r'\s+([0-9A-Fa-f]{16})\s',text)[1],16)-preferred
  zero=c.c_uint32(0);done=c.c_size_t();assert k.WriteProcessMemory(h,address,c.byref(zero),4,c.byref(done)) and done.value==4
 print('Reset two native capture counters; no guest memory changed')
finally:k.CloseHandle(h)

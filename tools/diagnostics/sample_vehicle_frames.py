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
 pointer=struct.unpack('<Q',read(sym('g_vehicle_frame_trace'),8))[0];assert pointer,'Trace not enabled'
 class Frame(c.Structure):
  _fields_=[('serial',c.c_uint64),('qpc',c.c_int64),('frequency',c.c_int64)]+[(x,c.c_uint32) for x in ['ticks','camera','owner','actor','mode']]+[(x,c.c_float) for x in ['dt','stick']]+[('source',c.c_uint32)]+[(x,c.c_float*16) for x in ['hull','chase','rendered']]
 assert c.sizeof(Frame)==248
 count=struct.unpack('<I',read(sym('g_vehicle_frame_trace_count'),4))[0];raw=read(pointer,c.sizeof(Frame)*4096)
 after=struct.unpack('<I',read(sym('g_vehicle_frame_trace_count'),4))[0]
 assert count <= after < count+4096, 'Ring wrapped during copy'
 # Exclude the potentially in-progress newest entry and any slots overwritten
 # during the read. All retained slots were complete before and stable during it.
 samples=[]
 for i in range(max(0,after-4096),max(0,count-1)):
  f=Frame.from_buffer_copy(raw,(i&4095)*248);samples.append({name:list(getattr(f,name)) if name in ['hull','chase','rendered'] else getattr(f,name) for name,_ in Frame._fields_})
 a.output.parent.mkdir(parents=True,exist_ok=True)
 with gzip.open(a.output,'wt',encoding='utf-8') as out:json.dump({'pid':pid,'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'count':count,'samples':samples},out)
 print(json.dumps({'count':count,'retained':len(samples),'output':str(a.output),'last':samples[-1] if samples else None}))
finally:k.CloseHandle(h)

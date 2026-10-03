"""Exercise production mod resolution, DSK deltas and non-destructive profile switching."""
from pathlib import Path
import os,subprocess,struct,tempfile,hashlib,shutil
ROOT=Path(__file__).resolve().parents[2]
CASE=ROOT/'artifacts/diagnostics/mod-loader-20261001/native'
HARNESS=r"""
#define UNICODE
#define _UNICODE
#include "mod_loader.cpp"
namespace mercmods { BOOL selector(HWND,const fs::path&,const fs::path&,Selection){return FALSE;} }
int wmain(int argc,wchar_t **argv){try{
 assert(argc>=4);mercmods::Selection s;s.separate_saves=wcscmp(argv[3],L"shared")!=0;
 for(int i=4;i<argc;++i)s.mods.push_back({argv[i],mercmods::fs::path(argv[1])/L"mods"/argv[i],true});
 auto l=mercmods::prepare(argv[1],argv[2],s,!wcscmp(argv[3],L"vanilla"));
 auto out=mercmods::fs::path(argv[1])/L"prepared.txt";std::wofstream f(out);
 f<<l.executable.wstring()<<L'\n'<<l.manifest.wstring()<<L'\n'<<l.saves.wstring()<<L'\n'<<l.cache.wstring()<<L'\n';f.close();
 mercmods::save_selection(argv[1],s);auto loaded=mercmods::discover(argv[1]);
 assert(loaded.separate_saves==s.separate_saves);
 for(size_t i=0;i<s.mods.size();++i)assert(loaded.mods[i].name==s.mods[i].name&&loaded.mods[i].enabled);
 if(GetEnvironmentVariableW(L"MOD_TEST_LAUNCH",nullptr,0))mercmods::start(l);
 return 0;
 }catch(const std::exception &e){fprintf(stderr,"%s\n",e.what());return 2;}}
"""
def msvc():
    command=r'cmd.exe /d /s /c ""C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && set"'
    output=subprocess.check_output(command,text=True)
    env={k.upper():v for k,v in os.environ.items()}
    for line in output.splitlines():
        if '=' in line:
            k,v=line.split('=',1);env[k.upper()]=v
    return env

def dsk(path,records):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(struct.pack('<II',len(records),0)+b''.join(struct.pack('<III',len(data),key,1) for key,data in records)+b''.join(data for key,data in records)+bytes(2048))

def read_dsk(path):
    data=path.read_bytes();n,tag=struct.unpack_from('<II',data);offset=8+12*n;out={}
    for i in range(n):
        size,key,kind=struct.unpack_from('<III',data,8+12*i);out[key]=data[offset:offset+size];offset+=size
    assert not any(data[offset:]);return out

def manifest(path):
    data=path.read_bytes();magic,n=struct.unpack_from('<II',data);assert magic==0x31444f4d;offset=8;out={}
    for _ in range(n):
        a,b=struct.unpack_from('<II',data,offset);offset+=8
        key=data[offset:offset+a*2].decode('utf-16le');offset+=a*2
        out[key]=Path(data[offset:offset+b*2].decode('utf-16le'));offset+=b*2
    assert offset==len(data);return out

def main():
    CASE.mkdir(parents=True,exist_ok=True);cpp=CASE/'backend.cpp';cpp.write_text('#include <cassert>\n'+HARNESS)
    exe=CASE/'backend.exe';env=msvc()
    subprocess.run([shutil.which('cl',path=env['PATH']),'/nologo','/std:c++17','/EHsc','/O2','/I'+str(ROOT/'ports/mercenaries/src'),str(cpp),'/Fo'+str(CASE/'backend.obj'),'/Fe'+str(exe),'/link','bcrypt.lib','user32.lib','/STACK:8388608'],env=env,check=True)
    with tempfile.TemporaryDirectory(dir=CASE,prefix='fixture-') as tmp:
        root=Path(tmp);game=root/'game_files/mercenaries-retail';game.mkdir(parents=True)
        (root/'mercenaries_recomp.exe').write_bytes(b'placeholder runtime')
        (root/'SDL2.dll').write_bytes(b'base DLL')
        (game/'default.xbe').write_bytes(b'base xbe')
        (game/'read-only.bin').write_bytes(b'vanilla')
        original=[(1,b'skin'),(2,b'vehicle'),(3,b'deleted-by-je3')]
        dsk(game/'DATAxbox/assets.dsk',original)
        dsk(root/'mods/JE3/assets.dsk',[(1,b'je3 skin'),(2,b'je3 vehicle'),(4,b'je3 addition')])
        dsk(root/'mods/Skin/assets.dsk',[(1,b'custom skin'),*original[1:]])
        added=root/'mods/JE3/game_files/new-folder/addition.bin';added.parent.mkdir(parents=True);added.write_bytes(b'added')
        (root/'mods/JE3/SDL2.dll').write_bytes(b'mod DLL')
        (root/'mods/JE3/camera.ini').write_bytes(b'[Camera]\nFOV=60')
        (root/'mods/JE3/untouched.cfg').write_bytes(b'old default')
        archive=root/'mods/JE3/assets.dsk'
        archive.write_bytes(archive.read_bytes()[:-4]+bytes.fromhex('1b0be67f'))
        snapshots={p:hashlib.sha256(p.read_bytes()).digest() for folder in [game,root/'mods'] for p in folder.rglob('*') if p.is_file()}
        def prepare(mode,*mods,ok=True):
            r=subprocess.run([str(exe),str(root),str(game),mode,*mods],capture_output=True,text=True)
            assert (r.returncode==0)==ok,r.stdout+r.stderr
            return (root/'prepared.txt').read_text().splitlines() if ok else None
        first=prepare('separate','JE3','Skin');view=manifest(Path(first[1]))
        assert read_dsk(view['dataxbox\\assets.dsk'])=={1:b'custom skin',2:b'je3 vehicle',4:b'je3 addition'}
        assert view['new-folder\\addition.bin']==added
        assert Path(first[0]).parent.joinpath('sdl2.dll').read_bytes()==b'mod DLL'
        assert Path(first[2])==root/'mod-saves/mercenaries'
        runtime=Path(first[0]).parent
        (runtime/'camera.ini').write_bytes(b'[Camera]\nFOV=85')
        Path(first[3]).mkdir();(Path(first[3])/'cached.dsk').write_bytes(b'obsolete cache')
        ready=Path(first[1]).parent/'ready';stamp=ready.stat().st_mtime_ns
        again=prepare('shared','JE3','Skin');assert ready.stat().st_mtime_ns==stamp
        assert first[1]==again[1] and Path(again[2])==root/'saves/mercenaries'
        reverse=prepare('separate','Skin','JE3');assert reverse[3]!=first[3]
        assert not Path(first[1]).parent.exists()
        assert (Path(reverse[0]).parent/'camera.ini').read_bytes()==b'[Camera]\nFOV=85'
        assert len(list((root/'mod-cache').iterdir()))==1
        saved_settings=[p for p in (root/'mod-settings').rglob('camera.ini') if '.defaults' not in p.parts]
        assert len(saved_settings)==1 and saved_settings[0].read_bytes()==b'[Camera]\nFOV=85'
        # Editors that atomically replace an INI break hard links; retirement still saves it.
        settings=Path(reverse[0]).parent/'camera.ini'
        replacement=settings.with_suffix('.new');replacement.write_bytes(b'[Camera]\nFOV=90');replacement.replace(settings)
        assert not list((root/'mod-settings').rglob('*.dll'))
        assert read_dsk(manifest(Path(reverse[1]))['dataxbox\\assets.dsk'])[1]==b'je3 skin'
        unicode_mod=root/'mods'/'Skin-\u96ea';unicode_mod.mkdir();(unicode_mod/'camera.ini').write_bytes(b'[Camera]')
        prepare('shared','Skin-\u96ea')
        vanilla=prepare('vanilla');assert not list((root/'mod-cache').iterdir());assert vanilla[1]=='' and vanilla[3]=='' and Path(vanilla[0])==root/'mercenaries_recomp.exe'
        skin=prepare('separate','Skin');assert 'new-folder\\addition.bin' not in manifest(Path(skin[1]))
        assert 4 not in read_dsk(manifest(Path(skin[1]))['dataxbox\\assets.dsk'])
        assert prepare('separate','JE3','Skin')[1]==first[1]
        for p,digest in snapshots.items():assert hashlib.sha256(p.read_bytes()).digest()==digest,p
        dsk(root/'mods/Skin/assets.dsk',[(1,b'updated skin'),*original[1:]])
        (root/'mods/JE3/untouched.cfg').write_bytes(b'new default')
        updated=prepare('separate','JE3','Skin');assert updated[3]!=first[3]
        assert (Path(updated[0]).parent/'untouched.cfg').read_bytes()==b'new default'
        assert (Path(updated[0]).parent/'camera.ini').read_bytes()==b'[Camera]\nFOV=90'
        assert len(list((root/'mod-cache').iterdir()))==1
        # A legacy profile's configuration is rescued before retiring its data.
        legacy=root/'mod-cache'/('a'*64);(legacy/'runtime').mkdir(parents=True)
        (legacy/'runtime/old.ini').write_bytes(b'legacy settings')
        (legacy/'storage').mkdir();(legacy/'storage/old.dsk').write_bytes(b'large disposable archive')
        prepare('separate','JE3','Skin')
        assert not legacy.exists()
        assert (root/'mod-settings/legacy'/('a'*64)/'old.ini').read_bytes()==b'legacy settings'
        # Preparation is excluded while another process owns the cache lease.
        import ctypes
        kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        kernel.CreateFileW.restype=ctypes.c_void_p
        handle=kernel.CreateFileW(str(root/'mod-loader.lock'),0x80000000,0,None,3,0,None)
        assert handle not in (None,ctypes.c_void_p(-1).value)
        try: prepare('vanilla',ok=False)
        finally: kernel.CloseHandle(ctypes.c_void_p(handle))
        assert Path(updated[1]).exists()
        (root/'mods/Skin/assets.dsk').write_bytes(b'invalid archive');prepare('separate','JE3','Skin',ok=False)
        assert Path(updated[1]).exists()
    # A real child must retain the lease after the launching process exits.
    with tempfile.TemporaryDirectory(dir=CASE,prefix='lease-') as tmp:
        root=Path(tmp);game=root/'game_files';game.mkdir()
        child=CASE/'lease_child.cpp'
        child.write_text('#include <windows.h>\n#include <fstream>\nint main(){std::ofstream("running") << "1"; for(int i=0;i<500;++i){if(GetFileAttributesW(L"release")!=INVALID_FILE_ATTRIBUTES)break;Sleep(20);}return 0;}')
        subprocess.run([shutil.which('cl',path=env['PATH']),'/nologo','/EHsc',str(child),'/Fo'+str(CASE/'lease_child.obj'),'/Fe'+str(root/'mercenaries_recomp.exe')],env=env,check=True)
        launch_env=dict(os.environ,MOD_TEST_LAUNCH='1')
        subprocess.run([str(exe),str(root),str(game),'vanilla'],env=launch_env,check=True)
        import time
        for _ in range(250):
            if (root/'running').exists():break
            time.sleep(.02)
        assert (root/'running').exists()
        blocked=subprocess.run([str(exe),str(root),str(game),'vanilla'],capture_output=True)
        assert blocked.returncode==2 and b'Close the running game' in blocked.stderr
        (root/'release').write_text('1')
        for _ in range(250):
            retry=subprocess.run([str(exe),str(root),str(game),'vanilla'],capture_output=True)
            if retry.returncode==0:break
            time.sleep(.02)
        assert retry.returncode==0,retry.stderr
    print('PASS: DSK asset replacement/addition/deletion, load order, DLL override, cache reuse/retirement, persistent/atomic settings, legacy rescue, inherited lifetime lock, optional saves, JE3-to-vanilla isolation, unchanged inputs, malformed archive rejection')
if __name__=='__main__':main()

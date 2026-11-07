"""Compile real x64 fixture DLLs and exercise production staging and plugin host."""
from pathlib import Path
import importlib.util,json,os,shutil,struct,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
CASE=ROOT/'artifacts/diagnostics/plugin-api-20261004/native'
HARNESS=r'''
#define UNICODE
#define _UNICODE
#include <cassert>
#include "mod_loader.cpp"
#include "plugin_host.h"
#include "plugin_hooks.h"
namespace mercmods {BOOL selector(HWND,const fs::path&,const fs::path&,Selection){return FALSE;}}
/* Guest-call bridge stand-in: function 0x1234 returns this_ptr + args[0]. */
static unsigned guest_depth;
extern "C" int recomp_plugin_guest_call_begin(uint32_t bytes,uint32_t *scratch){(void)bytes;*scratch=0x50000;++guest_depth;return 1;}
extern "C" int recomp_plugin_guest_call(uint32_t f,uint32_t self,const uint32_t *a,uint32_t n,uint32_t *eax){
    if(f!=0x1234||!guest_depth)return 0;if(eax)*eax=self+(n?a[0]:0);return 1;}
extern "C" void recomp_plugin_guest_call_end(void){assert(guest_depth);--guest_depth;}
static DWORD WINAPI worker(void*) { assert(merc_plugins_update());return 0; }
int wmain(int argc,wchar_t**argv) {
 try {
    assert(argc>=3);std::filesystem::path root=argv[1];std::wstring mode=argv[2];
    mercmods::Selection s;
    for(int i=3;i<argc;++i)s.mods.push_back({argv[i],root/L"mods"/argv[i],true});
    auto info=mercmods::inspect_plugins(s);
    auto launch=mercmods::prepare(root,root/L"game_files/mercenaries-retail",s,mode==L"vanilla");
    std::ofstream paths(root/L"paths.txt");
    paths<<mercplugins::utf8(launch.working.wstring())<<"\n"<<mercplugins::utf8(launch.plugins.wstring())<<"\n";
    paths.close();
    if(mode==L"stage"||mode==L"vanilla")return 0;
    SetEnvironmentVariableW(L"MERCENARIES_PLUGIN_LIST",launch.plugins.c_str());
    SetEnvironmentVariableW(L"MERCENARIES_PLUGIN_SETTINGS",launch.plugin_settings.c_str());
    auto ram=(unsigned char*)VirtualAlloc(nullptr,0x4000000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    assert(ram);*(uint32_t*)(ram+0x10000)=0x48454258;
    DWORD old;assert(VirtualProtect(ram+0x30000,4096,PAGE_READONLY,&old));
    assert(VirtualProtect(ram+0x40000,4096,PAGE_NOACCESS,&old));
    if(!merc_plugins_initialize(ram,0x4000000)) {
        fprintf(stderr,"%s\n",merc_plugins_error());VirtualFree(ram,0,MEM_RELEASE);return 3;
    }
    if(mode==L"hooks") {
        /* Interface 2: subscription, dispatch, writable args, guest calls. */
        assert(merc_plugins_hook_active(MERC_HOOK_TURRET_UPDATE));
        assert(!merc_plugins_hook_active(MERC_HOOK_TURRET_CAMERA_VIEW));
        MercPluginHookEvent e=merc_hook_event(2u,0x1000u,5u,0u,0u,0u);
        assert(merc_plugins_hook(MERC_HOOK_TURRET_UPDATE,&e));
        assert(e.args[1]==99&&e.result==42&&!strcmp(e.name,"turret.update")&&guest_depth==0);
        MercPluginHookEvent idle=merc_hook_event(0u,0u,0u,0u,0u,0u);
        assert(!merc_plugins_hook(MERC_HOOK_TURRET_CAMERA_VIEW,&idle));
        auto manifest=mercplugins::read_manifest(root/L"mods"/argv[3]);
        auto p=launch.working/L"plugins"/mercplugins::wide(manifest->id)/L"fixture.dll";
        HMODULE dll=GetModuleHandleW(p.c_str());assert(dll);
        auto outside=(int(*)())GetProcAddress(dll,"outside_guest_call");assert(outside&&outside()==0);
        merc_plugins_shutdown();assert(!merc_plugins_hook_active(MERC_HOOK_TURRET_UPDATE));
        std::ofstream results(root/L"result.txt");results<<"hooks ok";results.close();
        VirtualFree(ram,0,MEM_RELEASE);return 0;
    }
    auto thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);
    assert(thread);WaitForSingleObject(thread,INFINITE);CloseHandle(thread);
    assert(*(uint32_t*)(ram+0x20000)==0);
    for(int i=3;i<argc;++i) {
        auto manifest=mercplugins::read_manifest(root/L"mods"/argv[i]);
        if(!manifest)continue;
        auto p=launch.working/L"plugins"/mercplugins::wide(manifest->id)/L"fixture.dll";
        HMODULE dll=GetModuleHandleW(p.c_str());assert(dll);
        auto outside=(int(*)())GetProcAddress(dll,"outside_callback");assert(outside&&outside()==0);
    }
    int first=merc_plugins_update(),second=merc_plugins_update();
    std::ofstream results(root/L"result.txt");results<<first<<" "<<second<<" "<<*(uint32_t*)(ram+0x20000);results.close();
    if(mode==L"normal-exit") {assert(atexit(merc_plugins_shutdown)==0);return 0;}
    merc_plugins_shutdown();merc_plugins_shutdown();assert(merc_plugins_update());
    VirtualFree(ram,0,MEM_RELEASE);return 0;
 } catch(const std::exception&e) {fprintf(stderr,"%s\n",e.what());return 2;}
}
'''
PLUGIN=r'''
#define MERC_PLUGIN_IMPLEMENTATION
#include "mercenaries_plugin.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef VALUE
#define VALUE 1
#endif
__declspec(dllimport) int fixture_dependency(void);
static const MercPluginHost *h;
static int called;
static DWORD WINAPI worker(void *ignored) {uint32_t v; (void)ignored;return h->read_guest(h->context,0x10000,&v,4);}
__declspec(dllexport) int outside_callback(void) {uint32_t v;return h->read_guest(h->context,0x10000,&v,4);}
static void __cdecl done(void *u) {(void)u;h->log(h->context,"shutdown");}
static int __cdecl tick(void *u,const MercPluginUpdate *t) {
    uint32_t v;(void)u;
    if(t->struct_size!=sizeof(*t)||t->api_version!=1)return 0;
    if(t->update_index==1 && called)return 0;
    called=1;h->log(h->context,"update");
    const char *fail=getenv("PLUGIN_FAIL_UPDATE");
    if(fail && !strcmp(fail,h->plugin_id))return 0;
    if(!h->read_guest(h->context,0x20000,&v,4))return 0;
    v=v*10+VALUE;
    return h->write_guest(h->context,0x20000,&v,4);
}
MERC_PLUGIN_EXPORT int __cdecl MercenariesPlugin_Init(const MercPluginHost *host,MercPlugin *p,char *error,uint32_t capacity) {
    uint32_t v=0;DWORD old;(void)old;h=host;
    p->on_shutdown=done;p->on_update=tick;
    if(host->api_version!=1||host->struct_size<sizeof(*host))return 0;
    h->log(h->context,"init");
    const char *fail=getenv("PLUGIN_FAIL_INIT");
    if(fail&&!strcmp(fail,h->plugin_id)){snprintf(error,capacity,"fixture init failed");return 0;}
    if(fixture_dependency()!=73)return 0;
    if(!h->read_guest(h->context,0x10000,&v,4)||v!=0x48454258)return 0;
    if(h->read_guest(h->context,0xffffffff,&v,4)||h->read_guest(h->context,0xffff,&v,4)||
       h->read_guest(h->context,0x3fffffe,&v,4)||h->read_guest(h->context,0x10000,&v,(size_t)-1)||
       h->read_guest(h->context,0x10000,&v,0)||h->write_guest(h->context,0x30000,&v,4)||
       h->read_guest(h->context,0x40000,&v,4)||h->read_guest(NULL,0x10000,&v,4))return 0;
    if(!h->read_guest(h->context,0x30000,&v,4))return 0;
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);if(!thread)return 0;
    WaitForSingleObject(thread,INFINITE);GetExitCodeThread(thread,&old);CloseHandle(thread);if(old)return 0;
    return 1;
}
'''
PLUGIN_V2=r'''
#define MERC_PLUGIN_IMPLEMENTATION
#include "mercenaries_plugin.h"
#include <windows.h>
#include <string.h>
static const MercPluginHost *h;
static void __cdecl on_hook(void *u,MercPluginHookEvent *e) {
    uint32_t scratch=0,eax=0,args[1]={41};
    if((uintptr_t)u!=7||e->struct_size!=sizeof(*e)||e->api_version!=2||strcmp(e->name,"turret.update")||e->args[0]!=0x1000)return;
    if(!h->guest_call_begin(h->context,64,&scratch)||scratch!=0x50000)return;
    int ok=h->guest_call(h->context,0x1234,1,args,1,&eax);
    int bad=h->guest_call(h->context,0x9999,0,NULL,0,&eax);
    h->guest_call_end(h->context);
    if(!ok||bad||eax!=42)return;
    e->args[1]=99;e->result=42;e->handled=1;
}
__declspec(dllexport) int outside_guest_call(void) {uint32_t s;return h->guest_call_begin(h->context,16,&s);}
static void __cdecl done(void *u) {(void)u;h->log(h->context,"shutdown");}
MERC_PLUGIN_EXPORT int __cdecl MercenariesPlugin_Init(const MercPluginHost *host,MercPlugin *p,char *error,uint32_t capacity) {
    (void)error;(void)capacity;h=host;p->on_shutdown=done;
    if(host->api_version!=MERCENARIES_PLUGIN_API_VERSION_2||host->struct_size<sizeof(*host))return 0;
    if(p->api_version!=2)return 0;
    if(host->register_hook(host->context,"no.such.hook",on_hook,NULL))return 0;
    if(!host->register_hook(host->context,"turret.update",on_hook,(void*)7))return 0;
    return 1;
}
'''
def main():
 CASE.mkdir(parents=True,exist_ok=True)
 spec=importlib.util.spec_from_file_location('loader_tests',ROOT/'tools/recomp/test_mod_loader_native.py')
 mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);env=mod.msvc()
 compiler=shutil.which('cl',path=env['PATH'])
 def build(args):
  r=subprocess.run([compiler,'/nologo','/utf-8','/W3','/WX','/D_CRT_SECURE_NO_WARNINGS',*args],env=env,capture_output=True,text=True)
  assert r.returncode==0,r.stdout+r.stderr
 (CASE/'dependency.c').write_text('__declspec(dllexport) int fixture_dependency(void){return 73;}',encoding='utf-8')
 build(['/LD',str(CASE/'dependency.c'),'/Fo'+str(CASE/'dependency.obj'),'/Fe'+str(CASE/'unique_fixture_dependency.dll')])
 sdk=ROOT/'ports/mercenaries/sdk';src=ROOT/'ports/mercenaries/src'
 (CASE/'fixture.c').write_text(PLUGIN,encoding='utf-8')
 for value in [1,2,3]:
  build(['/LD','/I'+str(sdk),'/DVALUE='+str(value),str(CASE/'fixture.c'),str(CASE/'unique_fixture_dependency.lib'),'/Fo'+str(CASE/f'fixture{value}.obj'),'/Fe'+str(CASE/f'fixture{value}.dll')])
 (CASE/'fixture_v2.c').write_text(PLUGIN_V2,encoding='utf-8')
 build(['/LD','/I'+str(sdk),str(CASE/'fixture_v2.c'),'/Fo'+str(CASE/'fixture_v2.obj'),'/Fe'+str(CASE/'fixture_v2.dll')])
 (CASE/'harness.cpp').write_text(HARNESS,encoding='utf-8')
 build(['/EHsc','/std:c++17','/I'+str(src),str(CASE/'harness.cpp'),str(src/'plugin_host.cpp'),'/Fo'+str(CASE)+'\\','/Fe'+str(CASE/'harness.exe'),'/link','bcrypt.lib','user32.lib'])
 checks=[]
 with tempfile.TemporaryDirectory(prefix='fixtures-',dir=CASE) as tmp:
  base=Path(tmp).resolve();assert base.parent==CASE.resolve()
  def fixture(name):
   p=base/name;game=p/'game_files/mercenaries-retail';game.mkdir(parents=True)
   (p/'mercenaries_recomp.exe').write_bytes(b'fixture not executed');(game/'default.xbe').write_bytes(b'fixture')
   return p
  def plugin(p,folder,id,value=1,extra='',api=1):
   path=p/'mods'/folder;path.mkdir(parents=True)
   shutil.copyfile(CASE/f'fixture{value}.dll',path/'fixture.dll')
   shutil.copyfile(CASE/'unique_fixture_dependency.dll',path/'unique_fixture_dependency.dll')
   (path/'mod.ini').write_text(f'[Plugin]\nId={id}\nName={folder}\nApiVersion={api}\nLibrary=fixture.dll\n'+extra,encoding='utf-8')
   (path/'same.ini').write_text('[Settings]\nValue=default\n')
   return path
  def run(p,mode,*mods,code=0,contains='',overrides=None):
   e=dict(os.environ);e.update(overrides or {})
   r=subprocess.run([str(CASE/'harness.exe'),str(p),mode,*mods],capture_output=True,text=True,env=e)
   assert r.returncode==code,(r.returncode,r.stdout,r.stderr)
   assert contains in r.stderr,r.stderr
   return r
  p=fixture('order');a=plugin(p,'Alpha','alpha',1);b=plugin(p,'Beta','beta',2)
  run(p,'run','Alpha','Beta')
  assert (p/'result.txt').read_text()=='1 1 1212'
  log=(p/'plugin-settings/plugins.log').read_text()
  assert log.index('beta: shutdown')<log.index('alpha: shutdown')
  assert log.count('alpha: update')==2 and log.count('beta: update')==2
  run(p,'normal-exit','Alpha','Beta')
  log=(p/'plugin-settings/plugins.log').read_text()
  assert log.index('beta: shutdown')<log.index('alpha: shutdown')
  checks.append('normal process-exit cleanup')
  (p/'plugin-settings/alpha/same.ini').write_text('saved alpha')
  run(p,'run','Beta','Alpha');assert (p/'result.txt').read_text()=='1 1 2121'
  assert (p/'plugin-settings/alpha/same.ini').read_text()=='saved alpha'
  assert (p/'plugin-settings/beta/same.ini').read_text()=='[Settings]\nValue=default\n'
  run(p,'vanilla','Alpha','Beta');assert (p/'paths.txt').read_text().splitlines()[1:]==['']
  assert (p/'plugin-settings/alpha/same.ini').read_text()=='saved alpha'
  checks+=['real DLL init/update/reverse cleanup','same DLL filenames in private folders','dependency DLL loading','checked reads/writes, overflow, protections, callback/thread restrictions','load order and independent persistent settings','vanilla isolation']
  p=fixture('hooks');plugin(p,'Hooks','hooks','_v2',api=2)
  run(p,'hooks','Hooks');assert (p/'result.txt').read_text()=='hooks ok'
  log=(p/'plugin-settings/plugins.log').read_text()
  assert 'initialized (interface 2)' in log and 'hooked turret.update' in log and 'unknown hook point no.such.hook' in log
  checks+=['interface 2 hooks: subscribe, reject unknown, dispatch, writable args/result','interface 2 guest calls inside callbacks only, unknown function rejected','shutdown clears hook subscriptions']
  p=fixture('failure');plugin(p,'Alpha','alpha',1);plugin(p,'Beta','beta',2,'Requires=alpha\n');plugin(p,'Gamma','gamma',3);plugin(p,'Delta','delta',2,'Requires=beta\n')
  run(p,'run','Alpha','Beta','Delta','Gamma',overrides={'PLUGIN_FAIL_UPDATE':'alpha'})
  assert (p/'result.txt').read_text()=='0 1 33'
  log=(p/'plugin-settings/plugins.log').read_text();assert 'beta: update' not in log and 'delta: update' not in log
  run(p,'run','Alpha','Beta',code=3,contains='fixture init failed',overrides={'PLUGIN_FAIL_INIT':'beta'})
  log=(p/'plugin-settings/plugins.log').read_text();assert log.index('beta: shutdown')<log.index('alpha: shutdown')
  checks+=['update failure stops transitive dependents, unrelated plugins continue','failed init rollback']
  run(p,'stage','Beta',code=2,contains='Requires enabled plugin')
  run(p,'stage','Beta','Alpha',code=2,contains='Move required plugin')
  for name,extra,needle in [('api','ApiVersion=3\n','unsupported Plugin Interface'),('duplicate','Id=alpha\n','Duplicate [Plugin] key'),('unknown','Mystery=1\n','Unknown [Plugin] key'),('traversal','Library=../fixture.dll\n','relative .dll path'),('missing','Library=missing.dll\n','missing DLL')]:
   q=fixture(name);path=plugin(q,'Alpha','alpha')
   text=(path/'mod.ini').read_text()
   if name in ['api','traversal','missing']:
    key=extra.split('=')[0];text='\n'.join(x for x in text.splitlines() if not x.startswith(key+'='))+'\n'
   (path/'mod.ini').write_text(text+extra)
   run(q,'stage','Alpha',code=2,contains=needle)
  q=fixture('conflict');plugin(q,'Alpha','alpha',1,'Conflicts=beta\n');plugin(q,'Beta','beta',2)
  run(q,'stage','Alpha','Beta',code=2,contains='Declared conflict')
  q=fixture('exclusive');plugin(q,'Alpha','alpha',1,'Exclusive=camera\n');plugin(q,'Beta','beta',2,'Exclusive=camera\n')
  run(q,'stage','Alpha','Beta',code=2,contains='Exclusive resource')
  q=fixture('duplicate-id');plugin(q,'Alpha','same');plugin(q,'Beta','same')
  run(q,'stage','Alpha','Beta',code=2,contains='Duplicate plugin ID')
  q=fixture('entry');path=plugin(q,'Alpha','alpha');shutil.copyfile(CASE/'unique_fixture_dependency.dll',path/'fixture.dll')
  run(q,'run','Alpha',code=3,contains='does not export')
  q=fixture('dll-missing-dependency');path=plugin(q,'Alpha','alpha');(path/'unique_fixture_dependency.dll').unlink()
  run(q,'run','Alpha',code=3,contains='Windows error 126')
  q=fixture('invalid-dll');path=plugin(q,'Alpha','alpha');(path/'fixture.dll').write_bytes(b'not a DLL')
  run(q,'run','Alpha',code=3,contains='Windows error 193')
  checks+=['required IDs/order','unsupported API, malformed manifest, invalid paths','declared conflicts and exclusive claims','duplicate IDs','missing export/dependency and invalid DLL']
 (CASE/'results.json').write_text(json.dumps({'passed':checks},indent=2),encoding='utf-8')
 print('PASS: '+'; '.join(checks))
if __name__=='__main__':main()


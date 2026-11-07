#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "plugin_host.h"
#include "plugin_manifest.h"
#include "plugin_hooks.h"
#include <memory>
#include <cstdio>
#include <cstring>

namespace {
using namespace mercplugins;
struct Loaded {
    Manifest manifest;
    HMODULE module=nullptr;
    MercPluginHost host{};
    MercPlugin callbacks{};
    std::string directory,settings;
    bool entered=false,active=false;
};
std::vector<std::unique_ptr<Loaded>> loaded;
DWORD owner=0;
void *guest=nullptr;
size_t guest_size=0;
Loaded *calling=nullptr;
bool dispatching=false,initialized=false;
ULONGLONG previous=0;
uint64_t sequence=0;
std::string failure;
std::ofstream log_file;
/* Interface 2 hook subscriptions, in load order. */
struct Hook { Loaded *plugin; MercPluginHookFn fn; void *userdata; };
std::vector<Hook> hooks[MERC_HOOK_COUNT];
const char *const hook_names[MERC_HOOK_COUNT]={
    "input.vehicle_look","turret_camera.view","chase_camera.snap","chase_camera.yaw",
    "turret.update","turret.player_aim","turret.ballistic_arc","weapon.player_fire",
    "hud.crosshair","hud.dumb_target_turret","hud.dumb_target_point","hud.dumb_target_bracket"};
/* Guest calls nest (a hook can fire inside a plugin's guest call). */
std::vector<Loaded*> guest_call_owners;

std::wstring environment(const wchar_t *key) {
    DWORD n=GetEnvironmentVariableW(key,nullptr,0);
    if(!n)return {};
    std::wstring s(n,0);
    if(GetEnvironmentVariableW(key,s.data(),n)>=n)throw std::runtime_error("Plugin environment changed during startup.");
    s.resize(n-1);return s;
}
void record(const std::string &s) {
    if(log_file){log_file<<s<<"\n";log_file.flush();}
    fprintf(stderr,"[plugins] %s\n",s.c_str());
}
void MERC_PLUGIN_CALL plugin_log(void *ctx,const char *message) {
    if(GetCurrentThreadId()!=owner||!calling||ctx!=calling||!message)return;
    record(calling->manifest.id+": "+std::string(message));
}
bool accessible(void *address,size_t bytes,bool write) {
    auto p=reinterpret_cast<uintptr_t>(address),end=p+bytes;
    if(end<p)return false;
    while(p<end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if(!VirtualQuery(reinterpret_cast<void*>(p),&mbi,sizeof(mbi))||mbi.State!=MEM_COMMIT||
           (mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        DWORD protection=mbi.Protect&0xff;
        bool readable=protection==PAGE_READONLY||protection==PAGE_READWRITE||protection==PAGE_WRITECOPY||
                      protection==PAGE_EXECUTE_READ||protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
        bool writable=protection==PAGE_READWRITE||protection==PAGE_WRITECOPY||
                      protection==PAGE_EXECUTE_READWRITE||protection==PAGE_EXECUTE_WRITECOPY;
        if(!readable||(write&&!writable))return false;
        auto next=reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
        if(next<=p)return false;p=next;
    }return true;
}
bool span(void *ctx,uint32_t address,size_t bytes,bool write) {
    return GetCurrentThreadId()==owner&&calling&&ctx==calling&&guest&&bytes&&
        address>=0x10000u&&address<guest_size&&bytes<=guest_size-address&&
        accessible(static_cast<unsigned char*>(guest)+address,bytes,write);
}
int MERC_PLUGIN_CALL read_guest(void *ctx,uint32_t address,void *out,size_t bytes) {
    if(!out||!span(ctx,address,bytes,false)||!accessible(out,bytes,true))return 0;
    memmove(out,static_cast<unsigned char*>(guest)+address,bytes);return 1;
}
int MERC_PLUGIN_CALL write_guest(void *ctx,uint32_t address,const void *data,size_t bytes) {
    if(!data||!span(ctx,address,bytes,true)||!accessible(const_cast<void*>(data),bytes,false))return 0;
    memmove(static_cast<unsigned char*>(guest)+address,data,bytes);return 1;
}
bool callback_context(void *ctx) {
    return GetCurrentThreadId()==owner&&calling&&ctx==calling&&calling->manifest.api_version>=MERCENARIES_PLUGIN_API_VERSION_2;
}
int MERC_PLUGIN_CALL register_hook(void *ctx,const char *name,MercPluginHookFn fn,void *userdata) {
    if(!callback_context(ctx)||!name||!fn)return 0;
    for(unsigned id=0;id<MERC_HOOK_COUNT;++id)if(!strcmp(name,hook_names[id])) {
        hooks[id].push_back({calling,fn,userdata});
        record(calling->manifest.id+": hooked "+name);
        return 1;
    }
    record(calling->manifest.id+": unknown hook point "+name);
    return 0;
}
int MERC_PLUGIN_CALL guest_call_begin(void *ctx,uint32_t bytes,uint32_t *scratch) {
    if(!callback_context(ctx)||!scratch||bytes>4096u||guest_call_owners.size()>=4)return 0;
    if(!recomp_plugin_guest_call_begin(bytes,scratch))return 0;
    guest_call_owners.push_back(calling);return 1;
}
int MERC_PLUGIN_CALL guest_call(void *ctx,uint32_t function,uint32_t self,const uint32_t *args,uint32_t count,uint32_t *eax) {
    if(!callback_context(ctx)||guest_call_owners.empty()||guest_call_owners.back()!=calling||count>16||(count&&!args))return 0;
    Loaded *caller=calling;
    int ok=recomp_plugin_guest_call(function,self,args,count,eax);
    calling=caller; /* hooks fired by the callee restore their own context */
    return ok;
}
void MERC_PLUGIN_CALL guest_call_end(void *ctx) {
    if(!callback_context(ctx)||guest_call_owners.empty()||guest_call_owners.back()!=calling)return;
    guest_call_owners.pop_back();recomp_plugin_guest_call_end();
}
void shutdown_all() {
    for(auto it=loaded.rbegin();it!=loaded.rend();++it) {
        auto &p=**it;
        if(p.entered&&p.callbacks.on_shutdown) {
            calling=&p;
            try {p.callbacks.on_shutdown(p.callbacks.userdata);}
            catch(...) {record(p.manifest.id+": shutdown threw an exception.");}
            calling=nullptr;
        }
        if(p.module)FreeLibrary(p.module);
    }
    for(auto &h:hooks)h.clear();
    while(!guest_call_owners.empty()){guest_call_owners.pop_back();recomp_plugin_guest_call_end();}
    loaded.clear();guest=nullptr;guest_size=0;initialized=false;dispatching=false;
    if(log_file)log_file.close();
}
}
extern "C" const char *merc_plugins_error(void) {return failure.c_str();}
extern "C" int merc_plugins_hook_active(unsigned id) {return id<MERC_HOOK_COUNT&&!hooks[id].empty();}
extern "C" int merc_plugins_hook(unsigned id,MercPluginHookEvent *event) {
    if(id>=MERC_HOOK_COUNT||hooks[id].empty()||!event||GetCurrentThreadId()!=owner)return 0;
    event->name=hook_names[id];
    for(auto &h:hooks[id]) {
        if(!h.plugin->active)continue;
        Loaded *previous=calling;calling=h.plugin;
        const size_t depth=guest_call_owners.size();
        try {h.fn(h.userdata,event);}
        catch(...) {h.plugin->active=false;record(h.plugin->manifest.id+": hook "+hook_names[id]+" threw; plugin stopped.");}
        /* A guest call left open by this callback is closed for it. */
        while(guest_call_owners.size()>depth){guest_call_owners.pop_back();recomp_plugin_guest_call_end();}
        calling=previous;
    }
    return event->handled!=0;
}
extern "C" int merc_plugins_initialize(void *base,size_t bytes) {
    if(initialized){failure="Plugin host was initialized twice.";return 0;}
    owner=GetCurrentThreadId();guest=base;guest_size=std::min<size_t>(bytes,0x4000000u);
    sequence=0;failure.clear();
    try {
        auto path=environment(L"MERCENARIES_PLUGIN_LIST");
        if(path.empty()){initialized=true;return 1;}
        auto settings=environment(L"MERCENARIES_PLUGIN_SETTINGS");
        if(settings.empty()||!fs::path(settings).is_absolute())throw std::runtime_error("Missing absolute plugin settings path.");
        fs::path list_path(path);
        if(!list_path.is_absolute()||fs::file_size(list_path)>65536)throw std::runtime_error("Invalid plugin selection file.");
        fs::create_directories(settings);
        log_file.open(fs::path(settings)/L"plugins.log",std::ios::trunc);
        if(!log_file)throw std::runtime_error("Cannot open plugin-settings/plugins.log.");
        std::ifstream input(list_path,std::ios::binary);std::string line;
        std::getline(input,line);
        if(trim(line)!="MERCENARIES_PLUGINS_1")throw std::runtime_error("Unsupported plugin selection file.");
        std::vector<Manifest> manifests;std::set<std::string> seen;
        while(std::getline(input,line)) {
            line=trim(line);
            if(!identifier(line)||!seen.insert(line).second||seen.size()>128)throw std::runtime_error("Invalid or duplicate plugin selection.");
            auto m=read_manifest(list_path.parent_path()/wide(line));
            if(!m||m->id!=line)throw std::runtime_error("Staged plugin manifest does not match "+line);
            manifests.push_back(*m);
        }
        validate(manifests);
        if(!base||guest_size<=0x10000)throw std::runtime_error("Guest RAM is not ready for plugins.");
        for(auto &m:manifests) {
            auto p=std::make_unique<Loaded>();p->manifest=m;
            p->directory=utf8(m.root.wstring());p->settings=utf8((fs::path(settings)/wide(m.id)).wstring());
            fs::create_directories(fs::path(wide(p->settings)));
            p->host={sizeof(MercPluginHost),m.api_version,p.get(),
                p->manifest.id.c_str(),p->directory.c_str(),p->settings.c_str(),plugin_log,read_guest,write_guest,
                register_hook,guest_call_begin,guest_call,guest_call_end};
            p->callbacks.struct_size=sizeof(MercPlugin);p->callbacks.api_version=m.api_version;
            auto library=m.root/wide(m.library);
            // Own directory for dependencies; no working-directory or PATH search.
            p->module=LoadLibraryExW(library.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!p->module)throw std::runtime_error(m.name+": could not load "+m.library+" (Windows error "+std::to_string(GetLastError())+"). Check x64 architecture and dependency DLLs.");
            auto entry=reinterpret_cast<MercPluginEntry>(GetProcAddress(p->module,"MercenariesPlugin_Init"));
            loaded.push_back(std::move(p));
            auto &plugin=*loaded.back();
            if(!entry)throw std::runtime_error(m.name+": DLL does not export MercenariesPlugin_Init.");
            char error[1024]={};plugin.entered=true;calling=&plugin;
            int ok=0;
            try {ok=entry(&plugin.host,&plugin.callbacks,error,sizeof(error));}
            catch(...) {calling=nullptr;throw std::runtime_error(m.name+": initialization threw an exception.");}
            calling=nullptr;error[sizeof(error)-1]=0;
            if(plugin.callbacks.struct_size!=sizeof(MercPlugin)||plugin.callbacks.api_version!=m.api_version) {
                plugin.entered=false;
                throw std::runtime_error(m.name+": invalid plugin callback ABI.");
            }
            if(!ok)throw std::runtime_error(m.name+": initialization failed. "+error);
            plugin.active=true;record(m.name+" ["+m.id+"]: initialized (interface "+std::to_string(m.api_version)+")");
        }
        previous=GetTickCount64();initialized=true;return 1;
    }catch(const std::exception &e) {
        calling=nullptr;failure=e.what();record(failure);shutdown_all();return 0;
    }
}
extern "C" int merc_plugins_update(void) {
    if(!initialized||GetCurrentThreadId()!=owner||dispatching||loaded.empty())return 1;
    dispatching=true;
    ULONGLONG now=GetTickCount64();
    MercPluginUpdate update{sizeof(MercPluginUpdate),MERCENARIES_PLUGIN_API_VERSION_1,++sequence,double(now-previous)/1000.0};
    previous=now;std::set<std::string> stopped;failure.clear();
    for(auto &entry:loaded) {
        auto &p=*entry;
        if(!p.active)continue;
        bool dependency_failed=false;
        for(const auto &id:p.manifest.requires)if(stopped.count(id))dependency_failed=true;
        if(dependency_failed) {
            p.active=false;stopped.insert(p.manifest.id);record(p.manifest.name+": updates stopped because a required plugin failed.");continue;
        }
        if(!p.callbacks.on_update)continue;
        calling=&p;int ok=0;update.api_version=p.manifest.api_version;
        try {ok=p.callbacks.on_update(p.callbacks.userdata,&update);}catch(...) {ok=0;}
        while(!guest_call_owners.empty()&&guest_call_owners.back()==&p){guest_call_owners.pop_back();recomp_plugin_guest_call_end();}
        calling=nullptr;
        if(!ok) {
            p.active=false;stopped.insert(p.manifest.id);
            if(!failure.empty())failure+="\n";
            failure+=p.manifest.name+": update failed; this plugin and its dependents will receive no further updates. Restart the game after correcting the mod.";
            record(failure);
        }
    }
    dispatching=false;return failure.empty()?1:0;
}
extern "C" void merc_plugins_shutdown(void) {
    if(GetCurrentThreadId()!=owner||dispatching)return;
    dispatching=true;shutdown_all();
}


extern "C" void merc_plugins_show_error(void) {
    int n=MultiByteToWideChar(CP_UTF8,0,failure.c_str(),-1,nullptr,0);
    std::wstring text(n ? n : 1,0);
    if(n)MultiByteToWideChar(CP_UTF8,0,failure.c_str(),-1,text.data(),n);
    MessageBoxW(nullptr,text.c_str(),L"Plugin error",MB_OK|MB_ICONERROR);
}

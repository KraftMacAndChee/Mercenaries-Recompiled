#ifndef MERCENARIES_PLUGIN_MANIFEST_H
#define MERCENARIES_PLUGIN_MANIFEST_H
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <optional>
#include <stdexcept>
#include <algorithm>
#include "../sdk/mercenaries_plugin.h"
namespace mercplugins {
namespace fs=std::filesystem;
inline std::string trim(std::string s) {
    auto a=s.find_first_not_of(" \t\r\n");
    return a==s.npos ? "" : s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
inline std::string lower(std::string s) {
    for(auto &c:s) if(c>='A' && c<='Z') c+='a'-'A';
    return s;
}
inline std::wstring wide(const std::string &s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);
    if(!n) throw std::runtime_error("Text must be valid UTF-8.");
    std::wstring r(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),r.data(),n);return r;
}
inline std::string utf8(const std::wstring &s) {
    if(s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    if(!n) throw std::runtime_error("Invalid Unicode path.");
    std::string r(n,0);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr);return r;
}
inline bool identifier(const std::string &s) {
    if(s.empty() || s.size()>64) return false;
    for(char c:s) if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return false;
    static const std::set<std::string> reserved={"con","prn","aux","nul","com1","com2","com3","com4","com5","com6","com7","com8","com9","lpt1","lpt2","lpt3","lpt4","lpt5","lpt6","lpt7","lpt8","lpt9"};
    return !reserved.count(s);
}
inline std::vector<std::string> list(const std::string &s) {
    std::vector<std::string> r;if(s.empty()) return r;size_t p=0;
    do {auto end=s.find(',',p);auto id=trim(s.substr(p,end==s.npos?s.npos:end-p));
        if(!identifier(id)||std::find(r.begin(),r.end(),id)!=r.end()) throw std::runtime_error("Lists require unique lowercase IDs separated by commas.");
        r.push_back(id);if(end==s.npos)break;p=end+1;
    }while(true);return r;
}
inline bool relative_file(const std::string &s) {
    if(s.empty()||s.size()>160||s.find_first_of(":*?\"<>|\r\n")!=s.npos)return false;
    auto p=fs::path(wide(s));if(p.is_absolute()||p.has_root_path())return false;
    for(const auto &part:p) {
        auto x=utf8(part.wstring());
        if(x.empty()||x=="."||x==".."||x.back()=='.'||x.back()==' ')return false;
        if(!identifier(lower(x.substr(0,x.find('.')))))return false;
    }return true;
}
struct Manifest {
    std::string id,name,library;
    uint32_t api_version=MERCENARIES_PLUGIN_API_VERSION_1;
    std::vector<std::string> requires,conflicts,exclusive;
    fs::path root;
};
inline std::optional<Manifest> read_manifest(const fs::path &root) {
    auto path=root/L"mod.ini";if(!fs::exists(path))return {};
    if(!fs::is_regular_file(path)||fs::file_size(path)>65536)throw std::runtime_error("mod.ini must be a regular file under 64 KiB.");
    std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("Cannot read mod.ini.");
    std::string text((std::istreambuf_iterator<char>(f)),{});
    if(text.compare(0,3,"\xef\xbb\xbf")==0)text.erase(0,3);
    wide(text);if(text.find('\0')!=text.npos)throw std::runtime_error("mod.ini must be UTF-8 without NUL bytes.");
    std::map<std::string,std::string> fields;bool inside=false,found=false;size_t p=0;
    while(p<text.size()) {
        auto end=text.find('\n',p);auto line=trim(text.substr(p,end==text.npos?text.npos:end-p));p=end==text.npos?text.size():end+1;
        if(line.empty()||line[0]==';'||line[0]=='#')continue;
        if(line[0]=='[') {
            if(line.back()!=']')throw std::runtime_error("Malformed INI section.");
            inside=lower(trim(line.substr(1,line.size()-2)))=="plugin";
            if(inside&&found)throw std::runtime_error("Duplicate [Plugin] section.");
            found=found||inside;continue;
        }
        if(!inside)continue;
        auto eq=line.find('=');if(eq==line.npos)throw std::runtime_error("Expected key=value in [Plugin].");
        auto key=lower(trim(line.substr(0,eq))),value=trim(line.substr(eq+1));
        static const std::set<std::string> allowed={"id","name","library","apiversion","requires","conflicts","exclusive"};
        if(!allowed.count(key))throw std::runtime_error("Unknown [Plugin] key: "+key);
        if(!fields.emplace(key,value).second)throw std::runtime_error("Duplicate [Plugin] key: "+key);
    }
    if(!found)return {};
    Manifest m;m.root=fs::absolute(root);m.id=fields["id"];m.library=fields["library"];
    if(!identifier(m.id))throw std::runtime_error("Plugin Id must be 1-64 lowercase letters, digits, underscores or hyphens (no Windows device names).");
    m.name=fields["name"].empty()?m.id:fields["name"];
    if(fields["apiversion"]=="1")m.api_version=MERCENARIES_PLUGIN_API_VERSION_1;
    else if(fields["apiversion"]=="2")m.api_version=MERCENARIES_PLUGIN_API_VERSION_2;
    else throw std::runtime_error(m.name+": requires unsupported Plugin Interface "+fields["apiversion"]+"; this build provides 1 and 2.");
    if(!relative_file(m.library)||lower(fs::path(wide(m.library)).extension().string())!=".dll")throw std::runtime_error(m.name+": Library must be a relative .dll path without traversal.");
    auto library=m.root/fs::path(wide(m.library));
    if(!fs::is_regular_file(library))throw std::runtime_error(m.name+": missing DLL "+m.library);
    for(auto p=library;p!=m.root.parent_path();p=p.parent_path()) {
        auto a=GetFileAttributesW(p.c_str());
        if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error(m.name+": plugin paths must be ordinary files and directories.");
    }
    m.requires=list(fields["requires"]);m.conflicts=list(fields["conflicts"]);m.exclusive=list(fields["exclusive"]);
    return m;
}
inline std::vector<std::string> problems(const std::vector<Manifest> &mods) {
    std::vector<std::string> result(mods.size());std::map<std::string,size_t> ids,claims;
    auto add=[&](size_t i,const std::string &s){if(!result[i].empty())result[i]+="\n";result[i]+=s;};
    for(size_t i=0;i<mods.size();++i) {
        auto [it,ok]=ids.emplace(mods[i].id,i);
        if(!ok){add(i,"Duplicate plugin ID: "+mods[i].id);add(it->second,"Duplicate plugin ID: "+mods[i].id);}
    }
    for(size_t i=0;i<mods.size();++i) {
        const auto &m=mods[i];
        for(const auto &id:m.requires) {
            auto it=ids.find(id);
            if(it==ids.end())add(i,"Requires enabled plugin: "+id);
            else if(it->second>=i)add(i,"Move required plugin "+id+" before "+m.id+" in load order.");
        }
        for(const auto &id:m.conflicts) {
            auto it=ids.find(id);if(it!=ids.end()){add(i,"Declared conflict with "+id);if(it->second!=i)add(it->second,"Declared conflict with "+m.id);}
        }
        for(const auto &id:m.exclusive) {
            auto [it,ok]=claims.emplace(id,i);
            if(!ok){add(i,"Exclusive resource "+id+" also claimed by "+mods[it->second].id);add(it->second,"Exclusive resource "+id+" also claimed by "+m.id);}
        }
    }return result;
}
inline void validate(const std::vector<Manifest> &mods) {
    auto errors=problems(mods);std::string all;
    for(size_t i=0;i<errors.size();++i)if(!errors[i].empty())all+=mods[i].name+": "+errors[i]+"\n";
    if(!all.empty())throw std::runtime_error(all);
}
}
#endif


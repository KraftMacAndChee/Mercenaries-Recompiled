"""Test the real Windows guest filesystem with and without a mod overlay."""
from pathlib import Path
import os, shutil, struct, subprocess, tempfile
from test_mod_loader_native import ROOT, CASE, msvc

HARNESS = r'''#include "kernel.h"
#include "mod_overlay.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
volatile uint32_t g_recomp_trace_dump_requested, g_recomp_current_func;
void xbox_log(int level,const char *subsystem,const char *fmt,...){}
void xbox_preview_log_event(const char *category,const char *fmt,...){}
void xbox_preview_log_sample(const char *category,const char *fmt,...){}
int xbox_preview_log_enabled(void){return 0;}
int main(int argc,char **argv){
 assert(argc==4);int mod=!strcmp(argv[3],"mod");
 if(!strcmp(argv[3],"bad")){assert(!xbox_mod_overlay_init());return 0;}
 assert(xbox_mod_overlay_init());xbox_path_init(argv[1],argv[2]);
 WCHAR path[MAX_PATH];
 assert(xbox_translate_path("D:\\DATAxbox\\added.bin",path,MAX_PATH));
 HANDLE h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);
 if(mod){assert(h!=INVALID_HANDLE_VALUE);char b[10];DWORD n;assert(ReadFile(h,b,10,&n,0)&&n==9&&!memcmp(b,"mod-added",9));CloseHandle(h);assert(xbox_mod_overlay_readonly(path));}
 else assert(h==INVALID_HANDLE_VALUE);
 assert(xbox_translate_path("D:\\DATAxbox\\base.bin",path,MAX_PATH));
 h=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,0,0);assert(h!=INVALID_HANDLE_VALUE);
 char data[16];DWORD n;assert(ReadFile(h,data,16,&n,0)&&n==(mod?8:7));assert(!memcmp(data,mod?"replaced":"vanilla",n));CloseHandle(h);
 if(mod){
  assert(xbox_mod_overlay_path(L"..\\escape",path,MAX_PATH)&&!*path);
  assert(xbox_mod_overlay_path(L"DATAxbox\\base.bin",path,2)&&!*path);
  assert(xbox_translate_path("D:\\DATAxbox",path,MAX_PATH));
  h=CreateFileW(path,FILE_LIST_DIRECTORY,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,0);assert(h!=INVALID_HANDLE_VALUE);
  WIN32_FIND_DATAW entry={0};wcscpy_s(entry.cFileName,MAX_PATH,L"added.bin");xbox_mod_overlay_find_data(h,&entry);
  assert(entry.nFileSizeLow==9 && (entry.dwFileAttributes&FILE_ATTRIBUTE_READONLY));CloseHandle(h);
  const ULONG masks[]={GENERIC_WRITE,GENERIC_ALL,DELETE,FILE_WRITE_ATTRIBUTES,FILE_WRITE_EA,XBOX_FILE_APPEND_DATA};
  XBOX_ANSI_STRING name={0};name.Buffer="D:\\DATAxbox\\base.bin";name.Length=(USHORT)strlen(name.Buffer);
  XBOX_OBJECT_ATTRIBUTES attr={0};attr.ObjectName=&name;XBOX_IO_STATUS_BLOCK io;
  for(unsigned i=0;i<sizeof(masks)/sizeof(*masks);++i)assert(xbox_NtCreateFile(&h,masks[i],&attr,&io,0,0,FILE_SHARE_READ,XBOX_FILE_OPEN,0)==STATUS_ACCESS_DENIED);
  assert(xbox_NtCreateFile(&h,GENERIC_READ,&attr,&io,0,0,FILE_SHARE_READ,XBOX_FILE_OPEN,0)==STATUS_SUCCESS);CloseHandle(h);
  assert(xbox_NtCreateFile(&h,GENERIC_READ,&attr,&io,0,0,FILE_SHARE_READ,XBOX_FILE_OVERWRITE_IF,0)==STATUS_ACCESS_DENIED);
 }
 assert(xbox_translate_path("Z:\\assets.dsk",path,MAX_PATH));assert((wcsstr(path,L"profile\\storage")!=NULL)==mod);
 assert(xbox_translate_path("\\Device\\Harddisk0\\Partition5\\assets.dsk",path,MAX_PATH));assert((wcsstr(path,L"profile\\storage")!=NULL)==mod);
 assert(xbox_translate_path("\\Device\\Harddisk0\\Partition0",path,MAX_PATH));assert((wcsstr(path,L"profile\\storage")!=NULL)==mod);
 assert(xbox_translate_path("U:\\savegame",path,MAX_PATH));assert(wcsstr(path,L"player-saves")&&!wcsstr(path,L"profile"));
 puts("PASS: guest reads, additions, enumeration metadata, write denial, cache partitions and saves isolation");return 0;
}
'''
def main():
 CASE.mkdir(parents=True,exist_ok=True);source=CASE/'overlay-test.c';source.write_text(HARNESS)
 env=msvc();exe=CASE/'overlay-test.exe'
 sources=[source,ROOT/'src/kernel/mod_overlay.c',ROOT/'src/kernel/kernel_path.c',ROOT/'src/kernel/kernel_file.c']
 cmd=[shutil.which('cl',path=env['PATH']),'/nologo','/std:c11','/O2','/Gy','/D_CRT_SECURE_NO_WARNINGS','/I'+str(ROOT/'src/kernel'),'/I'+str(ROOT/'src'),*map(str,sources),'/Fe'+str(exe),'/link','/OPT:REF','shell32.lib']
 subprocess.run(cmd,cwd=CASE,env=env,check=True)
 with tempfile.TemporaryDirectory(dir=CASE,prefix='overlay-fixture-') as tmp:
  root=Path(tmp);game=root/'game';save=root/'player-saves';profile=root/'profile';namespace=profile/'namespace/DATAxbox';namespace.mkdir(parents=True);(game/'DATAxbox').mkdir(parents=True)
  (game/'DATAxbox/base.bin').write_bytes(b'vanilla');(root/'replacement.bin').write_bytes(b'replaced');(root/'added.bin').write_bytes(b'mod-added')
  pairs=[('DATAxbox\\base.bin',root/'replacement.bin'),('DATAxbox\\added.bin',root/'added.bin')]
  blob=struct.pack('<II',0x31444f4d,len(pairs))
  for name,target in pairs:
   key=name.encode('utf-16le');value=str(target).encode('utf-16le');blob+=struct.pack('<II',len(key)//2,len(value)//2)+key+value
   (namespace/Path(name).name).touch()
  index=profile/'overlay.bin';index.write_bytes(blob)
  child=os.environ.copy();child['MERCENARIES_MOD_OVERLAY']=str(index);child['MERCENARIES_MOD_CACHE']=str(profile/'storage')
  subprocess.run([str(exe),str(game),str(save),'mod'],env=child,check=True)
  child.pop('MERCENARIES_MOD_OVERLAY');child.pop('MERCENARIES_MOD_CACHE')
  subprocess.run([str(exe),str(game),str(save),'vanilla'],env=child,check=True)
  index.write_bytes(blob[:-2]);child['MERCENARIES_MOD_OVERLAY']=str(index)
  subprocess.run([str(exe),str(game),str(save),'bad'],env=child,check=True)
if __name__=='__main__':main()



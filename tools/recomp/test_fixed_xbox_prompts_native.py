"""Exercise the saved prompt override through production Recomp Options."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=r'''
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char runtime[MAX_PATH];
static DWORD module_path(HMODULE m,LPSTR out,DWORD size){(void)m;snprintf(out,size,"%s",runtime);return (DWORD)strlen(out);}
#define GetModuleFileNameA module_path
#define _TRUNCATE ((size_t)-1)
#define _snprintf_s(out,size,count,...) snprintf(out,size,__VA_ARGS__)
#define strcpy_s(out,size,in) snprintf(out,size,"%s",in)
#define strcat_s(out,size,in) snprintf(out+strlen(out),size-strlen(out),"%s",in)
#include "recomp_options.c"
static unsigned notified;
static void changed(uint32_t bits){notified|=bits;}
int main(int argc,char **argv){
 assert(argc==2);snprintf(runtime,sizeof(runtime),"%s\\runtime.exe",argv[1]);
 recomp_options_init();recomp_options_set_apply_callback(changed);
 assert(!recomp_options_fixed_xbox_prompts());
 recomp_options_begin_edit();assert(recomp_options_adjust(RECOMP_OPTIONS_FIXED_XBOX_HASH,1));
 assert(!strcmp(recomp_options_label(RECOMP_OPTIONS_FIXED_XBOX_HASH),"FIXED XBOX PROMPTS: ON"));
 assert(!recomp_options_fixed_xbox_prompts());
 recomp_options_cancel_edit();assert(!changed_options());
 recomp_options_adjust(RECOMP_OPTIONS_FIXED_XBOX_HASH,-1);
 assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_FIXED_XBOX);
 assert(notified==RECOMP_OPTIONS_CHANGE_FIXED_XBOX && recomp_options_fixed_xbox_prompts());
 assert(GetPrivateProfileIntA("RecompOptions","FixedXboxPrompts",-1,g_options.path)==1);
 memset(&g_options,0,sizeof(g_options));assert(recomp_options_fixed_xbox_prompts());
 assert(recomp_options_apply()==0);
 recomp_options_adjust(RECOMP_OPTIONS_FIXED_XBOX_HASH,1);recomp_options_apply();
 memset(&g_options,0,sizeof(g_options));assert(!recomp_options_fixed_xbox_prompts());
 assert(recomp_options_localization_hash(RECOMP_OPTIONS_FIXED_XBOX_HASH)==0x941EE5E8u);
 puts("fixed Xbox prompts: default, edit/cancel, apply callback and restart persistence pass");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-fixed-prompts-') as folder:
 d=Path(folder);(d/'test.c').write_text(source);exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O1','-I'+str(ROOT/'ports/mercenaries/src'),'-I'+str(ROOT/'src/input'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'src'),str(d/'test.c'),'-o',str(exe),'-lm'],check=True)
 subprocess.run([str(exe),str(d)],check=True)

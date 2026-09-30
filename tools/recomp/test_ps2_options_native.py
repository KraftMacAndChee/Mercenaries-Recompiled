"""Persist/apply/cancel the PS2 option and parse independent developer logging."""
from pathlib import Path
import subprocess,tempfile,os
os.environ["PATH"]="C:/msys64/mingw64/bin;"+os.environ["PATH"]
ROOT=Path(__file__).resolve().parents[2]
text=(ROOT/'ports/mercenaries/src/dev_menu.c').read_text(encoding='utf-8')
settings=text[text.index('static int developer_setting'):text.index('#define C_BG')]
source=r'''#include <assert.h>
#include "recomp_options.c"
'''+settings+r'''
int main(void) {
 assert(!recomp_options_ps2_upgrades());
 assert(!recomp_developer_logging_enabled() && !recomp_dev_menu_allowed());
 char path[MAX_PATH];GetModuleFileNameA(NULL,path,MAX_PATH);strcpy(strrchr(path,'\\')+1,"developer.ini");
 WritePrivateProfileStringA("Developer","logging","1",path);
 assert(recomp_developer_logging_enabled() && !recomp_dev_menu_allowed());
 WritePrivateProfileStringA("Developer","logging","2",path);assert(!recomp_developer_logging_enabled());
 recomp_options_begin_edit();assert(recomp_options_adjust(RECOMP_OPTIONS_PS2_UPGRADES_HASH,1));
 assert(!recomp_options_ps2_upgrades());assert(strstr(recomp_options_label(RECOMP_OPTIONS_PS2_UPGRADES_HASH),"ON"));
 recomp_options_cancel_edit();assert(strstr(recomp_options_label(RECOMP_OPTIONS_PS2_UPGRADES_HASH),"OFF"));
 recomp_options_adjust(RECOMP_OPTIONS_PS2_UPGRADES_HASH,1);
 assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_PS2_UPGRADES);assert(recomp_options_ps2_upgrades());
 memset(&g_options,0,sizeof(g_options));assert(recomp_options_ps2_upgrades());
 recomp_options_begin_edit();recomp_options_adjust(RECOMP_OPTIONS_PS2_UPGRADES_HASH,1);recomp_options_apply();
 memset(&g_options,0,sizeof(g_options));assert(!recomp_options_ps2_upgrades());
 puts("PASS: option defaults/apply/cancel/persistence and independent opt-in logging");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-ps2-options-') as folder:
 d=Path(folder);(d/'test.c').write_text(source);exe=d/'test.exe'
 subprocess.run(['C:/msys64/mingw64/bin/gcc.exe','-std=c11','-O2','-I'+str(ROOT/'ports/mercenaries/src'),'-I'+str(ROOT/'src/input'),'-I'+str(ROOT/'src'),'-I'+str(ROOT/'include'),str(d/'test.c'),'-o',str(exe),'-luser32','-lm'],check=True)
 subprocess.run([str(exe)],cwd=d,check=True)

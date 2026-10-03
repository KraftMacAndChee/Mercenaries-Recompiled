"""A staged runtime reads/writes the installation's settings, not its own copy."""
from pathlib import Path
import subprocess,shutil,tempfile
from test_mod_loader_native import ROOT,CASE,msvc

def main():
 menu=(ROOT/'ports/mercenaries/src/dev_menu.c').read_text(encoding='utf-8')
 settings=menu[menu.index('static int developer_setting'):menu.index('#define C_BG')]
 source=r"""
#include <assert.h>
#include "recomp_options.c"
"""+settings+r"""
int main(int argc,char **argv){
 assert(argc==2);SetEnvironmentVariableA("MERCENARIES_CONFIG_ROOT",argv[1]);
 char path[MAX_PATH];build_config_path(path);assert(strstr(path,argv[1])==path);
 WritePrivateProfileStringA("RecompOptions","FPSCap","90",path);
 WritePrivateProfileStringA("Keyboard.5","Action4","77",path);
 char developer[MAX_PATH];snprintf(developer,sizeof(developer),"%s\\developer.ini",argv[1]);
 WritePrivateProfileStringA("Developer","logging","1",developer);
 assert(recomp_developer_logging_enabled());
 recomp_options_init();assert(recomp_options_fps_cap()==90);
 recomp_options_begin_edit();recomp_options_adjust(RECOMP_OPTIONS_VSYNC_HASH,1);recomp_options_apply();
 assert(GetPrivateProfileIntA("RecompOptions","VSync",0,path)==1);
 assert(GetPrivateProfileIntA("Keyboard.5","Action4",0,path)==77);
 SetEnvironmentVariableA("MERCENARIES_CONFIG_ROOT",NULL);build_config_path(path);assert(strstr(path,argv[1])==NULL);
 puts("PASS: staged runtime settings resolve to installation, FPS loads, VSync persists, keyboard bind survives, no-overlay fallback retained");
}
"""
 CASE.mkdir(parents=True,exist_ok=True);c=CASE/'config-test.c';c.write_text(source)
 exe=CASE/'config-test.exe';env=msvc()
 subprocess.run([shutil.which('cl',path=env['PATH']),'/nologo','/std:c11','/O2','/D_CRT_SECURE_NO_WARNINGS','/I'+str(ROOT/'ports/mercenaries/src'),'/I'+str(ROOT/'src'),'/I'+str(ROOT/'src/input'),'/I'+str(ROOT/'include'),str(c),'/Fo'+str(CASE/'config-test.obj'),'/Fe'+str(exe),'/link','user32.lib'],env=env,check=True)
 with tempfile.TemporaryDirectory(dir=CASE,prefix='settings-fixture-') as tmp:subprocess.run([str(exe),tmp],check=True)
if __name__=='__main__':main()

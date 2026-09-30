"""Verify the real executable resources provide separate full-size/caption icons."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
SOURCE=r'''#include <assert.h>
#include <stdio.h>
#include "app_icon.h"
int main(int argc,char **argv){
 assert(argc==2); HINSTANCE module=LoadLibraryExA(argv[1],NULL,LOAD_LIBRARY_AS_DATAFILE);
 assert(module); HICON big=mercenaries_app_icon(module,1),caption=mercenaries_app_icon(module,0);
 assert(big && caption && big!=caption);
 for(int i=0;i<2;i++){
  ICONINFO info={0}; BITMAP bitmap={0};
  assert(GetIconInfo(i?big:caption,&info));
  assert(GetObject(info.hbmColor,sizeof(bitmap),&bitmap));
  assert(bitmap.bmWidth==(i?256:64));
  assert(bitmap.bmHeight==(i?256:64));
  assert(mercenaries_app_icon(module,i)==(i?big:caption));
  DeleteObject(info.hbmColor);DeleteObject(info.hbmMask);
 }
 puts("PASS: 256px large icon, independent 64px preview/caption icon, cached handles");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="merc-icon-") as td:
 p=Path(td);(p/"test.c").write_text(SOURCE)
 subprocess.run(["C:/MinGW/bin/gcc.exe","-I"+str(ROOT/"ports/mercenaries/src"),str(p/"test.c"),"-o",str(p/"test.exe"),"-lgdi32"],check=True)
 for name in ["mercenaries_recomp.exe","Mercenaries Recompiled.exe"]:
  subprocess.run([str(p/"test.exe"),str(ROOT/"build/mercenaries/bin/Release"/name)],check=True)

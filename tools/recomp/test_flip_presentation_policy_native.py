"""Native execution of presentation rollback/Wine policy, without a GPU."""
from pathlib import Path
import subprocess, tempfile, shutil
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
static int disabled,forced,wine,queries;
static const char *d3d8_cached_getenv(const char *s){if(!strcmp(s,"MERCENARIES_DISABLE_FLIP_PRESENT"))return disabled?"1":NULL;assert(!strcmp(s,"MERCENARIES_TEST_FLIP_PRESENT"));return forced?"1":NULL;}
static void *GetModuleHandleA(const char *s){assert(!strcmp(s,"ntdll.dll"));return (void*)1;}
static void *GetProcAddress(void *m,const char *s){assert(m==(void*)1&&!strcmp(s,"wine_get_version"));++queries;return wine?(void*)2:NULL;}
'''
POST=r'''
int main(void){
 for(wine=0;wine<2;wine++)for(forced=0;forced<2;forced++)for(disabled=0;disabled<2;disabled++){
  queries=0;assert(d3d8_flip_presentation_requested()==(!disabled&&(!wine||forced)));
  assert(queries==(!disabled&&!forced));
 }
 puts("PASS: Windows default, Wine compatibility, explicit experiment, rollback precedence");return 0;
}
'''
def main():
 s=(ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8');a=s.index('static BOOL d3d8_flip_presentation_requested(');b=s.index('static HRESULT d3d11_try_device_and_swap_chain(',a)
 with tempfile.TemporaryDirectory(prefix='flip-policy-') as td:
  p=Path(td)/'test.c';exe=p.with_suffix('.exe');p.write_text(PRE+s[a:b]+POST)
  cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
  subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()

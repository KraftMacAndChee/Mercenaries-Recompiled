"""Execute private spawn request parsing; normal builds never poll the file."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/dev_spawn.h').read_text()
func=s[s.index('static int dev_test_take_spawn'):s.index('void recomp_dev_spawn_tick')]
prefix=r'''#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef unsigned long long ULONGLONG;
typedef struct {const char *template_name;} DevVehicle;
static DevVehicle catalog[]={{"small"},{"large"}};
static const char *file_path;static int enabled, events, opens;static ULONGLONG now=1000;
static const char *environment(const char *key){return !strcmp(key,"MERCENARIES_TEST_GAMEPAD_FILE")?(enabled?"private-pad":NULL):file_path;}
static ULONGLONG GetTickCount64(void){return now;}
static unsigned recomp_dev_vehicle_count(void){return 2;}
static const DevVehicle *recomp_dev_vehicle_at(unsigned i){return &catalog[i];}
static unsigned recomp_dev_troop_count(void){return 0;}
static const DevVehicle *recomp_dev_troop_at(unsigned i){return NULL;}
static unsigned policy;
static unsigned recomp_dev_battle_flags(void){return policy;}
static void recomp_dev_battle_set(unsigned v){policy=v;}
#define DEV_SPAWN_TROOP 0x400u
static void xbox_preview_log_event(const char *a,const char *b,...){if(strcmp(a,"dev-test-enabled"))events++;}
static FILE *open_counted(const char *p,const char *m){opens++;return fopen(p,m);}
#define getenv environment
#define fopen open_counted
'''
harness=r'''
#undef fopen
static void request(const char *s){FILE *f=fopen(file_path,"wb");assert(f);fputs(s,f);fclose(f);now+=100;}
int main(int argc,char **argv){assert(argc==3);file_path=argv[1];enabled=atoi(argv[2]);unsigned i=99;
 request("1 large\n");int got=dev_test_take_spawn(&i);
 if(!enabled){assert(!got&&!opens&&!events);return 0;}
 assert(got&&i==1&&opens==1&&events==1);assert(!dev_test_take_spawn(&i)&&opens==1);
 now+=100;assert(!dev_test_take_spawn(&i));
 request("2 missing\n");assert(!dev_test_take_spawn(&i)&&events==2);
 request("3 small\n");assert(dev_test_take_spawn(&i)&&i==0&&events==3);
 const char *bad[]={"3 large\n","0 small\n","-1 small\n","4 small extra\n","4 small\n5 large\n","nonsense","4",""};
 for(unsigned n=0;n<sizeof(bad)/sizeof(bad[0]);n++){request(bad[n]);assert(!dev_test_take_spawn(&i));}
 assert(events==3);request("4 large\n");assert(dev_test_take_spawn(&i)&&i==1&&events==4);
 request("5 inspect\n");assert(dev_test_take_spawn(&i)&&i==0x7fffffffu);now+=100;assert(!dev_test_take_spawn(&i));
 puts("PASS: private opt-in, throttle, duplicate and malformed requests, catalogue lookup");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='merc-dev-request-') as temp:
 d=Path(temp);(d/'test.c').write_text(prefix+func+harness);exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(d/'test.c'),'-o',str(exe)],check=True)
 for enabled in (0,1):subprocess.run([str(exe),str(d/'request.txt'),str(enabled)],check=True)

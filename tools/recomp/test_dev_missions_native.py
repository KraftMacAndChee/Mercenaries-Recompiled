"""Exercise bounded developer commands, Lua failure cleanup and guest CPU preservation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class DeveloperMissions(unittest.TestCase):
    def test_policy_and_guest_bridge(self):
        source = r"""
#include <windows.h>
#define GetTickCount64() ((ULONGLONG)GetTickCount())
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dev_missions.c"
#define MERCENARIES_FREE_CAM_H
static unsigned char memory[0x700000];
static uintptr_t g_xbox_mem_offset=1;
static uint32_t g_esp=0x200000,g_ecx=0x1234;
static int allowed=1,fail_load,fail_call,missing,success,reports,restores,freecam=1,loads,calls;
typedef struct {uint32_t esp,ecx;} recomp_saved_guest_cpu_context;
int recomp_dev_menu_allowed(void){return allowed;}
static void *guest_ptr(uint32_t a){assert(a<sizeof(memory));return memory+a;}
static uint32_t guest_u32(uint32_t a){return *(uint32_t*)guest_ptr(a);}
static void put(uint32_t a,uint32_t v){*(uint32_t*)guest_ptr(a)=v;}
static int dev_guest_address(uint32_t a,uint32_t n){return a>=0x10000 && a<=sizeof(memory)-n;}
static int recomp_lookup(uint32_t a){return !missing;}
static void recomp_save_guest_cpu_context(recomp_saved_guest_cpu_context *s){s->esp=g_esp;s->ecx=g_ecx;}
static void recomp_restore_guest_cpu_context(recomp_saved_guest_cpu_context *s){g_esp=s->esp;g_ecx=s->ecx;restores++;}
static void recomp_freecam_set(int value){freecam=value;}
static void xbox_preview_log_event(const char*a,const char*b,...){ }
static void recomp_lua_pcall_checkpoint(uint32_t vm,uint32_t err,uint32_t name){assert(vm==0x10000 && err==1);reports++;}
void recomp_dev_transition_result(int ok,const char*message){success=ok;assert(message && *message);}
static uint32_t dev_call(uint32_t stack,uint32_t fn,uint32_t object,unsigned n,const uint32_t *args){
 assert(args[0]==0x10000 && stack<0x200000-0x1000);g_esp=stack;g_ecx=0x9999;
 if(fn==0x1dc970){assert(n==1);return 3;}
 if(fn==0x1dc980){assert(n==2 && args[1]==3);return 0;}
 if(fn==0x1dc480){assert(n==4);assert(strlen(guest_ptr(args[1]))==args[2]);loads++;return fail_load;}
 assert(fn==0x1dd5a0 && n==4 && args[1]==0 && args[2]==0 && args[3]==0);
 calls++;if(!fail_call)memory[0x3717d6]=1;return fail_call;
}
#include "dev_mission_guest.h"
static void ready(void){put(0x403970,0x4a5220af);put(0x413f6c,0x4249d707);put(0x413f68,0xc2cbd863);put(0x3717bc,0x10000);memory[0x3717d6]=0;recomp_dev_missions_tick();}
int main(void){
 char script[768];ready();assert(recomp_dev_current_province()==0);
 for(unsigned province=0;province<2;province++)for(unsigned f=0;f<4;f++){
  recomp_dev_missions_set_province(province);
  unsigned n=recomp_dev_mission_count(province,f);assert(n==(f==0 && province==1?5:6));
  for(unsigned row=0;row<n;row++){
   unsigned number=recomp_dev_mission_number(province,f,row);
   assert(recomp_dev_request_mission(f,number));assert(!recomp_dev_request_mission(f,number));
   unsigned command=recomp_dev_missions_take();assert(command==((province+1)<<8|f<<4|number));
   assert(recomp_dev_mission_script(command,script,sizeof(script)));
   assert(strstr(script,"DebugSkipToMission") && strstr(script,recomp_dev_mission_faction(f)));
   assert(!recomp_dev_mission_script(command,script,4));
  }
 }
 assert(!recomp_dev_request_mission(0,7) && !recomp_dev_request_mission(4,1));
 assert(!recomp_dev_mission_script(0x300,script,sizeof(script)));
 assert(!recomp_dev_mission_script(0x280,script,sizeof(script)));
 assert(!recomp_dev_mission_script(0x402,script,sizeof(script)));
 allowed=0;assert(!recomp_dev_request_mission(2,2));allowed=1;
 ready();assert(!recomp_dev_request_travel(0));assert(recomp_dev_request_travel(1));
 unsigned c=recomp_dev_missions_take();assert(c==0x401);
 assert(recomp_dev_mission_script(c,script,sizeof(script)) && strstr(script,"TeleportBetweenQuadrants()") && strstr(script,"mission_accepted"));
 assert(recomp_dev_request_mission(2,2));recomp_dev_missions_tick();
 assert(success && !freecam && restores==1 && calls==1 && loads==1 && g_esp==0x200000 && g_ecx==0x1234);
 ready();fail_load=1;assert(recomp_dev_request_mission(2,2));recomp_dev_missions_tick();
 assert(!success && calls==1 && reports==1 && restores==2);fail_load=0;
 ready();fail_call=1;assert(recomp_dev_request_mission(2,2));recomp_dev_missions_tick();
 assert(!success && reports==2 && restores==3);fail_call=0;
 ready();assert(recomp_dev_request_mission(2,2));put(0x413f68,0);recomp_dev_missions_tick();assert(!success && restores==3);
 ready();assert(recomp_dev_request_mission(2,2));memory[0x3717d6]=1;recomp_dev_missions_tick();assert(!success && restores==3);
 ready();missing=1;assert(recomp_dev_request_mission(2,2));recomp_dev_missions_tick();assert(!success && restores==3);
 assert(g_esp==0x200000 && g_ecx==0x1234);
 puts("PASS: mission catalogue, gate, queue, scripts, guards, Lua failures and CPU restoration");
}
"""
        with tempfile.TemporaryDirectory(prefix="merc-dev-missions-") as directory:
            folder=Path(directory);c=folder/"test.c";exe=folder/"test.exe"
            c.write_text(source,encoding="utf-8")
            subprocess.run([shutil.which("gcc") or "C:/MinGW/bin/gcc.exe","-std=c11","-O1",
                            "-I"+str(ROOT/"ports/mercenaries/src"),str(c),"-o",str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__ == "__main__": unittest.main()

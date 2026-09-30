"""Exercise the production two-observer loading adapter and retail hysteresis."""
from pathlib import Path
import shutil,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
HEADER=r"""
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
#define MERCENARIES_FREE_CAM_H
static unsigned char mem[0x4000000];
static uintptr_t g_xbox_mem_offset=1;
static uint32_t g_esp=0x200000,g_ecx=777;
typedef struct {uint32_t esp,ecx;} recomp_saved_guest_cpu_context;
static int enabled,calls,maintenance;
static float remote[3]={1000,50,0};
static int dev_guest_address(uint32_t a,unsigned n){return a>=0x10000u&&a<=sizeof(mem)-n;}
static void *guest_ptr(uint32_t a){assert(a<sizeof(mem));return mem+a;}
static void recomp_save_guest_cpu_context(recomp_saved_guest_cpu_context*s){s->esp=g_esp;s->ecx=g_ecx;}
static void recomp_restore_guest_cpu_context(recomp_saved_guest_cpu_context*s){g_esp=s->esp;g_ecx=s->ecx;}
static int recomp_lookup(uint32_t a){assert(a==0x1ee610);return 1;}
static int recomp_freecam_focus(float p[3],float d[3]){if(!enabled)return 0;memcpy(p,remote,12);d[0]=0;d[1]=0;d[2]=-1;return 1;}
static uint32_t dev_call(uint32_t,uint32_t,uint32_t,unsigned,const uint32_t*);
#include "freecam_streaming.h"
static void spore(float x,uint16_t flags,int16_t distance){
 *(float*)(mem+0x30008)=x;*(float*)(mem+0x30010)=0;
 *(uint16_t*)(mem+0x30030)=flags;
 *(int16_t*)(mem+0x3002c)=distance;*(int16_t*)(mem+0x3002e)=distance;
}
static uint32_t dev_call(uint32_t stack,uint32_t fn,uint32_t world,unsigned count,const uint32_t*a){
 assert(fn==0x1ee610&&world==0x4031b0&&count==3&&a[0]==0x3c888889);
 assert(stack<0x200000-0x1000);++calls;
 const float *p=guest_ptr(a[1]);assert(p[0]==(calls==1?0:1000));
 assert(recomp_freecam_world_secondary()==(calls==2));
 if(!recomp_freecam_world_secondary())++maintenance;
 // Recursion guard leaves the original generated pass to execute normally.
 assert(!recomp_freecam_world_update(world,a[0],a[1],a[2]));
 float secondary=calls==1?1000:0;
 spore(secondary,8,100);assert(recomp_freecam_stream_keep(0x30000,0));
 spore(500,8,100);assert(!recomp_freecam_stream_keep(0x30000,0));
 // Exact authored boundary: dormant wake <=100, preloading <100;
 // already-awake/unloading use the larger hysteresis radius.
 spore(secondary+100,8,100);assert(recomp_freecam_stream_keep(0x30000,0));
 spore(secondary+100.01f,8,100);assert(!recomp_freecam_stream_keep(0x30000,0));
 spore(secondary+102,0,100);assert(recomp_freecam_stream_keep(0x30000,0));
 spore(secondary+103,0,100);assert(!recomp_freecam_stream_keep(0x30000,0));
 spore(secondary+100,0,100);assert(!recomp_freecam_stream_keep(0x30000,1));
 spore(secondary+99.99f,0,100);assert(recomp_freecam_stream_keep(0x30000,1));
 spore(secondary+103,0x100,100);assert(!recomp_freecam_stream_keep(0x30000,1));
 spore(secondary+102,0x100,100);assert(recomp_freecam_stream_keep(0x30000,1));
 assert(!recomp_freecam_stream_keep(0,0));
 g_esp=stack;g_ecx=999;return 0;
}
int main(void){
 *(float*)(mem+0x2dc08c)=1.f;*(float*)(mem+0x2e4060)=1.05f;
 uint32_t p=0x40000,d=0x40010;
 assert(!recomp_freecam_world_update(0x4031b0,0x3c888889,p,d));assert(calls==0&&g_esp==0x200000);
 enabled=1;
 assert(!recomp_freecam_world_update(0x4031b0,0x3c888889,0,d));
 assert(recomp_freecam_world_update(0x4031b0,0x3c888889,p,d));
 assert(calls==2&&maintenance==1&&g_esp==0x200010&&g_ecx==777);
 assert(!recomp_freecam_world_secondary());assert(!recomp_freecam_stream_keep(0x30000,0));
 enabled=0;assert(!recomp_freecam_world_update(0x4031b0,0x3c888889,p,d));assert(calls==2);
 puts("PASS: two loading observers, recursion/ABI, single maintenance, retail hysteresis, invalid inputs and disabled pass-through");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-freecam-stream-') as d:
 c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(HEADER)
 subprocess.run([shutil.which('gcc'),'-O2','-std=c11','-I',str(ROOT/'ports/mercenaries/src'),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)

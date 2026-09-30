"""Ensure event crash evidence is bounded, survives bad pointers and is read-only."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
pre=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
static unsigned char memory[0x4000000], before[0x4000000];
static uint32_t g_esi,g_edi,g_esp,g_recomp_current_func;
static unsigned reports,contexts;
static char retained[12288];
static uint32_t guest_u32(uint32_t address){assert(address>=0x10000 && address<=sizeof(memory)-4);uint32_t value;memcpy(&value,memory+address,4);return value;}
static void put(uint32_t address,uint32_t value){memcpy(memory+address,&value,4);}
static void xbox_preview_log_set_crash_context(const char *s){++contexts;snprintf(retained,sizeof(retained),"%s",s);}
static void xbox_preview_log_event(const char *tag,const char *format,...){assert(!strcmp(tag,"EVENT-LIST-INVALID"));++reports;}
'''
body='#include "'+(ROOT/'ports/mercenaries/src/recomp_event_history.inc').as_posix()+'"\n'
harness=r'''
int main(void){
 put(0x20000,0x30000);put(0x20008,0x40000);put(0x40004,0x50000);put(0x40014,3);put(0x400A8,77);
 memcpy(memory+0x40038,"mission_callback",17);
 g_esi=0x20000;g_edi=123;g_esp=0x60000;
 memcpy(before,memory,sizeof(memory));
 for(unsigned i=0;i<1000;++i)recomp_event_record_return(0x20000,0x60000,123);
 assert(!reports && !contexts && g_event_history_available==32);
 assert(!memcmp(before,memory,sizeof(memory)));
 put(0x20000,0xffff0000);memcpy(before,memory,sizeof(memory));
 recomp_event_record_return(0x20000,0x60000,123);
 assert(reports==1 && contexts==1 && strstr(retained,"next=FFFF0000") && strstr(retained,"mission_callback"));
 assert(g_esi==0x20000 && g_edi==123 && g_esp==0x60000);
 assert(!memcmp(before,memory,sizeof(memory)));
 /* Damaged node/object pointers are recorded without dereferencing them. */
 g_esi=0xffffffff;
 for(unsigned i=0;i<100;++i)recomp_event_record_return(0xffffffff,0x60000,123);
 assert(reports==1 && contexts==1 && g_event_history_available==32);
 assert(!memcmp(before,memory,sizeof(memory)));
 puts("PASS: event history stays bounded, preserves guest state and captures invalid links without dereferencing them");
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(pre+body+harness)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)

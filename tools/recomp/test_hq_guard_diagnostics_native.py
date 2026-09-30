"""Read-only, bounded diagnostics for the guard's registered audio wait."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
body=s[s.index('static int recomp_guard_audio_range('):s.index('/* Retail RedMovie::Update contains')]
prelude=r"""
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static unsigned char memory[0x4000000], before[0x4000000];
static int g_xbox_mem_offset=1, enabled=1;
static uint64_t ticks;
static unsigned records, voice_depth;
static char output[8192];
static struct {uint32_t handle,vm,defining_depth;} guard_wait={99,0x8cad50};
static uint32_t managed=0x180044;
static uint8_t guest_u8(uint32_t a){CHECK(a<sizeof(memory));return memory[a];}
static uint16_t guest_u16(uint32_t a){uint16_t v;CHECK(a<=sizeof(memory)-2);memcpy(&v,memory+a,2);return v;}
static uint32_t guest_u32(uint32_t a){uint32_t v;CHECK(a<=sizeof(memory)-4);memcpy(&v,memory+a,4);return v;}
static float guest_f32(uint32_t a){float v;CHECK(a<=sizeof(memory)-4);memcpy(&v,memory+a,4);return v;}
static void put(uint32_t a,uint32_t v){memcpy(memory+a,&v,4);}
static uint32_t recomp_guard_managed_cue(uint32_t handle){CHECK(handle==guard_wait.handle);return managed;}
static int xbox_preview_log_enabled(void){return enabled;}
static uint64_t GetTickCount64(void){return ticks;}
static void xbox_preview_log_event(const char *category,const char *format,...){
 records++;size_t n=strlen(output);snprintf(output+n,sizeof(output)-n,"[%s] ",category);n=strlen(output);
 va_list v;va_start(v,format);vsnprintf(output+n,sizeof(output)-n,format,v);va_end(v);
 strcat(output,"\n");
}
"""
harness=r"""
int main(void){
 const uint32_t pool=0x85c600+0x7568, w=pool+0x84, cue=0x190000, sound=0x191000, track=0x192000, stream=0x193000;
 put(managed+8,71);put(managed+0x38,2);put(0x323acc,15);memory[pool+4]=1;put(w+4,71);put(w,1);put(w+8,cue);
 put(cue+0x28,sound);put(sound+0x44,cue);put(sound+0x34,track);put(sound+0x10,0x194000);memory[0x194008]=1;
 put(sound+0x30,5);put(track+0x40,stream);put(stream+0x18,0xaaaa);put(stream+0x1c,0x103);put(stream+0x44,1234567890);
 memcpy(before,memory,sizeof(memory));ticks=100;recomp_guard_preview_snapshot();CHECK(records==1);
 CHECK(strstr(output,"packet1=")&&strstr(output,"lua_depth=0")&&strstr(output,"state=5")&&strstr(output,"/00000103/"));CHECK(strstr(output,"1234567890\n"));CHECK(!memcmp(before,memory,sizeof(memory)));
 recomp_guard_preview_snapshot();CHECK(records==1);ticks=5099;recomp_guard_preview_snapshot();CHECK(records==1);
 ticks=5100;output[0]=0;recomp_guard_preview_snapshot();CHECK(records==2);
 enabled=0;ticks+=5000;recomp_guard_preview_snapshot();CHECK(records==2);enabled=1;
 guard_wait.handle=100;put(w+8,0xfffffff0);output[0]=0;recomp_guard_preview_snapshot();CHECK(records==3);
 ticks+=5000;managed=UINT32_MAX;recomp_guard_preview_snapshot();CHECK(records==4);
 ticks+=5000;managed=0;recomp_guard_preview_snapshot();CHECK(records==5);
 ticks+=5000;guard_wait.handle=0;recomp_guard_preview_snapshot();CHECK(records==5);
 CHECK(!recomp_guard_audio_range(0xffffffff,64));CHECK(!recomp_guard_audio_range(0,64));
 CHECK(recomp_guard_audio_range(0x3ffffb0,0x50));CHECK(!recomp_guard_audio_range(0x3ffffb1,0x50));
 puts("PASS guard diagnostics: live stream, read-only memory, five-second cadence, logging off, malformed pointers and retired cues");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-guard-diag-') as td:
 p=Path(td);(p/'test.c').write_text(prelude+body+harness)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)

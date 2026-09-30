"""Exercise bounded, read-only music snapshots against valid and corrupt guest state."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
def function(name):
 start=s.index(name+'\n{');return s[start:s.index('\n}\n',start)+3]
body=function('int recomp_xact_low_level_handle_exists(uint32_t handle)')+function('static void recomp_audio_source_preview_snapshot(void)')+function('void recomp_music_preview_snapshot(void)')+function('void recomp_music_checkpoint(uint32_t stage, uint32_t state, uint32_t value)')
fixture=r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
typedef uint64_t ULONGLONG;
static unsigned char memory[0x4000000];
static uintptr_t g_xbox_mem_offset=1;
static unsigned enabled=1,reads,events,audio_events;
static ULONGLONG now=1000;
static char line[1024],category[32];
static ULONGLONG GetTickCount64(void){return now;}
static int xbox_preview_log_enabled(void){return enabled;}
static uint32_t guest_u32(uint32_t a){assert(a>=0x10000 && a<=sizeof(memory)-4);++reads;uint32_t v;memcpy(&v,memory+a,4);return v;}
static uint16_t guest_u16(uint32_t a){assert(a>=0x10000 && a<=sizeof(memory)-2);++reads;uint16_t v;memcpy(&v,memory+a,2);return v;}
static uint8_t guest_u8(uint32_t a){assert(a>=0x10000 && a<sizeof(memory));++reads;return memory[a];}
static float guest_f32(uint32_t a){uint32_t v=guest_u32(a);float f;memcpy(&f,&v,4);return f;}
static void put(uint32_t a,uint32_t v){memcpy(memory+a,&v,4);}
static void log_event(const char*c,const char*f,...){if(!strcmp(c,"audio-sources")){++audio_events;return;}strcpy(category,c);va_list a;va_start(a,f);vsnprintf(line,sizeof(line),f,a);va_end(a);++events;}
#define xbox_preview_log_event log_event
#define xbox_preview_log_sample log_event
static char *no_env(const char*s){return NULL;}
#define getenv no_env
"""+body+r"""
int main(void){
 const uint32_t dj=0x37B9B4,pool=0x10000,cue=0x20000,low=0x85C600+0x7568;
 enabled=0;recomp_music_preview_snapshot();assert(!events && !audio_events && !reads);
 enabled=1;g_xbox_mem_offset=0;recomp_music_preview_snapshot();assert(!events && !reads);g_xbox_mem_offset=1;
 recomp_music_preview_snapshot();assert(events==1 && audio_events==1 && strstr(line,"cue=00000000") && strstr(line,"managed_count=4294967295"));
 unsigned n=reads;now+=4999;recomp_music_preview_snapshot();assert(events==1 && audio_events==1 && reads==n);now++;
 put(dj,123);put(dj+4,0x7270c346);put(dj+8,0x5da8b911);memory[dj+0x2c]=1;memory[dj+0x2d]=1;
 put(0x690ac0,pool);put(pool,64);put(pool+0x1444+63*8,123);put(pool+0x1448+63*8,cue);
 put(cue+4,123);put(cue+8,456);put(cue+0x38,2);memory[cue+0x3c]=memory[cue+0x3d]=1;
 put(low,1);put(low+0x1c84,456);
 unsigned char *copy=malloc(sizeof(memory));assert(copy);memcpy(copy,memory,sizeof(memory));
 recomp_music_preview_snapshot();assert(events==2 && strstr(line,"track=7270C346") && strstr(line,"mute=1 duck=1") && strstr(line,"cue=00020000 cue_state=2") && strstr(line,"low_level=1 prepared=1 called=1"));assert(!memcmp(copy,memory,sizeof(memory)));
 now+=5000;put(low+0x1c84,789);recomp_music_preview_snapshot();assert(strstr(line,"low_level=0"));
 now+=5000;put(pool,65);recomp_music_preview_snapshot();assert(strstr(line,"cue=00000000") && strstr(line,"managed_count=65"));
 now+=5000;put(pool,64);put(cue+4,321);recomp_music_preview_snapshot();assert(strstr(line,"cue=00000000"));
 now+=5000;put(pool+0x1448+63*8,0xfffffff0);recomp_music_preview_snapshot();assert(strstr(line,"cue=00000000"));
 now+=5000;put(0x690ac0,0x4000000-0x1643);recomp_music_preview_snapshot();assert(strstr(line,"managed_count=4294967295"));
 n=events;recomp_music_checkpoint(1,dj,0);assert(events==n);recomp_music_checkpoint(10,dj,0xc69a677b);assert(events==n+1 && !strcmp(category,"music-request") && strstr(line,"phase=begin requested=C69A677B"));
 recomp_music_checkpoint(11,dj,0xc69a677b);assert(events==n+2 && strstr(line,"phase=end"));
 recomp_music_checkpoint(10,0xfffffff0,0);assert(events==n+2);enabled=0;recomp_music_checkpoint(11,dj,0);assert(events==n+2);
 free(copy);puts("PASS: disabled fast path, five-second limit, 64-slot lookup, cue/sound presence, corrupt pointers/counts, read-only state and rare requests");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-music-preview-') as temp:
 p=Path(temp);(p/'test.c').write_text(fixture);exe=p/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

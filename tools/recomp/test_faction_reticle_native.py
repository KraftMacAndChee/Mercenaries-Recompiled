"""Exercise retail HUD color selection across changing personal/global attitudes.
Mocks actor lookup only; runs the actual retail color and faction query routines.
"""
from pathlib import Path
import re,sys,subprocess,tempfile,struct
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
names=['sub_00095B70','sub_00095EE0','sub_000F76F0']
bodies={}
for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'):
 if p.name=='recomp_dispatch.c':continue
 s=p.read_text(encoding='utf8')
 for n in names:
  m=re.search(r'void '+n+r'\(void\)\n\{.*?\n\}',s,re.S)
  if m:bodies[n]=m[0]
assert len(bodies)==len(names)
xbe=ROOT/'game_files/mercenaries-retail/default.xbe';raw=xbe.read_bytes();config.configure_from_xbe(str(xbe))
code='\n'.join(bodies[n] for n in names)
addresses=set(int(v,16) for v in re.findall(r'MEM(?:32|F)\(0x([0-9A-Fa-f]+)\)',code))|set(range(0x2E6C7C,0x2E6C7C+20,4))
addresses.update(int(v,16) for v in re.findall(r"recomp_xmm_loadss\([^,]+, 0x([0-9A-Fa-f]+)\)",code))
init=[]
for a in addresses:
 off=config.va_to_file_offset(a);assert off is not None
 init.append(f'MEM32(0x{a:X})=0x{int.from_bytes(raw[off:off+4],"little"):X}u;')
prelude='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
'''

stubs=r'''
typedef unsigned long long ULONGLONG;
#define __declspec(x)
static ULONGLONG clock_ms;
static char observed[1536];
static unsigned log_count;
static int enabled=1;
static int xbox_preview_log_enabled(void){return enabled;}
static ULONGLONG GetTickCount64(void){return clock_ms;}
static uint32_t guest_u32(uint32_t p){assert(p+4<=sizeof(memory));return MEM32(p);}
static float guest_f32(uint32_t p){assert(p+4<=sizeof(memory));return MEMF(p);}
#define xbox_preview_log_sample xbox_preview_log_event
static void xbox_preview_log_event(const char *category,const char *fmt,...){assert(!strcmp(category,"faction-target"));va_list ap;va_start(ap,fmt);vsnprintf(observed,sizeof(observed),fmt,ap);va_end(ap);log_count++;}
static unsigned personal,faction,has_ai;
enum{STACK=0x3e0000,TARGET=0x10000,ACTOR=0x20000,VT=0x21000,PLAYER_AI=0x22000,PLAYER_VT=0x23000,PLAYER_VEHICLE=0x24000,AI=0x25000};
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(address, saved) do{unsigned fn=(address);assert(esp==(saved)-4);if(fn==0x10101){assert(ecx==ACTOR);eax=has_ai?AI:0;}else{assert(fn==0x20202&&ecx==PLAYER_AI);eax=PLAYER_VEHICLE;}esp+=4;}while(0)
void sub_0008C7E0(void){assert(MEM32(esp+4)==0x660e4490);eax=PLAYER_AI;esp+=4;}
void sub_00067E10(void){assert(ecx==AI&&MEM32(esp+4)==PLAYER_VEHICLE);eax=personal;esp+=8;}
void sub_00013870(void){assert(ecx==ACTOR);eax=faction;esp+=4;}
'''
stubs += '\n#include \"'+(ROOT/'ports/mercenaries/src/faction_preview.h').as_posix()+'\"\n'

harness=r'''
static unsigned color(unsigned target){clock_ms+=500;esp=STACK;ecx=0x26000;ebx=0x12345678;esi=0x23456789;edi=0x3456789a;g_seh_ebp=0x456789ab;MEM32(esp+4)=target;sub_000F76F0();assert(esp==STACK+8&&ebx==0x12345678&&esi==0x23456789&&edi==0x3456789a&&g_seh_ebp==0x456789ab&&g_fp_top==0);return eax;}
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
'''+''.join(init)+r'''
MEM32(0x30e5a0)=0x80abcdef;MEM32(0x30e5a4)=0x80123456;MEM32(ACTOR)=VT;MEM32(VT+0xbc)=0x10101;MEM32(PLAYER_AI)=PLAYER_VT;MEM32(PLAYER_VT+0x2c)=0x20202;
const float standing[]={-.9f,-.6f,-.4f,0.f,.2f,.7f,1.f};unsigned cases=0;
for(unsigned repeat=0;repeat<8;repeat++)for(unsigned owner=0;owner<2;owner++)for(has_ai=0;has_ai<2;has_ai++)for(personal=0;personal<4;personal++)for(faction=0;faction<8;faction++)for(unsigned i=0;i<7;i++){
 MEM32(TARGET+0x5c)=owner?ACTOR:0;unsigned f=owner?faction:0;MEMF(0x323354+(f*8+1)*4)=standing[i];
 unsigned hostile=(owner&&has_ai&&personal<2)||(standing[i]<=-.6f);unsigned expected=hostile?0x80123456:(f?0x80c02020:0x80abcdef);
 assert(color(TARGET)==expected);
 char wanted[80];snprintf(wanted,sizeof(wanted),"selected_color=%08X",expected);assert(strstr(observed,wanted));
 snprintf(wanted,sizeof(wanted),"personal=%u ",owner&&has_ai?personal:0xFFFFFFFFu);assert(strstr(observed,wanted));
 assert(color(0)==0x80abcdef);cases++;
}
/* Observer must be inert when disabled and bounded during repeated queries. */
unsigned before=log_count;enabled=0;for(unsigned i=0;i<100;i++)color(TARGET);assert(log_count==before);enabled=1;
recomp_preview_faction_query(0,TARGET,0,0);recomp_preview_faction_query(1,TARGET,AI,PLAYER_VEHICLE);recomp_preview_faction_query(2,TARGET,0,0);
clock_ms+=500;recomp_preview_faction_query(3,TARGET,1,0);before=log_count;
for(unsigned i=0;i<100000;i++)recomp_preview_faction_query(3,TARGET,1,0);assert(log_count==before);
clock_ms+=499;recomp_preview_faction_query(3,TARGET,1,0);assert(log_count==before);
clock_ms++;recomp_preview_faction_query(3,TARGET,1,0);assert(log_count==before+1);
recomp_preview_faction_query(0,0xFFFFFFFE,0,0);clock_ms+=500;recomp_preview_faction_query(3,0xFFFFFFFE,1,0);assert(log_count==before+1);
printf("PASS: %u changing HUD target/personal/global combinations plus null targets, no stale color; preserved guest ABI; actual observer colors, disabled path, bounds and 100000-call throttle\n",cases);return 0;}
'''
with tempfile.TemporaryDirectory(prefix='reticle-native-') as temp:
 d=Path(temp);(d/'fixture.c').write_text(prelude+stubs+code+harness)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing',str(d/'fixture.c'),'-o',str(d/'test.exe')],check=True)
 subprocess.run([str(d/'test.exe')],check=True)

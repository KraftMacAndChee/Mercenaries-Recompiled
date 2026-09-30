"""Execute retail light update/recalculation against the captured stale HQ light.
The repair must retain authored color, fade distance, radius and ABI.
"""
from pathlib import Path
import re,sys,subprocess,tempfile,shutil
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
text=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0007.c').read_text()
def body(name,source=text):
 return re.search(r'(?:void|int) '+name+r'\([^\n]*\)\n\{.*?\n\}',source,re.S)[0]
code=body('sub_00177300')+'\n'+body('sub_00177A90')
helper=body('recomp_static_light_refresh_needed',(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text())
raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes();config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
init=[]
for a in set(int(x,16) for x in re.findall(r'MEM(?:F|32)\(0x([0-9A-Fa-f]+)\)',code)):
 try:off=config.va_to_file_offset(a)
 except Exception:continue
 if off is not None:init.append(f'MEM32(0x{a:X})=0x{int.from_bytes(raw[off:off+4],"little"):X}u;')
fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x4000000];
static unsigned enabled;
static char *setting(const char *key) { assert(!strcmp(key,"MERCENARIES_DISABLE_STATIC_LIGHT_REFRESH"));return enabled?NULL:"1"; }
#define getenv setting
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static uint8_t guest_u8(uint32_t a){return MEM8(a);}
static float guest_f32(uint32_t a){return MEMF(a);}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,b) abort()
'''+'\n'+helper+'\n'
calls=set(re.findall(r'\b(sub_[0-9A-F]{8})\(\)',code))-{'sub_00177300','sub_00177A90'}
for name in calls:
 impl='eax=(int)g_fp_stack[g_fp_top++&7u];esp+=4;' if name=='sub_002375B4' else 'abort();'
 fixture+=f'void {name}(void){{{impl}}}\n'
fixture+=code+r'''
enum { L=0x10000, CAM=0x20000, STACK=0x3e0000 };
static void frame(void) { ecx=L;esp=STACK;esi=0x11223344;ebx=0x33445566;edi=0x778899aa;MEMF(esp+4)=1.f/60;sub_00177A90();assert(esp==STACK+8&&esi==0x11223344&&ebx==0x33445566&&edi==0x778899aa&&ecx==L&&g_fp_top==0); }
int main(int argc,char **argv) {
 enabled=argc>1;g_xbox_mem_offset=(ptrdiff_t)memory;
'''+''.join(init)+r'''
 MEM32(0x6437B4)=CAM;MEMF(0x2F3F14)=100;MEMF(0x2F3F10)=10;
 MEM32(L+0x1A0)=1;MEM32(L+0x1A4)=0x20cbfffd;
 MEM32(L+0x1A8)=MEM32(L+0x240)=0x80000000;
 MEMF(L+0x154)=100;MEMF(L+0x1B0)=10;MEMF(L+0x23C)=7;
 MEMF(L+0x70)=2345.62256f;MEMF(L+0x78)=-.527103f;
 MEMF(CAM+0x40)=2347.78784f;MEMF(CAM+0x48)=5.155235f;
 frame();assert(MEM32(L+0x240)==(enabled?0x20cbfffd:0x80000000));
 assert(MEM32(L+0x1A4)==0x20cbfffd&&MEMF(L+0x23C)==7);
 assert(!recomp_static_light_refresh_needed(L));
 if(enabled) {
  MEM32(L+0x1A8)=MEM32(L+0x240)=0x80000000;
  MEMF(CAM+0x40)=2500;frame();assert(MEM32(L+0x240)==0x80000000);
  MEMF(CAM+0x40)=2347.78784f;MEM32(L+0x1A0)=0;frame();assert(MEM32(L+0x240)==0x80000000);
  MEM32(L+0x1A0)=1;MEM32(L+0x1A4)=0x20000000;frame();assert(MEM32(L+0x240)==0x80000000);
  MEM32(L+0x1A4)=0x20cbfffd;MEM32(L+0x1D0)=1;assert(!recomp_static_light_refresh_needed(L));
  MEM32(L+0x1D0)=0;MEMF(CAM+0x40)=NAN;assert(!recomp_static_light_refresh_needed(L));
  MEMF(CAM+0x40)=2347.78784f;MEMF(L+0x23C)=0;assert(!recomp_static_light_refresh_needed(L));
  MEMF(L+0x23C)=7;MEM32(L+0x1A8)=MEM32(L+0x240)=0x20112233;assert(!recomp_static_light_refresh_needed(L));
 }
 puts("PASS actual retail static-light update/recalculation, enabled/disabled, range/off/black/flicker/NaN guards and preserved frame");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='static-light-') as temp:
 p=Path(temp);(p/'fixture.c').write_text(fixture)
 cc=shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
 subprocess.run([cc,'-O1','-fno-strict-aliasing',str(p/'fixture.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True);subprocess.run([str(p/'test.exe'),'on'],check=True)

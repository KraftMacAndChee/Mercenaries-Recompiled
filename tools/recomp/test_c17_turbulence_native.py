"""Run the lifted turbulence function against its unmodified retail filter.
Checks original-rate equivalence, ABI/phase/RNG preservation and high-FPS response.
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config
from tools.recomp.generated_test_utils import generated_text_containing


def body(name, source):
    return re.search(r'(?:static )?(?:void|float|int32_t) ' + name + r'\([^\n]*\)\n\{.*?\n\}', source, re.S)[0]


def main():
    code = body('sub_0009BF60', generated_text_containing('void sub_0009BF60(void)'))
    assert code.count('recomp_turbulence_damping_dt(MEMF(esp + 0x28))') == 1
    retail = code.replace('sub_0009BF60', 'retail').replace(
        'recomp_turbulence_damping_dt(MEMF(esp + 0x28))', 'MEMF(esp + 0x28)')
    manual = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text()
    helper = body('recomp_turbulence_damping_dt', manual)
    trig = '\n'.join(body(name, manual) for name in (
        'merc_guest_f32', 'merc_pbl_trig_index', 'merc_pbl_sine_sample',
        'merc_fp_return', 'sub_001F9D70', 'sub_001F9DA0'))
    xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
    raw = xbe.read_bytes()
    config.configure_from_xbe(str(xbe))
    initializers = []
    for address in sorted(set(int(x, 16) for x in re.findall(r'(?:MEM(?:F|32)\(|xmm\dv, )0x([0-9A-Fa-f]+)', code))):
        offset = config.va_to_file_offset(address)
        if offset is not None:
            initializers.append(f'MEM32(0x{address:X})=0x{int.from_bytes(raw[offset:offset+4],"little"):X}u;')
    fixture = '#define RECOMP_GENERATED_CODE\n#include "' + (ROOT / 'ports/mercenaries/src/recomp/recomp_types.h').as_posix() + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x4000000];
static unsigned rollback, random_calls, paused, human=1;
static char *setting(const char *key) {
 assert(!strcmp(key,"MERCENARIES_DISABLE_TURBULENCE_DAMPING_FIX"));return rollback?"1":NULL;
}
#define getenv setting
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,b) abort()
void sub_001EC220(void) {eax=0;esp+=4;}
void sub_00178950(void) {eax=paused?0x7084D38D:0;esp+=4;}
void sub_00015A00(void) {eax=human;esp+=8;}
static uint32_t guest_u32(uint32_t address) {return MEM32(address);}
void sub_00012000(void) {random_calls++;g_fp_stack[--g_fp_top&7u]=0.5;esp+=4;}
''' + helper + '\n' + trig + '\n' + retail + '\n' + code + r'''
enum { CAM=0x10000, DIR=0x20000, STACK=0x3e0000 };
static void reset(void) {
 memset(memory+CAM,0,0x200);MEM8(CAM+0x17c)=1;random_calls=0;g_fp_top=0;
}
static void frame(void (*fn)(void),float dt) {
 ecx=CAM;esp=STACK;esi=0x11223344;ebx=0x33445566;edi=0x778899aa;
 MEMF(esp+4)=dt;MEM32(esp+8)=DIR;MEMF(DIR)=.1f;MEMF(DIR+4)=.2f;MEMF(DIR+8)=.3f;
 fn();assert(esp==STACK+12&&esi==0x11223344&&ebx==0x33445566&&edi==0x778899aa&&g_fp_top==0);
 assert(MEMF(STACK+4)==dt);
}
static void compare(float dt) {
 unsigned char expected[0x200],direction[12];unsigned draws;
 reset();for(int i=0;i<20;i++)frame(retail,dt);
 memcpy(expected,memory+CAM,sizeof(expected));memcpy(direction,memory+DIR,12);draws=random_calls;
 reset();for(int i=0;i<20;i++)frame(sub_0009BF60,dt);
 assert(!memcmp(expected,memory+CAM,sizeof(expected))&&!memcmp(direction,memory+DIR,12)&&draws==random_calls);
}
static double response(void (*fn)(void),unsigned fps) {
 reset();double sum=0;for(unsigned i=0;i<fps*60;i++) {
   frame(fn,1.f/fps);double n=MEMF(CAM+0x12c);sum+=n*n;
 }
 assert(random_calls==fps*60*3);
 return sqrt(sum/(fps*60));
}
int main(int argc,char **argv) {
 rollback=argc>1;g_xbox_mem_offset=(ptrdiff_t)memory;
''' + ''.join(initializers) + r'''
 for(unsigned i=0;i<1000;i++)compare(1.f/30.f+i*.000066f);
 if(rollback) {
  compare(1.f/60);compare(1.f/120);compare(1.f/240);
 } else {
  double original=response(retail,30), amplified=response(retail,60);
  double fixed60=response(sub_0009BF60,60), fixed120=response(sub_0009BF60,120);
  printf("noise RMS original30=%.9g original60=%.9g corrected60=%.9g corrected120=%.9g\n",original,amplified,fixed60,fixed120);
  assert(amplified/original>2.1&&amplified/original<2.4);
  assert(fabs(fixed60/original-1)<.03&&fabs(fixed120/original-1)<.03);
  reset();frame(sub_0009BF60,1.f/60);float phase=MEMF(CAM+0x14c);
  reset();frame(retail,1.f/60);assert(phase==MEMF(CAM+0x14c));
 }
 paused=1;reset();frame(sub_0009BF60,1.f/60);assert(!random_calls&&MEMF(CAM+0x14c)==0);
 paused=0;reset();MEM8(CAM+0x17c)=0;frame(sub_0009BF60,1.f/60);assert(!random_calls&&MEMF(CAM+0x14c)==0);
 reset();frame(sub_0009BF60,.11f);assert(!random_calls&&MEMF(CAM+0x14c)==0);
 human=0;reset();frame(sub_0009BF60,1.f/60);assert(!random_calls&&MEMF(CAM+0x128)==0&&MEMF(CAM+0x12c)==0&&MEMF(CAM+0x130)==0);
 assert(isnan(recomp_turbulence_damping_dt(NAN)));
 assert(recomp_turbulence_damping_dt(INFINITY)==INFINITY);
 assert(recomp_turbulence_damping_dt(-1)==-1&&recomp_turbulence_damping_dt(0)==0);
 puts("PASS retail-rate equivalence, phase/RNG/ABI, pause/off/slow/nonhuman guards and rollback");
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='turbulence-native-') as temp:
        p = Path(temp)
        (p / 'fixture.c').write_text(fixture)
        cc = shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
        subprocess.run([cc, '-O1', '-fno-strict-aliasing', str(p / 'fixture.c'), '-o', str(p / 'test.exe')], check=True)
        subprocess.run([str(p / 'test.exe')], check=True)
        subprocess.run([str(p / 'test.exe'), 'rollback'], check=True)


if __name__ == '__main__':
    main()

"""Run the lifted game-entry and frame-clear path; reproduce stale blended color."""
from pathlib import Path
import re, subprocess, tempfile, sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
entry=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0007.c').read_text()
renderer=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0010.c').read_text()
def block(text, start, end):
    return text[text.index(start):text.index(end,text.index(start))]
entry=block(entry,'loc_001848E7: ;','loc_001848ED: ;')
assert 'PUSH32(esp, 1); /* RedRenderer::SetTargetClear(true) */' in entry
setter=re.search(r'void sub_0020FAA0\(void\)\n\{.*?\n\}',renderer,re.S)[0]
frame=block(renderer,'loc_00211A78: ;','loc_00211AAA: ;')
# Verify that the disabled clear is really in the supported XBE.
xbe=ROOT/'game_files/mercenaries-retail/default.xbe'
config.configure_from_xbe(str(xbe));data=xbe.read_bytes()
sec=next(s for s in config._SECTIONS if s.va<=0x1848E7<s.va+s.raw_size)
assert data[sec.raw_addr+0x1848E7-sec.va]==0x57 # push edi (zero)
prelude='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x800000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
static unsigned flags, clears, render_calls;
static float background;
void sub_00210730(void){
 flags=MEM32(esp+4);assert(MEM32(esp+8)==0);
 assert(MEMF(esp+12)==1.f&&MEM32(esp+16)==0);
 if(flags&0xf0) background=0.f;
 clears++;esp+=4;
}
void sub_00209730(void){assert(clears==render_calls+1);render_calls++;esp+=4;}
"""
harness=prelude+setter+'\nstatic void enter_game(void){'+entry+'esp+=4;}\nstatic void frame_begin(void){'+frame+'}\n'+r"""
static void enter(int fixed){esp=0x7f0000;edi=0;
 if(fixed)enter_game();else {PUSH32(esp,0);PUSH32(esp,0);sub_0020FAA0();esp+=4;}
 assert(esp==0x7f0000&&edi==0);
}
static float draw(float stale,int opaque){
 background=stale;esp=0x7f0000;frame_begin();assert(esp==0x7f0000);
 if(opaque)background=.4f;
 /* Transparent water followed by fog, as in the captured ordering. */
 background=.3f*.6f+background*.4f;
 return .5f*.2f+background*.8f;
}
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
 enter(0);float old1=draw(.1f,0),old2=draw(.9f,0);assert(old1!=old2&&flags==3);
 float seabed=draw(.9f,1);
 enter(1);assert(MEM32(0x7AD5E8)==0xf0);
 float fixed1=draw(.1f,0),fixed2=draw(.9f,0);assert(fabsf(fixed1-fixed2)<1e-6f&&flags==0xf3);
 assert(fabsf(draw(.9f,1)-seabed)<1e-6f);
 for(int i=0;i<100;i++){assert(fabsf(draw(i/100.f,0)-fixed1)<1e-6f);assert(fabsf(draw(i/100.f,1)-seabed)<1e-6f);}
 /* Other callers retain normal setter semantics (e.g. menu state changes). */
 enter(0);assert(MEM32(0x7AD5E8)==0);MEM8(0x6437D0)=0x10;
 draw(.9f,0);assert(flags==0xf3);MEM8(0x6437D0)=0;
 enter(1);draw(.5f,0);assert(flags==0xf3);
 puts("PASS: lifted entry/frame clear, original stale-color reproduction, initialized blend independence, opaque seabed unchanged, scene reentry, wireframe, stack ABI");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-water-init-') as tmp:
    d=Path(tmp);(d/'test.c').write_text(harness)
    subprocess.run(['C:/MinGW/bin/gcc.exe','-O1','-fno-strict-aliasing','-I'+str(ROOT/'ports/mercenaries/src'),str(d/'test.c'),'-o',str(d/'test.exe')],check=True)
    subprocess.run([str(d/'test.exe')],check=True)


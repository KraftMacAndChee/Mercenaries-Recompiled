"""Run the production envelope through release, preserving the outgoing tail.

This isolates VP envelope arithmetic. It does not validate XACT register setup,
stream lifetime, or the audible result of a gameplay music transition.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src/apu/apu_vp.c").read_text(encoding="utf-8")
start = source.index("static float voice_step_envelope(")
end = source.index("/* ============================================================", start)
production = source[start:end]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include "apu_regs.h"
#ifndef M_E
#define M_E 2.71828182845904523536
#endif
typedef struct { uint32_t regs[64]; unsigned stopped; } MCPXAPUState;
static unsigned shift(uint32_t mask) { assert(mask); unsigned s=0; while(!(mask&1)){++s;mask>>=1;}return s; }
static uint32_t voice_get_mask(MCPXAPUState *d,uint16_t v,uint32_t reg,uint32_t mask) {
 assert(v==0 && reg/4<64);return (d->regs[reg/4]&mask)>>shift(mask);
}
static void voice_set_mask(MCPXAPUState *d,uint16_t v,uint32_t reg,uint32_t mask,uint32_t value) {
 assert(v==0 && reg/4<64);d->regs[reg/4]=(d->regs[reg/4]&~mask)|((value<<shift(mask))&mask);
}
static float clampf(float x,float lo,float hi){return fminf(hi,fmaxf(lo,x));}
static void voice_off(MCPXAPUState *d,uint16_t v){assert(v==0);++d->stopped;}
'''
harness += production
harness += r'''
static float step(MCPXAPUState *d) {
 return voice_step_envelope(d,0,NV_PAVS_VOICE_CFG_ENV0,NV_PAVS_VOICE_CFG_ENVA,
 NV_PAVS_VOICE_TAR_LFO_ENV,NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
 NV_PAVS_VOICE_PAR_OFFSET,NV_PAVS_VOICE_PAR_OFFSET_EALVL,
 NV_PAVS_VOICE_CUR_ECNT_EACOUNT,NV_PAVS_VOICE_PAR_STATE_EACUR);
}
int main(void) {
 const unsigned rates[]={0,1,94,281,844,4095};
 const unsigned levels[]={0,32,128,255};
 for(unsigned r=0;r<sizeof(rates)/sizeof(*rates);++r)for(unsigned l=0;l<4;++l){
  MCPXAPUState d={0};unsigned rate=rates[r],level=levels[l];
  voice_set_mask(&d,0,NV_PAVS_VOICE_TAR_LFO_ENV,NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,rate);
  voice_set_mask(&d,0,NV_PAVS_VOICE_PAR_OFFSET,NV_PAVS_VOICE_PAR_OFFSET_EALVL,level);
  voice_set_mask(&d,0,NV_PAVS_VOICE_CUR_ECNT,NV_PAVS_VOICE_CUR_ECNT_EACOUNT,rate*16);
  voice_set_mask(&d,0,NV_PAVS_VOICE_PAR_STATE,NV_PAVS_VOICE_PAR_STATE_EACUR,NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);
  float previous=1;
  for(unsigned i=0;i<rate*16;++i){
   float gain=step(&d);
   assert(isfinite(gain) && gain>=0 && gain<=previous+1e-6f);
   if(level)assert(gain>0);
   if(!i)assert(fabsf(gain-level/255.f)<1e-6f);
   assert(!d.stopped);
   previous=gain;
  }
  assert(step(&d)==0 && !d.stopped);
  assert(step(&d)==0 && d.stopped==1);
 }
 puts("PASS: 24 release rate/level cases retain a monotonic tail until envelope completion");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="merc-envelope-") as folder:
    path=Path(folder)
    (path/"test.c").write_text(harness,encoding="utf-8")
    exe=path/"test.exe"
    subprocess.run(["C:/MinGW/bin/gcc.exe","-std=c11","-O2","-I"+str(ROOT/"src/apu"),str(path/"test.c"),"-o",str(exe),"-lm"],check=True)
    subprocess.run([str(exe)],check=True)

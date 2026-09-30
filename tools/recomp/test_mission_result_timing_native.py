"""Execute retail result-screen slowdown phases; not UI/load/save validation."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config


class MissionResultTimingTests(unittest.TestCase):
    def test_no_pause_clock_reaches_frontend_phase_under_variable_frames(self):
        xbe=ROOT/'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe)); raw=xbe.read_bytes()
        source=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0007.c').read_text()
        full=re.search(r'void sub_00183DE0\(void\)\n\{.*?\n\}',source,re.S)[0]
        # Keep the unmodified production prologue, phase5/10 blocks and common
        # epilogue. Other switch destinations are traps, never simulated UI.
        timer=full[:full.index('loc_00183FA8: ;')]+'''
loc_00183FA8: ;
loc_001841CD: ;
loc_00184266: ;
    CHECK(0); /* This test stops before frontend initialization. */
'''+full[full.index('loc_0018429C: ;'):]
        initial=[]
        for va,size in ((0x1842B4,24),(0x1842CC,46),(0x6921A0,4),
                        (0x2DC3C4,4),(0x2F49D4,4),(0x2DC340,4),
                        (0x2DD414,4),(0x2E165C,4)):
            offset=config.va_to_file_offset(va)
            # BSS timebase scale is initialized at runtime, not stored in XBE.
            if va==0x6921A0:
                initial.append('MEMF(0x6921A0)=1.0f/3000.0f;');continue
            self.assertIsNotNone(offset)
            payload=raw[offset:offset+size]
            self.assertEqual(len(payload),size)
            initial.extend(f'MEM8(0x{va+i:X})={v};' for i,v in enumerate(payload))
        harness=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(2);}}while(0)
static uint8_t mem[0x700000];
static uint32_t eax,ecx,edx,esi,esp,g_seh_ebp;
static float xmm0v[4],xmm1v[4],xmm2v[4];
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define xmm2 xmm2v[0]
#define MEM8(a) mem[(uint32_t)(a)]
#define MEM32(a) (*(uint32_t*)(mem+(uint32_t)(a)))
#define MEMF(a) (*(float*)(mem+(uint32_t)(a)))
#define ZX8(a) ((uint8_t)(a))
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_A(a,b) ((uint32_t)(a)>(uint32_t)(b))
#define HI8(a) (((a)>>8)&255u)
#define SET_HI8(a,b) ((a)=((a)&0xFFFF00FFu)|((uint32_t)(uint8_t)(b)<<8))
#define PUSH32(s,v) do{uint32_t temp=(v);(s)-=4;MEM32(s)=temp;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define RECOMP_ITAIL(a) CHECK(0)
#define recomp_xmm_loadss(v,a) do{(v)[0]=MEMF(a);(v)[1]=(v)[2]=(v)[3]=0;}while(0)
#define recomp_xmm_copy(a,b) memcpy(a,b,sizeof(a))
#define recomp_xmm_zero(a) memset(a,0,sizeof(a))
static int EVEN_PARITY8(uint8_t a){a^=a>>4;a^=a>>2;a^=a>>1;return !(a&1);}
static float rate,blur;
static void sub_001592B0(void){CHECK(MEM32(esp+4)==1);esp+=4;}
static void sub_00159260(void){CHECK(MEM32(esp+4)==1);esp+=4;}
static void sub_001FFBB0(void){CHECK(ecx==0x4140CC);rate=MEMF(esp+4);esp+=8;}
static void sub_001F3290(void){blur=MEMF(esp+4);esp+=4;}
'''+timer+r'''
enum{STATE=0x1000,CLOCK=0x2000,STACK=0x3000};
static void step(unsigned ticks){
 MEM32(CLOCK)+=ticks;ecx=STATE;esp=STACK;esi=0x12345678;
 MEM32(esp)=0xBADCAFE;MEMF(esp+4)=0; /* paused game time must not stall UI timer */
 sub_00183DE0();
 CHECK(esp==STACK+8&&esi==0x12345678&&MEM32(STACK)==0xBADCAFE);
 CHECK(rate>=.1f&&rate<=1&&blur>=.1f&&blur<=.5f);
}
int main(void){
'''+''.join(initial)+r'''
 unsigned cases=0;
 for(unsigned wrap=0;wrap<2;wrap++)for(unsigned mode=0;mode<6;mode++){
  memset(mem+STATE,0,0x100);MEM32(STATE+0x10)=CLOCK;MEM32(STATE+0x24)=5;
  MEM32(CLOCK)=wrap?0xFFFFF000u:123456;MEM32(STATE+0xC)=MEM32(CLOCK);
  rate=1;blur=.1f;
  for(unsigned i=0;i<20;i++){step(0);CHECK(MEM32(STATE+0x24)==5);}
  const unsigned deltas[]={50,100,234,300,750,1500};
  unsigned total=0,steps=0;
  while(MEM32(STATE+0x24)!=30&&steps<2000){
   unsigned dt=mode==5?deltas[steps%6]:deltas[mode];
   total+=dt;step(dt);++steps;
   CHECK(MEM32(STATE+0x24)==5||MEM32(STATE+0x24)==10||MEM32(STATE+0x24)==30);
  }
  CHECK(MEM32(STATE+0x24)==30&&rate==.1f);
  CHECK(total>=10000&&total<=18000); /* authored slowdown is several seconds */
  CHECK(fabsf(MEMF(STATE+0x18)-(float)total/3000)<.003f);
  CHECK(fabsf(MEMF(STATE+0x1C)-(float)total/3000)<.003f);
  ++cases;
 }
 printf("%u retail result-timer sequences reach phase30, including clock wrap and paused game time\n",cases);
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-result-timer-') as directory:
            p=Path(directory); c=p/'test.c'; exe=p/'test.exe'
            c.write_text(harness)
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe',
                '-O2','-msse2','-mfpmath=sse','-fno-strict-aliasing','-std=c11',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__=='__main__':unittest.main()

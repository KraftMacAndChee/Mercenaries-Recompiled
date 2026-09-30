"""Keep authored walk/run hysteresis intact while fixing movement defects.

Executes the real translated selection block. It does not prove collision,
animation presentation, input delivery or wall-clock movement speed.
"""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from generated_test_utils import generated_text_containing


class RunSelectionTests(unittest.TestCase):
    def test_authored_thresholds_and_speed_selection(self):
        xbe=ROOT/'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe)); raw=xbe.read_bytes()
        constants={0x2DC394:.6,0x2DC3E0:.7,0x2E0B9C:.95,0x2DC090:2,0x2DC09C:.01}
        for address,value in constants.items():
            offset=config.va_to_file_offset(address)
            self.assertEqual(raw[offset:offset+4],struct.pack('<f',value))
        text=generated_text_containing('loc_0013E007: ;')
        block=text[text.index('loc_0013E007: ;'):text.index('loc_0013E08C: ;')]
        header=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(2);}}while(0)
static unsigned char mem[0x300000];
#define MEM8(a) mem[(uint32_t)(a)]
#define MEM32(a) (*(uint32_t*)(mem+(uint32_t)(a)))
#define MEMF(a) (*(float*)(mem+(uint32_t)(a)))
#define LO8(x) ((uint8_t)(x))
#define SET_LO8(x,v) ((x)=((x)&0xffffff00u)|(uint8_t)(v))
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define CMP_NE(a,b) (!CMP_EQ(a,b))
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define xmm2 xmm2v[0]
#define xmm3 xmm3v[0]
#define recomp_xmm_loadss(v,a) do{(v)[0]=MEMF(a);(v)[1]=(v)[2]=(v)[3]=0;}while(0)
static void recomp_human_move_selection_checkpoint(uint32_t physics,
                                                   uint32_t magnitude_bits)
{ (void)physics; (void)magnitude_bits; }
static void recomp_human_apply_landing_run_hysteresis(uint32_t physics,
                                                       float magnitude)
{ (void)physics; (void)magnitude; }
static float select_speed(float magnitude,int enabled,int mode,int previous,int human){
uint32_t eax=0xaaaa5555,ebx=0,esi=0x1000,edi=1,esp=0x2000;
float xmm0v[4]={0},xmm1v[4]={0},xmm2v[4]={0},xmm3v[4]={0};
MEM8(esi+0x65)=enabled;MEM8(esi+0xA4)=human;
MEM32(esi+8)=mode;MEM32(esi+12)=previous;
MEMF(esi+0x44)=1.7333333f;MEMF(esi+0x48)=8.4705887f;
MEMF(esp+0x3C)=magnitude;
'''
        tail=r'''
loc_0013E08C: ;
return MEMF(esp+0x3C);
}
int main(void){
MEMF(0x2DC394)=.6f;MEMF(0x2DC3E0)=.7f;MEMF(0x2E0B9C)=.95f;
MEMF(0x2DC090)=2;MEMF(0x2DC09C)=.01f;
const float boundaries[]={0,.01f,.6f,.7f,.95f,1};unsigned cases=0;
for(int enabled=0;enabled<2;enabled++)for(int mode=0;mode<3;mode++)
for(int previous=0;previous<3;previous++)for(int human=0;human<2;human++)
for(unsigned b=0;b<6;b++)for(int side=-1;side<=1;side++){
float v=boundaries[b];if(side)v=nextafterf(v,side<0?-INFINITY:INFINITY);
float threshold=!enabled||mode==1?2:previous==2?(human?.6f:.7f):.95f;
unsigned expected=v<.01f?0:v<threshold?1:2;
float actual=select_speed(v,enabled,mode,previous,human);
if(MEM32(0x100C)!=expected)fprintf(stderr,"v=%.9g enabled=%d mode=%d previous=%d human=%d expected=%u actual=%u speed=%.9g\n",v,enabled,mode,previous,human,expected,MEM32(0x100C),actual);
CHECK(MEM32(0x100C)==expected);
CHECK(actual==(expected==0?0:expected==1?1.7333333f:8.4705887f));
CHECK(MEM32(0x1008)==(uint32_t)mode);++cases;
}
select_speed(.65f,1,2,2,1);CHECK(MEM32(0x100C)==2);
select_speed(.65f,1,2,2,0);CHECK(MEM32(0x100C)==1);
printf("%u authored movement threshold/boundary cases pass\n",cases);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-run-selection-') as directory:
            path=Path(directory); c=path/'test.c'; exe=path/'test.exe'
            c.write_text(header+block+tail)
            # Match retail COMISS binary32 operands; MinGW's default x87
            # excess precision changes the reference at nextafter boundaries.
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-msse2','-mfpmath=sse','-fno-strict-aliasing','-std=c11',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__=='__main__':unittest.main()

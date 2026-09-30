"""Execute the full retail aircraft attack state with controlled engine callees.

This proves waypoint progression/fire commands and ABI, not bomb spawning,
collision, or mission completion. Original XBE bytes independently pin the fix.
"""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


class AirstrikeWaypointTests(unittest.TestCase):
    def test_full_update_and_old_premature_retreat(self):
        xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        for va, expected in (
            (0x6F517, '8b462c4083f80289462c0f850c010000'),
            (0x6F633, '0f8e0dffffff8b11ff92d40000005f5e5d83c418c20400'),
        ):
            off = config.va_to_file_offset(va)
            expected = bytes.fromhex(expected)
            self.assertEqual(raw[off:off + len(expected)], expected)
        text = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0002.c').read_text()
        fixed = re.search(r'void sub_0006F490\(void\)\n\{.*?\n\}', text, re.S)[0]
        patch = next(p for p in runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['PATCHES']
                     if p.name == 'Airstrike approach retains waypoint comparison across taken JNE')
        self.assertEqual(fixed.count(patch.after), 1)
        old = fixed.replace(patch.after, patch.before).replace('sub_0006F490(void)', 'old_update(void)')
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(2); } } while(0)
static unsigned char mem[0x400000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp;
#define g_esp esp
static double g_fp_stack[8]; static unsigned g_fp_top;
static float xmm0v[4];
#define xmm0 xmm0v[0]
#define MEM32(a) (*(uint32_t*)(mem+(uint32_t)(a)))
#define MEM8(a) mem[(uint32_t)(a)]
#define MEMF(a) (*(float*)(mem+(uint32_t)(a)))
#define PUSH32(s,v) do { uint32_t push_value=(v);(s)-=4;MEM32(s)=push_value; } while(0)
#define POP32(s,v) do { (v)=MEM32(s);(s)+=4; } while(0)
#define LO8(x) ((uint8_t)(x))
#define SET_LO8(x,v) ((x)=((x)&0xFFFFFF00u)|(uint8_t)(v))
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define CMP_NE(a,b) (!CMP_EQ(a,b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define recomp_xmm_loadss(v,a) do { (v)[0]=MEMF(a);(v)[1]=(v)[2]=(v)[3]=0; } while(0)
#define recomp_xmm_zero(v) memset(v,0,sizeof(v))
enum { STATE=0x1000, AI=0x2000, PLANE=0x3000, BEACON=0x4000 };
static unsigned retreat,fire,stop_fire,attack,removed,target_updated,detector_updated;
static unsigned empty_ammo,live_ammo;
static void sub_0006F110(void) { CHECK(ecx==STATE+0x84);esp+=8; }
static void sub_00042B80(void) { CHECK(ecx==PLANE);g_fp_stack[--g_fp_top&7]=100.0;esp+=4; }
static void sub_00170730(void) { eax=BEACON;esp+=4; }
static void sub_00042B90(void) { CHECK(ecx==PLANE);eax=empty_ammo;esp+=4; }
static void sub_00042BE0(void) { CHECK(ecx==PLANE);eax=live_ammo;esp+=4; }
static void sub_0006CB30(void) { CHECK(ecx==AI+0x580);++detector_updated;MEM8(AI+0x58D)=0;esp+=8; }
static void sub_00042520(void) { CHECK(ecx==PLANE);++target_updated;esp+=8; }
static void dispatch(uint32_t fn) {
 switch(fn) {
 case 1: CHECK(ecx==PLANE);eax=0x11;esp+=4;break;
 case 2: CHECK(ecx==PLANE && MEM32(esp+4)==1);
         if(MEMF(esp+8)==1.0f) {++fire;CHECK(MEM32(PLANE+0x4C4)==BEACON);}
         else { CHECK(MEMF(esp+8)==0.0f);++stop_fire;}esp+=12;break;
 case 3: CHECK(ecx==AI && MEM32(esp+4)==1);esp+=8;break;
 case 4: CHECK(ecx==PLANE && MEM32(esp+4)==STATE+0x10);++attack;esp+=8;break;
 case 5: CHECK(ecx==AI);++retreat;esp+=4;break;
 case 6: CHECK(ecx==BEACON);eax=0x22;esp+=4;break;
 case 7: CHECK(ecx==BEACON);++removed;esp+=4;break;
 default: CHECK(0);
 }
}
#define RECOMP_ICALL_SAFE(fn,s) dispatch(fn)
'''
        tail = r'''
static unsigned run(void(*fn)(void),int index,unsigned reached,unsigned no_ammo,
                    unsigned has_live_ammo,uint32_t stack,unsigned corrected) {
 memset(mem,0,sizeof(mem));memset(g_fp_stack,0,sizeof(g_fp_stack));g_fp_top=0;
 retreat=fire=stop_fire=attack=removed=target_updated=detector_updated=0;
 empty_ammo=no_ammo;live_ammo=has_live_ammo;
 MEM32(STATE+0xC)=AI;MEM32(STATE+0x2C)=index;MEM8(STATE+0x81)=index==2;
 MEM32(AI)=0x5000;MEM32(AI+0x10)=PLANE;MEM8(AI+0x58D)=reached;
 MEM32(AI+0x59C)=0x1234;MEM32(PLANE)=0x6000;MEM32(BEACON)=0x7000;
 MEM32(0x5000+0x1A0)=3;MEM32(0x5000+0xD4)=5;
 MEM32(0x6000+4)=1;MEM32(0x6000+0x1E4)=2;MEM32(0x6000+0x268)=4;
 MEM32(0x7000+4)=6;MEM32(0x7000+0x1AC)=7;
 MEM32(0x30D374)=0xFF;MEM32(0x30D370)=0x11;
 MEM32(0x30C144)=0xFF;MEM32(0x30C140)=0x22;
 eax=0;ebx=0xABCDEF12;ecx=STATE;edx=0;esi=0x12345678;edi=0x87654321;
 esp=stack;g_seh_ebp=0x76543210;MEM32(esp)=0xDEADC0DE;MEMF(esp+4)=1.0f/30.0f;
 fn();
 CHECK(esp==stack+8 && esi==0x12345678 && edi==0x87654321 && ebx==0xABCDEF12);
 CHECK(g_fp_top==0 && stop_fire==1 && MEM32(stack)==0xDEADC0DE);
 int next=index+(reached!=0),should_retreat=reached&&next>2;
 int attacking=(index==2)||(reached&&next==2);
 int should_fire=attacking&&!no_ammo&&!should_retreat;
 int should_remove=attacking&&no_ammo&&!has_live_ammo&&!should_retreat;
 unsigned ok=retreat==(unsigned)should_retreat && fire==(unsigned)should_fire && removed==(unsigned)should_remove;
 if(corrected) {
   CHECK(ok);CHECK(MEM32(STATE+0x2C)==(uint32_t)next);
   CHECK(attack==(unsigned)(reached&&next==2));
   CHECK(detector_updated==(unsigned)(reached&&next<=2));
   CHECK(MEM32(PLANE+0x4C4)==0);
 }
 return ok;
}
int main(void) {
 unsigned tested=0,old_failures=0;
 for(unsigned stack=0x200000;stack<=0x220000;stack+=0x10000)
 for(int index=0;index<3;++index)
 for(unsigned reached=0;reached<2;++reached)
 for(unsigned empty=0;empty<2;++empty)
 for(unsigned alive=0;alive<2;++alive) {
   run(sub_0006F490,index,reached,empty,alive,stack,1);
   old_failures+=!run(old_update,index,reached,empty,alive,stack,0);++tested;
 }
 CHECK(old_failures==12);
 printf("%u full attack-state cases passed; old build prematurely retreats in %u approach cases\n",tested,old_failures);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-airstrike-waypoint-') as directory:
            path = Path(directory)
            c = path / 'test.c'
            c.write_text(prelude + fixed + '\n' + old + '\n' + tail)
            exe = path / 'test.exe'
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-fno-strict-aliasing',
                            '-std=c11', str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()

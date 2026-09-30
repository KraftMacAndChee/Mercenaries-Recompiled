"""Exercise the actual retail grenade insertion and cleanup functions."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/"ports/mercenaries/src/recomp/gen/recomp_0001.c"
PRELUDE=r'''
#include <assert.h>
#include <stdlib.h>
#undef assert
#define assert(condition) do { if (!(condition)) exit(1); } while (0)
#include <stdint.h>
#include <string.h>
static uint8_t memory[0x20000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp;
static float xmm0, xmm0v[4];
static unsigned eliminated,dependent,hidden;
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define MEMF(a) (*(float *)(void *)(memory+(uint32_t)(a)))
#define PUSH32(s,v) do {uint32_t v_=(v);(s)-=4;MEM32(s)=v_;}while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;}while(0)
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define CMP_NE(a,b) (!CMP_EQ(a,b))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define CMP_G(a,b) ((int32_t)(a)>(int32_t)(b))
#define TEST_Z(a,b) (((uint32_t)(a)&(uint32_t)(b))==0)
#define TEST_NZ(a,b) (!TEST_Z(a,b))
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00)|(uint8_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define g_esp esp
#define recomp_xmm_loadss(v,a) (xmm0=MEMF(a))
static void dispatch(uint32_t target) {
    switch(target) {
    case 1: ++dependent; break;
    case 2: ++hidden; break;
    case 3: assert(ecx==0x1000); MEM32(ecx+0x7BC)=MEM32(esp+4); esp+=4; break;
    case 4: ++eliminated; break;
    case 5: eax=ecx; break;
    default: assert(!"Unexpected lifecycle call");
    }
    esp+=4;
}
#define RECOMP_ICALL_SAFE(t,s) dispatch(t)
static void sub_00061FF0(void) {assert(!"Unexpected drop matrix path");}
static void sub_00011D80(void) {assert(!"Unexpected drop matrix path");}
static void sub_00030B60(void) {assert(!"Unexpected live-weapon drop path");}
'''
HARNESS=r'''
static void insert(uint32_t item) {
    ecx=0x1000; esi=0x1111; edi=0x2222; ebx=0x3333;
    g_seh_ebp=0x1234; esp=0x1E003;
    PUSH32(esp,item); PUSH32(esp,0);
    sub_0005B1F0();
    assert(esp==0x1E003 && esi==0x1111 && edi==0x2222 && ebx==0x3333);
}
int main(void) {
    MEM32(0x1000)=0x7000; MEM32(0x7000+0x3DC)=3;
    MEM32(0x6000+0x1DC)=1; MEM32(0x6000+0x1CC)=2;
    MEM32(0x6000+0x10)=4; MEM32(0x6000+0x1B8)=5;
    for(unsigned a=0x2000;a<=0x4000;a+=0x1000) MEM32(a)=0x6000;
    insert(0x2000);
    assert(MEM32(0x17C8)==1 && MEM32(0x17C0)==0x2000);
    assert(MEM32(0x17BC)==0x2000 && dependent==1 && hidden==1);
    insert(0x3000);
    assert(MEM32(0x17C8)==2 && MEM32(0x17C4)==0x3000);
    assert(MEM32(0x17BC)==0x3000);
    /* Full inventory swaps the selected empty weapon, then eliminates it. */
    insert(0x4000);
    assert(MEM32(0x17C8)==2 && MEM32(0x17C4)==0x4000 && eliminated==1);
    ecx=0x1000; PUSH32(esp,0); sub_000579C0();
    assert(esp==0x1E003 && esi==0x1111 && edi==0x2222 && ebx==0x3333);
    assert(MEM32(0x17C8)==0 && MEM32(0x17BC)==0);
    assert(MEM32(0x17C0)==0 && MEM32(0x17C4)==0 && eliminated==3);
    return 0;
}
'''

class SecondaryInventoryTests(unittest.TestCase):
    def test_actual_insert_swap_clear_and_old_negative_control(self):
        compiler=shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists(): self.skipTest("GCC required")
        text=SOURCE.read_text(encoding="utf-8")
        def extract(name):
            start=text.index(f"void sub_{name}(void)")
            return text[start:text.index("\n}\n",start)+3]
        insert=extract("0005B1F0"); clear=extract("000579C0")
        check="if (CMP_GE(eax, 2)) goto loc_0005B344;"
        self.assertIn(check,insert)
        old=insert.replace(check,"if ((int8_t)LO8(eax) >= 0) goto loc_0005B344;")
        for label,body in (("fixed",insert),("old",old)):
            with self.subTest(label=label), tempfile.TemporaryDirectory(prefix="mercs-grenade-life-") as tmp:
                source=Path(tmp)/"inventory.c"; exe=Path(tmp)/"inventory.exe"
                source.write_text(PRELUDE+body+clear+HARNESS,encoding="utf-8")
                build=subprocess.run([compiler,"-std=c11",str(source),"-o",str(exe)],capture_output=True)
                self.assertEqual(build.returncode,0,build.stderr.decode(errors="replace"))
                result=subprocess.run([str(exe)],capture_output=True)
                if label=="fixed": self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))
                else: self.assertNotEqual(result.returncode,0,"Original orphaned-inventory bug escaped detection")

if __name__=="__main__": unittest.main()

"""Execute retail D3D flag fixes; reject the original code as negative controls."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/"ports/mercenaries/src/recomp/gen/recomp_0013.c"
PRELUDE=r'''
#include <assert.h>
#include <stdlib.h>
#undef assert
#define assert(condition) do { if (!(condition)) exit(1); } while (0)
#include <stdint.h>
#include <string.h>
static uint8_t memory[0x310000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp;
static unsigned waits;
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define PUSH32(s,v) do {uint32_t v_=(v);(s)-=4;MEM32(s)=v_;}while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;}while(0)
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define CMP_NE(a,b) (!CMP_EQ(a,b))
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define CMP_G(a,b) ((int32_t)(a)>(int32_t)(b))
#define TEST_Z(a,b) (((uint32_t)(a)&(uint32_t)(b))==0)
#define TEST_NZ(a,b) (!TEST_Z(a,b))
#define LO8(a) ((uint8_t)(a))
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void sub_0028EC90(void) {assert(MEM32(esp+4)==esi); ++waits; esp+=8;}
static void sub_0028D3F0(void) {assert(!"Unexpected special texture state");}
static void sub_0028D570(void) {assert(!"Unexpected special texture state");}
static void sub_0028D5B0(void) {assert(!"Unexpected special texture state");}
static void sub_0028D500(void) {assert(!"Unexpected special texture state");}
'''

class D3DConditionCodeTests(unittest.TestCase):
    def function(self,name):
        text=SOURCE.read_text(encoding="utf-8"); start=text.index(f"void sub_{name}(void)")
        return text[start:text.index("\n}\n",start)+3]

    def execute(self,fixed,old,harness):
        compiler=shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists(): self.skipTest("GCC required")
        self.assertNotEqual(fixed,old)
        for label,code in (("fixed",fixed),("old",old)):
            with self.subTest(label=label), tempfile.TemporaryDirectory(prefix="mercs-d3d-flags-") as tmp:
                source=Path(tmp)/"check.c"; exe=Path(tmp)/"check.exe"
                source.write_text(PRELUDE+code+harness,encoding="utf-8")
                result=subprocess.run([compiler,"-std=c11",str(source),"-o",str(exe)],capture_output=True)
                self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))
                result=subprocess.run([str(exe)],capture_output=True)
                if label=="fixed": self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))
                else: self.assertNotEqual(result.returncode,0,"Original flag bug escaped detection")

    def test_resource_flags_survive_push(self):
        fixed=self.function("0028C000")
        old=fixed.replace("    _flags = (TEST_NZ(MEM8(esp + 8), 0xA0)); /* preserve test flags across 2 instruction(s) */\n","").replace("if (_flags != 0)","if (TEST_NZ(MEM8(esp + 8), 0xA0))")
        self.execute(fixed,old,r'''
int main(void) {
 for(unsigned address=0x1000;address<=0x10A0;address+=0xA0)
  for(unsigned flags=0;flags<256;++flags) {
   esp=0x2F0000; esi=0x7777; waits=0; MEM32(address+4)=0x123456;
   PUSH32(esp,flags); PUSH32(esp,address); PUSH32(esp,0); sub_0028C000();
   assert(waits==((flags&0xA0)==0)); assert(eax==0x80123456);
   assert(esp==0x2F0000 && esi==0x7777);
  }
 return 0;
}
''')

    def test_texture_state_dispatch_and_carry(self):
        fixed=self.function("0028D5F0")
        old=fixed.replace("if (CMP_NE(eax, 0xC)) goto loc_0028D65C;","if (ecx != 0) goto loc_0028D65C;")
        harness=r'''
int main(void) {
 for(unsigned stage=0;stage<4;++stage)
  for(unsigned value=0;value<32;++value) {
   esp=0x2F0000; esi=0x7777; ecx=0xABCDEF;
   MEM32(0x298ED8)=0x100000; MEM32(0x298F10+stage*128)=0xEEEE;
   PUSH32(esp,value); PUSH32(esp,12); PUSH32(esp,stage); PUSH32(esp,0);
   sub_0028D5F0();
   assert(MEM32(0x298F10+stage*128)==value);
   assert(MEM32(0x298ED8)==(0x100000|(value<25?0x800:0x480F)));
   assert(esp==0x2F0000 && esi==0x7777);
  }
 return 0;
}
'''
        self.execute(fixed,old,harness)
        old_carry=fixed.replace("    _cf = ((uint32_t)(eax) < (uint32_t)(0x19)); /* preserve cmp carry across 0 instruction(s) */\n","")
        self.execute(fixed,old_carry,harness)

    def test_context_mask_survives_register_restore(self):
        fixed=self.function("00295304")
        old=fixed.replace("    _flags = (TEST_Z(MEM32(esi + 0x104), ecx)); /* preserve test flags across 2 instruction(s) */\n","").replace("if (_flags != 0) goto loc_002954D8;","if (TEST_Z(MEM32(esi + 0x104), ecx)) goto loc_002954D8;")
        self.execute(fixed,old,r'''
int main(void) {
 for(unsigned slot=0;slot<4;++slot)
  for(unsigned enabled=0;enabled<2;++enabled) {
   memset(memory,0,sizeof(memory)); esp=0x2F0000; ecx=0x1000;
   esi=0x7000; edi=0x8888; ebx=0x9999; g_seh_ebp=0xABCD;
   MEM32(0x1000)=0x8000; MEM32(0x1128)=0x100;
   MEM32(0x1104)=enabled?(1u<<slot):0; MEM32(0x7104)=enabled?0:(1u<<slot);
   MEM32(0x8000+0x3220)=0x5555;
   PUSH32(esp,slot); PUSH32(esp,0); sub_00295304();
   assert(MEM32(0x8000+0x3220)==(enabled && slot!=1?1:0x5555));
   assert(esp==0x2F0000 && esi==0x7000 && edi==0x8888 && ebx==0x9999);
  }
 return 0;
}
''')

if __name__=="__main__": unittest.main()

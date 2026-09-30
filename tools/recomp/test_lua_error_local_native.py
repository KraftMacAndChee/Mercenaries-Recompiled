"""Exercise the retail type-error path that previously lost lua_State in ESI."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from generated_test_utils import generated_text_containing

class LuaLocalError(unittest.TestCase):
    def test_local_error_preserves_state_and_stack(self):
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        if not Path(compiler).exists():
            self.skipTest("GCC required")
        bodies = []
        for name in ("sub_001DE320", "sub_001DE31A", "sub_001DE280", "sub_001DE760"):
            source = generated_text_containing(f"void {name}(void)")
            bodies.append(re.search(rf"void {name}\(void\)\n\{{.*?\n\}}",source,re.S)[0])
        prelude = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
static unsigned char memory[0x1000000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp;
static int named=1, errors;
#define MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define LO8(a) ((uint8_t)(a))
#define ZX8(a) ((uint8_t)(a))
#define PUSH32(s,v) do { uint32_t value=(v);(s)-=4;MEM32(s)=value; } while(0)
#define POP32(s,v) do { (v)=MEM32(s);(s)+=4; } while(0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_A(a,b) ((uint32_t)(a)>(uint32_t)(b))
#define CMP_AE(a,b) ((uint32_t)(a)>=(uint32_t)(b))
#define CMP_B(a,b) ((uint32_t)(a)<(uint32_t)(b))
#define CMP_EQ(a,b) ((a)==(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define RECOMP_ITAIL(a) assert(!"unexpected symbolic instruction branch")
enum {L=0x954B80, CI=0x955DB0, PROTO=0x972670, BASE=0x963120,
      VALUE=0x9631A0, CODE=0x960850, NAME=0xA00000, KIND=0xA00100,
      OP=0x2FBCA0, STACK=0x8BF900};
static void sub_001E2A30(void) {
    assert(MEM32(esp+4)==PROTO);
    assert(MEM32(esp+8)==9 && MEM32(esp+12)==0x26);
    eax=named?NAME:0;esp+=4;
}
static void sub_001DDE70(void) {eax=0x3f;esp+=4;}
static void sub_001DE540(void) {
    /* The formatter must receive the original L, never the instruction PC. */
    assert(MEM32(esp+4)==L);assert(esi==L);
    assert(MEM32(esp+12)==OP);
    if(named) {
        assert(MEM32(esp+8)==0x2FBA1C);
        assert(MEM32(esp+16)==0x2FB9DC);
        assert(MEM32(esp+20)==NAME && MEM32(esp+24)==KIND);
    } else {
        assert(MEM32(esp+8)==0x2FBA40 && MEM32(esp+16)==KIND);
    }
    ++errors;esp+=4;
}
"""
        harness = r"""
int main(void) {
    MEM32(L+0x14)=CI; MEM32(L+0xC)=BASE;
    MEM32(CI)=BASE;MEM32(CI+4)=BASE+0x100;MEM32(CI+8)=0;
    MEM32(BASE-8)=0xA00200;MEM32(0xA00200+0xC)=PROTO;
    MEM32(PROTO+0xC)=CODE;MEM32(CI+0xC)=CODE+(0x26+1)*4;
    MEM32(VALUE)=0;MEM32(0x2FBBA0)=KIND;
    for(named=0;named<=1;++named) for(int n=0;n<1000;++n) {
        esp=STACK;esi=0x111;edi=0x222;ebx=0x333;ecx=0x444;g_seh_ebp=0x555;
        MEM32(esp+4)=L;MEM32(esp+8)=VALUE;MEM32(esp+12)=OP;
        sub_001DE760();
        assert(esp==STACK+4 && esi==0x111 && edi==0x222 && ebx==0x333);
        assert(g_seh_ebp==0x555);
    }
    assert(errors==2000);
    puts("PASS: 2000 local/unnamed type errors preserve Lua state, arguments and stack");
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            source=Path(tmp)/"test.c"; binary=Path(tmp)/"test.exe"
            source.write_text(prelude+"\n".join(bodies)+harness)
            subprocess.run([compiler,"-std=c11","-O2",str(source),"-o",str(binary)],check=True,capture_output=True,text=True)
            run=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            print(run.stdout.strip())

if __name__=="__main__":
    unittest.main()

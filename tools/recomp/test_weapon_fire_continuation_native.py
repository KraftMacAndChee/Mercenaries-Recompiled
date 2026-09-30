"""Execute the retail weapon frame/return edges with controlled engine callees.

The middle projectile-spawn path is outside this test. Denied-fire executes the
full prologue and actual failure tail; successful-fire tests enter each real
late return/feedback edge after that same prologue. No live game is accessed.
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


class WeaponContinuationTests(unittest.TestCase):
    def test_native_stack_sound_and_feedback_edges(self):
        xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        for va, expected in (
            (0x34C60, '0f84920b0000'),
            (0x357F8, '8b168bceff9208020000'),
            (0x35832, '8b8c24c40a00008a4424175f5e64890d000000005b8be55dc21400'),
        ):
            off = config.va_to_file_offset(va)
            self.assertEqual(raw[off:off+len(bytes.fromhex(expected))], bytes.fromhex(expected))
        table = raw[config.va_to_file_offset(0x35874):config.va_to_file_offset(0x35896)]
        source = (ROOT/'ports/mercenaries/src/recomp/gen/recomp_0000.c').read_text()
        fixed = re.search(r'void sub_00034BD0\(void\)\n\{.*?\n\}', source, re.S)[0]
        marker = '    /* retail weapon fire continuation repair */'
        self.assertIn(marker, fixed)
        old = fixed[:fixed.index(marker)] + fixed[fixed.index('    #undef fp_push'):]
        local_switch = re.search(r'    \{ const uint32_t _fire_tail.*?return; \}', old, re.S)[0]
        old = old.replace(local_switch, '    g_seh_ebp = ebp; RECOMP_ITAIL(MEM32(eax * 4 + 0x35874)); return; /* indirect tail jmp */')
        for address in ('000357F8','00035832','000357E2'):
            old = old.replace(f'goto loc_{address};', f'{{ sub_{address}(); return; }}')
        # Patcher equivalence ignores an extra separating blank line only.
        patcher = runpy.run_path(str(ROOT/'ports/mercenaries/scripts/Patch-Generated.py'))['patch_weapon_fire_continuation']
        self.assertEqual(patcher(source), source)
        original_source = source.replace(fixed, old)
        repaired_again = patcher(original_source)
        normalize = lambda s: re.sub(r'\n\s*\n', '\n\n', s)
        self.assertEqual(normalize(repaired_again), normalize(source))
        for address in ('000357F8','00035832','000357E2'):
            self.assertNotIn(f'sub_{address}();', fixed)

        # Test routing does not replace any return code or stack operation.
        # It bypasses only the unrelated projectile creation middle on success.
        def route(body):
            return body.replace('loc_00034C66: ;', '''loc_00034C66: ;
    if (mode == 1) { eax=0; goto loc_00035789; }
    if (mode == 2) { eax=1; goto loc_00035789; }
    if (mode == 3) goto loc_000357AD;
    CHECK(0);''', 1)

        prelude = r'''
#define RECOMP_GENERATED_CODE
#include "templates/runtime/recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(2);}}while(0)
ptrdiff_t g_xbox_mem_offset;
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
double g_fp_stack[8]; uint32_t g_fp_top;
uint16_t g_x87_control_word,g_x87_status_word;
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4];
float g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
static unsigned char memory[0x400000];
static unsigned mode,dry_sound,feedback,feedback_type,gate208,gate20c,gate204;
enum { WEAPON=0x1000,OWNER=0x2000,WEAPON_VT=0x3000,OWNER_VT=0x4000,SOUND_VT=0x5000,CONTROL=0x6000 };
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void dispatch(uint32_t fn) {
 // An ordinary nested SEH callee can leave the bridge pointing at its frame.
 // The owner must retain its own local EBP until its complete retail epilogue.
 g_seh_ebp=0x18000;
 switch(fn) {
 case 1: CHECK(ecx==OWNER);eax=MEM32(esp+4);esp+=8;break;
 case 2: CHECK(ecx==WEAPON&&MEM32(esp+4)==OWNER);eax=mode!=0;esp+=8;break;
 case 3: CHECK(ecx==WEAPON);eax=gate208;esp+=4;break;
 case 4: CHECK(ecx==WEAPON&&MEM32(esp+4)==OWNER);eax=gate20c;esp+=8;break;
 case 5: CHECK(ecx==WEAPON);eax=gate204;esp+=4;break;
 case 6: CHECK(ecx==WEAPON+0x24c);++dry_sound;esp+=4;break;
 case 7: CHECK(ecx==OWNER);eax=CONTROL;esp+=4;break;
 case 8: CHECK(ecx==CONTROL);eax=0;esp+=4;break;
 default: fprintf(stderr,"unexpected virtual %08X\n",fn);CHECK(0);
 }
}
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(fn,s) dispatch(fn)
static void tail(uint32_t fn) {
 // Old split switch cases all lead to a minimal-ret stub, except default,
 // whose real tail restores ESP using the explicitly transferred frame.
 if(fn==0x357e2) {
  ++feedback;feedback_type=0;
  uint32_t original_frame=g_seh_ebp,chain=MEM32(esp+0xac4);
  SET_LO8(eax,MEM8(esp+0x17));POP32(esp,edi);POP32(esp,esi);
  MEM32(0)=chain;POP32(esp,ebx);esp=original_frame+28;
 } else { CHECK(fn==0x357c6||fn==0x357cd||fn==0x357d4||fn==0x357db);esp+=4; }
}
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(fn) tail(fn)
static void sub_000357F8(void){esp+=4;}
static void sub_00035832(void){esp+=4;}
static void sub_000357E2(void){tail(0x357e2);}
static void sub_000A47D0(void) {
 CHECK(ecx==0x323ad8&&MEM32(esp+8)==0x1234);
 feedback_type=MEM32(esp+4);++feedback;esp+=12;g_seh_ebp=0x18000;
}
'''
        direct = sorted(set(re.findall(r'\b(sub_[0-9A-F]+)\(\)',fixed)))
        stubs = ''.join(f'static void {name}(void){{fprintf(stderr,"unexpected {name}\\n");CHECK(0);}}\n'
                        for name in direct if name != 'sub_000A47D0')
        table_init = 'static const unsigned char retail_table[]={' + ','.join(map(str,table)) + '};\n'
        harness = r'''
static unsigned run(void(*fn)(void),unsigned m,unsigned mask,unsigned type,unsigned align,unsigned repaired) {
 memset(memory,0,sizeof(memory));g_xbox_mem_offset=(ptrdiff_t)memory;
 memset(g_fp_stack,0,sizeof(g_fp_stack));g_fp_top=0;
 MEM32(WEAPON)=WEAPON_VT;MEM32(OWNER)=OWNER_VT;
 MEM32(WEAPON+0x24c)=SOUND_VT;MEM32(CONTROL)=0x7000;MEM32(0x7000+0xf0)=8;
 MEM32(WEAPON_VT+0x1c4)=2;MEM32(WEAPON_VT+0x208)=3;
 MEM32(WEAPON_VT+0x20c)=4;MEM32(WEAPON_VT+0x204)=5;MEM32(SOUND_VT+0x14)=6;
 MEM32(OWNER_VT+0x34)=1;MEM32(OWNER_VT+0x38)=1;MEM32(OWNER_VT+0x58)=1;MEM32(OWNER_VT+0xc0)=7;
 MEM32(WEAPON+0x1a0)=type;MEM32(WEAPON+0x1cc)=0x1234;
 memcpy((void*)(g_xbox_mem_offset+0x35874),retail_table,sizeof(retail_table));
 mode=m;gate208=mask&1;gate20c=(mask>>1)&1;gate204=(mask>>2)&1;
 dry_sound=feedback=feedback_type=0;
 const uint32_t stack=0x30000+align;
 eax=edx=0;ecx=WEAPON;esi=0x11111111;edi=0x22222222;ebx=0x33333333;
 g_seh_ebp=0x87654321;esp=stack;MEM32(esp)=0xFEEDC0DE;
 MEM32(esp+4)=OWNER;MEM32(esp+8)=0x8000;MEM32(esp+12)=0x9000;
 MEM32(esp+16)=0;MEM32(esp+20)=0;MEM32(0)=0x12345678;
 fn();
 unsigned intact=esp==stack+24&&esi==0x11111111&&edi==0x22222222&&ebx==0x33333333&&MEM32(0)==0x12345678;
 if(repaired) {
  CHECK(intact);CHECK(LO8(eax)==(mode!=0));CHECK(MEM32(stack)==0xFEEDC0DE);
  CHECK(g_fp_top==0);CHECK(dry_sound==(mode==0&&mask==3));CHECK(feedback==(mode==3));
  if(mode==3) {
   uint32_t target=type>13?0x357e2:MEM32(0x35874+MEM8(0x35888+type)*4);
   CHECK(feedback_type==(target==0x357c6?4:target==0x357cd?1:target==0x357d4?2:target==0x357db?3:0));
  }
 }
 return intact;
}
int main(void) {
 unsigned cases=0,old_bad=0;
 for(unsigned a=0;a<16;a+=4)for(unsigned m=0;m<4;m++)for(unsigned k=0;k<8;k++)for(unsigned t=0;t<15;t++) {
  run(sub_00034BD0,m,k,t,a,1);old_bad+=!run(old_fire,m,k,t,a,0);++cases;
 }
 CHECK(old_bad>0);printf("%u weapon return cases passed; old broken stack in %u cases\n",cases,old_bad);return 0;
}
'''
        old=route(old).replace('void sub_00034BD0(void)','void old_fire(void)',1)
        compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-weapon-tail-') as directory:
            folder=Path(directory);code=folder/'test.c';exe=folder/'test.exe'
            code.write_text(prelude+stubs+table_init+route(fixed)+old+harness)
            subprocess.run([compiler,'-O2','-std=c11','-I',str(ROOT),str(code),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__=='__main__':unittest.main()

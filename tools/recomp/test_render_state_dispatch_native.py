"""Execute retail render-state dispatch, including actual depth pushbuffer emission."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


def function(text, name):
    return re.search(r'void sub_' + name + r'\(void\)\n\{.*?\n\}', text, re.S)[0]


class RenderStateDispatchTests(unittest.TestCase):
    def test_special_states_and_depth_clear(self):
        retail = ROOT / 'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(retail))
        raw = retail.read_bytes()
        def code(address, size):
            offset = config.va_to_file_offset(address)
            return raw[offset:offset + size]
        self.assertEqual(code(0x21028D, 8), bytes.fromhex('81fe880000007d1f'))
        self.assertEqual(code(0x2102B4, 2), bytes.fromhex('7509'))
        gen = ROOT / 'ports/mercenaries/src/recomp/gen'
        wrapper = function((gen/'recomp_0010.c').read_text(encoding='utf-8'), '00210250')
        depth = function((gen/'recomp_0013.c').read_text(encoding='utf-8'), '0028DDD0')
        # Expected dispatch table comes from the retail CALL instructions, not lifted C.
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        expected = {0x88: 0x28CB50}
        state = None
        for insn in Cs(CS_ARCH_X86, CS_MODE_32).disasm(code(0x2102BF, 0x1EA), 0x2102BF):
            if insn.mnemonic == 'cmp' and insn.op_str.startswith('esi, '):
                state = int(insn.op_str.split(', ')[1], 0)
            elif insn.mnemonic == 'call':
                expected[state] = int(insn.op_str, 0)
        self.assertEqual(set(expected), set(range(0x88, 0xA6)))
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d\n",__LINE__);exit(2);}}while(0)
static uint8_t memory[0x800000];
static uint32_t eax,ecx,edx,esi,edi,esp,called,argument,count;
#define MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define PUSH32(s,v) do{uint32_t t=(v);(s)-=4;MEM32(s)=t;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define CMP_NE(a,b) (!CMP_EQ(a,b))
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define CMP_B(a,b) ((uint32_t)(a)<(uint32_t)(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define RECOMP_TRACE_FUNC(a) do{if((a)==0x28DDD0){called=(a);argument=MEM32(esp+4);++count;}}while(0)
'''
        stubs = ''
        for name in sorted(set(re.findall(r'sub_(\w+)\(\);', wrapper + depth)) - {'0028DDD0'}):
            cleanup = 4 if name == '0028CB80' else 8
            stubs += f'void sub_{name}(void){{called=0x{name};argument=MEM32(esp+4);++count;esp+={cleanup};}}\n'
        table = ','.join(hex(expected[state]) for state in range(0x88, 0xA6))
        harness = r'''
int main(void) {
 uint32_t expected[] = {TABLE};
 uint32_t registers[] = {0,1,0xFFFFFFFF,0x299378};
 unsigned cases=0;
 for(unsigned state=0x88;state<0xA6;++state)
 for(unsigned value=0;value<2;++value)
 for(unsigned r=0;r<4;++r)
 for(unsigned attachment=0;attachment<2;++attachment) {
  memset(memory,0,sizeof(memory));
  MEM32(0x7AD320+state*4)=1-value;
  MEM32(0x299378)=0x1000; MEM32(0x1000)=0x2000; MEM32(0x1004)=0x3000;
  MEM32(0x1000+0x1A08)=attachment;
  esp=0x700000;esi=0x1234;edi=0x5678;ecx=registers[r];count=0;
  PUSH32(esp,value);PUSH32(esp,state);PUSH32(esp,0);sub_00210250();
  CHECK(called==expected[state-0x88] && argument==value && count==1);
  CHECK(esp==0x700000-8 && esi==0x1234 && edi==0x5678);
  CHECK(MEM32(0x7AD320+state*4)==value);
  if(state==0x8F){
   CHECK(MEM32(0x2000)==0x4030C);
   CHECK(MEM32(0x2004)==(value&&attachment));
   CHECK(MEM32(0x1000)==0x2010);
   CHECK(MEM32(0x29931C)==value);
  }
  /* The same cached state must not emit any second call. */
  PUSH32(esp,0);sub_00210250();CHECK(count==1);CHECK(esp==0x700000-8);
  ++cases;
 }
 printf("%u dispatch/cache/ABI cases passed\n",cases);return 0;
}
'''.replace('TABLE', table)
        old = wrapper.replace('if (CMP_NE(esi, 0x88)) goto loc_002102BF;', 'if (ecx != 0) goto loc_002102BF;')
        self.assertNotEqual(old, wrapper)
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        for label, candidate in [('fixed', wrapper), ('old', old)]:
            with self.subTest(label=label), tempfile.TemporaryDirectory(prefix='merc-render-state-') as directory:
                src, exe = Path(directory)/'check.c', Path(directory)/'check.exe'
                src.write_text(prelude+stubs+depth+candidate+harness, encoding='utf-8')
                result = subprocess.run([compiler,'-std=c11',str(src),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                result = subprocess.run([str(exe)],capture_output=True,text=True)
                if label == 'fixed':
                    self.assertEqual(result.returncode,0,result.stderr)
                    print(result.stdout.strip())
                else:
                    self.assertNotEqual(result.returncode,0,'Original branch bug escaped detection')


if __name__ == '__main__':
    unittest.main()

"""Execute the retail save-directory routine, with stale-stack negative controls.

Dependencies model filesystem responses, not a live game. Both failed and
successful status paths must retain their original meaning. Guest XBE bytes
and fresh translation are checked separately from the generated-unit repair.
"""
from pathlib import Path
import importlib.util
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator
from generated_test_utils import generated_text_containing

PRELUDE = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(2);}}while(0)
static uint8_t memory[0x20000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp;
static unsigned directory_status,chdir_status,mkdir_status,free_space,poison;
static unsigned allocated,frees,chdirs,mkdirs;
#define MEM32(a) (*(uint32_t *)(void *)(memory+(uint32_t)(a)))
#define LO8(a) ((uint8_t)(a))
#define PUSH32(s,v) do {uint32_t t_=(v);(s)-=4;MEM32(s)=t_;}while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;}while(0)
#define CMP_EQ(a,b) ((uint32_t)(a)==(uint32_t)(b))
#define CMP_NE(a,b) (!CMP_EQ(a,b))
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void sub_001F7230(void){CHECK(MEM32(esp+4)==0xA00);eax=0x4000;++allocated;esp+=4;}
static void sub_001F66F0(void){uint32_t p=MEM32(esp+4);CHECK(p==0||p==0x4000);if(p)++frees;esp+=4;}
static void sub_002370B8(void){eax=0;esp+=4;}
static void sub_002235E0(void){CHECK(ecx==0x1030);esp+=16;}
static void status(unsigned value){CHECK(ecx==0x1030);MEM32(MEM32(esp+4))=value;eax=1;esp+=8;}
static void sub_002237B0(void){status(directory_status);}
static void sub_00223880(void){CHECK(ecx==0x1030);++chdirs;esp+=8;}
static void sub_00223440(void){status(chdir_status);}
static void sub_00209040(void){CHECK(ecx==0x1030);esp+=4;}
static void sub_002233C0(void){CHECK(ecx==0x1030);MEM32(MEM32(esp+4))=1;MEM32(MEM32(esp+12))=free_space;eax=1;esp+=16;}
static void sub_0018CB80(void){CHECK(ecx==0x1000);eax=0x2000;esp+=4;}
static void sub_00087D20(void){CHECK(ecx==0x1030);++mkdirs;esp+=8;}
static void sub_00223420(void){status(mkdir_status);}
'''
HARNESS = r'''
int main(void){
 unsigned cases=0;
 for(directory_status=1;directory_status<=2;++directory_status)
 for(chdir_status=1;chdir_status<=2;++chdir_status)
 for(mkdir_status=1;mkdir_status<=2;++mkdir_status)
 for(unsigned enough=0;enough<2;++enough)
 for(poison=0;poison<2;++poison){
  memset(memory,0,sizeof(memory));allocated=frees=chdirs=mkdirs=0;
  free_space=enough?0x10000:0;
  MEM32(0x1174)=0x1000;esp=0x10000;ecx=0x1000;
  ebx=poison;esi=0xCCCC;edi=0xDDDD;PUSH32(esp,0x12345678);
  sub_0018CC80();
  unsigned expected=directory_status==1?(chdir_status==1?2:3):
     (!enough?3:(mkdir_status==1?1:3));
  CHECK(eax==expected);CHECK(esp==0x10000);
  CHECK(ebx==poison&&esi==0xCCCC&&edi==0xDDDD);
  CHECK(allocated==1&&frees==1);
  CHECK(chdirs==(directory_status==1));
  CHECK(mkdirs==(directory_status!=1&&enough));++cases;
 }
 printf("%u save-directory status cases passed\n",cases);return 0;
}
'''


class SaveDirectoryFlagsTests(unittest.TestCase):
    def test_retail_and_generated_directory_status(self):
        xbe = ROOT/'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        for address, code in ((0x18CD63, '837c240c016a0075c4'),
                              (0x18CDA0, '837c240c01577588')):
            off = config.va_to_file_offset(address)
            self.assertEqual(raw[off:off+len(code)//2], bytes.fromhex(code))
        info = {'name':'sub_0018CC80','start':'0x0018CC80',
                'end':0x18CDBF,'size':0x13F,'section':'.text'}
        translated = FunctionTranslator(raw,{0x18CC80:info}).translate_function(0x18CC80,info)
        self.assertEqual(translated.count('/* preserve cmp flags across 1 instruction(s) */'),2)
        text = generated_text_containing('void sub_0018CC80(void)')
        fixed = re.search(r'void sub_0018CC80\(void\)\n\{.*?\n\}',text,re.S)[0]
        old = re.sub(r'    _flags = .*?/\* save directory status before PUSH \*/\n','',fixed)
        old = old.replace(
            '    _flags = (CMP_NE(MEM32(esp + 0xC), 1)); '
            '/* preserve cmp flags across 1 instruction(s) */\n', ''
        )
        old = old.replace('if (_flags != 0) goto loc_0018CD30;',
                          'if (CMP_NE(MEM32(esp + 0xC), 1)) goto loc_0018CD30;')
        self.assertNotEqual(fixed,old)
        spec = importlib.util.spec_from_file_location('save_patches',ROOT/'ports/mercenaries/scripts/Patch-Generated.py')
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name]=module
        spec.loader.exec_module(module)
        patched = module.patch_save_directory_stack_flags(old)
        self.assertNotEqual(patched, old)
        self.assertEqual(module.patch_save_directory_stack_flags(fixed),fixed)
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        for label,body in (('fixed',fixed),('patched',patched),('old',old)):
            with self.subTest(version=label), tempfile.TemporaryDirectory(prefix='mercs-save-flags-') as tmp:
                source=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe'
                source.write_text(PRELUDE+body+HARNESS)
                build=subprocess.run([compiler,'-std=c11',str(source),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                run=subprocess.run([str(exe)],capture_output=True,text=True)
                if label in ('fixed', 'patched'):
                    self.assertEqual(run.returncode,0,run.stderr)
                    if label == 'fixed':
                        print(run.stdout.strip())
                else:
                    self.assertNotEqual(run.returncode,0,'Stale-stack negative control unexpectedly passed')


if __name__=='__main__':
    unittest.main()

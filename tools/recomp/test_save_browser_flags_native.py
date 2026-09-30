"""Retain the retail event/button distinction in both save browser callbacks."""
from pathlib import Path
import importlib.util
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from generated_test_utils import generated_text_containing

PRELUDE=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %d %s\n",__LINE__,#c);exit(2);}}while(0)
static unsigned char memory[0x440000];
static uint32_t eax,ecx,edx,esi,esp;
static unsigned sounds;
#define MEM32(a) (*(uint32_t*)(void*)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,v) ((a)=((a)&0xFFFFFF00u)|(uint8_t)(v))
#define PUSH32(s,v) do{uint32_t t_=(v);(s)-=4;MEM32(s)=t_;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define CMP_NE(a,b) ((uint32_t)(a)!=(uint32_t)(b))
#define CMP_EQ(a,b) (!CMP_NE(a,b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define g_esp esp
static void get_sound(uint32_t target,uint32_t before){CHECK(target==0xAABB&&ecx==0x3000);eax=0x1234;esp+=4;CHECK(esp==before);}
#define RECOMP_ICALL_SAFE(t,s) get_sound(t,s)
static void sub_001FFA60(void){CHECK(MEM32(esp+4)==0&&MEM32(esp+8)==0x1234);++sounds;esp+=4;}
'''


class SaveBrowserFlagsTests(unittest.TestCase):
    def test_callbacks_follow_event_not_input_number(self):
        xbe=ROOT/'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe));raw=xbe.read_bytes()
        text=generated_text_containing('void sub_00188690(void)')
        spec=importlib.util.spec_from_file_location('browser_patches',ROOT/'ports/mercenaries/scripts/Patch-Generated.py')
        module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
        compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        for address,target,offset,any_input in (('00188690','001886DC',0x10,False),('00188760','001887A6',0x14,True)):
            off=config.va_to_file_offset(int(address,16))
            self.assertEqual(raw[off:off+10],bytes.fromhex('837c240801568bf17542' if address=='00188690' else '837c240801568bf1753c'))
            fixed=re.search(r'void sub_'+address+r'\(void\)\n\{.*?\n\}',text,re.S)[0]
            old=re.sub(r'    _flags = .*?/\* save browser event before PUSH \*/\n','',fixed)
            old=old.replace(
                '    _flags = (CMP_NE(MEM32(esp + 8), 1)); '
                '/* preserve cmp flags across 2 instruction(s) */\n', ''
            )
            old=old.replace(f'if (_flags != 0) goto loc_{target};',f'if (CMP_NE(MEM32(esp + 8), 1)) goto loc_{target};')
            self.assertNotEqual(fixed,old)
            patched = module.patch_save_browser_stack_flags(old)
            self.assertNotEqual(patched, old)
            self.assertEqual(module.patch_save_browser_stack_flags(fixed),fixed)
            harness=r'''
int main(void){
 for(unsigned event=0;event<5;++event)for(unsigned input=0;input<8;++input){
  memset(memory,0,sizeof(memory));sounds=0;esp=0x20000;ecx=0x1000;esi=0xABCD;
  MEM32(0x4322F0)=0x2000;MEM32(0x2000+0xE6A0)=0x3000;
  MEM32(0x3000)=0x4000;MEM32(0x4034)=0xAABB;
  PUSH32(esp,event);PUSH32(esp,input);PUSH32(esp,0x98765432);FUNCTION();
  CHECK(esp==0x20000&&esi==0xABCD);
  CHECK(sounds==(event==1));
  CHECK(MEM8(0x1000+OFFSET)==(event==1&&input==5));
  CHECK(MEM8(0x1001+OFFSET)==(event==1&&input==4));
  CHECK(LO8(eax)==(event==1&&(ANY_INPUT||input==4||input==5||input==6)));
 }
 puts("40 save-browser input/event cases passed");return 0;
}
'''.replace('FUNCTION','sub_'+address).replace('OFFSET',str(offset)).replace('ANY_INPUT','1' if any_input else '0')
            for label,body in (('fixed',fixed),('patched',patched),('old',old)):
                with self.subTest(address=address,version=label),tempfile.TemporaryDirectory(prefix='mercs-save-browser-') as tmp:
                    source=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe';source.write_text(PRELUDE+body+harness)
                    build=subprocess.run([compiler,'-std=c11',str(source),'-o',str(exe)],capture_output=True,text=True)
                    self.assertEqual(build.returncode,0,build.stderr)
                    run=subprocess.run([str(exe)],capture_output=True,text=True)
                    if label in ('fixed', 'patched'):
                        self.assertEqual(run.returncode,0,run.stderr)
                        if label == 'fixed':
                            print(address,run.stdout.strip())
                    else:self.assertNotEqual(run.returncode,0,'Stale event comparison escaped detection')


if __name__=='__main__':unittest.main()

"""Execute retail Lua concat/conversion paths with controlled string allocation.

Runs the actual lifted concat and tostring bodies plus the production native
formatter. Allocator, string interning and unreachable error paths are test
doubles. No game scripts, live process, or assets are changed.
"""
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


class LuaConcatNumberTests(unittest.TestCase):
    def test_all_three_retail_conversion_sites(self):
        xbe = ROOT/'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        for address, expected in (
            (0x1E01A1, '6898bc2f0050e80c6f0500'),
            (0x1E09A4, '6898bc2f0050e809670500'),
            (0x1E0A9F, '6898bc2f0050e80e660500'),
        ):
            off = config.va_to_file_offset(address)
            self.assertEqual(raw[off:off+11], bytes.fromhex(expected))
        off = config.va_to_file_offset(0x2FBC98)
        self.assertEqual(raw[off:off+6], b'%.14g\0')
        generated = (ROOT/'ports/mercenaries/src/recomp/gen/recomp_0009.c').read_text()
        bodies = '\n'.join(re.search(r'void sub_'+fn+r'\(void\)\n\{.*?\n\}', generated, re.S)[0]
                           for fn in ('001E0180', '001E0960'))
        self.assertEqual(bodies.count('recomp_native_format_lua_number(eax, MEMD(esp));'), 3)
        self.assertNotIn('sub_002370B8();', bodies)
        manual = (ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
        formatter = re.search(r'uint32_t recomp_native_format_lua_number\(.*?\n\}', manual, re.S)[0]
        prelude = r'''
#define RECOMP_GENERATED_CODE
#include "templates/runtime/recomp_types.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %u: %s\n",__LINE__,#c);exit(2);}}while(0)
ptrdiff_t g_xbox_mem_offset;
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
double g_fp_stack[8]; uint32_t g_fp_top;
uint16_t g_x87_control_word,g_x87_status_word;
static unsigned char memory[0x400000];
static uint32_t next_string;
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
static uint32_t intern(const char *s, unsigned n) {
 uint32_t a=next_string;CHECK(n<1024);next_string+=(n+32+15)&~15u;
 CHECK(next_string<0x300000);MEM32(a+12)=n;
 memcpy(memory+a+16,s,n);MEM8(a+16+n)=0;return a;
}
static void sub_001E26E0(void) {
 CHECK(MEM32(esp+4)==0x10000);
 eax=intern((char*)memory+MEM32(esp+8),MEM32(esp+12));esp+=4;
}
static void sub_001E37C0(void) {
 CHECK(MEM32(esp+4)==0x10000);CHECK(MEM32(esp+12)<1024);
 eax=0x300000;esp+=4;
}
static void sub_001E00E0(void){CHECK(0);}
static void sub_001E02B0(void){CHECK(0);}
static void sub_001DE7F0(void){CHECK(0);}
static void sub_001DE540(void){CHECK(0);}
static void recomp_lua_concat_trace(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e,uint32_t f){CHECK(0);}
'''
        harness = r'''
static void put_string(unsigned i,const char*s){uint32_t a=0x20000+i*16;MEM32(a)=4;MEM32(a+8)=intern(s,strlen(s));}
static void put_number(unsigned i,double n){uint32_t a=0x20000+i*16;MEM32(a)=3;MEMD(a+8)=n;}
int main(void) {
 const double values[]={0,1,2,3,9,-1,1.5,12.75,99999,12345678901234.0,-0.0,1.234567890123456};
 unsigned cases=0;
 g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;
 for(unsigned v=0;v<sizeof(values)/sizeof(values[0]);v++)for(unsigned mode=0;mode<4;mode++)for(unsigned align=0;align<16;align+=4){
  memset(memory,0,sizeof(memory));next_string=0x210000;
  MEM32(0x1000c)=0x20000;MEM32(0x10010)=0x11000;
  char number[32],expected[160];snprintf(number,sizeof(number),"%.14g",values[v]);
  if(v<5){CHECK(strlen(number)==1);CHECK(number[0]=="01239"[v]);}
  unsigned count;
  if(mode==0){put_string(0,"ch");put_number(1,values[v]);put_string(2,"_allies_briefing");count=3;snprintf(expected,sizeof(expected),"ch%s_allies_briefing",number);}
  else if(mode==1){put_number(0,values[v]);put_string(1,"_");put_string(2,"allies");put_string(3,"_briefing");count=4;snprintf(expected,sizeof(expected),"%s_allies_briefing",number);}
  else if(mode==2){put_string(0,"sw_allies");put_number(1,values[v]);count=2;snprintf(expected,sizeof(expected),"sw_allies%s",number);}
  else {put_string(0,"ch");put_number(1,values[v]);put_string(2,"_allies");put_number(3,2);put_string(4,"_briefing");count=5;snprintf(expected,sizeof(expected),"ch%s_allies2_briefing",number);}
  esp=0x18000+align;uint32_t entry=esp;
  ebx=0xABCDEF01;esi=0xBCDEF012;edi=0xCDEF0123;g_seh_ebp=0xDEF01234;g_fp_top=4;
  PUSH32(esp,count-1);PUSH32(esp,count);PUSH32(esp,0x10000);PUSH32(esp,0x12345678);
  sub_001E0960();
  CHECK(esp==entry-12);CHECK(ebx==0xABCDEF01&&esi==0xBCDEF012&&edi==0xCDEF0123);
  CHECK(g_seh_ebp==0xDEF01234&&g_fp_top==4);CHECK(MEM32(0x20000)==4);
  uint32_t result=MEM32(0x20008);CHECK(MEM32(result+12)==strlen(expected));
  if(strcmp((char*)memory+result+16,expected)){fprintf(stderr,"got %s expected %s\n",memory+result+16,expected);CHECK(0);}
  ++cases;
 }
 printf("%u native Lua concat number/string and ABI cases passed\n",cases);return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-lua-concat-') as directory:
            folder=Path(directory); code=folder/'test.c'; exe=folder/'test.exe'
            code.write_text(prelude+formatter+bodies+harness)
            subprocess.run([compiler,'-O2','-std=c11','-I',str(ROOT),str(code),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    unittest.main()

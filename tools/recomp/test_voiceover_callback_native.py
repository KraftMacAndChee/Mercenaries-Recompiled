"""Retail voice completion: dispatch, callback order, real pool release and ABI."""
from pathlib import Path
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


class VoiceoverCallbackTests(unittest.TestCase):
    def test_retail_callback_and_pool(self):
        config.configure_from_xbe(str(ROOT / 'game_files/mercenaries-retail/default.xbe'))
        raw = (ROOT / 'game_files/mercenaries-retail/default.xbe').read_bytes()
        expected = bytes.fromhex('568b74240856b9b01b3700e8404cffff85c074168d4804518b08e83150ffff56b9b01b3700e856eaffff5ec3')
        offset = config.va_to_file_offset(0x11EA00)
        self.assertEqual(raw[offset:offset + len(expected)], expected)
        manual = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text()
        callback = re.search(r'static void mercenaries_voiceover_stop_callback\(void\)\n\{.*?\n\}', manual, re.S)[0]
        self.assertRegex(manual, r'case 0x0011EA00:\s+return mercenaries_voiceover_stop_callback;')
        seeds = json.loads((ROOT / 'ports/mercenaries/manual-seeds.json').read_text())
        self.assertTrue(any(int(s['start'], 0) == 0x11EA00 for s in seeds))
        generated = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0005.c').read_text()
        helpers = '\n'.join(re.search(r'void sub_' + fn + r'\(void\)\n\{.*?\n\}', generated, re.S)[0]
                            for fn in ('00113650', '0011D480'))
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %u\n",__LINE__);exit(2);}}while(0)
static unsigned char memory[0x400000];
static uint32_t eax,ecx,edx,esi,edi,esp,g_seh_ebp;
#define g_eax eax
#define g_ecx ecx
#define g_esi esi
#define g_esp esp
#define MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define MEM16(a) (*(uint16_t *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define LO16(a) ((uint16_t)(a))
#define ZX16(a) ((uint32_t)(uint16_t)(a))
#define PUSH32(s,v) do{uint32_t t=(v);(s)-=4;MEM32(s)=t;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define RECOMP_TRACE_FUNC(a) ((void)0)
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static void *guest_ptr(uint32_t a){return memory+a;}
static void xbox_preview_log_event(const char *category,const char *format,...) {(void)category;(void)format;}
static unsigned invokes,expected_count,expected_slot;
static void sub_00113A50(void) {
 const uint32_t p=0x371bb0;
 CHECK(ecx==0x12345678);CHECK(MEM32(esp+4)==p+8+expected_slot*0x68+4);
 CHECK(MEM16(p)==expected_count); /* invoke before releasing callback storage */
 CHECK(MEM8(p+4+expected_slot)==1);++invokes;
 eax=0xbad;ecx=0xbad;edx=0xbad;esp+=8;
}
'''
        harness = r'''
int main(void) {
 const uint32_t p=0x371bb0;unsigned cases=0;
 for(unsigned n=0;n<=4;++n)for(unsigned wanted=0;wanted<=5;++wanted)
 for(unsigned saved=0;saved<16;++saved) {
  memset(memory,0,sizeof(memory));MEM16(p)=n;MEM16(p+2)=n;
  for(unsigned i=0;i<n;++i){MEM8(p+4+i)=1;MEM32(p+0x1a8+i*8)=i+1;
   MEM32(p+0x1ac+i*8)=p+8+i*0x68;MEM32(p+8+i*0x68)=0x12345678;}
  expected_count=n;expected_slot=wanted-1;invokes=0;
  esp=0x10000;esi=0x98760000+saved;edi=0x87650000+saved;g_seh_ebp=0x76540000+saved;
  PUSH32(esp,wanted);PUSH32(esp,0x12345678);
  mercenaries_voiceover_stop_callback();
  unsigned hit=wanted>=1&&wanted<=n;
  CHECK(invokes==hit);CHECK(MEM16(p)==n-hit);
  CHECK(esp==0xfffc&&MEM32(esp)==wanted); /* cdecl arg remains */
  CHECK(esi==0x98760000+saved&&edi==0x87650000+saved&&g_seh_ebp==0x76540000+saved);
  if(hit){CHECK(MEM8(p+4+wanted-1)==0);CHECK(MEM16(p+2)==wanted-1);}
  ++cases;
 }
 printf("%u native voice callback/pool/stack cases passed\n",cases);return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-voice-cb-') as directory:
            path = Path(directory)
            (path / 'test.c').write_text(prelude + helpers + callback + harness)
            exe = path / 'test.exe'
            subprocess.run([compiler, '-O2', '-std=c11', str(path / 'test.c'), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()

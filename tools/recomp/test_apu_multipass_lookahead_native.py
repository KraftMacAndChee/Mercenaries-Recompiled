"""Execute bounded APU monitor lookahead, including run518's self-linked voice."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class MultipassLookaheadTests(unittest.TestCase):
    def test_real_lookahead_and_cyclic_negative_control(self):
        text=(ROOT/'src/apu/apu_vp.c').read_text(encoding='utf-8')
        code=re.search(r'static int peek_ahead_multipass_bin\(.*?\n\}',text,re.S)[0]
        prelude=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c))exit(2);}while(0)
typedef long LONG;
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
#define MCPX_HW_MAX_VOICES 256
#define NV_PAVS_VOICE_CFG_FMT 4
#define NV_PAVS_VOICE_CFG_FMT_MULTIPASS (1u<<21)
#define NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN (31u<<16)
#define NV_PAVS_VOICE_TAR_PITCH_LINK 124
#define NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE 65535
typedef struct {unsigned fmt[256],link[256],reads;int ep_frame_div;} MCPXAPUState;
static unsigned voice_get_mask(MCPXAPUState*d,uint16_t v,unsigned reg,unsigned mask){
 CHECK(v<256);++d->reads;unsigned x=reg==4?d->fmt[v]:d->link[v];
 while(!(mask&1)){mask>>=1;x>>=1;}return x&mask;
}
'''
        harness=r'''
int main(void){
 MCPXAPUState d;unsigned cases=0;
 /* All chain lengths, bin values, immediate multipass nodes and terminals. */
 for(unsigned n=1;n<=256;++n)for(unsigned bin=0;bin<32;++bin){
  memset(&d,0,sizeof(d));for(unsigned i=0;i<256;++i)d.link[i]=i+1;
  d.link[n-1]=65535;d.fmt[n-1]=(1u<<21)|(bin<<16);
  CHECK(peek_ahead_multipass_bin(&d,0)==(n==1?-1:(int)bin));
  CHECK(d.reads<=513);++cases;
 }
 memset(&d,0,sizeof(d));d.link[83]=0xE9950053;d.fmt[83]=0xA000E0A4;
 CHECK(peek_ahead_multipass_bin(&d,83)==-1);CHECK(d.reads==2);++cases;
 for(unsigned n=1;n<=256;++n){
  memset(&d,0,sizeof(d));for(unsigned i=0;i<n;++i)d.link[i]=(i+1)%n;
  CHECK(peek_ahead_multipass_bin(&d,0)==-1);CHECK(d.reads==n*2);++cases;
 }
 memset(&d,0,sizeof(d));d.link[0]=256;
 CHECK(peek_ahead_multipass_bin(&d,0)==-1);CHECK(d.reads==2);++cases;
 CHECK(peek_ahead_multipass_bin(&d,65535)==-1);++cases;
 CHECK(peek_ahead_multipass_bin(&d,256)==-1);++cases;
 printf("%u multipass chain cases passed\n",cases);return 0;
}
'''
        # Restore the former unbounded behavior, which hangs on the same self-loop.
        old=re.sub(r'        if \(v >= MCPX_HW_MAX_VOICES \|\| visited\[v\]\) \{.*?        visited\[v\] = true;\n','',code,flags=re.S)
        self.assertNotEqual(code,old)
        compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        for label,body in [('fixed',code),('old',old)]:
            with self.subTest(label=label),tempfile.TemporaryDirectory(prefix='merc-mp-chain-') as directory:
                src,exe=Path(directory)/'check.c',Path(directory)/'check.exe'
                src.write_text(prelude+body+harness,encoding='utf-8')
                result=subprocess.run([compiler,'-std=c11','-O1',str(src),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                if label=='old':
                    with self.assertRaises(subprocess.TimeoutExpired):subprocess.run([str(exe)],capture_output=True,timeout=1)
                else:
                    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
                    self.assertEqual(result.returncode,0,result.stderr);print(result.stdout.strip())


if __name__=='__main__':unittest.main()

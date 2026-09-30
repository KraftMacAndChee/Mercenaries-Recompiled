"""Exercise production texture hashing: byte coverage, padding and throughput."""
from pathlib import Path
import subprocess, tempfile, shutil
ROOT=Path(__file__).resolve().parents[2]
def block(s):
 a=s.index('static uint64_t hash_guest_rows64');return s[a:s.index('static void dump_locked_bgra_texture',a)]
def main():
 s=block((ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8'))
 test=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
'''+s+r'''
static volatile uint64_t sink;
int main(void){
 uint8_t *data=(uint8_t*)malloc(1048576+128); assert(data);
 for(unsigned i=0;i<1048576+128;i++)data[i]=(uint8_t)(i*37u+(i>>7));
 const unsigned widths[]={0,1,7,8,9,31,32,33,63,64,65,127,128,257,1024};
 unsigned checked=0;
 for(unsigned n=0;n<sizeof(widths)/sizeof(*widths);n++){
  unsigned w=widths[n],pitch=w+19;
  for(unsigned offset=0;offset<8;offset++){
   uint64_t h=hash_guest_rows64(data,offset,pitch,w,3);
   assert(h==hash_guest_rows64(data,offset,pitch,w,3));
   for(unsigned row=0;row<3;row++)for(unsigned x=0;x<pitch;x++){
    unsigned i=offset+row*pitch+x;
    data[i]^=0x80;
    uint64_t changed=hash_guest_rows64(data,offset,pitch,w,3);
    assert((x<w)?changed!=h:changed==h); data[i]^=0x80;checked++;
   }
  }
 }
 uint64_t h=hash_guest_rows64(data,3,4096,4096,1);
 for(unsigned i=3;i<4099;i++)for(unsigned bit=0;bit<8;bit++){
  data[i]^=(1u<<bit);assert(hash_guest_rows64(data,3,4096,4096,1)!=h);data[i]^=(1u<<bit);checked++;
 }
 printf("PASS: %u changed-byte/bit and excluded-padding checks; unaligned and row-tail coverage\n",checked);
 LARGE_INTEGER f,a,b;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
 for(unsigned i=0;i<4000;i++){data[i&1048575]^=1;sink=hash_guest_rows64(data,0,1048576,1048576,1);}
 QueryPerformanceCounter(&b);printf("4 GiB texture hash elapsed_ms=%.3f sink=%llu\n",1000.0*(b.QuadPart-a.QuadPart)/f.QuadPart,(unsigned long long)sink);
 free(data);return 0;
}
'''
 with tempfile.TemporaryDirectory(prefix='texture-hash-') as td:
  p=Path(td)/'test.c';exe=p.with_suffix('.exe');p.write_text(test,encoding='utf-8')
  subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,timeout=30)
  old=ROOT/'artifacts/authenticity/run1747-performance/original-texture-hash.c'
  if old.exists():
   p.write_text(test.replace(s,old.read_text(encoding='utf-8')),encoding='utf-8')
   subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(p),'-o',str(exe)],check=True)
   print('Previous hash comparison:',flush=True);subprocess.run([str(exe)],check=True,timeout=30)
if __name__=='__main__':main()

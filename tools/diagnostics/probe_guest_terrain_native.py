"""Run the actual lifted terrain query against an offline guest snapshot only."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    parser.add_argument('coordinates', nargs='+', help='World X,Z pairs')
    args = parser.parse_args()
    if args.snapshot.stat().st_size != 0x4000000:
        parser.error('Expected a complete 64 MiB guest snapshot')
    points = [tuple(map(float, pair.split(','))) for pair in args.coordinates]
    if any(len(point) != 2 for point in points):
        parser.error('Each coordinate must be X,Z')
    source = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0010.c').read_text()
    bodies = [re.search(r'void sub_' + name + r'\(void\)\n\{.*?\n\}', source, re.S)[0]
              for name in ('001F03C0', '001F1530')]
    prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static unsigned char memory[0x4000000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp,g_fp_top;
static double g_fp_stack[8];
static float xmm0v[4],xmm1v[4],xmm2v[4],xmm3v[4],xmm4v[4];
#define xmm0 xmm0v[0]
#define xmm1 xmm1v[0]
#define xmm2 xmm2v[0]
#define xmm3 xmm3v[0]
#define xmm4 xmm4v[0]
static void *at(uint32_t a,size_t n) {if(a>sizeof(memory)-n)exit(5);return memory+a;}
#define MEM32(a) (*(uint32_t*)at((uint32_t)(a),4))
#define MEMF(a) (*(float*)at((uint32_t)(a),4))
#define MEM8(a) (*(uint8_t*)at((uint32_t)(a),1))
#define SMEM16(a) (*(int16_t*)at((uint32_t)(a),2))
#define LO8(v) ((uint8_t)(v))
#define SET_LO8(v,b) ((v)=((v)&0xffffff00u)|(uint8_t)(b))
#define ZX8(v) ((uint32_t)(uint8_t)(v))
#define PUSH32(s,v) do{uint32_t q=(v);(s)-=4;MEM32(s)=q;}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define RECOMP_TRACE_FUNC(v) ((void)0)
#define recomp_xmm_loadss(v,a) ((v)[0]=MEMF(a))
#define recomp_xmm_copy(v,w) memcpy(v,w,sizeof(v))
'''
    harness = r'''
int main(int argc,char **argv) {
 FILE *file=fopen(argv[1],"rb");if(!file)return 2;
 if(fread(memory,1,sizeof(memory),file)!=sizeof(memory))return 3;fclose(file);
 for(int i=2;i+1<argc;i+=2) {
  float x=strtof(argv[i],NULL),z=strtof(argv[i+1],NULL);uint32_t xb,zb;
  memcpy(&xb,&x,4);memcpy(&zb,&z,4);esp=0x700000;g_fp_top=0;
  ebx=0x11111111;esi=0x22222222;edi=0x33333333;
  PUSH32(esp,zb);PUSH32(esp,xb);PUSH32(esp,0);sub_001F1530();
  if(esp!=0x6ffff8||ebx!=0x11111111||esi!=0x22222222||edi!=0x33333333)return 4;
  printf("x=%.8g z=%.8g terrain=%.8g ABI=ok\n",x,z,g_fp_stack[g_fp_top&7u]);
 }
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='merc-terrain-snapshot-') as directory:
        path = Path(directory)
        (path / 'probe.c').write_text(prelude + '\n'.join(bodies) + harness)
        exe = path / 'probe.exe'
        subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-O2', '-std=c11',
                        str(path / 'probe.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(args.snapshot.resolve()),
                        *(str(value) for point in points for value in point)], check=True)


if __name__ == '__main__':
    main()

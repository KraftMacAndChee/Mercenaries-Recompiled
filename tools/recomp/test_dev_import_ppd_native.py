"""Exercise the patched retail PPD loader using its generated production body."""
from pathlib import Path
import shutil, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0009.c').read_text(encoding='utf-8')
start=source.index('void sub_001EB110(void)\n{')
body=source[start:source.index('\n}',start)+2]
assert 'recomp_dev_region_import_active()' in body, 'Regenerate using Patch-Generated.py first'
header=r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static unsigned char mem[0x2000000];
static uint32_t eax,ecx,edx,esp,ebx,esi,edi;
static int active,found,stores;
#define MEM32(a) (*(uint32_t *)(mem+(uint32_t)(a)))
#define MEM16(a) (*(uint16_t *)(mem+(uint32_t)(a)))
#define PUSH32(s,v) do{(s)-=4;MEM32(s)=(v);}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
int recomp_dev_region_import_active(void){return active;}
void sub_001EA770(void){memset(mem+ecx,0,12);esp+=4;}
void sub_001F2A20(void){eax=found?0:UINT32_MAX;esp+=4;}
void sub_001F2A60(void){eax=0;stores++;esp+=4;}
"""
# Keep the actual property-address policy; this fixture exercises retail mode.
header += r"""
int recomp_mod_expanded_world_properties(void){return 0;}
uint32_t xbox_HeapAlloc(uint32_t bytes,uint32_t alignment){assert(0 && "retail mode must not allocate a mod pool");return 0;}
"""
header += '#include "'+(ROOT/'ports/mercenaries/src/recomp_world_properties.c').as_posix()+'"\n'
tail=r"""
static void run_case(int mode,int importing){
 const uint32_t self=0x4bb1a0,table=0x1000000,old=0x4bb100,older=0x4bb080;
 memset(mem,0,sizeof(mem));active=importing;stores=0;found=mode!=0;
 MEM32(0x4434c4)=table;MEM32(0x30ee60)=10;
 if(mode==1){MEM32(table+8)=old;MEM16(old+4)=3;}
 if(mode==2){MEM32(table+8)=old;MEM32(old)=older;MEM16(older+4)=7;}
 if(mode==3){MEM32(table+8)=self;MEM32(self)=self;}
 if(mode==4){MEM32(table+8)=old;}
 esp=0x1800000;MEM32(esp)=0xabc;MEM32(esp+4)=0x9a89627f;
 ecx=self;ebx=111;esi=222;edi=333;
 sub_001EB110();
 assert(esp==0x1800008&&ebx==111&&esi==222&&edi==333);
 assert(MEM32(self+8)==0x4434e0+80&&MEM16(self+4)==0);
 assert(MEM32(self)==(mode==1?old:mode==2?older:0));
 assert(stores==((mode==0||mode==3)?1:0));
 if(stores)assert(MEM32(table+8)==self);
}
int main(void){for(int active=0;active<2;active++)for(int mode=0;mode<5;mode++)if(mode!=3||active)run_case(mode,active);puts("PASS: retail PPD import self-link, valid inheritance, empty-chain and register/stack preservation (9 cases)");}
"""
with tempfile.TemporaryDirectory(prefix='merc-ppd-') as directory:
 c=Path(directory)/'test.c';exe=Path(directory)/'test.exe'
 c.write_text(header+body+tail,encoding='utf-8')
 subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I'+str(ROOT/'src'),'-I'+str(ROOT/'include'),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=5)

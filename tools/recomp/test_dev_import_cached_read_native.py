"""Production file-update body: late developer reads reuse pinned completed IO."""
from pathlib import Path
import subprocess,shutil,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0011.c').read_text(encoding='utf-8');a=s.index('void sub_002228C0(void)\n{');body=s[a:s.index('\n}',a)+2]
assert 'recomp_dev_region_import_active' in body
header=r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static unsigned char mem[0x1000000];
static uint32_t eax,ecx,edx,esp,ebx,esi,edi,g_seh_ebp,g_esp;
static int active;
#define MEM32(a) (*(uint32_t*)(mem+(uint32_t)(a)))
#define PUSH32(s,v) do{(s)-=4;MEM32(s)=(v);}while(0)
#define POP32(s,v) do{(v)=MEM32(s);(s)+=4;}while(0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
#define CMP_G(a,b) ((int32_t)(a)>(int32_t)(b))
#define CMP_L(a,b) ((int32_t)(a)<(int32_t)(b))
int recomp_dev_region_import_active(void){return active;}
void sub_00222310(void){assert(!"Pinned IO unexpectedly released");}
void sub_00222480(void){esp+=4;}
static void link(uint32_t root,uint32_t obj){MEM32(root)=MEM32(root+4)=obj;MEM32(obj)=MEM32(obj+4)=root;MEM32(obj+8)=obj;}
"""
tail=r"""
static void check(int importing,int state,int outside,int done){
 memset(mem,0,sizeof(mem));active=importing;
 const uint32_t file=0x10000,io=0x20000,req=0x21000,buf=0x50000;
 MEM32(0x85bf18)=0x30000;link(file+0x20,io);link(file+0x10,req);
 MEM32(io+0x48)=state;MEM32(io+0x44)=1;MEM32(io+0xc)=buf;MEM32(io+0x20)=100;MEM32(io+0x24)=120;
 MEM32(req+0x10)=outside?99:105;MEM32(req+0x14)=110;MEM32(req+0x18)=105*2048+17;MEM32(req+0x20)=done?2:0;
 const int fulfil=!outside&&!done&&(state==3||(state==4&&importing));
 for(int pass=0;pass<2;pass++){
  ecx=file;esp=0xf00000;ebx=111;esi=222;edi=333;g_seh_ebp=444;
  sub_002228C0();assert(esp==0xf00004&&ebx==111&&esi==222&&edi==333);
  assert(MEM32(io+0x44)==1+(uint32_t)fulfil);
  assert(MEM32(req+0x20)==(uint32_t)((done||fulfil)?2:0));
  if(fulfil){assert(MEM32(req+0x24)==buf+5*2048+17);assert(MEM32(req+0x28)==io);}
 }
}
int main(void){for(int a=0;a<2;a++)for(int s=2;s<=4;s++)for(int o=0;o<2;o++)for(int d=0;d<2;d++)check(a,s,o,d);puts("PASS: cached developer reads, ordinary completion, in-flight exclusion, bounds, single reference increment (24 cases twice)");}
"""
with tempfile.TemporaryDirectory(prefix='merc-cache-') as d:
 c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(header+body+tail,encoding='utf-8')
 subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)

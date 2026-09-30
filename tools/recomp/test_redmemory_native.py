"""Exercise actual disposable RedMemory functions under allocation churn.

Tests content preservation, aligned nonoverlapping blocks, metadata partitioning,
small-pool expansion/compaction and complete reclamation. No generated C edits.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = '\n'.join(p.read_text(encoding='utf-8') for p in (ROOT / 'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'))
    functions = {name: text for text, name in re.findall(
        r'(void (sub_[0-9A-F]{8})\(void\)\n\{.*?\n\})', source, re.S)}
    selected = set()
    def add(name):
        if name in selected or name in ('sub_001F6C20', 'sub_001F6E10'):
            return
        selected.add(name)
        for child in re.findall(r'(sub_[0-9A-F]{8})\(\);', functions[name]):
            add(child)
    for name in ('sub_001F6710','sub_001F6F70','sub_001F70D0','sub_001F6B60','sub_001F6E70'):
        add(name)
    fixture = '#define RECOMP_GENERATED_CODE\n#include "' + (ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix() + '"\n' + r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x4000000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,b) abort()
void sub_001F6C20(void) {fputs("metadata capacity exhausted\n",stderr);abort();}
void sub_001F6E10(void) {fputs("unexpected OOM callback\n",stderr);abort();}
''' + '\n'.join('void '+name+'(void);' for name in sorted(selected)) + '\n' + '\n'.join(functions[name] for name in sorted(selected)) + r'''
enum { POOL=0x645984, TABLE=0x700000, CAPACITY=7000, START=0x1000000, SIZE=0x1000000, STACK=0x3f00000, SLOTS=1200 };
typedef struct {uint32_t start,size;} Block;
static Block intervals[CAPACITY];
static uint32_t sequence=0x98273211, operations, peak_pools;
static uint32_t rng(void) {sequence^=sequence<<13;sequence^=sequence>>17;sequence^=sequence<<5;return sequence;}
static int order(const void *a,const void *b) {uint32_t x=((const Block*)a)->start,y=((const Block*)b)->start;return (x>y)-(x<y);}
static void begin(void) {esp=STACK;ebx=0x11223344;esi=0x55667788;edi=0x99aabbcc;ecx=POOL;}
static void end(unsigned pop) {assert(esp==STACK+pop&&ebx==0x11223344&&esi==0x55667788&&edi==0x99aabbcc);operations++;}
static void audit(void) {
 unsigned freeend=MEM32(POOL+0x18), allocbegin=MEM32(POOL+0x1c), n=0;uint32_t freebytes=0;
 assert(freeend<allocbegin&&allocbegin<=CAPACITY);
 for(unsigned i=0;i<freeend;i++){Block b={MEM32(TABLE+8*i),MEM32(TABLE+8*i+4)};freebytes+=b.size;if(b.size)intervals[n++]=b;}
 for(unsigned i=allocbegin;i<CAPACITY;i++)intervals[n++]=(Block){MEM32(TABLE+8*i),MEM32(TABLE+8*i+4)};
 assert(freebytes==MEM32(POOL+8));qsort(intervals,n,sizeof(Block),order);
 uint32_t position=START;for(unsigned i=0;i<n;i++){assert(intervals[i].start==position&&intervals[i].size%16==0);position+=intervals[i].size;}assert(position==START+SIZE);
 unsigned pools=MEM32(0x64592c);assert(pools<=256);if(pools>peak_pools)peak_pools=pools;
 for(unsigned i=0;i<pools;i++){
  uint32_t p=0x643928+32*i,start=MEM32(p),stop=MEM32(p+4),width=MEM32(p+12),count=0,node=MEM32(p+8);
  unsigned char seen[8192]={0};assert(width&&MEM32(p+20)<=MEM32(p+16));
  while(node){assert(node>=start&&node+width<=stop&&(node-start)%width==0);unsigned index=(node-start)/width;assert(index<8192&&!seen[index]);seen[index]=1;count++;node=MEM32(node);}
  assert(count==MEM32(p+20));
 }
}
static void init(void) {
 memset(memory,0,sizeof(memory));begin();MEM32(STACK+4)=START;MEM32(STACK+8)=SIZE;MEM32(STACK+12)=TABLE;MEM32(STACK+16)=CAPACITY;MEM32(STACK+20)=16;sub_001F6710();end(24);
 const unsigned widths[]={16,32,64,128,192,256,320,384,448,512,576,640};
 for(unsigned i=0;i<12;i++){begin();MEM32(STACK+4)=widths[i];MEM32(STACK+8)=4;MEM32(STACK+12)=1;sub_001F6E70();end(4);}audit();
}
static uint32_t allocate(unsigned bytes,unsigned temp) {
 begin();MEM32(STACK+4)=bytes;MEM32(STACK+8)=0;MEM32(STACK+12)=0;MEM32(STACK+16)=0;
 if(temp)sub_001F70D0();else sub_001F6F70();end(4);return eax;
}
static void release(uint32_t p) {begin();MEM32(STACK+4)=p;sub_001F6B60();end(4);}
typedef struct {uint32_t p,size;unsigned char pattern;} Live;
static Live live[SLOTS];
static void verify(Live b) {for(unsigned j=0;j<b.size;j++)assert(memory[b.p+j]==b.pattern);}
int main(void) {
 g_xbox_mem_offset=(ptrdiff_t)memory;init();uint32_t baseline=MEM32(POOL+8);
 /* Deliberately create several differently sized extra pools, then remove
    early descriptors so the actual overlapping record shift is exercised. */
 for(unsigned i=0;i<400;i++){unsigned size=i<200?600:208;live[i]=(Live){allocate(size,0),size,(unsigned char)(i+1)};assert(live[i].p);memset(memory+live[i].p,live[i].pattern,size);}audit();
 for(unsigned i=200;i<400;i++){verify(live[i]);release(live[i].p);live[i].p=0;}audit();
 for(unsigned i=0;i<200;i++){verify(live[i]);release(live[i].p);live[i].p=0;}audit();assert(MEM32(0x64592c)==12&&MEM32(POOL+8)==baseline);
 for(unsigned i=0;i<120000;i++){
  unsigned index=rng()%SLOTS;Live *b=&live[index];
  if(b->p){verify(*b);release(b->p);b->p=0;}else{
   unsigned size=(rng()%5)?1+rng()%640:641+rng()%16384;
   b->p=allocate(size,(rng()%8)==0);assert(b->p&&b->p%16==0);b->size=size;b->pattern=(unsigned char)rng();memset(memory+b->p,b->pattern,size);
  }
  if(i%127==0){audit();for(unsigned j=0;j<SLOTS;j++)if(live[j].p)verify(live[j]);}
 }
 for(unsigned i=0;i<SLOTS;i++)if(live[i].p){verify(live[i]);release(live[i].p);}audit();
 assert(MEM32(0x64592c)==12&&MEM32(POOL+8)==baseline);
 /* Oversized main allocation must fail without damaging its partition. */
 assert(allocate(SIZE+16,1)==0);audit();release(0);release(1);audit();
 printf("PASS %u guest allocator calls; peak %u pools; contents, ABI, partition, free lists and reclamation\n",operations,peak_pools);
}
'''
    with tempfile.TemporaryDirectory(prefix='redmemory-native-') as temp:
        p=Path(temp);(p/'fixture.c').write_text(fixture)
        cc=shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
        subprocess.run([cc,'-O2','-fno-strict-aliasing',str(p/'fixture.c'),'-o',str(p/'test.exe')],check=True)
        subprocess.run([str(p/'test.exe')],check=True,timeout=90)
        vs=Path('C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/Tools/VsDevCmd.bat')
        if vs.exists():
            cmd=p/'build-msvc.cmd'
            cmd.write_text(f'@call "{vs}" -arch=x64 -host_arch=x64 >nul\n@cl /nologo /O2 /std:c11 /Fe:"{p / "test-msvc.exe"}" /Fo:"{p / "fixture.obj"}" "{p / "fixture.c"}"\n',encoding='utf-8')
            subprocess.run(['cmd.exe','/c',str(cmd)],check=True,timeout=60)
            subprocess.run([str(p/'test-msvc.exe')],check=True,timeout=90)

    print(f'Validated {len(selected)} actual lifted functions')


if __name__=='__main__':
    main()

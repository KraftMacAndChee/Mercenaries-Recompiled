"""Exercise XMem fallback ownership with actual lifted main-pool alloc/free."""
from pathlib import Path
import re,subprocess,tempfile
import test_lua_pool_fallback_native as lua
ROOT=Path(__file__).resolve().parents[2]
def main():
 source='\n'.join(p.read_text() for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('recomp_*.c'))
 funcs={n:t for t,n in re.findall(r'(void (sub_[0-9A-F]{8})\(void\)\n\{.*?\n\})',source,re.S)}
 selected=set()
 def add(n):
  if n in selected or n=='sub_001F6C20':return
  selected.add(n)
  for child in re.findall(r'(sub_[0-9A-F]{8})\(\);',funcs[n]):add(child)
 for n in ['sub_001F6710','sub_001F6D80','sub_001F6970']:add(n)
 manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
 context=manual[manual.index('typedef struct recomp_saved_guest_cpu_context'):manual.index('#include "voice_callback_queue.h"')]
 pool=manual[manual.index('typedef struct merc_lua_pool_block'):manual.index('static uint32_t merc_lua_allocate(')]
 audio=manual[manual.index('static merc_lua_pool_block *g_merc_audio_pool_blocks'):manual.index('/*\n * Lua 5 luaS_newlstr')]
 extra=r"""
static int original_allocs,original_frees;
void sub_00229343(void){original_allocs++;eax=ordinary_enabled?ordinary_address:0;esp+=12;}
void sub_002293E3(void){original_frees++;esp+=12;}
"""
 harness=r"""
enum{POOL=0x645984,PTABLE=0x700000,STACK=0x87D000};
static void cpu(void){esp=STACK;eax=11;ecx=22;edx=33;ebx=44;esi=55;edi=66;g_seh_ebp=77;}
static uint32_t alloc(uint32_t n,uint32_t attr){cpu();MEM32(esp+4)=n;MEM32(esp+8)=attr;sub_001787C0();assert(esp==STACK+12&&ecx==22&&edx==33&&ebx==44&&esi==55&&edi==66&&g_seh_ebp==77);return eax;}
static void release(uint32_t p,uint32_t attr){cpu();MEM32(esp+4)=p;MEM32(esp+8)=attr;sub_001787D0();assert(esp==STACK+12&&ecx==22&&edx==33&&ebx==44&&esi==55&&edi==66&&g_seh_ebp==77);}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;
 assert(!alloc(64,0x6484A000)); /* before main-pool initialization */
 cpu();ecx=POOL;MEM32(esp+4)=0x1000000;MEM32(esp+8)=0x1000000;MEM32(esp+12)=PTABLE;MEM32(esp+16)=7000;MEM32(esp+20)=64;sub_001F6710();
 uint32_t free_before=MEM32(POOL+8);
 ordinary_enabled=1;assert(alloc(64,0x6484A000)==ordinary_address&&!g_merc_audio_pool_blocks);release(ordinary_address,0x6484A000);assert(original_frees==1);
 ordinary_enabled=0;
 const uint32_t attrs[]={0x6484800B,0x6484800D,0x6484800E,0x6484800F,0x64848010,0x64848011,0x64848015,0x6484A000,0x6484A002,0x6484A001,0x6484A003,0x6484A004,0x6484A005,0x6484A006,0x6482A000};
 for(unsigned round=0;round<100;round++){
  uint32_t p[15];
  for(unsigned i=0;i<15;i++){
   unsigned size=64+i*4111;p[i]=alloc(size,attrs[i]);assert(p[i]>=0x1000000&&!(p[i]&63)&&g_merc_audio_pool_blocks);
   for(unsigned j=0;j<size;j++)assert(!MEM8(p[i]+j));memset(guest_ptr(p[i]),0xA5,size);
  }
  for(unsigned i=15;i>0;i--)release(p[i-1],attrs[i-1]);
  assert(!g_merc_audio_pool_blocks&&MEM32(POOL+8)==free_before&&original_frees==1);
 }
 assert(!alloc(64,0xBE800000)&&!alloc(64,0x64800000)&&!alloc(0,0x6484A000));
 assert(!alloc(0x4000001,0x6484A000)&&!g_merc_audio_pool_blocks);
 uint32_t saved=MEM32(POOL+0x1c);MEM32(POOL+0x1c)=MEM32(POOL+0x18)+1;assert(!alloc(64,0x6484A000));MEM32(POOL+0x1c)=saved;
 assert(MEM32(POOL+8)==free_before);puts("PASS: original success unchanged; 1500 audio fallbacks zeroed/aligned/reclaimed; physical/unknown/empty/oversize/early/metadata failures rejected; guest ABI preserved");
}
"""
 fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+lua.PRELUDE+context+'\n'.join('void '+n+'(void);' for n in selected)+pool+audio+extra+funcs['sub_001787C0']+funcs['sub_001787D0']+'\n'.join(funcs[n] for n in selected)+harness
 with tempfile.TemporaryDirectory(prefix='merc-audio-pool-') as td:
  p=Path(td);(p/'test.c').write_text(fixture)
  subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-fno-strict-aliasing','-I',str(ROOT/'ports/mercenaries/src'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
  subprocess.run([str(p/'test.exe')],check=True)
if __name__=='__main__':main()

"""Targeted fringe ordering preserves all unrelated queues and render state."""
from pathlib import Path
import re,subprocess,tempfile
import test_lua_pool_fallback_native as lua
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
context=s[s.index('typedef struct recomp_saved_guest_cpu_context'):s.index('#include "voice_callback_queue.h"')]
helper=s[s.index('static int recomp_is_jennifer_fringe('):s.index('void recomp_redmodel_queue_checkpoint(')]
pre=r"""
static uint16_t guest_u16(uint32_t a){return MEM16(a);}
static unsigned begins,draws,ends;
static uint32_t drawn[16];
static void begin(void){begins++;assert(ecx==0x600000);eax=100;esp+=4;}
static void draw(void){drawn[draws++]=MEM32(esp+4);assert(ecx==0x600000);eax=200;esp+=8;}
static void end(void){ends++;assert(ecx==0x600000);eax=300;esp+=4;}
recomp_func_t recomp_lookup(uint32_t a){return a==0x200001?begin:a==0x200002?draw:a==0x209040?end:NULL;}
"""
harness=r"""
static void item(uint32_t p,int match){
 MEM32(p+0x60)=p+0x100;MEM32(p+0x70)=p+0x200;MEM32(p+0x6c)=0x1101;
 MEM32(p+0x114)=match?30:31;MEM32(p+0x130)=0xef72ce17;MEM32(p+0x224)=0xef72ce17;
}
int main(void){
 g_xbox_mem_offset=(ptrdiff_t)memory;uint32_t head=0x7acdd8+(2*38+20)*4;
 MEM32(0x8429b0+20*4)=0x600000;MEM32(0x600000)=0x610000;
 MEM32(0x610008)=0x200001;MEM32(0x61000c)=0x200002;MEM32(0x610010)=MEM32(0x610014)=0x209040;
 for(unsigned i=0;i<4;i++)item(0x700000+i*0x400,i%2==0);
 MEM32(head)=0x700000;MEM32(0x700090)=0x700400;MEM32(0x700490)=0x700800;MEM32(0x700890)=0x700c00;
 MEM32(0x7acdd8+(2*38+13)*4)=0x750000;item(0x750000,1); /* same signature in a non-skinned group is untouched */
 for(unsigned i=0;i<5;i++)for(unsigned j=0;j<38;j++)if(i!=2)MEM32(0x7acdd8+(i*38+j)*4)=0x12345678;
 esp=0x870000;eax=11;ecx=22;edx=33;ebx=44;esi=55;edi=66;g_seh_ebp=77;
 recomp_render_hair_before_glass();
 assert(begins==1&&draws==2&&ends==1&&drawn[0]==0x700000&&drawn[1]==0x700800);
 assert(MEM32(head)==0x700400&&MEM32(0x700490)==0x700c00&&MEM32(0x700c90)==0);
 assert(MEM32(0x7acdd8+(2*38+13)*4)==0x750000);
 assert(esp==0x870000&&eax==11&&ecx==22&&edx==33&&ebx==44&&esi==55&&edi==66&&g_seh_ebp==77);
 for(unsigned i=0;i<4;i++)assert(MEM32(0x700000+i*0x400+0x6c)==0x1101);
 for(unsigned i=0;i<5;i++)for(unsigned j=0;j<38;j++)if(i!=2)assert(MEM32(0x7acdd8+(i*38+j)*4)==0x12345678);
 recomp_render_hair_before_glass();assert(draws==2); /* not drawn twice */
 MEM32(head)=0x700000;MEM32(0x600108)=4;recomp_render_hair_before_glass();assert(draws==2&&MEM32(head)==0x700000);
 MEM32(0x600108)=0;MEM32(0x610014)=0x222222;recomp_render_hair_before_glass();assert(draws==2); /* refuse shader with stateful end dependency */
 MEM32(0x610014)=0x209040;MEM32(0x700224)=0;recomp_render_hair_before_glass();assert(draws==2);
 MEM32(0x700224)=0xef72ce17;MEM32(0x70006c)=0x1111;recomp_render_hair_before_glass();assert(draws==2);
 assert(!recomp_is_jennifer_fringe(0)&&!recomp_is_jennifer_fringe(0x3ffffff));
 puts("PASS: exact fringe draws once, correct shader Begin/Render/End, other queues/material/depth state unchanged, disabled/stateful shaders excluded, guest ABI preserved");
}
"""
fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+lua.PRELUDE+context+pre+helper+harness
with tempfile.TemporaryDirectory(prefix='merc-hair-order-') as td:
 p=Path(td);(p/'test.c').write_text(fixture)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O2','-fno-strict-aliasing','-I',str(ROOT/'ports/mercenaries/src'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)

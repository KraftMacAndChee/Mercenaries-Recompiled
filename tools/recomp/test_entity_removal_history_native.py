"""Exercise actual removal diagnostics; never mutate or suppress guest state."""
from pathlib import Path
import shutil, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
def main():
    source = (ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
    start = source.index('typedef struct RecompEntityRemovalSample')
    end = source.index('uint32_t recomp_havok_callback_count_checkpoint', start)
    body = source[start:end]
    prelude = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static unsigned char ram[0x4000000];
static unsigned lines, flushes;
static char saved_context[16384];
static uint32_t guest_u32(uint32_t p) {
 assert(p<=sizeof(ram)-4);uint32_t v;memcpy(&v,ram+p,4);return v;
}
static void put(uint32_t p,uint32_t v){memcpy(ram+p,&v,4);}
static void xbox_preview_log_event(const char *cat,const char *fmt,...) {
 (void)cat;(void)fmt;++lines;
}
static void xbox_preview_log_set_crash_context(const char *text) {
 assert(strstr(text,"[HAVOK-ENTITY-HISTORY]"));if(!flushes)assert(strstr(text,"133F0000"));
 assert(strlen(text)<16384);strcpy(saved_context,text);++flushes;
}
"""
    tail = r"""
int main(void){
 const uint32_t e=0x20000;unsigned char before[0xA4];
 put(e,0x2F1090);put(e+4,0x200C0);
 put(e-16,0xA110CA7E);put(e+0xBC,0xDEADBEEF);put(0x90000,0x12345678);
 for(unsigned a=0x80;a<=0x98;a+=12)put(e+a+8,0x80000000);
 memcpy(before,ram+e,sizeof(before));
 for(unsigned i=0;i<100000;i++)recomp_entity_removal_checkpoint(i%5,e,0x30000,0x90000);
 assert(!lines&&!flushes&&g_entity_removal_history_count==100000);
 assert(!memcmp(before,ram+e,sizeof(before)));
 // Real, nonempty owned/nonowned arrays and last valid RAM element.
 for(unsigned flags=0;flags<2;flags++){
  put(e+0x8C,0x3FFFFFC);put(e+0x90,1);put(e+0x94,1|(flags<<31));
  recomp_entity_removal_checkpoint(2,e,0x30000,0x90000);assert(!lines);
 }
 // Retained crash signature: null constraint data, huge positive count.
 put(e+0x8C,0);put(e+0x90,0x133F0000);put(e+0x94,0x80000000);
 memcpy(before,ram+e,sizeof(before));
 recomp_entity_removal_checkpoint(0,e,0x30000,0x90000);
 assert(flushes==1&&lines==1);
 assert(strstr(saved_context,"[HAVOK-ENTITY-OBJECT] 0001FFF0: A110CA7E"));
 assert(strstr(saved_context,"DEADBEEF"));
 assert(strstr(saved_context,"[HAVOK-ENTITY-STACK] 00090000: 12345678"));
 assert(g_entity_removal_history[(g_entity_removal_history_count-1)&63].invalid==2);
 assert(!memcmp(before,ram+e,sizeof(before)));
 // Overflow must be detected without reading array entries, invalid owners
 // must not be dereferenced, and reports remain bounded.
 put(e+0x8C,0xFFFFFFFC);put(e+0x90,1);put(e+0x94,1);
 recomp_entity_removal_checkpoint(1,e,0x30000,0x90000);
 recomp_entity_removal_checkpoint(2,0xFFFFFFFF,0x30000,0x90000);
 recomp_entity_removal_checkpoint(3,0x3FFFF60,0x30000,0x90000);
 assert(flushes==1&&lines==4);
 for(unsigned i=0;i<1000;i++)recomp_entity_removal_checkpoint(0,e,0,0);
 assert(flushes==1&&lines==4);
 // First-failure snapshots must also tolerate unmapped and end-of-RAM pointers.
 g_entity_removal_reports=0;
 recomp_entity_removal_checkpoint(0,0xFFFFFFFF,0,0xFFFFFFF0);
 assert(flushes==2&&!strstr(saved_context,"[HAVOK-ENTITY-OBJECT]")&&!strstr(saved_context,"[HAVOK-ENTITY-STACK]"));
 g_entity_removal_reports=0;
 recomp_entity_removal_checkpoint(0,0x3FFFF60,0,0x3FFFFC0);
 assert(flushes==3&&!strstr(saved_context,"[HAVOK-ENTITY-OBJECT]"));
 assert(strstr(saved_context,"[HAVOK-ENTITY-STACK] 03FFFFF0:"));
 return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix='merc-entity-history-') as t:
        c=Path(t)/'test.c';exe=Path(t)/'test.exe'
        c.write_text(prelude+body+tail,encoding='utf-8')
        subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    generated=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0008.c').read_text(encoding='utf-8')
    start=generated.index('void sub_001C04D0(void)')
    body=generated[start:generated.index('void sub_001C0580(void)',start)]
    assert body.count('recomp_entity_removal_checkpoint(')==10 # declaration + call
    print('PASS: 100000 healthy checkpoints without output; impossible arrays and owners; read-only and bounded failure history; five regeneration hooks')
if __name__=='__main__':main()

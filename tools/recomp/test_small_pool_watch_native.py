"""Compile the production opt-in observer; never attach to a game process."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SmallPoolWatchTests(unittest.TestCase):
    def test_read_only_observer(self):
        source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        observer = source[source.index('#define MERC_SMALL_POOL_WATCH_SLOTS'):source.index('void recomp_guest_watch_trace(')]
        prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ptrdiff_t g_xbox_mem_offset;
static uint32_t g_ecx,g_esp,g_ebx,g_esi,g_edi;
static uint32_t g_recomp_recent_game_func_idx,g_recomp_recent_game_funcs[256];
static unsigned env_calls, mode;
static char *test_getenv(const char *name) {
    ++env_calls;
    if(strcmp(name,"MERCENARIES_TRACE_SMALL_POOL_OWNERSHIP")) abort();
    return mode ? "1" : NULL;
}
#define getenv test_getenv
#define CHECK(x) do {if(!(x)){fprintf(stderr,"CHECK line%d: %s\n",__LINE__,#x);return 2;}}while(0)
'''
        harness = r'''
static uint32_t *word(uint32_t a) {return (uint32_t *)(g_xbox_mem_offset+a);}
static int observe(uint32_t fn) {
    unsigned char before[1024], pool[32], stack[8];
    memcpy(before,word(0x9C0000),sizeof(before));
    memcpy(pool,word(0x643928),sizeof(pool));
    memcpy(stack,word(g_esp),sizeof(stack));
    recomp_small_pool_watch(fn);
    CHECK(!memcmp(before,word(0x9C0000),sizeof(before)));
    CHECK(!memcmp(pool,word(0x643928),sizeof(pool)));
    CHECK(!memcmp(stack,word(g_esp),sizeof(stack)));
    return 0;
}
int main(int argc,char **argv) {
    CHECK(argc==2);mode=(unsigned)atoi(argv[1]);
    g_ecx=0x643928;g_esp=0x800000;
    if(!mode){
        /* Invalid base proves the disabled observer performs no guest reads. */
        g_xbox_mem_offset=1;
        for(unsigned i=0;i<10000;++i)recomp_small_pool_watch(0x1F66C0);
        CHECK(env_calls==1);return 0;
    }
    g_xbox_mem_offset=(ptrdiff_t)calloc(1,0x4000000);CHECK(g_xbox_mem_offset);
    uint32_t *pool=word(g_ecx);
    pool[0]=0x9C0000;pool[1]=0x9C0400;pool[2]=pool[0];
    pool[3]=16;pool[4]=64;pool[5]=64;
    for(unsigned i=0;i<64;++i)*word(pool[0]+i*16)=i==63?0:pool[0]+(i+1)*16;
    /* Normal observations; only the harness performs retail link updates. */
    for(unsigned i=0;i<1000;++i){
        uint32_t address=pool[2];CHECK(!observe(0x1F66C0));
        pool[2]=*word(address);--pool[5];
        *word(address)=0xAABBCCDD;
        *word(g_esp+4)=address;CHECK(!observe(0x1F6AB0));
        *word(address)=pool[2];pool[2]=address;++pool[5];
    }
    CHECK(env_calls==1);
    if(mode==1){free((void *)g_xbox_mem_offset);return 0;}
    if(mode==2){
        *word(pool[2])=0;CHECK(!observe(0x1F66C0));
    }else if(mode==3){
        *word(g_esp+4)=pool[2];CHECK(!observe(0x1F6AB0));
    }else if(mode==4){
        pool[2]=0;CHECK(!observe(0x1F66C0));
    }else if(mode==5){
        pool[2]=0;pool[5]=0;CHECK(!observe(0x1F66C0));
    }else if(mode==6){
        pool[2]=0xFFFFFFFF;
        for(unsigned i=0;i<100;++i)CHECK(!observe(0x1F66C0));
    }else if(mode==7){
        pool[2]=pool[0]+16;*word(pool[2])=0xFFFFFFFF;
        CHECK(!observe(0x1F66C0));
    }else if(mode==8){
        CHECK(!observe(0x1F66C0));CHECK(!observe(0x1F66C0));
    }else if(mode==9){
        *word(g_esp+4)=0xFFFFFFFF;CHECK(!observe(0x1F6AB0));
    }else if(mode==10){
        pool[1]=0xFFFFFFFF;CHECK(!observe(0x1F66C0));
        pool[1]=0x9C0400;pool[3]=0;CHECK(!observe(0x1F66C0));
    }
    free((void *)g_xbox_mem_offset);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-small-pool-watch-') as directory:
            c = Path(directory) / 'observer.c'
            exe = c.with_suffix('.exe')
            c.write_text(prelude + observer + harness, encoding='utf-8')
            build = subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-std=c11', '-O2', str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            expected = {2: 'free-link-overwritten', 3: 'double-free', 4: 'invalid-or-empty-head',
                        6: 'invalid-or-empty-head', 7: 'invalid-next-link', 8: 'allocate-already-live',
                        9: 'invalid-or-empty-head'}
            for mode in range(11):
                with self.subTest(mode=mode):
                    run = subprocess.run([str(exe), str(mode)], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    if mode in expected:
                        self.assertIn('reason=' + expected[mode], run.stderr)
                        self.assertEqual(run.stderr.count('[SMALL-POOL-OWNERSHIP]'), 16 if mode == 6 else 1)
                    else:
                        self.assertEqual(run.stderr, '')


if __name__ == '__main__':
    unittest.main(verbosity=2)

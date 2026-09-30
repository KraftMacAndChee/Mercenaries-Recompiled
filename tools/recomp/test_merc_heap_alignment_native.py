"""Exercise production ordinary/kernel heaps against sub-page fragmentation.

No game process is opened. The legacy alignment is compiled separately to
prove that the same native regression fails before the policy correction.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OrdinaryHeapAlignmentTests(unittest.TestCase):
    def test_fragmentation_alignment_and_ownership(self):
        kernel = (ROOT/'src/kernel/xbox_memory_layout.c').read_text(encoding='utf-8')
        manual = (ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        structures = kernel[kernel.index('#define XBOX_HEAP_MAX_ALLOCS'):kernel.index('static void xbox_HeapCaptureOwner(')]
        backing = kernel[kernel.index('static xbox_heap_allocation *xbox_HeapAppendAllocation('):kernel.index('HANDLE xbox_GetMappingHandle(')]
        ordinary = manual[manual.index('#define MERC_HEAP_ARENA_START'):manual.index('/* Lua normally uses the isolated title heap')]
        self.assertEqual(ordinary.count('xbox_HeapAlloc(capacity, 16u)'), 1)
        ordinary = ordinary.replace('malloc((size_t)capacity', 'metadata_malloc((size_t)capacity')
        prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);return 2;}}while(0)
#define XBOX_HEAP_BASE 0x008C0000u
#define XBOX_HEAP_SIZE (0x04000000u-XBOX_HEAP_BASE)
#define XBOX_CPU_ALIAS_BASE 0x80000000u
#define XBOX_CPU_ALIAS_END  0xC0000000u
typedef unsigned SRWLOCK;
#define SRWLOCK_INIT 0
#define AcquireSRWLockExclusive(x) ((void)(x))
#define ReleaseSRWLockExclusive(x) ((void)(x))
#define AcquireSRWLockShared(x) ((void)(x))
#define ReleaseSRWLockShared(x) ((void)(x))
static char *recomp_patch_mission_script(const char *s,size_t n,size_t *out) {(void)s;(void)n;(void)out;return NULL;}
static int fail_metadata;
static void *metadata_malloc(size_t n){return fail_metadata ? NULL : malloc(n);}
static uint32_t xbox_GetGraphicsMemorySize(void) { return 0x04000000u; }
static uintptr_t g_memory_offset;
static uint32_t g_recomp_current_func;
static volatile uint32_t g_recomp_recent_game_funcs[256];
static volatile uint32_t g_recomp_recent_game_func_idx;
static void *guest_ptr(uint32_t a){return (void*)(g_memory_offset+a);}
static uint32_t guest_u32(uint32_t a){return *(uint32_t*)guest_ptr(a);}
'''
        stubs = r'''
static void xbox_HeapCaptureOwner(xbox_heap_allocation*p){memset(p->owner_ring,0,sizeof(p->owner_ring));}
static int xbox_HeapTraceEnabled(void){return 0;}
static void xbox_HeapDumpCensus(void){}
'''
        harness = r'''
static void reset(void){
 xbox_HeapReset();
 if(g_merc_heap_blocks!=g_merc_heap_initial_blocks)free(g_merc_heap_blocks);
 g_merc_heap_blocks=g_merc_heap_initial_blocks;
 g_merc_heap_block_capacity=MERC_HEAP_INITIAL_BLOCKS;fail_metadata=0;
 memset(g_merc_heap_blocks,0,g_merc_heap_block_capacity*sizeof(*g_merc_heap_blocks));
 g_merc_heap_block_count=0;g_merc_heap_initialized=0;g_merc_heap_oom_reported=0;
}
int main(void){
 g_memory_offset=(uintptr_t)calloc(1,0x04000000u);CHECK(g_memory_offset);
 reset();CHECK(xbox_HeapAlloc(0x100000,4096)==XBOX_HEAP_BASE);
 CHECK(merc_heap_allocate(1,0,MERC_HEAP_ARENA_END-MERC_HEAP_ARENA_START)==MERC_HEAP_ARENA_START);
 /* Leave900 genuine tracked sub-page holes, then exhaust the frontier. */
 for(unsigned i=0;i<900;++i)CHECK((xbox_HeapAlloc(16,4096)&4095)==0);
 uint32_t remaining=XBOX_HEAP_BASE+XBOX_HEAP_SIZE-g_heap_next;
 CHECK(remaining>0 && xbox_HeapAlloc(remaining,16));
 CHECK(g_heap_next==0x04000000u);
 uint64_t holes=0;
 for(int i=0;i<g_heap_alloc_count;++i)
  if(!g_heap_allocations[i].in_use)holes+=g_heap_allocations[i].allocation_size;
 CHECK(holes>3000000);
 uint32_t first=merc_heap_allocate(1,0,48);
#ifdef LEGACY
 CHECK(first==0);puts("Legacy4096 alignment reproduces48-byte OOM with>3MB free");
 free((void*)g_memory_offset);return 0;
#else
 CHECK(first && !(first&15) && (first&4095));CHECK(merc_heap_free(first));
 const unsigned sizes[]={0,1,15,16,17,48,127,257};uint32_t addresses[1024];
 for(unsigned i=0;i<1024;++i){
  unsigned n=sizes[i%8],cap=0;addresses[i]=merc_heap_allocate(1,MERC_HEAP_ZERO_MEMORY,n);
  CHECK(addresses[i] && !(addresses[i]&15));CHECK(merc_heap_size(addresses[i],&cap));
  CHECK(cap==((n?n:1)+15u)/16u*16u);
  CHECK(xbox_HeapGetAllocationSize(addresses[i])==cap);
  for(unsigned j=0;j<cap;++j)CHECK(((unsigned char*)guest_ptr(addresses[i]))[j]==0);
  memset(guest_ptr(addresses[i]),(i%251)+1,cap);
 }
 for(unsigned i=0;i<1024;++i){
  unsigned n=sizes[i%8],cap=((n?n:1)+15u)/16u*16u;
  for(unsigned j=0;j<cap;++j)CHECK(((unsigned char*)guest_ptr(addresses[i]))[j]==(i%251)+1);
 }
 uint32_t a=addresses[7];CHECK(merc_heap_reallocate(1,MERC_HEAP_REALLOC_IN_PLACE_ONLY,a,700)==0);
 uint32_t moved=merc_heap_reallocate(1,MERC_HEAP_ZERO_MEMORY,a,700);CHECK(moved && moved!=a);
 for(unsigned j=0;j<257;++j)CHECK(((unsigned char*)guest_ptr(moved))[j]==8);
 for(unsigned j=257;j<700;++j)CHECK(((unsigned char*)guest_ptr(moved))[j]==0);
 CHECK(xbox_HeapGetAllocationSize(a)==0);addresses[7]=moved;
 CHECK(merc_heap_reallocate(1,0,moved,24)==moved);
 uint32_t cap=0;CHECK(merc_heap_size(moved,&cap) && cap==704);
 CHECK(merc_heap_allocate(1,0,UINT32_MAX)==0);
 CHECK(merc_heap_reallocate(1,0,moved,UINT32_MAX)==0);
 for(unsigned i=0;i<1024;++i){CHECK(merc_heap_free(addresses[i]));CHECK(!xbox_HeapGetAllocationSize(addresses[i]));}
 CHECK(!merc_heap_free(moved));
 CHECK(g_merc_heap_block_count==1);CHECK(g_heap_oom_count==0);
 /* Ordinary large requests also retain16-byte alignment; explicitly
  * contiguous requests retain their own page alignment and alias release. */
 reset();CHECK(xbox_HeapAlloc(0x100000,4096)==XBOX_HEAP_BASE);
 CHECK(merc_heap_allocate(1,0,MERC_HEAP_ARENA_END-MERC_HEAP_ARENA_START));
 a=merc_heap_allocate(1,0,8193);CHECK(a && !(a&15));
 CHECK(merc_heap_size(a,&cap) && cap==8208);CHECK(merc_heap_free(a));
 a=xbox_HeapAlloc(8192,4096);CHECK(a && !(a&4095));
 xbox_HeapFree(a|0x80000000u);CHECK(!xbox_HeapGetAllocationSize(a));
 /* Reproduce the dump's full ordinary table with only 16-byte holes.
  * Guest RAM still has room, so a new 20-byte Lua object must succeed. */
 reset();CHECK(xbox_HeapAlloc(0x100000,4096)==XBOX_HEAP_BASE);
 merc_heap_initialize(1);
 g_merc_heap_block_count=MERC_HEAP_INITIAL_BLOCKS;
 for(unsigned i=0;i<g_merc_heap_block_count;++i){
  g_merc_heap_blocks[i]=(merc_heap_block_t){MERC_HEAP_ARENA_START+i*16,16,16,1};
 }
 for(unsigned i=0;i<262;++i){g_merc_heap_blocks[2*i].in_use=0;g_merc_heap_blocks[2*i].requested=0;}
 /* Emulate host allocation failure at the old ceiling. Nothing is lost. */
 fail_metadata=1;uint32_t frontier=g_heap_next;
 CHECK(merc_heap_allocate(1,0,20)==0);
 CHECK(g_heap_next==frontier && g_merc_heap_block_count==16384);
 CHECK(g_merc_heap_blocks[16383].address==MERC_HEAP_ARENA_START+16383*16);
 fail_metadata=0;a=merc_heap_allocate(1,0,20);
#ifdef LEGACY_RECORDS
 CHECK(a==0 && g_merc_heap_block_count==16384 && g_heap_next==frontier);
 puts("Legacy16384 record limit reproduces20-byte Lua allocation failure with guest RAM available");
 free((void*)g_memory_offset);return 0;
#endif
 CHECK(a && !(a&15) && g_merc_heap_block_count==16385);
 CHECK(g_merc_heap_block_capacity==32768);
 CHECK(merc_heap_size(a,&cap) && cap==32);
 CHECK(merc_heap_free(a) && !xbox_HeapGetAllocationSize(a));
 CHECK(g_merc_heap_block_count==16384);
 /* Growth while splitting moves the table: old pointers cannot be reused. */
 reset();CHECK(xbox_HeapAlloc(0x100000,4096)==XBOX_HEAP_BASE);merc_heap_initialize(1);
 g_merc_heap_block_count=MERC_HEAP_INITIAL_BLOCKS;
 for(unsigned i=0;i<g_merc_heap_block_count;++i)
  g_merc_heap_blocks[i]=(merc_heap_block_t){MERC_HEAP_ARENA_START+i*32,32,32,1};
 g_merc_heap_blocks[8000].in_use=0;g_merc_heap_blocks[8000].requested=0;
 a=merc_heap_allocate(1,MERC_HEAP_ZERO_MEMORY,8);
 CHECK(a==MERC_HEAP_ARENA_START+8000*32 && g_merc_heap_block_count==16385);
 CHECK(g_merc_heap_blocks[8001].address==a+16 && !g_merc_heap_blocks[8001].in_use);
 CHECK(g_merc_heap_blocks[8002].address==MERC_HEAP_ARENA_START+8001*32);
 CHECK(merc_heap_free(a));CHECK(g_merc_heap_block_count==16384);
 CHECK(g_merc_heap_blocks[8000].capacity==32);
 /* Reallocation can also move the metadata table. Preserve the payload. */
 reset();CHECK(xbox_HeapAlloc(0x100000,4096)==XBOX_HEAP_BASE);merc_heap_initialize(1);
 g_merc_heap_block_count=MERC_HEAP_INITIAL_BLOCKS;
 for(unsigned i=0;i<g_merc_heap_block_count;++i)
  g_merc_heap_blocks[i]=(merc_heap_block_t){MERC_HEAP_ARENA_START+i*32,32,32,1};
 a=g_merc_heap_blocks[100].address;memset(guest_ptr(a),0x5a,32);
 moved=merc_heap_reallocate(1,0,a,64);CHECK(moved && moved!=a);
 for(unsigned i=0;i<32;++i)CHECK(((unsigned char*)guest_ptr(moved))[i]==0x5a);
 CHECK(g_merc_heap_block_capacity==32768);
 CHECK(!g_merc_heap_blocks[100].in_use);CHECK(merc_heap_free(moved));
 reset();
 puts("16384-record exhaustion, host failure rollback, split growth and realloc growth passed");
 puts("1024 fragmented ordinary allocations, zeroing, non-overlap, realloc, free, overflow and physical alignment passed");
 free((void*)g_memory_offset);return 0;
#endif
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-heap-alignment-') as directory:
            root = Path(directory)
            for variant in ("alignment", "records", "fixed"):
                legacy = variant == "alignment"
                body = ordinary.replace('xbox_HeapAlloc(capacity, 16u)', 'xbox_HeapAlloc(capacity, 4096u)') if legacy else ordinary
                if variant == 'records':
                    body = body.replace('if (g_merc_heap_block_capacity >= MERC_HEAP_MAX_BLOCKS)', 'if (g_merc_heap_block_capacity >= MERC_HEAP_INITIAL_BLOCKS)')
                c = root/(variant+'.c')
                exe = c.with_suffix('.exe')
                c.write_text(('#define LEGACY\n' if legacy else '#define LEGACY_RECORDS\n' if variant == 'records' else '')+prelude+structures+stubs+backing+body+harness, encoding='utf-8')
                build = subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(c),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                run = subprocess.run([str(exe)],capture_output=True,text=True)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                print(run.stdout.strip())


if __name__ == '__main__':
    unittest.main(verbosity=2)

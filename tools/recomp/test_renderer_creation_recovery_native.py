"""Exercise the failed-device recovery against native code and crash heap layout."""
from pathlib import Path
import json, re, shutil, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[2]

def compile_run(source):
 with tempfile.TemporaryDirectory(prefix='mercs-device-recovery-') as tmp:
  c=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe';c.write_text(source)
  cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
  subprocess.run([cc,'-std=c11','-O2',str(c),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,timeout=10)

class RendererRecoveryTests(unittest.TestCase):
 def test_recovery_preserves_original_success_and_only_retries_memory_failure(self):
  text=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0010.c').read_text()
  code=text.split('loc_0020FC02: ;',1)[1].split('    ecx = MEM32(0x7AD604);',1)[0]
  self.assertIn('recomp_renderer_recovery_report',code)
  compile_run(r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
static uint32_t mem[0x800000/4],esp,eax,ebp,edi;
#define MEM32(a) mem[(uint32_t)(a)/4]
#define PUSH32(sp,v) do {uint32_t x=(v);sp-=4;MEM32(sp)=x;}while(0)
static unsigned calls,report_mask,fail_retry;
static void recomp_renderer_recovery_report(uint32_t result,uint32_t stage){report_mask|=1u<<stage;}
static void sub_0028CAA0(void){
 assert(MEM32(esp+4)==0 && MEM32(esp+8)==1 && MEM32(esp+12)==0);
 assert(MEM32(esp+16)==0x40 && MEM32(esp+24)==0x7ad604);
 unsigned pp=MEM32(esp+20);assert(pp==0x10034);
 assert(MEM32(pp)==640 && MEM32(pp+4)==480 && MEM32(pp+8)==6);
 assert(MEM32(pp+12)==1 && MEM32(pp+16)==0x11);
 assert(MEM32(pp+20)==1 && MEM32(pp+32)==1 && MEM32(pp+36)==0x2a);
 assert(MEM32(0x30f270)==0x11);
 ++calls;esp+=28;eax=fail_retry?0x8007000e:0;MEM32(0x7ad604)=fail_retry?0:0x299380;
}
static void boundary(void){
"""+code+r"""
}
static void setup(void){
 memset(mem,0,sizeof(mem));calls=report_mask=fail_retry=0;
 esp=0x10000;ebp=0;edi=1;
 MEM32(esp+0x34)=640;MEM32(esp+0x38)=480;MEM32(esp+0x3c)=6;
 MEM32(esp+0x40)=2;MEM32(esp+0x44)=0x1021;MEM32(esp+0x48)=1;
 MEM32(esp+0x54)=1;MEM32(esp+0x58)=0x2a;MEM32(0x30f270)=0x1021;
}
int main(void){
 setup();eax=0;MEM32(0x7ad604)=0x299380;boundary();assert(!calls&&!report_mask&&MEM32(0x30f270)==0x1021);
 for(unsigned i=0;i<2;i++){
  setup();eax=i?0x8876017c:0x8007000e;boundary();assert(calls==1&&report_mask==6&&eax==0&&esp==0x10000);
 }
 setup();eax=0x80070057;boundary();assert(!calls&&report_mask==8);
 setup();eax=0x8007000e;MEM32(esp+0x44)=0x11;boundary();assert(!calls&&report_mask==8);
 setup();eax=0x8007000e;fail_retry=1;boundary();assert(calls==1&&report_mask==14&&esp==0x10000);
 puts("PASS: original success, both memory HRESULTs, retry ABI/parameters, bounded failure");return 0;
}
""")

 def test_injection_can_target_gameplay_device_without_consuming_startup(self):
  text=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
  start=text.index('uint32_t recomp_renderer_test_oom(uint32_t multisample)')
  body=text[start:text.index('\n}',start)+2]
  common=r"""
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static const char *enabled, *selected, *gate;
static char *test_getenv(const char *key) {
 if (!strcmp(key,"XBOXRECOMP_TEST_RENDERER_OOM_ONCE")) return (char *)enabled;
 if (!strcmp(key,"XBOXRECOMP_TEST_RENDERER_OOM_MULTISAMPLE")) return (char *)selected;
 return (char *)gate;
}
#define getenv test_getenv
"""+body
  compile_run(common+r"""
int main(void) {
 assert(!recomp_renderer_test_oom(0x1121));
 enabled="1";selected="0x1121";
 assert(!recomp_renderer_test_oom(0x1021));
 assert(!recomp_renderer_test_oom(0x11));
 gate="nonexistent-renderer-recovery-test-gate";
 assert(!recomp_renderer_test_oom(0x1121));
 gate=NULL;
 assert(recomp_renderer_test_oom(0x1121));
 assert(!recomp_renderer_test_oom(0x1121));
 puts("PASS: selected gameplay failure follows successful front-end without repeat");
}
""")
  compile_run(common+r"""
int main(void) {
 enabled="1";
 assert(!recomp_renderer_test_oom(0x11));
 assert(recomp_renderer_test_oom(0x1021));
 assert(!recomp_renderer_test_oom(0x1121));
 puts("PASS: default startup injection unchanged");
}
""")

 def test_crash_heap_can_recreate_retail_non_aa_buffers(self):
  source=(ROOT/'src/kernel/xbox_memory_layout.c').read_text()
  structures=source[source.index('#define XBOX_HEAP_MAX_ALLOCS'):source.index('static void xbox_HeapCaptureOwner(')]
  body=source[source.index('static xbox_heap_allocation *xbox_HeapAppendAllocation('):source.index('static const xbox_heap_allocation *xbox_HeapFindAllocation(')]
  free=source[source.index('void xbox_HeapFree('):source.index('HANDLE xbox_GetMappingHandle(')]
  data=json.loads((ROOT/'tools/recomp/fixtures/ace-clubs-post-device-failure-heap.json').read_text())
  graphics_start=source.index('uint32_t xbox_GetGraphicsMemorySize(void)')
  graphics_size=source[graphics_start:source.index('\n}',graphics_start)+2]
  rows=',\n'.join('{'+','.join(str(v)+'u' for v in row)+'}' for row in data['blocks'])
  harness=r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define XBOX_TOTAL_RAM 0x04000000u
static int g_extended_graphics_memory;
#define XBOX_HEAP_BASE 0x008c0000u
#define XBOX_HEAP_SIZE (0x04000000u-XBOX_HEAP_BASE)
#define XBOX_CPU_ALIAS_BASE 0x80000000u
#define XBOX_CPU_ALIAS_END 0xc0000000u
static uintptr_t g_memory_offset;
static uint32_t g_recomp_current_func,g_recomp_recent_game_func_idx,g_recomp_recent_game_funcs[256];
"""+graphics_size+"\n"+structures+r"""
static void xbox_HeapCaptureOwner(xbox_heap_allocation *p){memset(p->owner_ring,0,sizeof(p->owner_ring));}
static int xbox_HeapTraceEnabled(void){return 0;}
static void xbox_HeapDumpCensus(void){}
"""+body+free+'\nstatic const uint32_t blocks[][5]={'+rows+'};\n'+r"""
int main(void){
 g_memory_offset=(uintptr_t)calloc(1,0x4000000);assert(g_memory_offset);
 xbox_HeapReset();
"""+f' g_heap_next={data["frontier"]}u;g_heap_page_next={data["pageFrontier"]}u;\n'+r"""
 for(unsigned i=0;i<sizeof(blocks)/sizeof(blocks[0]);i++){
  const uint32_t *r=blocks[i];assert(r[0]>=XBOX_HEAP_BASE && r[0]+r[2]<=0x4000000);
  assert(xbox_HeapAppendAllocation(r[0],r[1],r[2],r[3],r[4]));
  for(unsigned j=0;j<i;j++) assert(r[0]+r[2]<=blocks[j][0] || blocks[j][0]+blocks[j][2]<=r[0]);
 }
 uint32_t aa=xbox_HeapAllocPageRounded(2457600,16384);assert(aa);
 assert(!xbox_HeapAllocPageRounded(2457600,16384));xbox_HeapFree(aa);
 unsigned oom=g_heap_oom_count;
 /* Recreate kernel DMA context/FIFO then two non-AA color surfaces and depth.
  * This deliberately also reserves fresh FIFO space even though the snapshot
  * may retain some driver bookkeeping after failed creation. */
 assert(xbox_HeapAllocPageRounded(96,4096));
 assert(xbox_HeapAllocPageRounded(786432,4096));
 for(unsigned i=0;i<3;i++)assert(xbox_HeapAllocPageRounded(1228800,16384));
 for(unsigned i=0;i<2;i++)assert(xbox_HeapAllocPageRounded(307200,4096));
 assert(g_heap_oom_count==oom);
 puts("PASS: captured heap rejects AA recreation and fits recovery buffers without moving live memory");
 free((void*)g_memory_offset);return 0;
}
"""
  compile_run(harness)

if __name__=='__main__':unittest.main()

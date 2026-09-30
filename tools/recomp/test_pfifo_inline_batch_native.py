"""Compare batched PFIFO data ingestion to the original scalar handlers."""
from pathlib import Path
import os, subprocess, tempfile, unittest
from tools.recomp.test_shader_program_key_cache_native import function
ROOT=Path(__file__).resolve().parents[2]
class InlineBatchTests(unittest.TestCase):
 def test_scalar_equivalence_and_split_submissions(self):
  pg=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
  core=(ROOT/'src/nv2a/nv2a_core.c').read_text(encoding='utf-8')
  scalar=pg[pg.index('    case NV097_ARRAY_ELEMENT16:'):pg.index('    case NV097_DRAW_ARRAYS:')]
  scalar+=pg[pg.index('    case NV097_INLINE_ARRAY:'):pg.index('    /* -- Clear -- */',pg.index('    case NV097_INLINE_ARRAY:'))]
  code=r'''
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "src/nv2a/nv2a_regs.h"
#define GET_MASK(v,m) (((v)&(m)) >> __builtin_ctz(m))
#define SET_MASK(v,m,x) ((v)=((v)&~(m))|(((x)<<__builtin_ctz(m))&(m)))
#define MAX_INLINE_VERTS 8
#define INLINE_VERT_DWORDS 5
#define MAX_INLINE_ELEMENTS 64
typedef uint64_t hwaddr;
typedef struct {struct {uint32_t regs[0x10000];} pfifo;struct {uint32_t regs[0x10000];uint8_t subchannel_class[8];} pgraph;uint8_t *vram_ptr;} NV2AState;
static uint32_t words[8192];static hwaddr length;static uint32_t test_budget;
static uint8_t *nv_dma_map(NV2AState *d,uint32_t i,hwaddr *l){*l=length;return (uint8_t*)words;}
static uint32_t ldl_le_p(const uint32_t *p){uint32_t v;memcpy(&v,p,4);return v;}
static uint32_t g_nv2a_pfifo_diag_submission,g_nv2a_pfifo_diag_get,g_nv2a_pfifo_diag_put,g_nv2a_pfifo_diag_stage,g_nv2a_pfifo_diag_word,g_nv2a_pfifo_diag_method,g_nv2a_pfifo_diag_subchannel;
static uint32_t g_pgraph_method_count;static int g_pgraph_trace_enabled;
static struct RenderState {
 int initialized,in_draw,inline_array_mode;
 uint32_t draw_array_count,draw_array_start,inline_element_count,inline_count;
 uint32_t inline_elements[MAX_INLINE_ELEMENTS],inline_data[MAX_INLINE_VERTS*INLINE_VERT_DWORDS];
 struct {uint32_t methods_handled;} stats;
} g_pg;
'''
  code+=function(pg,'uint32_t pgraph_d3d11_inline_batch(')+'\n'
  code+='static void pgraph_method(NV2AState *d,uint32_t sub,uint32_t method,uint32_t param){ ++g_pgraph_method_count; if(!g_pg.initialized || d->pgraph.subchannel_class[sub]!=NV_KELVIN_PRIMITIVE)return; ++g_pg.stats.methods_handled; switch(method){\n'+scalar+' default:return;}}\n'
  # The extracted scalar cases return 1; their return value is irrelevant to this decoder fixture.
  code=code.replace('static void pgraph_method(','static int pgraph_method(').replace('!=NV_KELVIN_PRIMITIVE)return;','!=NV_KELVIN_PRIMITIVE)return 0;').replace(' default:return;',' default:return 0;')
  code+=function(core,'static void pfifo_process_pushbuffer(').replace('1024u * 1024u','test_budget')+'\n'
  code+=r'''
static NV2AState gpu;
static uint32_t seed=9;
static uint32_t rnd(void){seed=seed*1664525+1013904223;return seed;}
struct Snapshot {struct RenderState pg;uint32_t get,state,count,lastword;};
static struct Snapshot run(struct RenderState initial,unsigned n,unsigned split,int scalar,int other_class){
 memset(&gpu,0,sizeof(gpu));gpu.vram_ptr=(uint8_t*)words;gpu.pgraph.subchannel_class[0]=other_class?0:NV_KELVIN_PRIMITIVE;
 g_pg=initial;g_pgraph_method_count=0;g_pgraph_trace_enabled=scalar;
 if(split)pfifo_process_pushbuffer(&gpu,split*4);
 pfifo_process_pushbuffer(&gpu,n*4);
 struct Snapshot out={g_pg,gpu.pfifo.regs[NV_PFIFO_CACHE1_DMA_GET],gpu.pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE],g_pgraph_method_count,g_nv2a_pfifo_diag_word};return out;
}
int main(void){
 const uint32_t methods[]={NV097_ARRAY_ELEMENT16,NV097_ARRAY_ELEMENT32,NV097_INLINE_ARRAY};
 for(unsigned t=0;t<10000;t++){
  struct RenderState initial={0};initial.initialized=(t%17)!=0;initial.in_draw=(t%9)!=0;
  initial.inline_element_count=rnd()%65;initial.inline_count=rnd()%41;
  initial.draw_array_start=rnd()%65536;initial.draw_array_count=(t%5)?0:17;
  unsigned n=2+rnd()%2000;uint32_t method=methods[t%3];
  words[0]=(t%11?0x40000000u:0u)|((n-1)<<18)|method;
  for(unsigned i=1;i<n;i++)words[i]=rnd();
  test_budget=(t%7)?1048576:128;length=(n+1)*4;unsigned split=(t%4)?rnd()%n:0;
  struct Snapshot a=run(initial,n,split,1,t%19==0), b=run(initial,n,split,0,t%19==0);
  assert(!memcmp(&a,&b,sizeof(a)));
  assert(a.get==n*4);assert(a.count<=n-1);if(test_budget==1048576)assert(a.count==n-1);
 }
 puts("PASS: 10000 scalar/batch comparisons, partial PUTs, capacity limits, inactive draws, and other classes");
}
'''
  with tempfile.TemporaryDirectory() as temp:
   p=Path(temp);(p/'test.c').write_text(code,encoding='utf-8');env=os.environ.copy();env['PATH']='C:/msys64/mingw64/bin;'+env['PATH']
   result=subprocess.run(['C:/msys64/mingw64/bin/gcc.exe','-O2','-std=c11','-I',str(ROOT),str(p/'test.c'),'-o',str(p/'test.exe')],capture_output=True,text=True,env=env)
   self.assertEqual(result.returncode,0,result.stderr)
   result=subprocess.run([str(p/'test.exe')],capture_output=True,text=True,env=env)
   self.assertEqual(result.returncode,0,result.stderr);print(result.stdout.strip())
if __name__=='__main__':unittest.main()


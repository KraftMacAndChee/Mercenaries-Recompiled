"""Exercise production vertex prewarm without altering active guest draw state."""
from pathlib import Path
import importlib.util,json,tempfile,subprocess,shutil
ROOT=Path(__file__).resolve().parents[2]
def function(s,name):
 a=s.index(name);a=s.rfind('\n',0,a)+1;b=s.index('{',a);depth=1;i=b+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
def main():
 src=(ROOT/'src/d3d/d3d8_vsh.c').read_text(encoding='utf-8');body='\n'.join(function(src,n) for n in ['static uint32_t fnv1a_hash(','void d3d8_vsh_set_input_layout(','BOOL d3d8_vsh_prewarm('])
 spec=importlib.util.spec_from_file_location('gen',ROOT/'ports/mercenaries/scripts/Generate-Shader-Warmup.py');gen=importlib.util.module_from_spec(spec);spec.loader.exec_module(gen)
 data=json.loads((ROOT/'ports/mercenaries/data/shader-warmup.json').read_text(encoding='utf-8'))
 states='static const NV2AVshWarmup samples[]={'+','.join(gen.vertex(x) for x in data['vertex_states'])+'};\n'
 hashes='static const uint32_t expected[]={'+','.join('0x'+x['hash']+'u' for x in data['vertex_states'])+'};\n'
 pre=r'''
#include <windows.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "d3d8_vsh.h"
static DXGI_FORMAT g_vsh_input_formats[16];
static UINT g_vsh_input_offsets[16],g_vsh_input_components[16];
static uint32_t g_vsh_input_bgra_mask,g_vsh_input_layout_key;
typedef struct VshCacheEntry {int value;} VshCacheEntry;
static VshCacheEntry entry;
static int hit,compile_fail,layout_fail,compiles;
static uint32_t seen_hash;
static VshCacheEntry *cache_lookup(uint32_t h){seen_hash=h;return hit?&entry:NULL;}
static VshCacheEntry *compile_shader(const DWORD*p,int n,uint32_t h){assert(p&&n>0);assert(h==seen_hash);compiles++;return compile_fail?NULL:&entry;}
static void *get_cached_layout(VshCacheEntry*e){assert(e==&entry);return layout_fail?NULL:&entry;}
'''
 test=r'''
int main(void){
 unsigned cases=0;
 for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);i++)for(int mode=0;mode<4;mode++){
  DXGI_FORMAT f[16];UINT o[16],c[16];
  for(int j=0;j<16;j++){f[j]=j;o[j]=j*13;c[j]=j%4;}
  d3d8_vsh_set_input_layout(f,o,c,8);uint32_t old_key=g_vsh_input_layout_key;
  hit=mode==1;compile_fail=mode==2;layout_fail=mode==3;compiles=0;
  assert(d3d8_vsh_prewarm(&samples[i])==(mode<2));assert(seen_hash==expected[i]);assert(compiles==(hit?0:1));
  assert(memcmp(f,g_vsh_input_formats,sizeof(f))==0 && memcmp(o,g_vsh_input_offsets,sizeof(o))==0 && memcmp(c,g_vsh_input_components,sizeof(c))==0);
  assert(g_vsh_input_bgra_mask==8 && g_vsh_input_layout_key==old_key);cases++;
 }
 assert(!d3d8_vsh_prewarm(NULL));NV2AVshWarmup bad=samples[0];bad.length=0;assert(!d3d8_vsh_prewarm(&bad));bad.length=137;assert(!d3d8_vsh_prewarm(&bad));
 printf("PASS: %u production warm-up cases: exact program/declaration hashes, cache hits/misses, compile/layout failure and active input-state restoration; invalid programs rejected\n",cases);
 return 0;
}
'''
 with tempfile.TemporaryDirectory(prefix='vertex-prewarm-') as td:
  (Path(td)/'d3d11.h').write_text('#pragma once\ntypedef int DXGI_FORMAT;\n')
  p=Path(td)/'test.c';exe=p.with_suffix('.exe');p.write_text(pre+body+states+hashes+test,encoding='utf-8')
  subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11','-I',td,'-I',str(ROOT/'src/d3d'),str(p),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()

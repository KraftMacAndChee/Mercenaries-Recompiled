"""Compile the generated title warm-up states and exercise production warm-up."""
from pathlib import Path
import importlib.util,json,subprocess,tempfile,shutil,copy
ROOT=Path(__file__).resolve().parents[2]
p=ROOT/'ports/mercenaries/scripts/Generate-Shader-Warmup.py'
spec=importlib.util.spec_from_file_location('warmup_generator',p);g=importlib.util.module_from_spec(spec);spec.loader.exec_module(g)
def main():
 d=json.loads((ROOT/'ports/mercenaries/data/shader-warmup.json').read_text(encoding='utf-8'));generated=g.generate(d)
 for alter in [lambda x:x.update(version=2),lambda x:x.update(states=[]),lambda x:x['states'][0].update(num_stages=9),lambda x:x['states'][0]['tex_mode'].__setitem__(0,32),lambda x:x['states'][0].update(c0=[0]*8),lambda x:x['states'][0]['input_tex'].__setitem__(0,-2),lambda x:x['states'][0].update(screen_depth_stage=5)]:
  bad=copy.deepcopy(d);alter(bad)
  try:g.generate(bad)
  except ValueError:pass
  else:raise AssertionError('Invalid catalogue accepted')
 source=(ROOT/'src/d3d/d3d8_combiners.c').read_text(encoding='utf-8');a=source.index('static uint32_t fnv1a_hash_update');b=source.index('uint32_t d3d8_combiners_debug_state_hash',a);hashcode=source[a:b]
 vsh=(ROOT/'src/d3d/d3d8_vsh.c').read_text(encoding='utf-8')
 a=vsh.index('static uint32_t fnv1a_hash(');b=vsh.index('\n}',a)+2
 vertex_hashcode=vsh[a:b]
 a=vsh.index('void d3d8_vsh_set_input_layout(');b=vsh.index('BOOL d3d8_vsh_prewarm(',a)
 vertex_hashcode+='\n'+vsh[a:b]
 warm=(ROOT/'ports/mercenaries/src/shader_warmup.c').read_text(encoding='utf-8');warm='\n'.join(l for l in warm.splitlines() if not l.startswith('#include'))
 pre=r'''
#include <windows.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "d3d8_combiners.h"
#include "d3d8_vsh.h"
static unsigned calls, reports, last_ready, failures;
static int disabled;
static void (*callback)(void), (*vertex_callback)(void);
static unsigned vertex_calls;
static DXGI_FORMAT g_vsh_input_formats[16];
static UINT g_vsh_input_offsets[16],g_vsh_input_components[16];
static uint32_t g_vsh_input_bgra_mask,g_vsh_input_layout_key;
void d3d8_vsh_set_warmup(void (*p)(void)){vertex_callback=p;}
BOOL d3d8_vsh_prewarm(const NV2AVshWarmup *p){assert(p);vertex_calls++;return TRUE;}
static const char *fake_getenv(const char *name) { return disabled ? "1" : NULL; }
#define getenv fake_getenv
#define GetTickCount64() ((ULONGLONG)GetTickCount())
void d3d8_combiners_set_warmup(void (*p)(void)){ callback=p; }
ID3D11PixelShader *d3d8_combiners_get_shader(const NV2ACombinerState *s){assert(s); ++calls; return failures && calls%7==0?NULL:(ID3D11PixelShader*)(uintptr_t)1;}
void xbox_preview_log_event(const char *kind,const char *fmt,...){va_list v;va_start(v,fmt);last_ready=va_arg(v,unsigned);va_end(v);++reports;}
'''
 expected='static const uint32_t expected[]={'+','.join('0x'+h+'u' for h in d['structural_hashes'])+'};'
 expected += '\nstatic const uint32_t expected_vertex[]={'+','.join('0x'+v['hash']+'u' for v in d['vertex_states'])+'};'
 expected += f'\n#define EXPECTED_PS {len(d["states"])}u\n#define EXPECTED_VS {len(d["vertex_states"])}u\n'
 maincode=r'''
int main(void){
 assert(sizeof(warmup_states)/sizeof(*warmup_states)==sizeof(expected)/sizeof(*expected));
 for(unsigned i=0;i<sizeof(expected)/sizeof(*expected);i++){
  assert(combiner_state_hash(&warmup_states[i])==expected[i]);
  NV2ACombinerState q=warmup_states[i];q.c0[0]=123;q.c1[7]=456;q.final_c0=789;q.final_c1=111;
  assert(combiner_state_hash(&q)==expected[i]);
 }
 for(unsigned i=0;i<EXPECTED_VS;i++){
  const NV2AVshWarmup *v=&warmup_vertex_states[i];
  d3d8_vsh_set_input_layout(v->formats,v->offsets,v->components,v->bgra_mask);
  uint32_t h=fnv1a_hash(v->microcode,(size_t)v->length*4*sizeof(DWORD));
  assert(((h^g_vsh_input_layout_key)*16777619u)==expected_vertex[i]);
 }
 recomp_register_shader_warmup();assert(callback && vertex_callback);
 callback();assert(calls==EXPECTED_PS && reports==1 && last_ready==EXPECTED_PS);
 disabled=1;callback();assert(calls==EXPECTED_PS && reports==1);
 disabled=0;calls=0;failures=1;callback();assert(calls==EXPECTED_PS && reports==2 && last_ready==EXPECTED_PS-EXPECTED_PS/7);
 failures=0;vertex_callback();assert(vertex_calls==EXPECTED_VS && last_ready==EXPECTED_VS);
 puts("PASS: catalogue typed states match captured structural identities; constants excluded; warm-up success, disabled path and partial failures; seven malformed catalogues rejected");
 return 0;
}
'''
 with tempfile.TemporaryDirectory(prefix='shader-warmup-') as td:
  (Path(td)/'d3d11.h').write_text('#pragma once\ntypedef struct ID3D11PixelShader ID3D11PixelShader;\ntypedef int DXGI_FORMAT;\n')
  p=Path(td)/'test.c';exe=p.with_suffix('.exe');p.write_text(pre+generated+hashcode+vertex_hashcode+warm+expected+maincode,encoding='utf-8')
  subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-O2','-std=c11','-I',td,'-I',str(ROOT/'src/d3d'),str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()

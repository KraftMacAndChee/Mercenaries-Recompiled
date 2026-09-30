"""Exercise actual shader-key code against uncached reference hashing."""
from pathlib import Path
import os, re, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[2]

def function(source, signature):
    start=source.index(signature); brace=source.index('{',start); depth=1; end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]

class ShaderProgramKeyTests(unittest.TestCase):
    def test_keys_follow_program_writes_entry_points_and_handle_reuse(self):
        pg=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text()
        vs=(ROOT/'src/d3d/d3d8_vsh.c').read_text()
        header=(ROOT/'src/d3d/d3d8_vsh.h').read_text()
        write_start=pg.index('    if (method >= NV097_SET_TRANSFORM_PROGRAM &&')
        write_end=pg.index('    if (method >= NV097_SET_TRANSFORM_CONSTANT &&',write_start)
        start_branch=pg[pg.index('    case NV097_SET_TRANSFORM_PROGRAM_START:'):pg.index('    case NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN:')]
        prepare=function(vs,'BOOL d3d8_vsh_prepare_draw(')
        self.assertIn('hash = vsh->microcode_hash;',prepare)
        self.assertIn('hash ^= g_vsh_input_layout_key;',prepare)
        self.assertNotIn('fnv1a_hash(',prepare)
        prelude=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
typedef uint32_t DWORD;
typedef int HRESULT;
#define NV2A_MAX_TRANSFORM_PROGRAM_LENGTH 136
#define NV2A_VS_MAX_INSTRUCTIONS 136
#define NV2A_VS_MAX_SLOTS 4
#define NV097_SET_TRANSFORM_PROGRAM 0xB00
#define NV097_SET_TRANSFORM_PROGRAM_START 0x1EA0
#define E_INVALIDARG -1
#define E_OUTOFMEMORY -2
#define S_OK 0
static struct {
 uint32_t transform_program_start, transform_program_load;
 uint32_t transform_program[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][4];
 uint32_t transform_program_hash, transform_program_length;
 int transform_program_key_valid;
} g_pg;
static const char *vsh_cached_getenv(const char *s){return NULL;}
static int d3d8_vsh_is_programmable(DWORD h){return h>=0x10000;}
'''
        slot=re.search(r'typedef struct NV2AVshSlot \{.*?\} NV2AVshSlot;',header,re.S)[0]
        code=prelude+slot+'\nstatic NV2AVshSlot g_vsh_slots[NV2A_VS_MAX_SLOTS];\nstatic int g_vsh_slot_count;\n'
        code+=function(vs,'static uint32_t fnv1a_hash(')+'\n'
        code+=function(pg,'static uint32_t transform_program_key(')+'\n'
        code+='static int method_write(uint32_t method,uint32_t param){\n'+pg[write_start:write_end]+'switch(method){\n'+start_branch+'default:return 0;}}\n'
        code+=function(vs,'HRESULT d3d8_vsh_create_shader(')+'\n'+function(vs,'HRESULT d3d8_vsh_delete_shader(')+'\n'
        code+=r'''
static uint32_t random_state=12345;
static uint32_t next_random(void){random_state=random_state*1664525u+1013904223u;return random_state;}
static void verify(void){
 uint32_t n=0,got,begin=g_pg.transform_program_start;
 for(uint32_t i=begin;i<136;i++){++n;if(g_pg.transform_program[i][3]&1)break;}
 uint32_t expected=fnv1a_hash(g_pg.transform_program[begin],n*16);
 assert(transform_program_key(&got)==expected);assert(got==n);
 for(int i=0;i<10;i++){assert(transform_program_key(&got)==expected);assert(got==n);}
}
int main(void){
 verify(); /* All-zero program has no terminator and uses the remaining RAM. */
 for(unsigned i=0;i<20000;i++){
  uint32_t insn=next_random()%136, component=next_random()%4, value=next_random();
  g_pg.transform_program_load=insn;
  assert(method_write(0xB00+4*component,value));verify();
  int valid=g_pg.transform_program_key_valid;
  g_pg.transform_program_load=insn;
  assert(method_write(0xB00+4*component,value));
  assert(g_pg.transform_program_key_valid==valid);verify(); /* Identical upload. */
  assert(method_write(0x1EA0,next_random()%136));verify();
 }
 assert(!method_write(0x1EA0,136));verify();
 g_pg.transform_program_load=136;assert(!method_write(0xB00,42));verify();
 DWORD data[136*4];for(unsigned i=0;i<136*4;i++)data[i]=next_random();
 for(int length=1;length<=140;length++){
  DWORD handle;assert(d3d8_vsh_create_shader(data,length,&handle)==0);
  int slot=handle-0x10000,clamped=length>136?136:length;
  assert(g_vsh_slots[slot].microcode_hash==fnv1a_hash(data,clamped*16));
  data[0]^=0xffffffffu; /* The caller's source is not the handle's storage. */
  assert(g_vsh_slots[slot].microcode_hash==fnv1a_hash(g_vsh_slots[slot].microcode,clamped*16));
  assert(d3d8_vsh_delete_shader(handle)==0);
 }
 assert(!g_vsh_slot_count);
 puts("PASS: 20000 program mutations, entry points, unchanged writes, bounds, and handle reuse");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory); (path/'test.c').write_text(code)
            env=os.environ.copy();env['PATH']='C:/msys64/mingw64/bin;'+env['PATH']
            subprocess.run(['C:/msys64/mingw64/bin/gcc.exe','-O2','-std=c11',str(path/'test.c'),'-o',str(path/'test.exe')],check=True,env=env,capture_output=True)
            result=subprocess.run([str(path/'test.exe')],check=True,env=env,capture_output=True,text=True)
            print(result.stdout.strip())

if __name__=='__main__':unittest.main()

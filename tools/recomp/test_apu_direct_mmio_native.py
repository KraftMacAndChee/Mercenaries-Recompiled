"""Check scoped patch fidelity and exact direct APU handler forwarding."""
from pathlib import Path
import importlib.util,sys,re,tempfile,subprocess
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('apu_patch',ROOT/'ports/mercenaries/scripts/Patch-Generated.py');module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
generated=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0013.c').read_text()
body=re.search(r'void sub_002A3A5A\(void\)\n\{.*?\n\}',generated,re.S)[0]
# Accept a freshly regenerated tree by recovering only these six substitutions.
original=body.replace('    extern uint32_t recomp_apu_read32(uint32_t);\n    extern void recomp_apu_write32(uint32_t, uint32_t);\n','')
original=original.replace('ecx = recomp_apu_read32((uint32_t)-25034736);','ecx = MEM32(-25034736);')
for address,value in [('0xFE820160u','eax'),('ebx','edx'),('0xFE82043Cu','eax'),('0xFE8202F8u','eax'),('0xFE82031Cu','eax')]:original=original.replace(f'recomp_apu_write32({address}, {value});',f'MEM32({address}) = {value};')
patched=module.patch_apu_voice_parameter_mmio(original)
assert module.patch_apu_voice_parameter_mmio(patched)==patched
assert patched!=original and patched.count('recomp_apu_write32(')==6
assert module.patch_apu_voice_parameter_mmio('void unrelated(void) {}')=='void unrelated(void) {}'
try:module.patch_apu_voice_parameter_mmio(original.replace('MEM32(ebx) = edx;','MEM32(ebx) = eax;'))
except RuntimeError:pass
else:raise AssertionError('Changed retail block must fail closed')
assert 'MEM32(ebp + 8) = 0xFE820400u;' in patched and '0xFE82043Cu)) goto loc_002A3B39' in patched
source=(ROOT/'src/apu/apu_mmio_hook.c').read_text();a=source.index('int apu_hook_try_read32(');b=source.index('bool apu_hook_handle_mmio(',a);helpers=source[a:b]
manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8');assert manual.count('uint32_t recomp_apu_read32(uint32_t address)')==1
assert manual.count('void recomp_apu_write32(uint32_t address, uint32_t value)')==1
wrappers='\n'.join(re.search(r'(?:uint32_t|void) '+name+r'\(.*?\n\}',manual,re.S)[0] for name in ['recomp_apu_read32','recomp_apu_write32'])
fixture=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define APU_MMIO_BASE 0xFE800000u
#define APU_MMIO_SIZE 0x80000u
static void *g_apu_state=(void*)1;
static unsigned g_apu_mmio_read_count,g_apu_mmio_write_count,calls,last_offset,last_size;
static uint64_t last_value;
static uint64_t apu_mmio_read(void *state,uint32_t offset,unsigned size){assert(state==g_apu_state);calls++;last_offset=offset;last_size=size;return 0xABCDEF0112345678ull;}
static void apu_mmio_write(void *state,uint32_t offset,uint64_t value,unsigned size){assert(state==g_apu_state);calls++;last_offset=offset;last_size=size;last_value=value;}
static ptrdiff_t g_xbox_mem_offset;
''' + helpers + wrappers + r'''
int main(void){
 uint32_t value=0;
 for(unsigned off=0;off<APU_MMIO_SIZE;off+=4){assert(apu_hook_try_read32(APU_MMIO_BASE+off,&value));assert(value==0x12345678&&last_offset==off&&last_size==4);uint32_t expected=off*0x9E3779B1u;assert(apu_hook_try_write32(APU_MMIO_BASE+off,expected));assert(last_offset==off&&last_size==4&&last_value==expected);}
 unsigned prior=calls;uint32_t invalid[]={0,0xFE7FFFFC,0xFE880000,0xFEFFFFFF,0xFFFFFFFF,0xFE800001,0xFE800002,0xFE87FFFF};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){assert(!apu_hook_try_read32(invalid[i],&value));assert(!apu_hook_try_write32(invalid[i],123));}
 assert(!apu_hook_try_read32(APU_MMIO_BASE,NULL));assert(calls==prior);
 g_apu_state=NULL;assert(!apu_hook_try_read32(APU_MMIO_BASE,&value));assert(!apu_hook_try_write32(APU_MMIO_BASE,123));assert(calls==prior);g_apu_state=(void*)1;
 assert(recomp_apu_read32(0xFE820010)==0x12345678&&last_offset==0x20010&&last_size==4);
 uint32_t sequence[]={0xFE820160,0xFE820400,0xFE820404,0xFE820408,0xFE82040C,0xFE820410,0xFE820414,0xFE820418,0xFE82041C,0xFE820420,0xFE820424,0xFE820428,0xFE82042C,0xFE820430,0xFE820434,0xFE820438,0xFE82043C,0xFE8202F8,0xFE82031C};
 for(unsigned i=0;i<sizeof(sequence)/sizeof(*sequence);i++){recomp_apu_write32(sequence[i],i);assert(last_offset==sequence[i]-APU_MMIO_BASE&&last_value==i&&last_size==4);}
 uint32_t memory[4]={123,456,789,999};g_xbox_mem_offset=(ptrdiff_t)memory;prior=calls;assert(recomp_apu_read32(4)==456);recomp_apu_write32(8,42);assert(memory[2]==42&&calls==prior);
 puts("PASS: 131072 aligned registers forward exact address/value/size; rejected ranges and unavailable APU retain original memory path; retail parameter upload order preserved");return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='apu-direct-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(fixture)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
print('PASS: scoped patch idempotent and refuses changed anchors; retail arithmetic, loops and stack remain unchanged')

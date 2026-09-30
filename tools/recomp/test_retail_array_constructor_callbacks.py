"""Regression for retail CRT array callbacks, including bunker-buster rays."""
from pathlib import Path
import json,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
GEN=ROOT/'ports/mercenaries/src/recomp/gen'
texts=[p.read_text(encoding='utf-8') for p in GEN.glob('recomp_*.c')]
dispatch=(GEN/'recomp_dispatch.c').read_text(encoding='utf-8')
entries={int(x,16) for x in re.findall(r'\{ 0x([0-9A-F]+)u,',dispatch)}
pattern=r'PUSH32\(esp, (0x[0-9A-F]+)\);\s*PUSH32\(esp, (0x[0-9A-F]+)\);\s*PUSH32\(esp, ([^;]+)\);\s*PUSH32\(esp, ([^;]+)\);\s*PUSH32\(esp, ([^;]+)\);\s*PUSH32\(esp, 0\); sub_0023792F'
callbacks=[m for t in texts for m in re.finditer(pattern,t)]
assert callbacks,'No retail CRT vector constructors found'
missing={int(m[2],16) for m in callbacks}-entries
assert not missing,f'Missing array constructor callbacks: {[hex(x) for x in missing]}'
seeds=json.loads((ROOT/'ports/mercenaries/manual-seeds.json').read_text(encoding='utf-8'))
assert any(int(x['start'],16)==0xAFF40 for x in seeds)
body=next(m[0] for t in texts if (m:=re.search(r'void sub_000AFF40\(void\)\n\{.*?\n\}',t,re.S)))
fixture=r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned char memory[0x10000];
static uint32_t eax,ecx,esp;
#define MEM32(a) (*(uint32_t *)(memory+(a)))
#define RECOMP_TRACE_FUNC(a) ((void)0)
''' + body + r'''
int main(void){
 memset(memory,0xA5,sizeof(memory));
 for(unsigned i=0;i<8;i++){
  unsigned base=0x100+i*0x70;ecx=base;esp=0x8000;sub_000AFF40();
  assert(eax==base&&ecx==0&&esp==0x8004);
  assert(MEM32(base)==0x2E7628);
  for(unsigned j=4;j<0x70;j++) assert(memory[base+j]==((j>=8&&j<24)?0:0xA5));
 }
 assert(memory[0xFF]==0xA5&&memory[0x480]==0xA5);
 puts("PASS: all eight ray brushes receive the retail vtable and cleared links; other fields and stack semantics preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='retail-array-ctor-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(fixture)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
print(f'PASS: all {len(callbacks)} statically identified CRT array constructors resolve through generated dispatch')

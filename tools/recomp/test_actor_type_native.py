"""Compare the lifted actor classifier with the original retail x86 branches.

The property lookup is mocked identically on both sides. The oracle executes
only the original classifier's cmp/jcc/mov/ret instructions after that lookup.
"""
from pathlib import Path
import re,sys,subprocess,tempfile,random
import capstone
ROOT=Path(__file__).resolve().parents[2];sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.recomp.generated_test_utils import generated_text_containing

def main():
 xbe=ROOT/'game_files/mercenaries-retail/default.xbe';raw=xbe.read_bytes();config.configure_from_xbe(str(xbe))
 start,end=0x117be,0x11a17;off=config.va_to_file_offset(start)
 ins={i.address:i for i in capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32).disasm(raw[off:off+end-start],start)}
 def original(value):
  pc=start;a=value;equal=above=False
  for _ in range(100):
   i=ins[pc];pc+=i.size;m=i.mnemonic;o=i.op_str
   if m=='cmp':
    assert o.startswith('eax, ');v=int(o.split(', ')[1],0);equal=a==v;above=a>v
   elif m in ('ja','je','jne','jmp'):
    if m=='jmp' or m=='ja' and above or m=='je' and equal or m=='jne' and not equal:pc=int(o,0)
   elif m=='mov':assert o.startswith('eax, ');a=int(o.split(', ')[1],0)
   elif m=='xor':assert o=='eax, eax';a=0
   elif m=='ret':return a
   else:raise AssertionError((hex(i.address),m,o))
  raise AssertionError('Oracle did not return')
 values={0,0xffffffff}
 for i in ins.values():
  if i.mnemonic=='cmp':
   v=int(i.op_str.split(', ')[1],0);values.update(((v-1)&0xffffffff,v,(v+1)&0xffffffff))
 rng=random.Random(1822);values.update(rng.getrandbits(32) for _ in range(4096));values=sorted(values)
 body=re.search(r'void sub_000117B0\(void\)\n\{.*?\n\}',generated_text_containing('void sub_000117B0(void)'),re.S)[0]
 code='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x20000];static unsigned hash;
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) do{assert(MEM32(esp+4)==0xb1d4007a);eax=hash;esp=(s);}while(0)
'''+body+r'''
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
while(scanf("%x",&hash)==1){esp=0x10000;MEM32(esp+4)=0x200;MEM32(0x200)=0x300;
 ebx=0x12345678;esi=0x23456789;edi=0x3456789a;g_seh_ebp=0x456789ab;
 sub_000117B0();assert(esp==0x10004&&ebx==0x12345678&&esi==0x23456789&&edi==0x3456789a&&g_seh_ebp==0x456789ab);printf("%u\n",eax);}}
'''
 with tempfile.TemporaryDirectory(prefix='recomp-actor-types-') as td:
  c=Path(td)/'test.c';exe=Path(td)/'test.exe';c.write_text(code)
  subprocess.run(['C:/MinGW/bin/gcc.exe','-O2',str(c),'-o',str(exe)],check=True)
  got=list(map(int,subprocess.run([str(exe)],input=''.join(f'{v:x}\n' for v in values),capture_output=True,text=True,check=True).stdout.split()))
 assert len(got)==len(values)
 bad=[(hex(v),g,original(v)) for v,g in zip(values,got) if g!=original(v)]
 print(f'Actor classification: {len(values)} cases, {len(bad)} mismatches; ABI preserved')
 for row in bad[:10]:print('hash, lifted, original:',row)
 if bad:sys.exit(1)
if __name__=='__main__':main()

"""Compare the production weapon-selection event predicate with retail x86.

External actor lookup/virtual calls are controlled fixtures; the actual event
predicate, invalid-owner cancellation, result, and guest ABI are compared.
"""
from pathlib import Path
import json,re,struct,subprocess,tempfile,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.generated_test_utils import generated_text_containing
from tools.recomp import config
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn.x86_const import *

def test_weapon_selection_event_matches_retail():
 text=generated_text_containing('void sub_00110D00(void)')
 if 'void sub_00110D00(void) { esp += 4;' in text:
  body=re.search(r'void sub_00110D00\(void\) \{[^\n]+',text)[0]
 else:body=re.search(r'void sub_00110D00\(void\)\n\{.*?\n\}',text,re.S)[0]
 pre=r'''
#define RECOMP_GENERATED_CODE
#include "HEADER"
#include <stdio.h>
#include <assert.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x400000];
static uint32_t valid,hero,weapon,calls,order[8];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void call(uint32_t a){order[calls++]=a;if(a==0x1ebc10)eax=valid;else if(a==0x8c7e0)eax=hero;else if(a==0x10fa80){assert(MEM32(esp+4)==0x12345678);MEM32(0x2000a8)=0xffffffff;eax=0x1234;}else if(a==0x310000)eax=0x230000;else if(a==0x310010)eax=weapon;else assert(0);esp+=4;}
void sub_001EBC10(void){call(0x1ebc10);}void sub_0008C7E0(void){call(0x8c7e0);}void sub_0010FA80(void){call(0x10fa80);}
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(a,s) call(a)
'''.replace('HEADER',(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix())
 harness=r'''
int main(void){g_xbox_mem_offset=(ptrdiff_t)memory;
for(unsigned mask=0;mask<32;++mask){memset(memory,0,sizeof(memory));calls=0;memset(order,0,sizeof(order));
valid=(mask&2)?0x11223344:0x11223345;hero=(mask&4)?0x220000:0;weapon=(mask&8)?0x55667788:0x12345600;
esp=0x300000;esi=0xaabbccdd;edi=0xfaceface;ebx=0xdecafbad;eax=(mask&16)?0xaaaaaa00:0xaaaaaa01;ecx=0x200018;MEM32(esp)=0x320000;MEM32(esp+4)=0x3cc756b3;
MEM32(0x200018)=0x200000;MEM32(0x20001c)=(mask&1)?0x210000:0;MEM32(0x200020)=0x11223344;MEM32(0x210024)=0x55667788;MEM32(0x2000a8)=0x12345678;
MEM32(0x220000)=0x240000;MEM32(0x240034)=0x310000;MEM32(0x230000)=0x250000;MEM32(0x25038c)=0x310010;
sub_00110D00();printf("%x %x %x %x %x %x %x",eax,esp,esi,edi,ebx,MEM32(0x2000a8),calls);for(unsigned i=0;i<8;++i)printf(" %x",order[i]);puts("");}}
'''
 with tempfile.TemporaryDirectory() as td:
  p=Path(td);(p/'test.c').write_text(pre+body+harness)
  subprocess.run(['C:/MinGW/bin/gcc.exe','-O2',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
  lines=subprocess.check_output([str(p/'test.exe')],text=True).splitlines()
 xbe=ROOT/'game_files/mercenaries-retail/default.xbe';config.configure_from_xbe(str(xbe));raw=xbe.read_bytes();code=raw[config.va_to_file_offset(0x110d00):config.va_to_file_offset(0x110d5b)]
 for mask,line in enumerate(lines):
  u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0,0x400000);u.mem_write(0x110d00,code);order=[]
  def put(a,v):u.mem_write(a,struct.pack('<I',v&0xffffffff))
  def get(a):return struct.unpack('<I',u.mem_read(a,4))[0]
  vals={0x300000:0x320000,0x300004:0x3cc756b3,0x200018:0x200000,0x20001c:0x210000 if mask&1 else 0,0x200020:0x11223344,0x210024:0x55667788,0x2000a8:0x12345678,0x220000:0x240000,0x240034:0x310000,0x230000:0x250000,0x25038c:0x310010}
  for a,v in vals.items():put(a,v)
  for r,v in [(UC_X86_REG_ESP,0x300000),(UC_X86_REG_ESI,0xaabbccdd),(UC_X86_REG_EDI,0xfaceface),(UC_X86_REG_EBX,0xdecafbad),(UC_X86_REG_EAX,0xaaaaaa00 if mask&16 else 0xaaaaaa01),(UC_X86_REG_ECX,0x200018)]:u.reg_write(r,v)
  def hook(uc,a,size,data):
   if a not in [0x1ebc10,0x8c7e0,0x10fa80,0x310000,0x310010]:return
   order.append(a);sp=uc.reg_read(UC_X86_REG_ESP)
   value={0x1ebc10:0x11223344 if mask&2 else 0x11223345,0x8c7e0:0x220000 if mask&4 else 0,0x10fa80:0x1234,0x310000:0x230000,0x310010:0x55667788 if mask&8 else 0x12345600}[a]
   if a==0x10fa80:assert get(sp+4)==0x12345678;put(0x2000a8,0xffffffff)
   uc.reg_write(UC_X86_REG_EAX,value);uc.reg_write(UC_X86_REG_ESP,sp+4);uc.reg_write(UC_X86_REG_EIP,get(sp))
  u.hook_add(UC_HOOK_CODE,hook);u.emu_start(0x110d00,0x320000,count=1000)
  assert u.reg_read(UC_X86_REG_EIP)==0x320000
  want=[u.reg_read(r) for r in [UC_X86_REG_EAX,UC_X86_REG_ESP,UC_X86_REG_ESI,UC_X86_REG_EDI,UC_X86_REG_EBX]]+[get(0x2000a8),len(order)]+order+[0]*(8-len(order))
  got=[int(x,16) for x in line.split()];assert got==want,(mask,[hex(x) for x in got],[hex(x) for x in want])
 print('PASS: 32 production event predicate paths match original x86, including return value, cancellation, call order, stack and saved registers')

def test_dispatcher_has_no_empty_event_predicates():
 text=generated_text_containing('void sub_001113A0(void)');body=re.search(r'void sub_001113A0\(void\)\n\{.*?\n\}',text,re.S)[0]
 targets=set(re.findall(r'\bsub_([0-9A-F]{8})\(\)',body))
 stubs=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_stubs_unresolved.c').read_text()
 empty=set(re.findall(r'void sub_([0-9A-F]{8})\(void\) \{ esp \+= 4;',stubs))
 assert not targets&empty,sorted(targets&empty)
 seeds=json.loads((ROOT/'ports/mercenaries/manual-seeds.json').read_text())
 assert any(int(x['start'],0)==0x110d00 for x in seeds)

if __name__=='__main__':
 test_weapon_selection_event_matches_retail();test_dispatcher_has_no_empty_event_predicates()

"""Compare generated Lua callback return against the retail x86 instructions."""
from pathlib import Path
import re, struct, subprocess, tempfile, sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from tools.recomp import config
from tools.recomp.generated_test_utils import generated_text_containing
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import *
ROOT=Path(__file__).resolve().parents[2]
STATE,CI,VALUES,STACK,STOP=0x200000,0x210000,0x220000,0x230000,0x240000
text=generated_text_containing('void sub_001DF030(void)')
body=re.search(r'void sub_001DF030\(void\)\n\{.*?\n\}',text,re.S)[0]
pre='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <stdio.h>
#include <assert.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;static unsigned char memory[0x300000];
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
void sub_001DEFD0(void){assert(0);}
void recomp_lua_callframe_checkpoint(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e){}
'''
harness=r'''
int main(void){int results,available,delta;g_xbox_mem_offset=(ptrdiff_t)memory;
while(scanf("%d %d %d",&results,&available,&delta)==3){
 memset(memory,0,sizeof(memory));
 for(unsigned i=0;i<512;i++)MEM32(0x220000+4*i)=0xA5000000+i*0x109;
 MEM32(0x200008)=0x220300+available*16;MEM32(0x20000C)=0x220100+delta*16;
 MEM32(0x200014)=0x210018;MEM32(0x210000)=0x220040;
 esp=0x230000;MEM32(esp)=0x240000;MEM32(esp+4)=0x200000;MEM32(esp+8)=results;MEM32(esp+12)=0x220300;
 ebx=0x12345678;esi=0x23456789;edi=0x3456789a;
 sub_001DF030();printf("%x %x %x %x %x %x %x",esp,ebx,esi,edi,MEM32(0x200008),MEM32(0x20000C),MEM32(0x200014));
 for(unsigned i=0;i<512;i++)printf(" %x",MEM32(0x220000+4*i));puts("");
}}
'''
xbe=ROOT/'game_files/mercenaries-retail/default.xbe';config.configure_from_xbe(str(xbe));raw=xbe.read_bytes();off=config.va_to_file_offset(0x1df030);code=raw[off:off+0x100]
cases=[(r,a,d) for r in (-1,0,1,2,4,8,16,32) for a in range(33) for d in (0,1,4,8)]
with tempfile.TemporaryDirectory() as td:
 c=Path(td)/'probe.c';exe=Path(td)/'probe.exe';c.write_text(pre+body+harness)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-O2',str(c),'-o',str(exe)],check=True)
 lines=subprocess.check_output([str(exe)],input=''.join('%d %d %d\n'%v for v in cases),text=True).splitlines()
 assert len(lines)==len(cases)
 for case,line in zip(cases,lines):
  r,a,d=case;u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0,0x300000);u.mem_write(0x1df030,code)
  def put(p,v):u.mem_write(p,struct.pack('<I',v&0xffffffff))
  def get(p):return struct.unpack('<I',u.mem_read(p,4))[0]
  u.mem_write(VALUES,struct.pack('<512I',*[0xA5000000+i*0x109 for i in range(512)]))
  for p,v in ((STATE+8,VALUES+0x300+a*16),(STATE+12,VALUES+0x100+d*16),(STATE+20,CI+24),(CI,VALUES+0x40),(STACK,STOP),(STACK+4,STATE),(STACK+8,r),(STACK+12,VALUES+0x300)):put(p,v)
  for reg,v in ((UC_X86_REG_ESP,STACK),(UC_X86_REG_EBX,0x12345678),(UC_X86_REG_ESI,0x23456789),(UC_X86_REG_EDI,0x3456789a),(UC_X86_REG_EBP,0x456789ab)):u.reg_write(reg,v)
  u.emu_start(0x1df030,STOP,count=20000);assert u.reg_read(UC_X86_REG_EIP)==STOP
  expected=[u.reg_read(reg) for reg in (UC_X86_REG_ESP,UC_X86_REG_EBX,UC_X86_REG_ESI,UC_X86_REG_EDI)]+[get(STATE+n) for n in (8,12,20)]+list(struct.unpack('<512I',u.mem_read(VALUES,2048)))
  actual=[int(v,16) for v in line.split()]
  assert actual==expected,(case,[(i,hex(x),hex(y)) for i,(x,y) in enumerate(zip(actual,expected)) if x!=y][:8])
print(f'PASS: {len(cases)} Lua callback returns match retail x86 stack, preserved registers, frame pointers and all result slots')

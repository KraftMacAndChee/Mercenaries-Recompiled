"""Native retail seat/automatic-hatch and rider-state stack regressions."""
from pathlib import Path
import json,re,subprocess,tempfile,unittest,sys,shutil
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
OWNERS={0x162610:0x162652,0x167E40:0x168016}
class DoorTests(unittest.TestCase):
 def test_generation_and_native_paths(self):
  gen=ROOT/'ports/mercenaries/src/recomp/gen'; found={}
  for p in gen.glob('recomp_*.c'):
   text=p.read_text(encoding='utf-8')
   for a in OWNERS:
    m=re.search(r'void sub_'+f'{a:08X}'+r'\(void\)\n\{.*?\n\}',text,re.S)
    if m: found[a]=m[0]
  self.assertEqual(set(found),set(OWNERS))
  seeds=json.loads((ROOT/'ports/mercenaries/manual-seeds.json').read_text(encoding='utf-8-sig'))
  script=(ROOT/'ports/mercenaries/scripts/Recompile.ps1').read_text(encoding='utf-8-sig')
  for a,b in OWNERS.items():
   self.assertIn(f'0x{a:08X}',[r['start'] for r in seeds]); self.assertIn(f'0x{a:08X}:0x{b:08X}',script)
  raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes()
  config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
  table=[]
  for a in range(0x168018,0x168028,4):
   o=config.va_to_file_offset(a);table.append(f'MEM32(0x{a:X})=0x{int.from_bytes(raw[o:o+4],"little"):X};')
  source='\n'.join(found.values()); calls=set(re.findall(r'\b(sub_[0-9A-F]{8})\(\)',source))-{f'sub_{a:08X}' for a in OWNERS}
  fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset; double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
enum {SEAT=0x10000,MANAGER=0x12000,VEHICLE=0x14000,ENTRANCES=0x16000,RIDER=0x18000,OTHER=0x1a000,VT=0x20000};
static unsigned automatic,open_calls,manager_present,mode,lookup_count,remove_after_first,setstate_calls,detach_calls;
static void unexpected(unsigned a){fprintf(stderr,"Unexpected %08X\n",a);abort();}
static void indirect(unsigned a){
 switch(a){
 case 1:eax=manager_present?ENTRANCES:0;esp+=4;break;
 case 2:assert(MEM32(esp+4)==0);esp+=8;break;
 case 3:eax=(mode!=1);esp+=4;break;
 case 4:eax=1;esp+=4;break;
 default:unexpected(a);
 }
}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(va) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(va,saved) do{indirect(va);assert(esp==(saved));}while(0)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(va) unexpected(va)
'''
  custom={
   '0016CFC0':'assert(ecx==ENTRANCES);assert(MEM32(esp+4)==2);eax=automatic;esp+=8;',
   '0003E970':'assert(ecx==VEHICLE && MEM32(esp+4)==0);open_calls++;esp+=8;',
   '001624B0':'lookup_count++;eax=(remove_after_first && lookup_count>1)?0:RIDER;esp+=4;',
   '00167D70':'eax=(mode==0);esp+=8;',
   '00165400':'eax=1;esp+=20;',
   '001620D0':'esp+=16;',
   '00162960':'esp+=4;',
   '00161FA0':'assert(MEM32(esp+4)==0);setstate_calls++;esp+=8;',
   '00165EF0':'assert(MEM32(esp+4)==RIDER && MEM32(esp+8)==1);detach_calls++;esp+=12;',
  }
  for name in sorted(calls):fixture+=f'void {name}(void) {{ '+custom.get(name[4:],f'unexpected(0x{name[4:]});')+' }\n'
  fixture+=source+r'''
static void reset(void){
 memset(memory,0,sizeof(memory));g_xbox_mem_offset=(ptrdiff_t)memory;
 esp=0x3e0000;ebx=0x11111111;esi=0x22222222;edi=0x33333333;g_seh_ebp=0x44444444;
 MEM32(SEAT)=MANAGER;MEM32(MANAGER)=VEHICLE;MEM32(VEHICLE)=VT;MEM32(RIDER)=VT;
 MEM32(VT+0x20c)=1;MEM32(VT+0x28c)=2;MEM32(VT+0x230)=3;MEM32(VT+0xf0)=4;
 MEM32(SEAT+0x128)=2;automatic=manager_present=1;
 open_calls=lookup_count=remove_after_first=setstate_calls=detach_calls=mode=0;
'''+''.join(table)+r'''
}
static void frame(void){assert(esp==0x3e0004);assert(ebx==0x11111111&&esi==0x22222222&&edi==0x33333333);assert(g_seh_ebp==0x44444444);}
int main(void){
 for(unsigned test=0;test<5;test++){
  reset();if(test==1)MEM32(SEAT+0x128)=-1;if(test==2)MEM32(MANAGER)=0;if(test==3)manager_present=0;if(test==4)automatic=0;
  ecx=SEAT;sub_00162610();frame();assert(open_calls==(test==0));
 }
 for(mode=0;mode<4;){
  unsigned which=mode;reset();mode=which;if(mode==2)MEM32(SEAT+0x11c)=6;
  ecx=SEAT;sub_00167E40();frame();assert(MEM32(SEAT+0x118)==(mode==0?9:mode==1?13:12));assert(setstate_calls==1);mode++;
 }
 for(unsigned which=2;which<4;which++){
  reset();mode=which;if(mode==2)MEM32(SEAT+0x11c)=6;remove_after_first=1;
  ecx=SEAT;sub_00167E40();frame();assert(MEM32(SEAT+0x118)==0);assert(setstate_calls==0);
 }
 for(unsigned which=0;which<2;which++){
  reset();mode=which;MEM32(SEAT+0x114)=1;MEM32(MANAGER+0x8c0)=OTHER;MEM32(OTHER+0x120)=3;
  ecx=SEAT;sub_00167E40();frame();assert(MEM32(OTHER+0x118)==(which?12:9));assert(setstate_calls==2);assert(detach_calls==which);
  assert(MEM32(MANAGER+0x8c4)==(which?0:RIDER));
 }
 puts("PASS: automatic hatch eligibility/open request, all rider branches, missing-rider exits and both handoff tails preserve guest stack/registers");return 0;
}
'''
  with tempfile.TemporaryDirectory(prefix='retail-doors-') as td:
   p=Path(td);(p/'fixture.c').write_text(fixture,encoding='utf-8')
   subprocess.run([shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe','-std=c11','-O1','-fno-strict-aliasing',str(p/'fixture.c'),'-o',str(p/'test.exe')],check=True)
   subprocess.run([str(p/'test.exe')],check=True)
if __name__=='__main__':unittest.main()


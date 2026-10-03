"""Execute restored retail pickup, winch and mounted-reticle code with controlled actors.

Game decisions and stack frames come from freshly generated C. Only engine services
(class lookup, transforms, rendering/audio and collision) are replaced by fixtures.
"""
from pathlib import Path
import json,re,shutil,subprocess,sys,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
GEN=ROOT/'ports/mercenaries/src/recomp/gen'
OWNERS={0x330A0:0x33457,0x108A00:0x108E7D,0x145460:0x1455D2}
NATIVE_FUNCTIONS=(*OWNERS,0x145420,0x1438F0)

def bodies():
 result={}
 for file in GEN.glob('recomp_*.c'):
  text=file.read_text(encoding='utf-8-sig')
  for start in NATIVE_FUNCTIONS:
   m=re.search(r'void sub_'+f'{start:08X}'+r'\(void\)\n\{.*?\n\}',text,re.S)
   if m: result[start]=m[0]
 assert set(result)==set(NATIVE_FUNCTIONS)
 return result
class RetailPathTests(unittest.TestCase):
 def test_iso_generation_inputs(self):
  seeds={int(row['start'],16) for row in json.loads((ROOT/'ports/mercenaries/manual-seeds.json').read_text(encoding='utf-8-sig'))}
  script=(ROOT/'ports/mercenaries/scripts/Recompile.ps1').read_text(encoding='utf-8-sig')
  stubs=(GEN/'recomp_stubs_unresolved.c').read_text(encoding='utf-8-sig')
  for start,end in OWNERS.items():
   self.assertIn(start,seeds)
   self.assertIn(f'0x{start:08X}:0x{end:08X}',script)
  for missing in (0x330A0,0x3344B,0x108A00,0x145460,0x1455A5,0x1455C6):
   self.assertNotIn(f'void sub_{missing:08X}',stubs)
 def test_actual_generated_behavior_and_stack(self):
  source='\n'.join(bodies().values())
  calls=set(re.findall(r'\b(sub_[0-9A-F]{8})\(\)',source))-set(f'sub_{a:08X}' for a in NATIVE_FUNCTIONS)
  raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes()
  config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
  init=[]
  for va in [0x2DF154,0x2DC098,0x2DC874,0x2DC3DC,0x2DDA48,0x2DBED0,0x2DC08C,0x2DC090,*range(0x33458,0x33470,4),*range(0x1455D4,0x1455E4,4)]:
   off=config.va_to_file_offset(va); value=int.from_bytes(raw[off:off+4],'little');init.append(f'MEM32(0x{va:X})=0x{value:X}u;')
  header=(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()
  fixture=f'#define RECOMP_GENERATED_CODE\n#include "{header}"\n'+r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
ptrdiff_t g_xbox_mem_offset;
double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
/* Supplier mappings are disabled in this retail pickup regression. */
int recomp_mod_weapon_allowed(uint32_t weapon, uint32_t actor) { (void)weapon; (void)actor; return 1; }
enum { USER=0x10000, ITEM=0x12000, AI=0x14000, VEHICLE=0x16000, WEAPON=0x18000, HUD=0x1a000, TURRET=0x1c000, ROPE=0x20000, VT=0x30000 };
static unsigned user_type,cache_type,render_calls,complete_calls,has_turret;
static unsigned search_calls,attachment_calls,detach_calls,cargo_present;
static void unexpected(unsigned address) { fprintf(stderr,"Unexpected service %08X\n",address); abort(); }
static void indirect(unsigned address) {
 unsigned object=ecx;
 switch(address) {
 case 1: eax=user_type; esp+=4; break;
 case 2: eax=MEM32(esp+4); memcpy((void*)XBOX_PTR(eax),(void*)XBOX_PTR(object+0x100),12); esp+=8; break;
 case 3: g_fp_stack[--g_fp_top&7]=100.0; esp+=4; break;
 case 4: eax=VEHICLE; esp+=4; break;
 case 5: eax=WEAPON; esp+=4; break;
 case 6: eax=has_turret?TURRET:0; esp+=4; break;
 case 7: eax=0; esp+=4; break;
 case 8: eax=0; esp+=4; break;
 default: unexpected(address);
 }
}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(va) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(va,saved) do { indirect(va); assert(esp==(saved)); } while(0)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(va) unexpected(va)
'''
  custom={
  '0003EE20':'eax=cache_type; esp+=4;',
  '00015A00':'eax=MEM32(esp+4); esp+=8;',
  '0008C7E0':'eax=AI; esp+=4;',
  '00154620':'eax=0; esp+=8;',
  '001EC220':'eax=0; esp+=4;',
  '000F76F0':'eax=0xff55aaffu; esp+=8;',
  '0003C420':'eax=MEM32(esp+4); memset((void*)XBOX_PTR(eax),0,64); MEMF(eax+0x14)=1; esp+=12;',
  '000187A0':'memset((void*)XBOX_PTR(ecx),0,0x88); esp+=12;',
  '0012C650':'eax=0; esp+=4;',
  '001F3400':'eax=MEM32(esp+4); memset((void*)XBOX_PTR(eax),0,12); esp+=4;',
  '0020F910':'eax=384; esp+=4;',
  '0020F920':'eax=512; esp+=4;',
  '00130180':'eax=MEM32(esp+4); memset((void*)XBOX_PTR(eax),0,12); esp+=12;',
  '00130090':'eax=MEM32(esp+4); memset((void*)XBOX_PTR(eax),0,12); esp+=4;',
  '001FFB30':'eax=17; esp+=4;',
  '00143B00':'assert(ecx==ROPE+0x20); render_calls++; esp+=4;',
  '00144270':'assert(ecx==ROPE); complete_calls++; MEM32(ROPE+0x790)=0; esp+=4;',
  '001FA1D0':'esp+=4;',
  '00143700':'render_calls++; esp+=8;',
  '00143740':'eax=edx=0; esp+=4;',
  '001442B0':'assert(MEMF(ROPE+0x7a8)<=0); search_calls++; eax=cargo_present?ITEM:0; esp+=16;',
  '001451C0':'assert(ecx==ROPE+0x20 && MEM32(esp+4)==ITEM); attachment_calls++; esp+=8;',
  '0012FFE0':'assert(ecx==ITEM && MEM32(esp+4)==ROPE); esp+=8;',
  '00130410':'assert(ecx==ITEM); detach_calls++; esp+=4;',
  '00143680':'assert(ecx==ROPE+0x20); esp+=4;',
  }
  for name in sorted(calls):fixture+=f'void {name}(void) {{ '+custom.get(name[4:],f'unexpected(0x{name[4:]});')+' }\n'
  fixture+=''.join(f'void sub_{a:08X}(void);\n' for a in NATIVE_FUNCTIONS)
  fixture+=source+r'''
static void reset(void) {
 memset(memory,0,sizeof(memory));g_xbox_mem_offset=(ptrdiff_t)memory;
 g_fp_top=0; esp=0x3e0000; ebx=0xabc12345; esi=0x12345678; edi=0x76543210; g_seh_ebp=0x22222222;
 render_calls=complete_calls=search_calls=attachment_calls=detach_calls=cargo_present=0; MEM32(0)=0x13572468;
 MEM32(USER)=VT; MEM32(ITEM)=VT; MEM32(VT+4)=1; MEM32(VT+0x34)=2; MEM32(VT+0xe8)=3;
 MEM32(AI)=VT+0x400; MEM32(VT+0x42c)=4; MEM32(VT+0x474)=7;
 MEM32(VEHICLE)=VT+0x800; MEM32(VT+0xa24)=5; MEM32(VT+0xa20)=6; MEM32(VT+0x924)=8;
 MEM32(0x30d4b8)=MEM32(0x30d394)=MEM32(0x30d3b4)=MEM32(0x30d8a8)=0xffff;
 MEM32(0x30d4b4)=11; MEM32(0x30d390)=12; MEM32(0x30d3b0)=13; MEM32(0x30d8a4)=14;
 MEMF(0x30c240)=5.0f; MEMF(USER+0x98)=50.0f;
'''+''.join(init)+r'''
}
static void check_frame(void) { assert(esp==0x3e0008); assert(ebx==0xabc12345 && esi==0x12345678 && edi==0x76543210); assert(MEM32(0)==0x13572468); }
static unsigned pickup(float distance,unsigned type,unsigned kind,float health) {
 reset();user_type=type;cache_type=kind;MEMF(USER+0x98)=health;MEMF(ITEM+0x100)=distance;
 MEM32(esp+4)=USER;ecx=ITEM;sub_000330A0();check_frame();return LO8(eax);
}
int main(void) {
 assert(pickup(2.249f,11,1,50)==1); assert(pickup(2.25f,11,1,50)==0); assert(pickup(2.251f,11,1,50)==0);
 assert(pickup(10.0f,11,1,50)==0); assert(pickup(1.0f,11,1,100)==0);
 assert(pickup(4.999f,12,0,50)==1); assert(pickup(5.0f,12,0,50)==0);
 assert(pickup(NAN,11,1,50)==0);
 for(unsigned kind=0;kind<2;kind++) {
  reset(); has_turret=1; MEM32(WEAPON+0x1a0)=kind; ecx=HUD;MEMF(esp+4)=1.0f/30.0f;
  sub_00108A00();check_frame(); assert(MEM32(HUD+0x30)==2);assert(MEM8(HUD+0x34)==1);
  assert(MEM32(HUD+0x44)==(kind?0x55:0xaa));
  assert(fabsf(MEMF(HUD+0x38)-25.6f)<0.001f); assert(fabsf(MEMF(HUD+0x3c)-12.8f)<0.1f);
 }
 reset();has_turret=0;ecx=HUD;MEMF(esp+4)=1.0f/30;sub_00108A00();check_frame();assert(MEM32(HUD+0x30)==2);
 reset();MEM32(ROPE+0x790)=1;MEM32(ROPE+0x794)=VEHICLE;MEMF(ROPE+0x780)=3.0f;
 for(unsigned i=0;i<91;i++) {
  esp=0x3e0000;MEMF(esp+4)=1.0f/30.0f;ecx=ROPE;sub_00145460();check_frame();
  if(i<89) {assert(render_calls==i+1);assert(complete_calls==0);}
 }
 assert(complete_calls==1 && render_calls>=89 && render_calls<=90);assert(MEM32(ROPE+0x790)==0);
 reset();MEM32(ROPE+0x790)=2;MEMF(ROPE+0x7a8)=0.2f;MEMF(esp+4)=0.05f;ecx=ROPE;sub_00145460();check_frame();assert(fabsf(MEMF(ROPE+0x7a8)-0.15f)<0.0001f);
 reset();MEM32(ROPE+0x790)=3;MEMF(esp+4)=0.05f;ecx=ROPE;sub_00145460();check_frame();assert(render_calls==1);
 /* Execute original cooldown -> search -> real attachment state -> detach -> reacquire.
  * Collision/constraint services supply a nearby eligible cargo; retail decisions run unchanged. */
 for(unsigned rate=30;rate<=60;rate+=30) {
  reset();MEM32(ROPE+0x790)=2;MEM32(ROPE+0x794)=VEHICLE;MEMF(ROPE+0x7a8)=1.0f;cargo_present=1;
  for(unsigned i=0;i<rate+3;i++) {
   esp=0x3e0000;MEMF(esp+4)=1.0f/rate;ecx=ROPE;sub_00145460();check_frame();
   if(i<rate-1) assert(search_calls==0);
  }
  assert(search_calls==1 && attachment_calls==1 && MEM32(ROPE+0x790)==3 && MEM32(ROPE+0x798)==ITEM);
  esp=0x3e0000;ecx=ROPE;sub_001438F0();assert(esp==0x3e0004);
  assert(detach_calls==1 && MEM32(ROPE+0x790)==2 && MEM32(ROPE+0x798)==0 && MEMF(ROPE+0x7a8)==2.0f);
  for(unsigned i=0;i<rate*2+3;i++) {
   esp=0x3e0000;MEMF(esp+4)=1.0f/rate;ecx=ROPE;sub_00145460();check_frame();
   if(i<rate*2-1) assert(search_calls==1);
  }
  assert(search_calls==2 && attachment_calls==2 && MEM32(ROPE+0x790)==3 && MEM32(ROPE+0x798)==ITEM);
 }
 puts("PASS: pickup/reticles/retraction, cooldown -> attach -> release -> reacquire at30/60Hz, native stack/register preservation");return 0;
}
'''
  with tempfile.TemporaryDirectory(prefix='retail-restored-paths-') as tmp:
   p=Path(tmp); (p/'fixture.c').write_text(fixture)
   compiler=shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
   subprocess.run([compiler,'-std=c11','-O1','-fno-strict-aliasing',str(p/'fixture.c'),'-o',str(p/'test.exe')],check=True)
   subprocess.run([str(p/'test.exe')],check=True)
if __name__=='__main__':unittest.main()


"""Exercise production cargo eligibility and retail player boarding/visibility paths."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
gen=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0006.c').read_text()
addresses=['00166130','00162A50','00162C40','00164740']
functions='\n'.join(re.search(r'void sub_'+a+r'\(void\)\n\{.*?\n\}',gen,re.S)[0] for a in addresses)
manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
cpu=manual[manual.index('typedef struct recomp_saved_guest_cpu_context'):manual.index('static void recomp_guest_push_u32')]
source='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'ports/mercenaries/src/recomp/recomp_types.h').as_posix()+'"\n'+r"""
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top;
volatile uint32_t g_recomp_current_func;
uint16_t g_x87_control_word,g_x87_status_word;
uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
ptrdiff_t g_xbox_mem_offset; double g_fp_stack[8];
float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static unsigned char memory[0x400000];
enum {SEAT=0x10000,MANAGER=0x12000,VEHICLE=0x14000,STATE=0x16000,RIDER=0x18000,AI=0x1a000,VT=0x20000};
static int isplayer,has_anim,hidden,connects,started,finished,snapped,played,frozen;
static void *guest_ptr(uint32_t a){return memory+a;}
static int ray_found,ray_calls;
static float ray_height;
static int dev_ray(uint32_t s,uint32_t scratch,const float*start,const float*end,float*hit){
 ++ray_calls;g_eax=123;g_ecx=987;g_esi=33;g_esp=s;hit[1]=ray_height;return ray_found;
}
static uint32_t guest_u32(uint32_t a){return MEM32(a);}
static int dev_guest_address(uint32_t a,uint32_t n){return a>=0x10000 && a<=sizeof(memory)-n;}
static void xbox_preview_log_event(const char*a,const char*b,...){(void)a;(void)b;}
static uint32_t dev_call(uint32_t stack,uint32_t va,uint32_t object,unsigned count,const uint32_t*args){
 /* Deliberately clobber registers to verify helper preservation. */
 g_esp=stack;g_ebx=23;g_esi=42;g_edi=99;g_xmm0[0]=17;g_fp_top=3;
 if(va==0x162540){assert(object==SEAT);hidden=1;return 0;}
 if(va==0x12000){assert(object==VEHICLE && count==1 && args[0]<2);return 0x28000+0x300*args[0];}
 if(va==0x11000){assert(object==RIDER);return isplayer;}
 if(va==0x1625a0){assert(object==SEAT && count==1);return has_anim;}
 if(va==0x63d70){assert(count==3);return has_anim;}
 if(va==0x165260){assert(object==STATE);snapped++;return 0;}
 abort();
}
"""+cpu+'\n#include "'+(ROOT/'ports/mercenaries/src/cargo_helicopters.h').as_posix()+'"\n'+r"""
static void indirect(uint32_t a){
 switch(a){case 0x11000:eax=isplayer;break;case 2:eax=AI;break;case 3:started++;break;case 4:finished++;break;case 5:eax=1;break;default:fprintf(stderr,"bad icall %x\n",a);abort();}esp+=4;
}
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(va) ((void)0)
#undef RECOMP_ICALL_SAFE
#define RECOMP_ICALL_SAFE(va,saved) do{indirect(va);assert(esp==(saved));}while(0)
#undef RECOMP_ITAIL
#define RECOMP_ITAIL(va) abort()
"""
custom={
'00164680':'eax=1;esp+=12;',
'00161FA0':'assert(MEM32(esp+4)==0);esp+=8;',
'00162480':'assert(MEM32(esp+4)==RIDER);esp+=8;',
'001624B0':'eax=RIDER;esp+=4;',
'00162660':'esp+=4;',
'00164820':'esp+=4;',
'001623C0':'esp+=8;',
'001644A0':'connects++;esp+=4;',
'00162540':'hidden=1;esp+=4;',
'00162560':'hidden=0;esp+=4;',
'00162810':'frozen=1;esp+=4;',
'00162840':'frozen=0;esp+=4;',
'00061E50':'played++;eax=1;esp+=16;',
}
calls=set(re.findall(r'\b(sub_[0-9A-F]{8})\(\)',functions))-{f'sub_{a}' for a in addresses}
for f in sorted(calls):source+='void '+f+'(void){'+custom.get(f[4:],'fprintf(stderr,"Unexpected '+f+'\\n");abort();')+'}\n'
source+=functions+r"""
static void reset(uint32_t model){
 memset(memory,0,sizeof(memory));g_xbox_mem_offset=(ptrdiff_t)memory;
 esp=0x3e0000;ebx=0x11111111;esi=0x22222222;edi=0x33333333;g_seh_ebp=0x44444444;
 MEM32(SEAT)=MANAGER;MEM32(MANAGER)=VEHICLE;MEM32(VEHICLE+0x58)=model;MEM32(SEAT+0x114)=1;
 MEM32(STATE+4)=SEAT;MEM32(RIDER)=VT;MEM32(RIDER+0x6b0)=0x22000;MEM32(AI)=VT;
 MEM32(VT+0xf0)=0x11000;MEM32(VT+0x1b8)=2;MEM32(VT+0xe0)=3;MEM32(VT+0xe4)=4;MEM32(VT+0x284)=5;
 isplayer=1;has_anim=0;hidden=connects=started=finished=snapped=played=frozen=0;
}
static void frame(unsigned bytes){assert(esp==0x3e0000+bytes);assert(ebx==0x11111111&&esi==0x22222222&&edi==0x33333333);assert(g_seh_ebp==0x44444444);}
int main(void){
 const uint32_t models[]={316777732u,1811243807u,281358970u};
 for(unsigned m=0;m<3;m++){
  reset(models[m]);assert(recomp_cargo_dock(SEAT,0));assert(recomp_cargo_dock(SEAT,123)==123);
  MEM32(SEAT+0x114)=3;assert(!recomp_cargo_dock(SEAT,0));MEM32(SEAT+0x114)=1;
  for(unsigned anim=0;anim<2;anim++){
   reset(models[m]);has_anim=anim;ecx=SEAT;MEM32(esp+4)=RIDER;MEM32(esp+8)=1;MEM32(esp+12)=0;
   sub_00166130();frame(16);assert(MEM32(SEAT+0x118)==(anim?1:6));assert(MEM32(SEAT+0x11c)==(anim?1:7));assert(connects==1&&started==1&&finished==!anim);
   reset(models[m]);has_anim=anim;assert(recomp_cargo_skip_door(STATE,RIDER)==!anim);frame(0);assert(snapped==!anim);
  }
  reset(models[m]);ecx=STATE;sub_00162A50();frame(4);assert(hidden&&!played&&!frozen);
  esp=0x3e0000;ecx=STATE;sub_00162C40();frame(4);assert(!hidden&&!frozen);
  reset(models[m]);isplayer=0;assert(recomp_cargo_entry_type(SEAT,RIDER,1)==1);assert(!recomp_cargo_skip_door(STATE,RIDER));frame(0);
  ecx=STATE;sub_00162A50();frame(4);assert(!hidden&&played==1&&frozen);
 }
 for(unsigned m=0;m<3;++m)for(int found=0;found<2;++found){
  reset(models[m]);ray_found=found;ray_calls=0;ray_height=60;
  uint32_t matrix=0x24000;MEMF(matrix+0x30)=12;MEMF(matrix+0x34)=63;MEMF(matrix+0x38)=34;
  MEM32(SEAT+0xd8)=recomp_cargo_dock(SEAT,0);ecx=SEAT;MEM32(esp+4)=matrix;MEM32(esp+8)=0;
  sub_00164740();frame(12);assert(eax==1);assert(ray_calls==(m==2));
  assert(MEMF(matrix+0x34)==((m==2&&found)?60:63));assert(MEMF(matrix+0x30)==12 && MEMF(matrix+0x38)==34);
 }
 reset(models[2]);ray_found=1;
 for(unsigned h=0;h<3;++h){
  ray_height=h==0?80:h==1?50:NAN;MEMF(0x24034)=63;
  recomp_cargo_ground_dock(SEAT,0x24000);frame(0);assert(MEMF(0x24034)==63);
 }
 for(unsigned model=0;model<2;model++)for(unsigned index=1;index<=2;index++){
  reset(models[model]);MEM8(SEAT+0x111)=index;MEM32(SEAT+0x114)=model?2:3;
  MEM32(VEHICLE)=VT;MEM32(VT+0x254)=0x12000;
  assert(recomp_cargo_dock(SEAT,0)==(index==1?0xD5B81640u:0xE5E54F0Eu));
  recomp_cargo_configure_seat(SEAT);frame(0);assert(MEM32(SEAT+0x114)==2);
  if(!model){assert(MEM32(SEAT+0x124)==2-index);assert(MEM32(0x28000+0x300*(2-index)+0x2d8)==2);assert(MEM32(SEAT+0xe8)==(index==1?0x1b66a546u:0x246d66fbu));}
  assert(!recomp_cargo_player(SEAT,RIDER));recomp_cargo_hide_gunner(SEAT,RIDER);frame(0);assert(hidden);has_anim=1;
  ecx=SEAT;MEM32(esp+4)=RIDER;MEM32(esp+8)=1;MEM32(esp+12)=0;sub_00166130();frame(16);assert(MEM32(SEAT+0x118)==5);assert(started==1 && finished==0);
  esp=0x3e0000;assert(recomp_cargo_skip_door(STATE,RIDER));frame(0);assert(snapped==1);
  isplayer=0;hidden=0;recomp_cargo_hide_gunner(SEAT,RIDER);assert(!hidden);assert(recomp_cargo_entry_type(SEAT,RIDER,1)==7);assert(recomp_cargo_skip_door(STATE,RIDER));
  assert(!recomp_cargo_defer_gunner_completion(SEAT,RIDER));
  esp=0x3e0000;started=finished=0;ecx=SEAT;MEM32(esp+4)=RIDER;MEM32(esp+8)=1;MEM32(esp+12)=0;
  sub_00166130();frame(16);assert(MEM32(SEAT+0x118)==6);assert(started==1 && finished==1);
 }
 reset(models[2]);MEM8(SEAT+0x111)=1;MEM32(SEAT+0x114)=3;
 assert(recomp_cargo_dock(SEAT,0)==0xD9E53C2Au);assert(!recomp_cargo_player(SEAT,RIDER));
 has_anim=1;assert(recomp_cargo_entry_type(SEAT,RIDER,1)==1);assert(!recomp_cargo_skip_door(STATE,RIDER));
 /* A passive Mi-26 passenger remains an animated NPC rider, not a pilot or
  * player-only deferred gunner. Exercise the original generated entry path. */
 isplayer=0;ecx=SEAT;MEM32(esp+4)=RIDER;MEM32(esp+8)=1;MEM32(esp+12)=0;
 sub_00166130();frame(16);assert(MEM32(SEAT+0x118)==1 && MEM32(SEAT+0x11c)==1);
 assert(connects==1 && started==1 && finished==0);esp=0x3e0000;
 ray_found=1;ray_height=60;MEMF(0x24034)=63;recomp_cargo_ground_dock(SEAT,0x24000);assert(MEMF(0x24034)==60);
 MEM8(SEAT+0x111)=2;assert(!recomp_cargo_dock(SEAT,0));
 reset(0x12345678);assert(!recomp_cargo_dock(SEAT,0));assert(recomp_cargo_entry_type(SEAT,RIDER,1)==1);
 ecx=STATE;sub_00162A50();frame(4);assert(!hidden&&played==1);
 puts("PASS: transport gunner setup, NPC passenger docks, animation-aware boarding/exit, pilot visibility, AI and ordinary helicopter regressions");
}
"""
with tempfile.TemporaryDirectory(prefix='cargo-heli-') as td:
 p=Path(td);(p/'test.c').write_text(source);exe=p/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O1','-fno-strict-aliasing',str(p/'test.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

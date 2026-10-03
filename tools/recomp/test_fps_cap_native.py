"""Production FPS option migration/cycling, frame pacing, and fractional clock tests."""
from pathlib import Path
import re,runpy,subprocess,tempfile,shutil
ROOT=Path(__file__).resolve().parents[2]
def function(s,name):
    return re.search(r'(?:void|double) '+name+r'\([^)]*\)\s*\{.*?\n\}',s,re.S)[0]
cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
options=r"""
#include <assert.h>
static unsigned calls;
static uint32_t expected_flags=RECOMP_OPTIONS_CHANGE_FPS;
static void changed(uint32_t flags){assert(flags==expected_flags);calls++;}
int main(int argc,char **argv){
 assert(argc==3);char path[MAX_PATH];build_config_path(path);
 WritePrivateProfileStringA("RecompOptions","60FPS",argv[1],path);
 if(strcmp(argv[2],"missing"))WritePrivateProfileStringA("RecompOptions","FPSCap",argv[2],path);
 WritePrivateProfileStringA("Keyboard.5","Action15","75",path);
 recomp_options_init();recomp_options_set_apply_callback(changed);
 int expected=atoi(argv[1])?60:30;
 if(!strcmp(argv[2],"0")||!strcmp(argv[2],"30")||!strcmp(argv[2],"60")||!strcmp(argv[2],"90")||!strcmp(argv[2],"120"))expected=atoi(argv[2]);
 assert(recomp_options_fps_cap()==expected);assert(recomp_options_apply()==0 && calls==0);

 /* V-Sync is opt-in, independently persisted, and respects Apply/Cancel. */
 assert(recomp_options_vsync()==0);
 assert(!strcmp(recomp_options_label(RECOMP_OPTIONS_VSYNC_HASH),"V-SYNC: OFF"));
 assert(recomp_options_localization_hash(RECOMP_OPTIONS_VSYNC_HASH)==0x941EE5E8u);
 recomp_options_adjust(RECOMP_OPTIONS_VSYNC_HASH,1);
 assert(!recomp_options_vsync());recomp_options_cancel_edit();assert(!recomp_options_apply());
 expected_flags=RECOMP_OPTIONS_CHANGE_VSYNC;
 for(int enabled=1;enabled>=0;--enabled){
  recomp_options_begin_edit();assert(recomp_options_adjust(RECOMP_OPTIONS_VSYNC_HASH,-1));
  assert(!strcmp(recomp_options_label(RECOMP_OPTIONS_VSYNC_HASH),enabled?"V-SYNC: ON":"V-SYNC: OFF"));
  assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_VSYNC);
  assert(GetPrivateProfileIntA("RecompOptions","VSync",9,path)==enabled);
  memset(&g_options,0,sizeof(g_options));recomp_options_init();
  assert(recomp_options_vsync()==enabled&&recomp_options_fps_cap()==expected);
  assert(GetPrivateProfileIntA("Keyboard.5","Action15",0,path)==75);
  recomp_options_set_apply_callback(changed);
 }
 expected_flags=RECOMP_OPTIONS_CHANGE_FPS;
 /* Pending changes do not affect the renderer or INI until Apply; Back cancels. */
 recomp_options_adjust(RECOMP_OPTIONS_FPS_HASH,1);assert(recomp_options_fps_cap()==expected);
 recomp_options_cancel_edit();assert(!recomp_options_apply());
 /* Walk backwards to 30, then exercise each label and both wrap directions. */
 while(recomp_options_fps_cap()!=30){recomp_options_adjust(RECOMP_OPTIONS_FPS_HASH,-1);recomp_options_apply();}
 const int values[]={60,90,120,0,30};
 const char *labels[]={"FPS CAP: 60","FPS CAP: 90","FPS CAP: 120","FPS CAP: UNCAPPED","FPS CAP: 30"};
 for(unsigned i=0;i<5;i++){
  recomp_options_begin_edit();unsigned old=calls;int before=recomp_options_fps_cap();
  assert(recomp_options_adjust(RECOMP_OPTIONS_FPS_HASH,1));assert(!strcmp(recomp_options_label(RECOMP_OPTIONS_FPS_HASH),labels[i]));
  assert(recomp_options_fps_cap()==before);assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_FPS);assert(calls==old+1);
  assert(recomp_options_fps_cap()==values[i]);assert(GetPrivateProfileIntA("RecompOptions","FPSCap",999,path)==values[i]);
  assert(GetPrivateProfileIntA("RecompOptions","60FPS",999,path)==(values[i]!=30));
  assert(GetPrivateProfileIntA("Keyboard.5","Action15",0,path)==75);
  memset(&g_options,0,sizeof(g_options));recomp_options_init();assert(recomp_options_fps_cap()==values[i]);recomp_options_set_apply_callback(changed);
 }
 recomp_options_adjust(RECOMP_OPTIONS_FPS_HASH,-1);assert(!strcmp(recomp_options_label(RECOMP_OPTIONS_FPS_HASH),"FPS CAP: UNCAPPED"));
 assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_FPS);assert(recomp_options_fps_cap()==0);
 DeleteFileA(path);return 0;
}
"""
pre=r"""
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
typedef unsigned UINT;typedef unsigned long DWORD;typedef int64_t LONGLONG;
typedef struct {LONGLONG QuadPart;} LARGE_INTEGER;
typedef int BOOL;
static BOOL g_vsync_enabled;
static UINT g_frame_cap_fps=30;
static LONGLONG g_next_frame_slot,g_frame_pacing_late_ticks,ticks=9000000000LL;
static unsigned sleeps,yields;static int disable;
static void QueryPerformanceFrequency(LARGE_INTEGER *x){x->QuadPart=900000;}
static void QueryPerformanceCounter(LARGE_INTEGER *x){x->QuadPart=ticks;}
static const char *d3d8_cached_getenv(const char *x){(void)x;return disable?"1":NULL;}
static void Sleep(DWORD ms){sleeps++;ticks+=ms*900;}
static void SwitchToThread(void){yields++;ticks++;}
"""
post=r"""
int main(void){
 const unsigned caps[]={30,60,90,120,0,120,90,60,30};
 for(unsigned c=0;c<sizeof(caps)/sizeof(caps[0]);c++){
  d3d8_SetFrameCap(caps[c]);assert(g_frame_cap_fps==caps[c]);LONGLONG start=ticks;
  for(unsigned i=0;i<1000;i++){ticks+=900;d3d8_WaitForGuestFrameSlot();}
  if(caps[c])assert(ticks-start==900+1000*(900000/caps[c]));else assert(ticks-start==900000);
 }
 /* Slow work must not sleep another frame; live cap changes drop old deadlines. */
 d3d8_SetFrameCap(30);ticks+=900000;LONGLONG before=ticks;unsigned waited=sleeps+yields;
 d3d8_WaitForGuestFrameSlot();assert(ticks==before && sleeps+yields==waited);
 d3d8_SetFrameCap(120);before=ticks;d3d8_WaitForGuestFrameSlot();assert(ticks-before==7500);
 d3d8_SetFrameCap(0);before=ticks;d3d8_WaitForGuestFrameSlot();assert(ticks==before && !g_next_frame_slot);
 d3d8_SetFrameCap(60);disable=1;before=ticks;d3d8_WaitForGuestFrameSlot();assert(ticks==before && !g_next_frame_slot);disable=0;
 d3d8_SetFrameCap(999);assert(g_frame_cap_fps==30);

 /* V-Sync changes clear stale deadlines without changing the FPS cap. */
 g_next_frame_slot=123;d3d8_SetVSync(1);assert(g_vsync_enabled&&g_next_frame_slot==0&&g_frame_cap_fps==30);
 g_next_frame_slot=456;d3d8_SetVSync(7);assert(g_next_frame_slot==456);
 d3d8_SetVSync(0);assert(!g_vsync_enabled&&!g_next_frame_slot);
 /* A blocking 60 Hz Present consumes half of a 30 FPS frame, not an extra frame. */
 d3d8_WaitForGuestFrameSlot();ticks+=15000;before=ticks;
 d3d8_WaitForGuestFrameSlot();assert(ticks-before==15000);
 /* Variable high-FPS deltas conserve elapsed time to less than one retail tick. */
 const unsigned rates[]={30,60,90,120,144,240,1000};
 for(unsigned j=0;j<sizeof(rates)/sizeof(rates[0]);j++){
  recomp_frame_ticks_with_remainder(-1);double total=0,whole=0,legacy=0;
  for(unsigned i=0;i<100000;i++){
   double dt=3000.0/rates[j]+(double)(i%7)*.013;total+=dt;
   double n=recomp_frame_ticks_with_remainder(dt);assert(n==floor(n));whole+=n;legacy+=floor(dt);
  }
  assert(fabs(total-whole)<1.00001);assert(total-legacy>1000);
 }
 recomp_frame_ticks_with_remainder(-1);assert(recomp_frame_ticks_with_remainder(.75)==0);
 assert(recomp_frame_ticks_with_remainder(0)==0);assert(recomp_frame_ticks_with_remainder(.5)==1);
 assert(isnan(recomp_frame_ticks_with_remainder(NAN)));assert(recomp_frame_ticks_with_remainder(.5)==0);
 puts("PASS: cap cadence, switching, slow frames, uncapped, and fractional timer conservation");
 return 0;
}
"""
def main():
 source=(ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
 manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
 clock=function(manual,'recomp_frame_ticks_with_remainder')
 patches=runpy.run_path(str(ROOT/'ports/mercenaries/scripts/Patch-Generated.py'))['PATCHES']
 patch=next(x for x in patches if x.name=='Carry fractional main-loop clock ticks')
 generated=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0007.c').read_text(encoding='utf-8')
 assert generated.count(patch.after)==1
 with tempfile.TemporaryDirectory(prefix='fps-cap-') as tmp:
  d=Path(tmp);p=d/'options.c';exe=p.with_suffix('.exe')
  source_path=(ROOT/'ports/mercenaries/src/recomp_options.c').as_posix()
  p.write_text('#include <stdio.h>\n#include <string.h>\n#define _TRUNCATE ((size_t)-1)\n#define _snprintf_s(b,n,t,...) snprintf(b,n,__VA_ARGS__)\n#define strcpy_s(b,n,s) ((void)(n),strcpy(b,s))\n#define strcat_s(b,n,s) ((void)(n),strcat(b,s))\n#include "'+source_path+'"\n'+options,encoding='utf-8')
  subprocess.run([cc,'-std=c11','-O2','-I'+str(ROOT/'src/input'),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'src'),str(p),'-o',str(exe)],check=True)
  for legacy in ['0','1']:
   for setting in ['missing','30','60','90','120','0','junk','90junk','-1','240','9999999999999999999999999999999999999999999999']:
    subprocess.run([str(exe),legacy,setting],check=True)
  print('PASS: 22 INI migrations, exact labels, bidirectional cycling, Apply/Cancel, restart, preserved bindings, V-Sync persistence/Apply/Cancel')
  p=d/'pacing.c';exe=p.with_suffix('.exe')
  p.write_text(pre+function(source,'d3d8_SetVSync')+function(source,'d3d8_SetFrameCap')+function(source,'d3d8_WaitForGuestFrameSlot')+clock+post,encoding='utf-8')
  subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,timeout=30)
if __name__=='__main__':main()

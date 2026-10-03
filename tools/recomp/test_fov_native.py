"""Exercise production FOV persistence, frustum scaling and slider draw bounds."""
from pathlib import Path
import runpy
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(encoding="utf-8")
    drawing = manual[manual.index("void recomp_options_paint_fov_slider("):manual.index('#include "dev_spawn.h"')]
    options = (ROOT / "ports/mercenaries/src/recomp_options.c").as_posix()
    fixture = r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define _TRUNCATE ((size_t)-1)
#define _snprintf_s(b,n,t,...) snprintf(b,n,__VA_ARGS__)
#define strcpy_s(b,n,s) ((void)(n),strcpy(b,s))
#define strcat_s(b,n,s) ((void)(n),strcat(b,s))
''' + '#include "' + options + '"\n' + r'''
static unsigned char memory[0x800000];
static uint32_t g_esp=0x20000, g_ecx;
typedef struct {uint32_t esp, ecx;} recomp_saved_guest_cpu_context;
typedef void (*recomp_func_t)(void);
static void *guest_ptr(uint32_t a){assert(a<sizeof(memory));return memory+a;}
static uint32_t guest_u32(uint32_t a){uint32_t v;memcpy(&v,guest_ptr(a),4);return v;}
static uint32_t recomp_float_bits(float f){uint32_t v;memcpy(&v,&f,4);return v;}
static void recomp_guest_push_u32(uint32_t v){g_esp-=4;memcpy(guest_ptr(g_esp),&v,4);}
static void recomp_save_guest_cpu_context(recomp_saved_guest_cpu_context *s){s->esp=g_esp;s->ecx=g_ecx;}
static void recomp_restore_guest_cpu_context(const recomp_saved_guest_cpu_context *s){g_esp=s->esp;g_ecx=s->ecx;}
static float rectangles[2][4];static unsigned boxes;
static void begin(void){assert(guest_u32(g_esp+4)==0);g_esp+=24;}
static void box(void){assert(boxes<2);memcpy(rectangles[boxes++],guest_ptr(g_esp+4),16);g_esp+=36;}
static recomp_func_t recomp_lookup(uint32_t a){return a==0x20A0D0?begin:box;}
''' + drawing + r'''
int main(void){
 char path[MAX_PATH];build_config_path(path);DeleteFileA(path);
 recomp_options_init();assert(g_options.applied.fov==55);
 assert(!strcmp(recomp_options_label(RECOMP_OPTIONS_FOV_HASH),"FOV (4:3): 55"));
 assert(recomp_options_localization_hash(RECOMP_OPTIONS_FOV_HASH)==0x941EE5E8u);
 WritePrivateProfileStringA("Keyboard.5","Action15","75",path);
 recomp_options_adjust(RECOMP_OPTIONS_FOV_HASH,1);
 assert(g_options.applied.fov==55);recomp_options_cancel_edit();assert(!recomp_options_apply());
 for(int n=0;n<200;n++)recomp_options_adjust(RECOMP_OPTIONS_FOV_HASH,-1);
 assert(g_options.pending.fov==40);assert(recomp_options_fov_slider_position()==0.f);
 const uint32_t brush=0x10000;
 *(uint32_t*)guest_ptr(brush+0x3c)=4;
 *(uint32_t*)guest_ptr(brush+0x4c)=RECOMP_OPTIONS_FOV_HASH;
 *(float*)guest_ptr(brush+0xc8)=1.f;*(float*)guest_ptr(brush+0xc4)=10.f;
 for(int n=40;n<=100;n++){
  assert(g_options.pending.fov==n);
  float t=(n-40)/60.f;assert(fabsf(recomp_options_fov_slider_position()-t)<1e-6f);
  boxes=0;g_ecx=0xDEADBEEF;
  recomp_options_paint_fov_slider(brush,10.f,12.f);
  assert(boxes==2 && g_esp==0x20000 && g_ecx==0xDEADBEEF);
  float left=rectangles[0][0],right=left+rectangles[0][2];
  float thumb=rectangles[1][0],w=rectangles[1][2];
  assert(thumb>=left && thumb+w<=right);
  assert(fabsf(thumb-(left+(right-left-w)*t))<5e-5f);
  recomp_options_adjust(RECOMP_OPTIONS_FOV_HASH,1);
 }
 assert(g_options.pending.fov==100);assert(recomp_options_fov_slider_position()==1.f);
 assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_FOV);
 assert(GetPrivateProfileIntA("RecompOptions","FOV",0,path)==100);
 assert(GetPrivateProfileIntA("Keyboard.5","Action15",0,path)==75);
 memset(&g_options,0,sizeof(g_options));recomp_options_init();assert(g_options.applied.fov==100);
 for(int fov=40;fov<=100;fov++)for(int mode=0;mode<4;mode++){
  g_options.applied.fov=fov;g_options.applied.aspect=mode;
  float base=55.f*.017453292519943295f;
  float f=recomp_options_perspective_fov(base,.764f);
  float a=recomp_options_perspective_aspect(.764f);
  float result=recomp_options_scale_fov(f);
  float desired=tanf(fov*.017453292519943295f*.5f);
  assert(fabsf(tanf(result*.5f)*a/.764f-desired)<2e-6f);
  float zoom=2.f*atanf(tanf(f*.5f)/4.f);
  assert(fabsf(tanf(result*.5f)/tanf(recomp_options_scale_fov(zoom)*.5f)-4.f)<2e-5f);
  if(fov==55)assert(result==f);
 }
 *(uint32_t*)guest_ptr(brush)=0x300DC4;
 assert(recomp_options_camera_fov(brush,.8f)==recomp_options_scale_fov(.8f));
 *(uint32_t*)guest_ptr(brush)=0;
 assert(fabsf(recomp_options_camera_fov(brush,.8f)-.8f)<1e-6f);
 *(uint32_t*)guest_ptr(brush+0x4c)=RECOMP_OPTIONS_VSYNC_HASH;
 boxes=0;recomp_options_paint_fov_slider(brush,10,12);assert(boxes==0);
 const char *invalid[]={"-1","0","39","101","1000"};
 for(unsigned i=0;i<5;i++){
  WritePrivateProfileStringA("RecompOptions","FOV",invalid[i],path);
  memset(&g_options,0,sizeof(g_options));recomp_options_init();
  assert(g_options.applied.fov==(i<3?40:100));
 }
 DeleteFileA(path);puts("PASS: all 61 slider positions bounded/proportional; default/Apply/Cancel/persistence; aspect and zoom scaling; gameplay-camera restriction");
}
'''
    with tempfile.TemporaryDirectory(prefix="merc-fov-") as temp:
        source = Path(temp) / "test.c"
        exe = source.with_suffix(".exe")
        source.write_text(fixture, encoding="utf-8")
        subprocess.run(["C:/MinGW/bin/gcc.exe", "-std=c11", "-O2", "-I"+str(ROOT/"src/input"), "-I"+str(ROOT/"include"), "-I"+str(ROOT/"src"), str(source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    patches = runpy.run_path(str(ROOT/"ports/mercenaries/scripts/Patch-Generated.py"))["PATCHES"]
    generated = "".join(p.read_text(encoding="utf-8") for p in (ROOT/"ports/mercenaries/src/recomp/gen").glob("recomp_*.c"))
    for patch in patches:
        if patch.name.startswith("Recomp FOV"):
            assert generated.count(patch.after) == 1, patch.name


if __name__ == "__main__":
    main()

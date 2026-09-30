"""Execute the production FMV-volume and LOD/distance helpers against guest fixtures."""
from pathlib import Path
import shutil, subprocess, tempfile
ROOT = Path(__file__).resolve().parents[2]
manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
options=(ROOT/'ports/mercenaries/src/recomp_options.c').read_text(encoding='utf-8')
def function(text, marker):
    start=text.index(marker); body=text.index('{', start); depth=1; end=body+1
    while depth:
        depth += (text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]

def test_volume_and_visual_option_behavior():
    code=r"""
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char memory[0x4000000];
static void *guest_ptr(uint32_t p){return memory+p;}
static uint32_t guest_u32(uint32_t p){return *(uint32_t*)(memory+p);}
static uint8_t guest_u8(uint32_t p){return memory[p];}
static uint32_t g_eax,g_ecx,g_edx,g_esp=0x200000,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top,g_recomp_current_func;
static uint16_t g_x87_control_word,g_x87_status_word;
static double g_fp_stack[8];
static float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
typedef void (*recomp_func_t)(void);
static unsigned calls;
static void set_volume(void){
    uint32_t stream=guest_u32(g_esp+4), settings=guest_u32(stream+0x14);
    int volume=(int)guest_u32(g_esp+8);
    *(int*)(memory+settings+0x1c)=volume-(int)guest_u32(settings+0x20);
    ++calls; g_eax=123;g_ecx=789;g_esp+=12;g_xmm3[2]=987.f;g_fp_top=6;g_mm7=987;
}
static recomp_func_t recomp_lookup(uint32_t p){assert(p==0x29e155);return set_volume;}
static void recomp_options_init(void){}
static int g_object_distance_for_world;
static struct { struct { int object_distance; } applied; } g_options;
"""
    code += function(manual, 'typedef struct recomp_saved_guest_cpu_context') + ' recomp_saved_guest_cpu_context;\n'
    code += function(manual, 'static void recomp_save_guest_cpu_context(')
    code += function(manual, 'static void recomp_restore_guest_cpu_context(')
    code += function(manual, 'static void recomp_guest_push_u32(')
    code += function(manual,'void recomp_movie_apply_volume(')
    code += function(options,'uint32_t recomp_options_high_npc_lod_mask(')
    code += function(options,'int recomp_options_begin_world_load(')
    code += function(options,'float recomp_options_object_distance_multiplier(')
    code += function(options,'float recomp_options_scale_object_distance(')
    code += function(manual,'float recomp_options_camera_far_plane(')
    code += r"""
int main(void){
    uint32_t movie=0x30000, stream=0x40000, settings=0x50000;
    *(uint32_t*)(memory+movie+4)=stream;memory[movie+0x14]=1;
    *(uint32_t*)(memory+movie+0x24)=1;*(uint32_t*)(memory+stream+0x14)=settings;
    *(uint32_t*)(memory+settings+0x20)=600;
    *(float*)(memory+0x842130)=-3000.f;
    g_eax=42;g_xmm3[2]=1.25f;g_mm7=77;
    recomp_movie_apply_volume(movie);
    assert(calls==1 && (int)guest_u32(settings+0x1c)==-3600);
    assert(g_eax==42 && g_ecx==0 && g_esp==0x200000 && g_xmm3[2]==1.25f && g_fp_top==0 && g_mm7==77);
    recomp_movie_apply_volume(movie);assert(calls==1);
    *(float*)(memory+0x842130)=-10000.f;recomp_movie_apply_volume(movie);
    assert(calls==2 && (int)guest_u32(settings+0x1c)==-10600);
    *(float*)(memory+0x842130)=0.f;recomp_movie_apply_volume(movie);
    assert(calls==3 && (int)guest_u32(settings+0x1c)==-600);
    *(float*)(memory+0x842130)=NAN;recomp_movie_apply_volume(movie);assert(calls==3);
    recomp_movie_apply_volume(0);recomp_movie_apply_volume(0x3ffffff);assert(calls==3);
    *(float*)(memory+0x842130)=-4000.f;memory[movie+0x14]=0;recomp_movie_apply_volume(movie);assert(calls==3);
    memory[movie+0x14]=1;*(uint32_t*)(memory+movie+0x24)=5;recomp_movie_apply_volume(movie);assert(calls==3);
    *(uint32_t*)(memory+movie+0x24)=1;*(uint32_t*)(memory+stream+0x14)=0;recomp_movie_apply_volume(movie);assert(calls==3);
    /* Preserve invisible outer LOD and eliminate duplicate crossfade mesh. */
    assert(recomp_options_high_npc_lod_mask(4)==4);
    assert(recomp_options_high_npc_lod_mask(0)==0);
    assert(recomp_options_high_npc_lod_mask(2)==1);
    assert((recomp_options_high_npc_lod_mask(2)&~recomp_options_high_npc_lod_mask(1))==0);
    uint32_t scenery[]={0xD93E11F8,0xBBCAE592,0x49451132,0x6D8B34D5,0xD290C23B,0x9625EE4D,0x38DF0375};
    uint32_t other[]={0x714DAA7A,0xB38C6BF0,0xF542BDC8,0x6958F085,0x1A142D16,0};
    for(int mode=0;mode<4;mode++){
        g_object_distance_for_world=mode;
        g_options.applied.object_distance=mode;
        assert(recomp_options_object_distance_multiplier()==1.f);
        for(unsigned i=0;i<sizeof(scenery)/sizeof(*scenery);i++)assert(recomp_options_scale_object_distance(212.f,scenery[i])==212.f);
        for(unsigned i=0;i<sizeof(other)/sizeof(*other);i++)assert(recomp_options_scale_object_distance(212.f,other[i])==212.f);
        assert(recomp_options_begin_world_load()==(mode!=0));
        assert(g_object_distance_for_world==0);
        assert(recomp_options_begin_world_load()==0);
    }
    uint32_t camera=0x60000;*(uint32_t*)(memory+camera)=0x300DC4;
    *(float*)(memory+camera+0xa0)=300.f;
    assert(recomp_options_camera_far_plane(camera,300.f)==300.f);
    assert(recomp_options_camera_far_plane(camera,600.f)==600.f);
    assert(*(float*)(memory+camera+0xa0)==300.f);
    assert(recomp_options_camera_far_plane(0,300.f)==300.f);
    puts("PASS: FMV first packet/volume changes/headroom/CPU preservation; invisible LOD preserved; scenery-only ranges and terrain separation");
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix='merc-visual-audio-') as d:
        c=Path(d)/'test.c'; exe=Path(d)/'test.exe';c.write_text(code,encoding='utf-8')
        subprocess.run([shutil.which('gcc'),'-O2','-std=c11',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)


def test_cached_model_spawn_distance_rebuilds_lod():
    generated=(ROOT/'ports/mercenaries/src/recomp/gen/recomp_0011.c').read_text(encoding='utf-8')
    code=r"""
#include <stdint.h>
#include <assert.h>
static unsigned char memory[4096];
static uint32_t eax,ecx=128,esp=2048;
static float xmm0v[4];
#define xmm0 xmm0v[0]
#define MEM32(a) (*(uint32_t*)(memory+(a)))
#define MEM8(a) memory[(a)]
#define MEMF(a) (*(float*)(memory+(a)))
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define recomp_xmm_loadss(v,a) ((v)[0]=MEMF(a))
"""
    code+=function(generated,'void sub_00220020(')
    code+=r"""
static void set_distance(uint32_t type,float range){
    esp=2048;MEM32(esp+4)=type;MEMF(esp+8)=range;
    sub_00220020();assert(esp==2060);
}
int main(void){
    MEM32(ecx+0x2c)=3;MEMF(ecx+0x30)=300.f;MEM8(ecx+0x28)=0xad;
    set_distance(3,300.f);assert(MEM8(ecx+0x28)==0xad);
    set_distance(3,600.f);assert(MEM8(ecx+0x28)==0xa5);
    assert(MEMF(ecx+0x30)==600.f);
    MEM8(ecx+0x28)|=8;
    set_distance(3,300.f);assert(MEM8(ecx+0x28)==0xa5);
    MEM8(ecx+0x28)|=8;
    set_distance(4,300.f);assert(MEM8(ecx+0x28)==0xa5);
    assert(MEM32(ecx+0x2c)==4);
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix='merc-model-reload-') as d:
        c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(code,encoding='utf-8')
        subprocess.run([shutil.which('gcc'),'-O2','-std=c11',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)


def test_disabled_object_distance_migrates_and_consumes_menu_input():
    header=(ROOT/'ports/mercenaries/src/recomp_options.h').as_posix()
    code='#include "'+header+'"\n'+r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define MAX_PATH 260
#define _snprintf_s(dst,size,trunc,...) snprintf(dst,size,__VA_ARGS__)
#define _TRUNCATE 0
static int old_distance,stored_distance,object_reads;
static unsigned GetPrivateProfileIntA(const char *section,const char *key,int fallback,const char *path){
 if(!strcmp(key,"ObjectDrawDistance")){object_reads++;return old_distance;}
 if(!strcmp(key,"NpcDrawDistance"))return 2;
 return fallback;
}
"""
    code+=options[options.index('typedef struct recomp_option_values'):options.index('static void build_config_path')]
    code+=r"""
static void build_config_path(char *p){strcpy(p,"test.ini");}
static void apply_test_overrides(recomp_option_values *p){}
static void write_setting(const char *key,int v){if(!strcmp(key,"ObjectDrawDistance"))stored_distance=v;}
"""
    for marker in ['static void write_all_settings(', 'void recomp_options_init(',
                   'void recomp_options_begin_edit(', 'void recomp_options_cancel_edit(',
                   'static uint32_t changed_options(', 'uint32_t recomp_options_apply(',
                   'static int cycle(', 'int recomp_options_adjust(',
                   'const char *recomp_options_label(', 'int recomp_options_object_draw_distance(']:
        code+=function(options,marker)+'\n'
    code+=r"""
int main(void){
 for(int old=0;old<5;old++){
  memset(&g_options,0,sizeof(g_options));old_distance=old;stored_distance=-1;object_reads=0;
  recomp_options_init();
  assert(recomp_options_object_draw_distance()==0&&g_object_distance_for_world==0);
  assert(g_options.applied.npc_draw_distance==0);
  recomp_options_begin_edit();
  for(int i=0;i<8;i++){
   assert(recomp_options_adjust(RECOMP_OPTIONS_OBJECT_DISTANCE_HASH,i%2?1:-1)==1);
   assert(recomp_options_adjust(RECOMP_OPTIONS_NPC_DISTANCE_HASH,i%2?1:-1)==1);
   assert(g_options.pending.npc_draw_distance==0);
   assert(g_options.pending.object_distance==0&&recomp_options_apply()==0);
  }
  assert(strstr(recomp_options_label(RECOMP_OPTIONS_OBJECT_DISTANCE_HASH),"DISABLED"));
  assert(!strstr(recomp_options_label(RECOMP_OPTIONS_OBJECT_DISTANCE_HASH),"RELOAD"));
  recomp_options_adjust(RECOMP_OPTIONS_FPS_HASH,1);
  assert(recomp_options_apply()==RECOMP_OPTIONS_CHANGE_FPS&&stored_distance==0);
  assert(g_options.applied.npc_draw_distance==0&&object_reads==0);
 }
 puts("PASS: old object overrides ignored; disabled menu cannot change range; other settings preserved");
}
"""
    with tempfile.TemporaryDirectory(prefix='merc-disabled-distance-') as d:
        c=Path(d)/'test.c';exe=Path(d)/'test.exe';c.write_text(code)
        subprocess.run([shutil.which('gcc'),'-O2','-std=c11',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=10)

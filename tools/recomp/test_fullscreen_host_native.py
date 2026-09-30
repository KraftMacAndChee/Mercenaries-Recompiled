"""Exercise production focus/display coordination without changing desktop mode."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'ports/mercenaries/src/main.c').read_text()
body=s[s.index('static HWND g_host_window;'):s.index('static BOOL host_graphics_init(')]
source=r"""
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include "recomp_options.h"
typedef void IDirect3DDevice8;
static int mode=2,allowed,toggles,requests,resizes,full,x,y,width,height,fail_exclusive;
static UINT rw,rh;
static BOOL resize(UINT w,UINT h,BOOL f){++resizes;rw=w;rh=h;full=f;return !(f && fail_exclusive);}
static HMONITOR test_monitor(HWND h,DWORD f){return (HMONITOR)1;}
static BOOL info(HMONITOR h,LPMONITORINFO m){m->rcMonitor=(RECT){-1920,0,0,1080};return 1;}
static LONG_PTR test_style(HWND h,int i,LONG_PTR s){return 0;}
static BOOL pos(HWND a,HWND b,int xx,int yy,int w,int h,UINT flags){assert(flags & SWP_NOACTIVATE);x=xx;y=yy;width=w;height=h;return 1;}
static BOOL post(HWND h,UINT m,WPARAM w,LPARAM l){++requests;return 1;}
#define MonitorFromWindow test_monitor
#define GetMonitorInfoA info
#undef SetWindowLongPtrA
#define SetWindowLongPtrA test_style
#define SetWindowPos pos
#define PostMessageA post
#define d3d8_ResizePresentation resize
void d3d8_SetFrameCap(int x){}
void d3d8_SetForceAnisotropic16x(int x){}
void pgraph_d3d11_set_haze_mode(int x){}
void d3d8_SetPresentationAspect(uint32_t a,uint32_t b){}
void xbox_set_widescreen_enabled(int x){}
void pgraph_d3d11_set_internal_resolution(uint32_t a,uint32_t b){}
void recomp_options_refresh_retail_camera_projection(void){}
int recomp_options_fps_cap(void){return 60;}
int recomp_options_anisotropic_16x(void){return 0;}
int recomp_options_authentic_haze(void){return 1;}
int recomp_options_aspect_ratio(void){return 1;}
void recomp_options_presentation_aspect(uint32_t*a,uint32_t*b){*a=16;*b=9;}
void recomp_options_resolution_size(uint32_t*a,uint32_t*b){*a=1280;*b=720;}
void recomp_options_internal_resolution_size(uint32_t*a,uint32_t*b){*a=1280;*b=720;}
recomp_display_mode recomp_options_display_mode(void){return (recomp_display_mode)mode;}
void preview_snapshot(const char*s){}
int recomp_controls_message(HWND h,UINT m,WPARAM w,LPARAM l){return 0;}
int recomp_dev_menu_allowed(void){return allowed;}
void recomp_dev_menu_toggle(void){++toggles;}
void recomp_dev_freecam_toggle(void){}
void xbox_preview_log_event(const char*a,const char*b){}
"""+body+r"""
int main(void){
 g_host_window=(HWND)1;g_host_ready=1;
 host_apply_recomp_options(RECOMP_OPTIONS_CHANGE_DISPLAY);
 assert(full && rw==1280 && rh==720 && x==-1920 && y==0 && width==1280 && height==720);
 host_dev_display(1);assert(!full && rw==1920 && rh==1080 && width==1920 && x==-1920);
 host_dev_display(0);assert(requests==1);host_window_proc(g_host_window,WM_RECOMP_DISPLAY,0,0);assert(full);
 host_window_proc(g_host_window,WM_ACTIVATEAPP,0,0);assert(requests==2);
 host_window_proc(g_host_window,WM_RECOMP_DISPLAY,0,0);assert(!full && width==1920 && height==1080);
 host_window_proc(g_host_window,WM_ACTIVATEAPP,1,0);
 host_window_proc(g_host_window,WM_RECOMP_DISPLAY,0,0);assert(full && width==1280 && height==720);
 for(unsigned i=0;i<3;++i){host_dev_display(1);host_dev_display(0);host_window_proc(g_host_window,WM_RECOMP_DISPLAY,0,0);assert(full);}
 host_window_proc(g_host_window,WM_KEYDOWN,VK_F9,0);assert(toggles==0);
 allowed=1;host_window_proc(g_host_window,WM_KEYDOWN,VK_F9,0);host_window_proc(g_host_window,WM_KEYDOWN,VK_F9,1L<<30);assert(toggles==1);
 fail_exclusive=1;host_apply_recomp_options(RECOMP_OPTIONS_CHANGE_DISPLAY);assert(!full && width==1920 && height==1080);fail_exclusive=0;
 mode=1;host_apply_recomp_options(RECOMP_OPTIONS_CHANGE_DISPLAY);assert(!full && width==1920 && height==1080);
 mode=0;host_apply_recomp_options(RECOMP_OPTIONS_CHANGE_DISPLAY);assert(!full && rw==1280 && rh==720);
 int before=requests;host_window_proc(g_host_window,WM_ACTIVATEAPP,0,0);assert(requests==before);
 puts("PASS: exclusive/panel/focus restoration, secondary monitor placement, borderless/windowed and F9 opt-in");
}
"""
with tempfile.TemporaryDirectory(prefix='merc-host-display-') as folder:
 d=Path(folder);(d/'test.c').write_text(source);exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I'+str(ROOT/'ports/mercenaries/src'),str(d/'test.c'),'-o',str(exe),'-luser32'],check=True)
 subprocess.run([str(exe)],check=True)

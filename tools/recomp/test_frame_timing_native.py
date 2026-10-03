"""Native checks for bounded successful-Present interval telemetry."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef int BOOL;typedef long HRESULT;typedef unsigned UINT;typedef void IDXGISwapChain;
#define FALSE 0
#define S_OK 0
#define SUCCEEDED(hr) ((hr)>=0)
#define DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING 2048
#define DXGI_PRESENT_ALLOW_TEARING 512
#define DXGI_PRESENT_TEST 1
#define FAILED(hr) ((hr)<0)
static HRESULT present_result;static int failures;
static struct {void *d3d11_device, *d3d11_context, *current_rtv, *current_dsv; int flip_model; UINT swap_chain_flags;} g_device_state;
static unsigned last_flags,last_sync,rebinds,fullscreen_queries;static BOOL fullscreen;static HRESULT query_result;
static HRESULT IDXGISwapChain_GetFullscreenState(IDXGISwapChain *c,BOOL *f,void *o){(void)c;(void)o;++fullscreen_queries;*f=fullscreen;return query_result;}
static void ID3D11DeviceContext_OMSetRenderTargets(void *c,UINT n,void **r,void *d){assert(c==g_device_state.d3d11_context&&n==1&&*r==g_device_state.current_rtv&&d==g_device_state.current_dsv);++rebinds;}
static HRESULT IDXGISwapChain_Present(IDXGISwapChain *c,UINT s,UINT f){(void)c;last_sync=s;last_flags=f;return present_result;}
static HRESULT ID3D11Device_GetDeviceRemovedReason(void *d){(void)d;return -7;}
static void xbox_preview_log_event(const char *c,const char *f,...){(void)c;(void)f;failures++;}
typedef struct {long long QuadPart;} LARGE_INTEGER;
static long long clock_now=1;
static int QueryPerformanceFrequency(LARGE_INTEGER *v){v->QuadPart=1000000;return 1;}
static int QueryPerformanceCounter(LARGE_INTEGER *v){v->QuadPart=clock_now;return 1;}
'''
POST=r'''
int main(void){
 D3D8FrameTimingSnapshot s,t;d3d8_GetFrameTimingSnapshot(&s);assert(s.samples==0 && s.submitted_frames==0);
 uint32_t history[512];uint64_t serial;
 assert(d3d8_GetFrameIntervalHistory(0,history,512,&serial)==0 && serial==0);
 record_frame_submission();
 for(int i=1;i<=100;i++){clock_now+=i*1000;record_frame_submission();}
 d3d8_GetFrameTimingSnapshot(&s);
 assert(d3d8_GetFrameIntervalHistory(0,history,512,&serial)==100 && serial==100);
 for(int i=0;i<100;i++)assert(history[i]==(uint32_t)(i+1)*1000);
 assert(d3d8_GetFrameIntervalHistory(90,history,4,&serial)==4 && history[0]==97000 && history[3]==100000);
 assert(d3d8_GetFrameIntervalHistory(serial,history,512,&serial)==0);
 assert(d3d8_GetFrameIntervalHistory(9999,history,512,&serial)==0);
 assert(s.submitted_frames==101 && s.samples==100);assert(s.mean_ms==50.5);
 assert(s.p50_ms==50 && s.p95_ms==95 && s.p99_ms==99 && s.max_ms==100);
 d3d8_GetFrameTimingSnapshot(&t);assert(memcmp(&s,&t,sizeof(s))==0);
 for(int i=0;i<600;i++){clock_now+=16667;record_frame_submission();}
 d3d8_GetFrameTimingSnapshot(&s);assert(s.samples==512 && s.submitted_frames==701);
 assert(fabs(s.p50_ms-16.667)<1e-6 && fabs(s.p99_ms-16.667)<1e-6 && fabs(s.max_ms-16.667)<1e-6);
 assert(d3d8_GetFrameIntervalHistory(0,history,512,&serial)==512 && serial==700);
 for(int i=0;i<512;i++)assert(history[i]==16667);
 clock_now+=5000000000LL;record_frame_submission();d3d8_GetFrameTimingSnapshot(&s);
 assert(fabs(s.max_ms-4294967.295)<1e-6);d3d8_GetFrameTimingSnapshot(NULL);
 uint64_t count=s.submitted_frames;
 g_pending_scanout_present=1;present_result=0;preview_present(NULL,0);
 assert(!g_pending_scanout_present && g_frame_submissions==count+1 && failures==0);
 g_pending_scanout_present=1;present_result=-1;preview_present(NULL,0);
 assert(!g_pending_scanout_present && g_frame_submissions==count+1 && failures==1);
 g_pending_scanout_present=1;present_result=1;preview_present(NULL,0);
 assert(!g_pending_scanout_present && g_frame_submissions==count+1 && failures==1);
 assert(!rebinds && !fullscreen_queries);
 g_device_state.flip_model=1;g_device_state.current_rtv=&s;
 g_device_state.swap_chain_flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
 present_result=0;preview_present(NULL,0);assert(rebinds==1&&last_flags==DXGI_PRESENT_ALLOW_TEARING&&last_sync==0);
 fullscreen=1;preview_present(NULL,0);assert(rebinds==2&&last_flags==0);
 fullscreen=0;g_vsync_enabled=1;preview_present(NULL,DXGI_PRESENT_ALLOW_TEARING);assert(rebinds==3&&last_flags==0&&last_sync==1);
 g_vsync_enabled=0;
 query_result=-1;preview_present(NULL,0);assert(rebinds==4&&last_flags==0);
 present_result=-1;preview_present(NULL,0);assert(rebinds==4);
 present_result=0;g_device_state.current_rtv=NULL;preview_present(NULL,0);assert(rebinds==4);
 g_vsync_enabled=1;g_device_state.flip_model=0;preview_present(NULL,DXGI_PRESENT_ALLOW_TEARING);assert(last_sync==1&&last_flags==0);
 g_vsync_enabled=0;preview_present(NULL,0);assert(last_sync==0&&last_flags==0);
 puts("PASS: V-Sync intervals and tearing flags; exact nearest-rank percentiles, bounded rolling window, non-destructive reads and long-gap clamp");
}
'''
class TimingTests(unittest.TestCase):
 def test_production_frame_statistics(self):
  s=(ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
  a=s.index('static BOOL g_pending_scanout_present;');b=s.index('/* Preserve Present',a)
  # No Windows/D3D emulation: exercise just the production collector with a controlled clock.
  end=s.index('\n}\n',s.index('static HRESULT preview_present',b))+3
  block=s[a:end]
  h=(ROOT/'src/d3d/d3d8_frame_timing.h').read_text(encoding='utf-8')
  with tempfile.TemporaryDirectory(prefix='frame-times-') as td:
   p=Path(td)/'test.c';exe=p.with_suffix('.exe');p.write_text(PRE+h+block+POST,encoding='utf-8')
   cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
   r=subprocess.run([cc,'-O2','-std=c11','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stdout+r.stderr)
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
   print(r.stdout.strip())
if __name__=='__main__':unittest.main()

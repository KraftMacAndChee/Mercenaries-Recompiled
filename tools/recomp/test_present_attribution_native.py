"""Execute the real Present path with deterministic fine/coarse clocks.

Distinguishes frame-limit waits, capture/preparation, DXGI work and work outside
Present, while proving disabled tracing leaves the render-call order intact.
"""
from pathlib import Path
import subprocess, tempfile, shutil
ROOT = Path(__file__).resolve().parents[2]
PRE = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
typedef int64_t LONGLONG; typedef uint64_t ULONGLONG; typedef unsigned UINT;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
static LONGLONG ticks=5184000000010LL;
static int enabled=1, frequency_ok=1, qpc_calls, frequency_calls;
static int prepare_us, wait_us, present_us, capture_us;
static unsigned pumps, waits, presents, refreshes, callbacks, captures;
static unsigned samples; static char record[1024];
static LONGLONG submission_tick, g_frame_pacing_late_ticks;
static uint64_t g_frame_submissions;
static unsigned g_frame_interval_count, g_frame_interval_next, g_frame_intervals_us[512];
static unsigned g_present_frame_count; static int g_frame_cap_fps=60;
static int g_debug_force_capture, g_debug_capture_at_flip;
static char g_debug_armed_capture_path[64];
static struct {void *swap_chain;} g_device_state;
static void callback(void){callbacks++;}
static void (*g_debug_present_callback)(void)=callback;
static int QueryPerformanceFrequency(LARGE_INTEGER *v){frequency_calls++;v->QuadPart=1000000;return frequency_ok;}
static int QueryPerformanceCounter(LARGE_INTEGER *v){qpc_calls++;v->QuadPart=ticks;return 1;}
/* Deliberately use the coarse Windows clock granularity as a negative control. */
static ULONGLONG GetTickCount64(void){return (ULONGLONG)(ticks/15625)*15625/1000;}
static const char *d3d8_cached_getenv(const char *name){return enabled && !strcmp(name,"MERCENARIES_TRACE_PRESENT_TIMING")?"1":NULL;}
static void d3d8_pump_host_messages(void){pumps++;ticks+=prepare_us;}
static void d3d8_debug_backbuffer_stats(void){assert(g_debug_capture_at_flip);captures++;ticks+=capture_us;}
static void d3d8_debug_capture_display_refresh(ULONGLONG ms){(void)ms;refreshes++;}
static void d3d8_WaitForGuestFrameSlot(void){waits++;ticks+=wait_us;}
static void preview_present(void *p,int flags){assert(p && !flags);presents++;ticks+=present_us;g_frame_submissions++;g_frame_interval_count=1;g_frame_interval_next=1;g_frame_intervals_us[0]=submission_tick?(unsigned)(ticks-submission_tick):0;submission_tick=ticks;}
static void xbox_preview_log_sample(const char *cat,const char *fmt,...){
 assert(!strcmp(cat,"present-slow"));va_list args;va_start(args,fmt);vsnprintf(record,sizeof(record),fmt,args);va_end(args);samples++;
}
"""
POST = r"""
static double field(const char *name){const char *p=strstr(record,name);assert(p);return strtod(p+strlen(name),NULL);}
static void near(const char *name,double v){assert(fabs(field(name)-v)<.00051);}
int main(int argc,char **argv){
 assert(argc==2);enabled=strcmp(argv[1],"disabled")!=0;frequency_ok=strcmp(argv[1],"no-frequency")!=0;
 g_device_state.swap_chain=&g_device_state;
 if(!enabled || !frequency_ok){
  for(unsigned i=0;i<100000;i++)d3d8_PresentFrame();
  assert(!qpc_calls && !samples);assert(frequency_calls==(enabled?1:0));
  assert(pumps==100000 && waits==pumps && presents==pumps && callbacks==pumps && refreshes==pumps);
  puts("PASS: 100000 disabled/frequency-failure calls, no QPC work, render path unchanged");return 0;
 }
 /* A normal frame whose duration is almost all frame-cap wait is not a hitch. */
 prepare_us=250;wait_us=16000;present_us=125;d3d8_PresentFrame();assert(!samples);
 /* Preparation/capture cost is attributed separately from intentional waiting. */
 ticks+=1000;prepare_us=13000;wait_us=3000;present_us=500;d3d8_PresentFrame();
 assert(samples==1);near("prepare_ms=",13);near("wait_ms=",3);near("present_ms=",.5);near("outside_ms=",1);near("gap_ms=",17.375);near("total_ms=",16.5);
 /* Expensive DXGI call, sub-millisecond prep, no extra waiting. */
 ticks+=500;prepare_us=100;wait_us=0;present_us=9123;d3d8_PresentFrame();
 assert(samples==2);near("prepare_ms=",.1);near("wait_ms=",0);near("present_ms=",9.123);near("total_ms=",9.223);
 /* A hitch outside Present must not be mislabeled as Present/limiter work. */
 ticks+=28000;prepare_us=125;present_us=250;d3d8_PresentFrame();
 assert(samples==3);near("other_submissions=",0);near("submitted_interval_us=",28375);near("outside_ms=",28);near("gap_ms=",37.223);near("total_ms=",.375);
 /* 30 Hz movie cadence is normal; a genuinely late 30 Hz frame is recorded. */
 g_frame_cap_fps=30;prepare_us=0;wait_us=0;present_us=0;
 ticks+=33000;d3d8_PresentFrame();assert(samples==3);
 ticks+=51000;d3d8_PresentFrame();assert(samples==4);near("gap_ms=",51);
 /* Alternate scanout submissions fill a long direct-call gap: no false hitch. */
 ticks+=51000;g_frame_submissions+=2;submission_tick=ticks-16667;d3d8_PresentFrame();assert(samples==4);
 /* Absent swap chain does not record fictional DXGI time. */
 g_device_state.swap_chain=NULL;prepare_us=8500;present_us=10000;d3d8_PresentFrame();
 assert(samples==5);near("present_ms=",0);near("prepare_ms=",8.5);
 /* Armed screenshot work remains visible in preparation, capture flag restored. */
 g_debug_force_capture=1;capture_us=12345;prepare_us=125;d3d8_PresentFrame();
 assert(samples==6 && captures==1 && !g_debug_capture_at_flip);near("prepare_ms=",12.470);
 /* Once-per-second aggregates retain decimal QPC precision and reset. */
 ticks+=1000000;g_debug_force_capture=0;capture_us=0;prepare_us=0;d3d8_PresentFrame();
 assert(frequency_calls==1 && qpc_calls==4*(int)pumps && pumps==waits && refreshes==pumps && callbacks==pumps);
 /* Minor hitches must be observable, including limiter overshoot. */
 unsigned before=samples;g_device_state.swap_chain=&g_device_state;g_frame_cap_fps=60;
 present_us=0;submission_tick=ticks;ticks+=19000;g_frame_pacing_late_ticks=1600;d3d8_PresentFrame();
 assert(samples==before+1);near("submitted_interval_us=",19000);near("pacing_late_ms=",1.6);
 before=samples;g_frame_cap_fps=30;g_frame_pacing_late_ticks=0;
 ticks+=37000;d3d8_PresentFrame();assert(samples==before+1);near("submitted_interval_us=",37000);
 puts("PASS: sub-ms attribution, intentional waiting, outside work, 30/60 Hz, absent swapchain, capture and long uptime");return 0;
}
"""
def main():
    source=(ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
    block=source[source.index('void d3d8_PresentFrame(void)'):source.index('void d3d8_DebugCaptureFrameNow(void)')]
    with tempfile.TemporaryDirectory(prefix='present-attribution-') as tmp:
        p=Path(tmp)/'test.c';exe=p.with_suffix('.exe')
        p.write_text(PRE+block+POST,encoding='utf-8')
        cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
        for mode in ('disabled','no-frequency','enabled'):
            r=subprocess.run([str(exe),mode],text=True,capture_output=True,timeout=10)
            print(r.stdout.strip());print(r.stderr.strip());r.check_returncode()
        assert 'max_present_ms=9.123' in r.stderr and 'max_prepare_ms=13.000' in r.stderr
if __name__=='__main__': main()


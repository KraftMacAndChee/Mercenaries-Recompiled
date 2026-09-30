"""Execute production pending scanout queue, including frame-boundary consumption."""
from pathlib import Path
import subprocess, shutil, tempfile, unittest
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint64_t ULONGLONG;typedef struct {long long QuadPart;} LARGE_INTEGER;
static long long clock_ticks, clock_hz=1000000;
static int QueryPerformanceFrequency(LARGE_INTEGER *v){v->QuadPart=clock_hz;return 1;}
static int QueryPerformanceCounter(LARGE_INTEGER *v){v->QuadPart=clock_ticks;return 1;}
static ULONGLONG GetTickCount64(void){return (clock_ticks/clock_hz)*1000;}
typedef struct {uint32_t offset,width,height,logical_width,logical_height,anti_aliasing,draw_generation;int drawn;uint64_t write_serial;void *texture,*srv;} GuestColorSurface;
static struct {GuestColorSurface *pending_scanout;ULONGLONG pending_scanout_write_us;uint32_t surface_generation;} g_pg;
static int g_debug_post_pda_resolve_trace_active;static unsigned g_debug_post_pda_scanout_trace_index;
static uintptr_t g_xbox_mem_offset;static unsigned g_mercenaries_gameplay_capture_active;
static int copies,composites,allow_copy=1;
static const char *pgraph_cached_getenv(const char *name){(void)name;return NULL;}
static int d3d8_CopyTextureToBackbuffer(void *a,void *b,uint32_t w,uint32_t h){assert(a && b && w==640 && h==480);copies++;return allow_copy;}
static void composite_pvideo_overlay(GuestColorSurface *s){assert(s==g_pg.pending_scanout);composites++;}
static void d3d8_DebugCaptureFrameToPath(const char *s){(void)s;assert(0);}
static void d3d8_DebugCaptureFrameNow(void){assert(0);}
'''
POST=r'''
int main(void){
 GuestColorSurface surface={0};surface.width=640;surface.height=480;surface.texture=surface.srv=&surface;
 /* Every timer phase: the ordinary service keeps the full quiet interval. */
 for(int phase=0;phase<32000;phase+=113){
  clock_ticks=phase;g_pg.pending_scanout=&surface;g_pg.pending_scanout_write_us=pgraph_scanout_clock_us();
  clock_ticks+=49999;assert(!service_pending_scanout(0));assert(g_pg.pending_scanout==&surface);
  clock_ticks++;assert(service_pending_scanout(0));assert(!g_pg.pending_scanout);
  assert(!service_pending_scanout(1));
 }
 /* Normal 30/60 Hz rendering owns its pending image even when a host
  * poll occurs more than 2 ms after the resolve. Sweep poll phases. */
 for(int period=16667;period<=33334;period+=16667){
  for(int phase=0;phase<period;phase+=137){
   clock_ticks=1000000;g_pg.pending_scanout=&surface;
   g_pg.pending_scanout_write_us=pgraph_scanout_clock_us();
   clock_ticks+=phase;assert(!service_pending_scanout(0));
   clock_ticks=1000000+period;assert(service_pending_scanout(1));
   assert(!g_pg.pending_scanout);assert(!service_pending_scanout(0));
  }
 }
 /* A next flip consumes the completed previous resolve even if writes were recent. */
 for(int frame=0;frame<1000;frame++){
  g_pg.pending_scanout=&surface;g_pg.pending_scanout_write_us=pgraph_scanout_clock_us();
  clock_ticks+=100;assert(service_pending_scanout(1));assert(!g_pg.pending_scanout);
 }
 g_pg.pending_scanout=&surface;allow_copy=0;int before=composites;
 assert(!service_pending_scanout(1));assert(g_pg.pending_scanout==&surface);assert(composites==before);
 allow_copy=1;assert(service_pending_scanout(1));assert(!g_pg.pending_scanout);
 clock_ticks=5184000000123LL;assert(pgraph_scanout_clock_us()==5184000000123ULL);
 assert(copies==composites+1);
 puts("PASS: timer phases, 1000 fast frame boundaries, exactly-once consume, copy failure and long uptime");
}
'''
class ScanoutTests(unittest.TestCase):
 def test_real_pending_queue(self):
  s=(ROOT/'src/nv2a/nv2a_pgraph_d3d11.c').read_text(encoding='utf-8')
  a=s.index('static ULONGLONG pgraph_scanout_clock_us(void)');b=s.index('\n}\n',a)+3
  a2=s.index('static int service_pending_scanout(int force)\n{');b2=s.index('int pgraph_d3d11_service_scanout(void)',a2)
  flip=s[s.index('case NV097_FLIP_STALL:'):a2]
  self.assertLess(flip.index('if (service_pending_scanout(1))'),flip.index('if (g_pg.deferred_flip)'))
  cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
  with tempfile.TemporaryDirectory(prefix='scanout-fence-') as temp:
   p=Path(temp)/'test.c';exe=p.with_suffix('.exe');p.write_text(PRE+s[a:b]+s[a2:b2]+POST,encoding='utf-8')
   r=subprocess.run([cc,'-std=c11','-O2','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],capture_output=True,text=True)
   self.assertEqual(r.returncode,0,r.stdout+r.stderr)
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
   print(r.stdout.strip())
if __name__=='__main__':unittest.main()

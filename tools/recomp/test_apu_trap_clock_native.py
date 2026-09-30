"""Execute the production frame loop: trap waits must not create audio samples."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class TrapClockTests(unittest.TestCase):
    def test_frame_clock_output_and_host_sources(self):
        source=(ROOT/'src/apu/apu_core.c').read_text(encoding='utf-8')
        function=re.search(r'static void \*mcpx_apu_frame_thread\(.*?\n\}',source,re.S)[0]
        prelude=r'''
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed line%d\n",__LINE__);exit(2);}}while(0)
#define MCPX_APU_DEVICE(x) ((MCPXAPUState*)(x))
#define qatomic_read(p) (*(p))
#define NV_PAPU_SECTL 0
#define NV_PAPU_FECTL 1
#define NV_PAPU_SECTL_XCNTMODE 3
#define NV_PAPU_SECTL_XCNTMODE_OFF 0
#define NV_PAPU_FECTL_FEMETHMODE_TRAPPED 4
#define NV_PAPU_FECTL_FEMETHMODE_HALTED 8
#define GET_MASK(v,m) ((v)&(m))
#define NUM_SAMPLES_PER_FRAME 32
#define QEMU_CLOCK_REALTIME 0
typedef struct MCPXAPUState {
 bool exiting,pause_requested,is_idle,set_irq;
 int lock,cond,idle_cond,ep_frame_div; unsigned regs[2];
 int pending[256];
} MCPXAPUState;
static struct {bool active;} g_test_tone;
static int g_mixer_active_count,g_trace_apu_perf;
static int hw_frames,host_frames,waits,irq_calls,throttles,resume;
static MCPXAPUState *current;
static void qemu_mutex_lock(int*p){++*p;}
static void qemu_mutex_unlock(int*p){--*p;}
static void qemu_cond_signal(int*p){}
static void qemu_cond_wait(int*c,int*l){current->exiting=true;}
static void qemu_cond_timedwait(int*c,int*l,int ms){
 CHECK(*l==1&&ms==5);++waits;
 if(resume){CHECK(current->ep_frame_div==17);CHECK(current->pending[31]==42);
  if(waits==7)current->regs[1]=0;}
}
static void update_irq(MCPXAPUState*d){++irq_calls;}
static void throttle(MCPXAPUState*d){
 ++throttles;if(!resume||throttles>30)d->exiting=true;
}
static void se_frame(MCPXAPUState*d){++hw_frames;++d->ep_frame_div;d->exiting=true;}
static void mcpx_apu_monitor_frame(MCPXAPUState*d){++host_frames;memset(d->pending,0,sizeof(d->pending));}
static int64_t qemu_clock_get_us(int clock){return 0;}
'''
        harness=r'''
int main(void){
 MCPXAPUState d;unsigned cases=0;
 for(int mode=0;mode<4;++mode)for(int phase=0;phase<8;++phase)
 for(int sources=0;sources<4;++sources)for(int pause=0;pause<2;++pause){
  memset(&d,0,sizeof(d));current=&d;d.ep_frame_div=phase;d.pending[31]=42;
  d.regs[0]=mode?1:0;d.regs[1]=mode==2?4:mode==3?8:0;
  d.set_irq=(phase&1)!=0;d.pause_requested=pause;
  g_test_tone.active=sources&1;g_mixer_active_count=(sources>>1)&1;
  hw_frames=host_frames=waits=irq_calls=throttles=0;
  mcpx_apu_frame_thread(&d);CHECK(d.lock==0&&!d.is_idle);
  if(pause&&!sources){CHECK(!hw_frames&&!host_frames&&!waits&&!irq_calls);CHECK(d.set_irq==(phase&1));}
  else {
   CHECK(irq_calls==(phase&1)&&!d.set_irq);
   if(mode==1&&!g_test_tone.active){CHECK(hw_frames==1&&!host_frames&&!waits);}
   else if(sources){CHECK(host_frames==1&&!hw_frames&&!waits);}
   else {CHECK(waits==1&&!host_frames&&!hw_frames);CHECK(d.pending[31]==42);}
  }
  CHECK(d.ep_frame_div==phase+hw_frames+host_frames);++cases;
 }
 memset(&d,0,sizeof(d));current=&d;d.ep_frame_div=17;d.pending[31]=42;
 d.regs[0]=1;d.regs[1]=4;d.set_irq=true;resume=1;
 g_test_tone.active=false;g_mixer_active_count=0;
 hw_frames=host_frames=waits=irq_calls=throttles=0;
 mcpx_apu_frame_thread(&d);
 CHECK(waits==7&&hw_frames==1&&!host_frames&&irq_calls==1&&!d.set_irq);
 CHECK(d.ep_frame_div==18&&d.pending[31]==42&&d.lock==0);
 printf("%u clock/source/pause cases and seven-wait trap resume passed\n",cases);
 return 0;
}
'''
        old_clock=re.sub(r'        \} else if \(!g_test_tone.active && !g_mixer_active_count\) \{.*?qemu_cond_timedwait\(&d->cond, &d->lock, 5\);\n', '', function,flags=re.S)
        manufactured_irq=function.replace(
            '        if (d->set_irq) {',
            '        if (d->set_irq || qatomic_read(&d->regs[NV_PAPU_SECTL]) == 0) {',
            1,
        )
        dropped_event=re.sub(
            r'        if \(d->set_irq\) \{.*?        \}\n', '', function,
            count=1, flags=re.S,
        )
        self.assertNotEqual(old_clock,function)
        self.assertNotEqual(manufactured_irq,function)
        self.assertNotEqual(dropped_event,function)
        compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        for label,body in [('fixed',function),('old-clock',old_clock),
                           ('manufactured-inactive-irq',manufactured_irq),
                           ('dropped-device-event',dropped_event)]:
            with self.subTest(label=label),tempfile.TemporaryDirectory(prefix='merc-apu-trap-') as directory:
                src,exe=Path(directory)/'check.c',Path(directory)/'check.exe'
                src.write_text(prelude+body+harness,encoding='utf-8')
                build=subprocess.run([compiler,'-std=c11','-O1',str(src),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
                if label=='fixed':
                    self.assertEqual(result.returncode,0,result.stderr);print(result.stdout.strip())
                else:self.assertNotEqual(result.returncode,0)


if __name__=='__main__':
    unittest.main()

"""Run real worker/dispatch code around captured cyclic monitor lookahead."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class MultipassWorkerTests(unittest.TestCase):
    def test_worker_returns_and_releases_frame_lock(self):
        source=(ROOT/'src/apu/apu_vp.c').read_text(encoding='utf-8')
        def extract(name):
            return re.search(r'static [^\n]*\b'+name+r'\(.*?\n\}',source,re.S)[0]
        lookahead=extract('peek_ahead_multipass_bin')
        worker=extract('voice_worker_thread')
        schedule=extract('voice_work_schedule')
        dispatch=extract('voice_work_dispatch')
        types=(ROOT/'src/apu/apu_state.h').read_text(encoding='utf-8')
        types=types[types.index('typedef struct VoiceWorkItem {'):types.index('typedef struct MCPXAPUVPState {')]
        prelude=r'''
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define CHECK(c) do{if(!(c))exit(2);}while(0)
#define NUM_MIXBINS 32
#define NUM_SAMPLES_PER_FRAME 32
#define MCPX_HW_MAX_VOICES 256
#define MCPX_APU_DEBUG_MON_VP 1
#define MULTIPASS_BIN_MASK 0x3C0
#define NV_PAVS_VOICE_CFG_FMT 4
#define NV_PAVS_VOICE_CFG_FMT_MULTIPASS (1u<<21)
#define NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN (31u<<16)
#define NV_PAVS_VOICE_TAR_PITCH_LINK 124
#define NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE 65535
typedef HANDLE QemuThread;
typedef CRITICAL_SECTION QemuMutex;
typedef CONDITION_VARIABLE QemuCond;
#define qemu_mutex_lock EnterCriticalSection
#define qemu_mutex_unlock LeaveCriticalSection
#define qemu_cond_signal WakeConditionVariable
#define qemu_cond_broadcast WakeAllConditionVariable
#define qatomic_read(p) (*(p))
static void qemu_cond_wait(QemuCond*c,QemuMutex*m){CHECK(SleepConditionVariableCS(c,m,INFINITE));}
static void qemu_cond_timedwait(QemuCond*c,QemuMutex*m,int ms){SleepConditionVariableCS(c,m,ms);}
static void rcu_register_thread(void){}
static void rcu_unregister_thread(void){}
static int ctz64(uint64_t x){int n=0;CHECK(x);while(!(x&1)){++n;x>>=1;}return n;}
'''
        state=r'''
typedef struct MCPXAPUState {
 QemuMutex lock;QemuCond cond;bool pause_requested;int ep_frame_div;
 struct {int point;} monitor;
 struct {VoiceWorkDispatch voice_work_dispatch;float sample_buf[32][2];} vp;
 unsigned fmt[256],link[256],reads;int result;
} MCPXAPUState;
static unsigned voice_get_mask(MCPXAPUState*d,uint16_t v,unsigned reg,unsigned mask){
 CHECK(v<256);++d->reads;unsigned x=reg==4?d->fmt[v]:d->link[v];
 while(!(mask&1)){mask>>=1;x>>=1;}return x&mask;
}
static bool any_queued_voice_locked(MCPXAPUState*d){return false;}
static void get_voice_bin_src_dst(MCPXAPUState*d,int v,uint32_t*src,uint32_t*dst,uint32_t*clr){*src=0;*dst=1;*clr=0;}
'''
        # Isolate the monitor lookup inside the otherwise real worker pipeline;
        # do not pretend this fixture decodes complete audio samples.
        process=r'''
static void voice_process(MCPXAPUState*d,float mix[32][32],float samples[32][2],uint16_t v,int list){
 d->result=peek_ahead_multipass_bin(d,v);mix[0][0]=.25f;samples[0][0]=.5f;
}
'''
        harness=r'''
static DWORD WINAPI worker_entry(void*p){voice_worker_thread(p);return 0;}
int main(void){
 MCPXAPUState d;memset(&d,0,sizeof(d));VoiceWorkDispatch*w=&d.vp.voice_work_dispatch;
 InitializeCriticalSection(&d.lock);InitializeConditionVariable(&d.cond);
 InitializeCriticalSection(&w->lock);InitializeConditionVariable(&w->work_pending);InitializeConditionVariable(&w->work_finished);
 w->num_workers=1;w->workers=calloc(1,sizeof(*w->workers));d.monitor.point=1;
 EnterCriticalSection(&w->lock);w->workers_pending=1;
 HANDLE thread=CreateThread(NULL,0,worker_entry,&d,0,NULL);CHECK(thread);
 while(w->workers_pending)qemu_cond_wait(&w->work_finished,&w->lock);
 LeaveCriticalSection(&w->lock);
 for(unsigned pass=0;pass<3;++pass){
  memset(d.fmt,0,sizeof(d.fmt));memset(d.link,0,sizeof(d.link));d.reads=0;
  if(pass==0){d.link[0]=1;d.fmt[1]=(1u<<21)|(7u<<16);}
  if(pass==1){d.link[83]=0xE9950053;d.fmt[83]=0xA000E0A4;}
  if(pass==2){d.link[0]=1;d.link[1]=0;}
  float mix[32][32]={{0}};EnterCriticalSection(&d.lock);
  w->queue_len=1;w->queue[0].voice=pass==1?83:0;w->queue[0].list=2;
  voice_work_dispatch(&d,mix);
  CHECK(!w->workers_pending&&!w->queue_len);CHECK(d.result==(pass==0?7:-1));
  CHECK(mix[0][0]==.25f&&d.reads<=4);LeaveCriticalSection(&d.lock);
 }
 EnterCriticalSection(&w->lock);w->workers_should_exit=true;qemu_cond_broadcast(&w->work_pending);LeaveCriticalSection(&w->lock);
 CHECK(WaitForSingleObject(thread,1000)==WAIT_OBJECT_0);CloseHandle(thread);free(w->workers);
 puts("Real worker/dispatch completed valid, self-loop and two-node-cycle jobs; frame lock released");return 0;
}
'''
        old=re.sub(r'        if \(v >= MCPX_HW_MAX_VOICES \|\| visited\[v\]\) \{.*?        visited\[v\] = true;\n','',lookahead,flags=re.S)
        self.assertNotEqual(old,lookahead)
        compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        for label,lookup in [('fixed',lookahead),('old',old)]:
            with self.subTest(label=label),tempfile.TemporaryDirectory(prefix='merc-mp-worker-') as directory:
                src,exe=Path(directory)/'check.c',Path(directory)/'check.exe'
                src.write_text(prelude+types+state+lookup+process+worker+schedule+dispatch+harness,encoding='utf-8')
                result=subprocess.run([compiler,'-std=c11','-O1',str(src),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                if label=='old':
                    with self.assertRaises(subprocess.TimeoutExpired):subprocess.run([str(exe)],capture_output=True,timeout=2)
                else:
                    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
                    self.assertEqual(result.returncode,0,result.stderr);print(result.stdout.strip())


if __name__=='__main__':unittest.main()

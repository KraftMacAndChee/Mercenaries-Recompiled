"""Exercise production guest waits against precise/coarse clocks and real Windows sleep."""
from pathlib import Path
import shutil, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[2]
MOCK=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t DWORD; typedef int BOOL; typedef int BOOLEAN; typedef int NTSTATUS;
typedef uint64_t ULONGLONG; typedef struct {long long QuadPart;} LARGE_INTEGER;
#define TRUE 1
#define WAIT_IO_COMPLETION 192
#define STATUS_ALERTED 257
#define STATUS_SUCCESS 0
static long long us; static int fail_frequency, apc, sleeps, services, cost, max_slice;
static int QueryPerformanceFrequency(LARGE_INTEGER *v){v->QuadPart=1000000;return !fail_frequency;}
static int QueryPerformanceCounter(LARGE_INTEGER *v){v->QuadPart=us;return 1;}
static ULONGLONG GetTickCount64(void){return (ULONGLONG)(us/16000)*16;}
static void service(void){services++;us+=cost;}
static void (*g_host_service_callback)(void)=service;
static void Sleep(DWORD ms){assert(ms>0 && ms<=16);us+=(long long)ms*1000;sleeps++;if((int)ms>max_slice)max_slice=ms;}
static DWORD SleepEx(DWORD ms, BOOL alertable){assert(alertable);Sleep(ms);return apc?WAIT_IO_COMPLETION:0;}
'''
HARNESS=r'''
static void reset(void){us=0;sleeps=services=cost=max_slice=fail_frequency=apc=0;g_host_service_callback=service;}
int main(void){
 for(int start=0;start<16000;start+=137){
  for(int duration=0;duration<=65;duration++){
   reset();us=start;
   assert(xbox_host_aware_sleep(duration,0)==STATUS_SUCCESS);
   assert(us-start==(long long)duration*1000);
   assert(services>=1 && max_slice<=16);
  }
 }
 reset();cost=20000;assert(xbox_host_aware_sleep(1,0)==STATUS_SUCCESS);assert(sleeps==0);
 reset();cost=400;assert(xbox_host_aware_sleep(17,0)==STATUS_SUCCESS);assert(us>=17000 && us<18800);
 reset();apc=1;assert(xbox_host_aware_sleep(100,1)==STATUS_ALERTED);assert(sleeps==1);
 reset();assert(xbox_host_aware_sleep(33,1)==STATUS_SUCCESS);assert(us==33000);
 reset();g_host_service_callback=0;assert(xbox_host_aware_sleep(1,0)==STATUS_SUCCESS);assert(us==1000);
 reset();fail_frequency=1;assert(xbox_host_aware_sleep(1,0)==STATUS_SUCCESS);assert(us==16000);
 puts("PASS: 7722 precise deadline/phase cases, callback cost, alertable APC, long waits and fallback");
}
'''
REAL=r'''
#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
/* Older MinGW SDK lacks the declaration/import; call the real Windows API. */
static ULONGLONG test_windows_tick64(void){
 typedef ULONGLONG (WINAPI *Fn)(void); static Fn fn;
 if(!fn) fn=(Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"),"GetTickCount64");
 return fn();
}
#define GetTickCount64 test_windows_tick64
typedef LONG NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_ALERTED 257
static void (*g_host_service_callback)(void);
'''
BENCH=r'''
static void old_wait(DWORD ms){ULONGLONG until=GetTickCount64()+ms;while(GetTickCount64()<until)Sleep(1);}
int main(void){
 LARGE_INTEGER f,a,b;QueryPerformanceFrequency(&f);timeBeginPeriod(1);
 QueryPerformanceCounter(&a);for(int i=0;i<128;i++)old_wait(1);QueryPerformanceCounter(&b);
 printf("old128x1ms=%.3fms\n",(b.QuadPart-a.QuadPart)*1000.0/f.QuadPart);
 QueryPerformanceCounter(&a);for(int i=0;i<128;i++)xbox_host_aware_sleep(1,0);QueryPerformanceCounter(&b);
 printf("precise128x1ms=%.3fms\n",(b.QuadPart-a.QuadPart)*1000.0/f.QuadPart);
 timeEndPeriod(1);return 0;
}
'''
def body():
 s=(ROOT/'src/kernel/kernel_thread.c').read_text(encoding='utf-8')
 a=s.index('static NTSTATUS xbox_host_aware_sleep(');return s[a:s.index('\n/* ===',a)]
class WaitTests(unittest.TestCase):
 def test_production_deadlines(self):
  cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
  with tempfile.TemporaryDirectory(prefix='guest-wait-') as t:
   p=Path(t)/'test.c';exe=p.with_suffix('.exe')
   p.write_text(MOCK+body()+HARNESS,encoding='utf-8')
   subprocess.run([cc,'-O2','-std=c11','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True,capture_output=True)
   result=subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=10)
   print(result.stdout.strip())
def benchmark():
 cc=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
 with tempfile.TemporaryDirectory(prefix='guest-wait-real-') as t:
  p=Path(t)/'test.c';exe=p.with_suffix('.exe');p.write_text(REAL+body()+BENCH,encoding='utf-8')
  subprocess.run([cc,'-O2',str(p),'-o',str(exe),'-lwinmm'],check=True)
  subprocess.run([str(exe)],check=True,timeout=15)
if __name__=='__main__':
 import sys
 if '--benchmark' in sys.argv:benchmark()
 else:unittest.main()

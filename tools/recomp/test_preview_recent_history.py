"""Verify automatic pre-incident exports without a marker or unbounded formatting."""
from pathlib import Path
import re,shutil,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class RecentHistory(unittest.TestCase):
 def test_before_any_marker(self):
  src=(ROOT/'ports/mercenaries/src/main.c').read_text(encoding='utf-8')
  fn=re.search(r'static void preview_recent_history\(ULONGLONG now\)\n\{.*?\n\}',src,re.S)[0]
  pre=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>
typedef uint64_t ULONGLONG;
static unsigned calls,snapshots;static uint64_t serial_now;static uint32_t interval=16667;
static char record[768];
static uint32_t d3d8_GetFrameIntervalHistory(uint64_t after,uint32_t*out,uint32_t cap,uint64_t*serial){
 *serial=serial_now;uint64_t n=serial_now-after;if(n>cap)n=cap;
 for(unsigned i=0;i<n;i++)out[i]=interval;return (uint32_t)n;
}
static void xbox_preview_log_event(const char*c,const char*f,...){
 assert(!strcmp(c,"frame-history"));calls++;va_list a;va_start(a,f);
 int n=vsnprintf(record,sizeof(record),f,a);va_end(a);assert(n>=0 && (unsigned)n<sizeof(record));
}
static void preview_snapshot(const char*s){assert(!strcmp(s,"scene"));snapshots++;}
'''
  post=r'''
int main(void){
 serial_now=60;preview_recent_history(10000);assert(calls==1 && snapshots==1);
 assert(strstr(record,"samples=60 omitted=0 chronological_us=16667,16667"));
 serial_now=119;preview_recent_history(10999);assert(calls==1);
 serial_now=120;preview_recent_history(11000);assert(calls==2 && snapshots==2);
 assert(strstr(record,"samples=60 omitted=0"));
 serial_now=500;preview_recent_history(16000);assert(strstr(record,"samples=96 omitted=284"));
 interval=UINT32_MAX;serial_now+=96;preview_recent_history(17000);
 assert(strlen(record)<768 && strstr(record,"4294967295"));
 assert(!strstr(record,"omitted=0"));
 puts("PASS: automatic history before F8, one-second cadence, explicit long-stall omissions, bounded long-interval formatting");
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'test.c';e=p.with_suffix('.exe');p.write_text(pre+fn+post)
   subprocess.run([shutil.which('gcc'),'-std=c11','-O2',str(p),'-o',str(e)],check=True)
   subprocess.run([str(e)],check=True)
if __name__=='__main__':unittest.main()

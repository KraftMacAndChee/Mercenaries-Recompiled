"""Bounded, non-presenting captures of the actual upcoming display scanout."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DisplayCaptureTests(unittest.TestCase):
    def test_schedule_bounds_and_default_no_work(self):
        text = (ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
        state = text[text.index('static int g_debug_display_capture_initialized;'):
                     text.index('void d3d8_DebugStartDisplayCapture')]
        body = text[text.index('static void d3d8_debug_capture_display_refresh(ULONGLONG now_ms)\n{'):
                    text.index('\nBOOL d3d8_ServiceDisplayRefresh(BOOL present_pending_scanout)')]
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint64_t ULONGLONG;
#define MAX_PATH 260
#define _TRUNCATE ((size_t)-1)
#define strncpy_s(destination,size,source,count) \
 snprintf((destination),(size),"%s",(source))
static const char *prefix,*interval,*limit;
static unsigned captures, reads, completed_frame, captured_frame;
static char last_path[1024];
static const char *env(const char *name) {
 ++reads;
 if(!strcmp(name,"MERCENARIES_CAPTURE_DISPLAY_PREFIX"))return prefix;
 if(!strcmp(name,"MERCENARIES_CAPTURE_DISPLAY_INTERVAL_MS"))return interval;
 if(!strcmp(name,"MERCENARIES_CAPTURE_DISPLAY_COUNT"))return limit;
 assert(0);return NULL;
}
#define getenv env
static void d3d8_DebugCaptureFrameToPath(const char *path) {
 captured_frame=completed_frame; ++captures; snprintf(last_path,sizeof(last_path),"%s",path);
}
static void d3d8_debug_detect_present_flash(ULONGLONG now_ms) {
 (void)now_ms;
}
'''
        tail = r'''
int main(int argc,char **argv) {
 int mode=argc>1?atoi(argv[1]):0;
 if(mode==6){
  completed_frame=1;
  d3d8_DebugQueueFrameCapture(NULL);d3d8_DebugQueueFrameCapture("");
  d3d8_DebugQueueFrameCapture("first.bmp");
  d3d8_DebugQueueFrameCapture("must-not-replace.bmp");
  assert(captures==0);
  completed_frame=9;d3d8_debug_capture_display_refresh(1000);
  assert(captures==1 && captured_frame==9 && !strcmp(last_path,"first.bmp"));
  d3d8_debug_capture_display_refresh(1001);assert(captures==1);
  d3d8_DebugQueueFrameCapture("second.bmp");completed_frame=10;
  d3d8_debug_capture_display_refresh(1002);
  assert(captures==2 && captured_frame==10 && !strcmp(last_path,"second.bmp"));
  for(uint64_t now=1003;now<100000;++now)d3d8_debug_capture_display_refresh(now);
  assert(captures==2 && reads==1);return 0;
 }
 if(mode==1)prefix="";
 if(mode>=2)prefix="display";
 if(mode==3){interval="1";limit="9999";}
 if(mode==4){interval="90000";limit="2";}
 if(mode==5)limit="0";
 d3d8_debug_capture_display_refresh(1000);
 assert(captures==0);
 for(uint64_t now=1001;now<=701001;++now)d3d8_debug_capture_display_refresh(now);
 if(mode<2){assert(captures==0 && reads==1);}
 if(mode==2){assert(captures==32 && !strcmp(last_path,"display-032-64000.bmp"));}
 if(mode==3){assert(captures==256 && !strcmp(last_path,"display-256-64000.bmp"));}
 if(mode==4){assert(captures==2 && !strcmp(last_path,"display-002-120000.bmp"));}
 if(mode==5)assert(captures==0);
 if(mode>=2)assert(reads==3);
 return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='mercs-display-capture-') as directory:
            c, exe = Path(directory)/'test.c',Path(directory)/'test.exe'
            c.write_text(prelude+state+body+tail,encoding='utf-8')
            result=subprocess.run([compiler,'-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            for mode in range(7):
                result=subprocess.run([str(exe),str(mode)],capture_output=True)
                self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
        refresh=text[text.index('BOOL d3d8_ServiceDisplayRefresh(BOOL present_pending_scanout)'):text.index('void d3d8_UploadFrameX8R8G8B8')]
        self.assertIn('g_pending_scanout_present = TRUE;', refresh)
        self.assertIn('if (g_pending_scanout_present)', refresh)
        self.assertIn('g_pending_scanout_present = FALSE;', refresh)
        self.assertLess(refresh.index('d3d8_debug_capture_display_refresh(now_ms)'),
                        refresh.index('preview_present'))
        present=text[text.index('void d3d8_PresentFrame(void)'):text.index('void d3d8_DebugCaptureFrameNow(void)')]
        self.assertIn('d3d8_debug_capture_display_refresh(start_ms);', present)
        self.assertNotIn('d3d8_PresentFrame',body)


if __name__=='__main__':
    unittest.main()

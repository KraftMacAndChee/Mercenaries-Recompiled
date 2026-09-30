"""An explicit frame filename must not inherit the flip-series suffix."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CapturePathTests(unittest.TestCase):
    def test_production_path_selection(self):
        source = (ROOT/'src/d3d/d3d8_device.c').read_text(encoding='utf-8')
        start = source.index('    write_path = capture_path;')
        body = source[start:source.index('    hr = IDXGISwapChain_GetBuffer', start)]
        prelude = r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define _TRUNCATE 0
#define _snprintf_s(p,n,t,...) snprintf(p,n,__VA_ARGS__)
static void check(unsigned capture_count, int g_debug_force_capture) {
 unsigned capture_attempts = 1897;
 const char *capture_path = "gamepad-000001.bmp", *write_path;
 char series_path[256];
'''
        tail = r'''
 const char *expected = (!g_debug_force_capture && capture_count > 1) ?
     "gamepad-000001.bmp-1897.bmp" : "gamepad-000001.bmp";
 assert(!strcmp(write_path, expected));
}
int main(void) {
 for(unsigned count=1; count<100; ++count) {
   check(count,0); check(count,1);
 }
 return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='mercs-capture-path-') as directory:
            c, exe = Path(directory)/'test.c',Path(directory)/'test.exe'
            for negative in (False, True):
                code = body.replace(' && !g_debug_force_capture', '') if negative else body
                c.write_text(prelude+code+tail,encoding='utf-8')
                built = subprocess.run([compiler,'-std=c11',str(c),'-o',str(exe)],capture_output=True)
                self.assertEqual(built.returncode,0,built.stderr.decode(errors='replace'))
                result = subprocess.run([str(exe)],capture_output=True)
                if negative:
                    self.assertNotEqual(result.returncode,0,'Old filename bug escaped the test')
                else:
                    self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))


if __name__ == '__main__':
    unittest.main()

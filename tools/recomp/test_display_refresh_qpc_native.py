from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef int BOOL;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
static LONGLONG qpc_now;
static unsigned presents, captures, pumps;
static struct { void *swap_chain; } g_device_state = {(void *)1};
static void d3d8_pump_host_messages(void) { ++pumps; }
static void QueryPerformanceFrequency(LARGE_INTEGER *value) { value->QuadPart = 60000; }
static void QueryPerformanceCounter(LARGE_INTEGER *value) { value->QuadPart = qpc_now; }
static ULONGLONG GetTickCount64(void) { return (ULONGLONG)(qpc_now / 60); }
static void d3d8_debug_capture_display_refresh(ULONGLONG now) { (void)now; ++captures; }
static BOOL g_pending_scanout_present;
static void d3d8_DebugSetSubmissionSource(uint32_t s){assert(s==5u);}
#define preview_present(chain, flags) (++presents)
'''
HARNESS = r'''
int main(void) {
    unsigned refreshes = 0;
    qpc_now = 100000;
    refreshes += d3d8_ServiceDisplayRefresh(TRUE);
    assert(presents == 1 && captures == 1);
    qpc_now = 100500;
    refreshes += d3d8_ServiceDisplayRefresh(TRUE);
    assert(presents == 1);
    for (unsigned frame = 1; frame < 60; ++frame) {
        qpc_now = 100000 + (LONGLONG)frame * 1000;
        refreshes += d3d8_ServiceDisplayRefresh(FALSE);
    }
    assert(refreshes == 60);
    assert(presents == 2 && captures == 2);
    assert(pumps == 61);
    qpc_now = 170000;
    refreshes += d3d8_ServiceDisplayRefresh(FALSE);
    assert(refreshes == 61);
    assert(d3d8_ServiceDisplayRefresh(FALSE) == FALSE);
    assert(refreshes == 61);
    return 0;
}
'''


class DisplayRefreshQpcTests(unittest.TestCase):
    def test_real_refresh_gate_runs_at_precise_sixty_hz(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if compiler is None and Path("C:/MinGW/bin/gcc.exe").exists():
            compiler = "C:/MinGW/bin/gcc.exe"
        if compiler is None:
            self.skipTest("native display-refresh test requires GCC or Clang")
        source = (ROOT / "src/d3d/d3d8_device.c").read_text(encoding="utf-8")
        start = source.index("BOOL d3d8_ServiceDisplayRefresh(")
        end = source.index("void d3d8_UploadFrameX8R8G8B8", start)
        body = source[start:end]
        self.assertNotIn("now_ms - last_refresh_ms < 16u", body)
        self.assertIn("refresh_frequency.QuadPart / 60", body)
        with tempfile.TemporaryDirectory(prefix="display-refresh-native-") as temp:
            test_source = Path(temp) / "display-refresh-test.c"
            executable = Path(temp) / "display-refresh-test.exe"
            test_source.write_text(PRELUDE + body + HARNESS, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra",
                            "-Werror", str(test_source), "-o", str(executable)],
                           check=True, text=True, timeout=60)
            result = subprocess.run([str(executable)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
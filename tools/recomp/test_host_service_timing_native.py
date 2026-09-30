"""Execute the real host-service callback with controlled clocks/display results."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long long LONGLONG;
typedef uint64_t ULONGLONG;
typedef unsigned int UINT;
typedef int BOOL;
#define FALSE 0
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
static unsigned char memory[0x800000];
static ptrdiff_t g_xbox_mem_offset;
static int enabled, invalid_frequency, display_calls, scanout_calls, pacing_calls, qpc_calls;
static const LONGLONG clocks[] = {1000,1005,1012,1500,1505,1512,2050,2060,2080};
static char *test_getenv(const char *name) {
    if (strcmp(name, "MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS") == 0)
        return NULL;
    if (strcmp(name, "MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS_DELAY_MS") == 0)
        return NULL;
    if (strcmp(name, "MERCENARIES_TEST_PRESENTATION_RESIZE_TRANSITIONS") == 0)
        return NULL;
    assert(strcmp(name, "MERCENARIES_TRACE_PRESENT_TIMING") == 0);
    return enabled ? "1" : NULL;
}
#define getenv test_getenv
static void QueryPerformanceFrequency(LARGE_INTEGER *value) {
    value->QuadPart = invalid_frequency ? 0 : 1000;
}
static void QueryPerformanceCounter(LARGE_INTEGER *value) {
    assert(qpc_calls < 9);
    value->QuadPart = clocks[qpc_calls++];
}
static ULONGLONG GetTickCount64(void) { return 0; }
static void d3d8_DebugStartDisplayCapture(const char *prefix,
                                          unsigned interval,
                                          unsigned limit) {
    (void)prefix; (void)interval; (void)limit;
    assert(0 && "test-only capture must remain opt-in");
}
static void preview_frame_sample(BOOL scanout, BOOL refreshed) {
    assert(scanout == (scanout_calls != 2));
    assert(refreshed == (display_calls != 2));
}
static BOOL nv2a_hook_service_scanout(void) {
    assert(scanout_calls == display_calls);
    ++scanout_calls;
    return scanout_calls != 2;
}
static void d3d8_WaitForGuestFrameSlot(void) {
    assert(scanout_calls == display_calls + 1);
    ++pacing_calls;
}
static BOOL d3d8_ServiceDisplayRefresh(BOOL present_pending_scanout) {
    assert(scanout_calls == display_calls + 1);
    assert(present_pending_scanout == (scanout_calls != 2));
    return ++display_calls != 2;
}static void pgraph_d3d11_set_internal_resolution(uint32_t width,
                                                  uint32_t height) {
    (void)width;
    (void)height;
    assert(0 && "test-only resolution transition must remain opt-in");
}
static int GetSystemMetrics(int metric) {
    (void)metric;
    return 1920;
}
static void d3d8_ResizePresentation(UINT width, UINT height, BOOL fullscreen) {
    (void)width; (void)height; (void)fullscreen;
    assert(0 && "test-only presentation resize must remain opt-in");
}
"""
HARNESS = r"""
int main(int argc, char **argv) {
    assert(argc == 2);
    enabled = strcmp(argv[1], "off") != 0;
    invalid_frequency = strcmp(argv[1], "bad-frequency") == 0;
    g_xbox_mem_offset = (ptrdiff_t)(void *)memory;
    *(uint32_t *)(void *)(memory + 0x793564) = 7;
    *(uint32_t *)(void *)(memory + 0x299378) = 0x10000;
    *(uint32_t *)(void *)(memory + 0x10000 + 0x1DE8) = 10;
    mercenaries_host_service();
    assert(*(uint32_t *)(void *)(memory + 0x793564) == 8);
    mercenaries_host_service();
    assert(*(uint32_t *)(void *)(memory + 0x793564) == 8);
    mercenaries_host_service();
    assert(*(uint32_t *)(void *)(memory + 0x793564) == 9);
    assert(*(uint32_t *)(void *)(memory + 0x10000 + 0x1DE8) == 10);
    assert(display_calls == 3 && scanout_calls == 3 && pacing_calls == 2);
    assert(qpc_calls == (enabled && !invalid_frequency ? 9 : 0));
    return 0;
}
"""


class HostServiceTimingTests(unittest.TestCase):
    def test_real_callback_preserves_refresh_and_vblank(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if compiler is None and Path("C:/MinGW/bin/gcc.exe").exists():
            compiler = "C:/MinGW/bin/gcc.exe"
        if compiler is None:
            self.skipTest("native host-service test requires GCC or Clang")
        source = (ROOT / "ports/mercenaries/src/main.c").read_text(encoding="utf-8")
        start = source.index("static void mercenaries_host_service(void)")
        end = source.index("/* ", source.index("\n}\n", start))
        body = source[start:end]
        with tempfile.TemporaryDirectory(prefix="host-service-native-") as temp:
            test_source = Path(temp) / "host-service-test.c"
            executable = Path(temp) / "host-service-test.exe"
            test_source.write_text(PRELUDE + body + HARNESS, encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            str(test_source), "-o", str(executable)],
                           check=True, text=True, timeout=60)
            for mode in ("on", "off", "bad-frequency"):
                with self.subTest(mode=mode):
                    result = subprocess.run([str(executable), mode], capture_output=True,
                                            text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if mode == "on":
                        self.assertIn("polls=3 refreshes=2 frames=2 interval_ms=1080.000", result.stderr)
                        self.assertIn("max_poll_gap_ms=550.000 max_scanout_ms=10.000 max_refresh_ms=20.000", result.stderr)
                    else:
                        self.assertEqual(result.stderr, "")


if __name__ == "__main__":
    unittest.main(verbosity=2)

"""Native regression coverage for Xbox heap allocation-class segregation."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class HeapLargeBlockPlacementTests(unittest.TestCase):
    def test_page_ranges_are_not_fragmented_by_normal_pool_allocations(self):
        source = (ROOT / "src/kernel/xbox_memory_layout.c").read_text(
            encoding="utf-8"
        )
        structures = source[
            source.index("#define XBOX_HEAP_MAX_ALLOCS"):
            source.index("static void xbox_HeapCaptureOwner(")
        ]
        body = source[
            source.index("static xbox_heap_allocation *xbox_HeapAppendAllocation("):
            source.index("static const xbox_heap_allocation *xbox_HeapFindAllocation(")
        ]
        free_body = source[
            source.index("void xbox_HeapFree("):
            source.index("HANDLE xbox_GetMappingHandle(")
        ]
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define XBOX_HEAP_BASE 0x008C0000u
#define XBOX_HEAP_SIZE (0x04000000u-XBOX_HEAP_BASE)
#define XBOX_CPU_ALIAS_BASE 0x80000000u
#define XBOX_CPU_ALIAS_END  0xC0000000u
static uint32_t xbox_GetGraphicsMemorySize(void) { return 0x04000000u; }
static uintptr_t g_memory_offset;
static uint32_t g_recomp_current_func;
static uint32_t g_recomp_recent_game_func_idx;
static uint32_t g_recomp_recent_game_funcs[256];
'''
        stubs = r'''
static void xbox_HeapCaptureOwner(xbox_heap_allocation *allocation)
{ memset(allocation->owner_ring, 0, sizeof(allocation->owner_ring)); }
static int xbox_HeapTraceEnabled(void) { return 0; }
static void xbox_HeapDumpCensus(void) {}
'''
        harness = r'''
int main(void)
{
    uint32_t small0, small1, small2, depth, frame0, frame1;
    uint32_t expected_frame;
    uint32_t normal0, normal1, physical_pool, fifo;
    uint32_t temp_color, temp_depth, half_surface;
    uint32_t buffers[3], tiny[105];
    unsigned i;
    g_memory_offset = (uintptr_t)calloc(1, 0x04000000u);
    if (!g_memory_offset) return 1;

    small0 = xbox_HeapAlloc(4096u, 4096u);
    frame0 = xbox_HeapAllocPageRounded(2457600u, 16384u);
    small1 = xbox_HeapAlloc(4096u, 4096u);
    expected_frame =
        (XBOX_HEAP_BASE + XBOX_HEAP_SIZE - 2457600u) & ~16383u;
    if (small0 != XBOX_HEAP_BASE) return 10;
    if (frame0 != expected_frame) return 11;
    if (small1 != small0 + 4096u) return 12;

    xbox_HeapFree(frame0);
    small2 = xbox_HeapAlloc(4096u, 4096u);
    if (small2 != small1 + 4096u) return 13;
    depth = xbox_HeapAllocPageRounded(1228800u, 16384u);
    if (!depth || depth <= frame0) return 14;
    xbox_HeapFree(depth);
    frame1 = xbox_HeapAllocPageRounded(2457600u, 16384u);
    if (frame1 != frame0) return 15;

    xbox_HeapFree(small1);
    if (xbox_HeapAlloc(4096u, 4096u) != small1) return 16;

    /* A complete physical teardown must recover the descending frontier and
     * recreate the same full-size target without consuming pool space. */
    xbox_HeapFree(frame1);
    frame1 = xbox_HeapAllocPageRounded(2457600u, 16384u);
    if (frame1 != frame0) return 17;

    /* Reproduce the retail lifetime which previously exhausted a fragmented
     * single bump arena: two long-lived title heaps, one long-lived physical
     * arena, many aligned physical pages, repeated triple-buffer teardown,
     * then the movie's half-size surface before the next triple buffer. */
    xbox_HeapReset();
    normal0 = xbox_HeapAlloc(1048576u, 4096u);
    normal1 = xbox_HeapAlloc(19503520u, 4096u);
    physical_pool = xbox_HeapAllocPageRounded(23592960u, 4096u);
    fifo = xbox_HeapAllocPageRounded(786432u, 4096u);
    if (!normal0 || !normal1 || !physical_pool || !fifo) return 18;
    for (i = 0; i < 105u; ++i) {
        tiny[i] = xbox_HeapAllocPageRounded(20480u, 32768u);
        if (!tiny[i]) return 19;
    }
    temp_color = xbox_HeapAllocPageRounded(2457600u, 16384u);
    temp_depth = xbox_HeapAllocPageRounded(1228800u, 16384u);
    if (!temp_color || !temp_depth) return 20;
    xbox_HeapFree(temp_depth);
    xbox_HeapFree(temp_color);
    for (i = 0; i < 3u; ++i) {
        buffers[i] = xbox_HeapAllocPageRounded(2457600u, 16384u);
        if (!buffers[i]) return 21;
    }
    for (i = 0; i < 3u; ++i) xbox_HeapFree(buffers[i]);
    for (i = 0; i < 3u; ++i) {
        buffers[i] = xbox_HeapAllocPageRounded(2457600u, 16384u);
        if (!buffers[i]) return 22;
    }
    for (i = 0; i < 3u; ++i) xbox_HeapFree(buffers[i]);
    temp_color = xbox_HeapAllocPageRounded(2457600u, 16384u);
    temp_depth = xbox_HeapAllocPageRounded(1228800u, 16384u);
    half_surface = xbox_HeapAllocPageRounded(614400u, 4096u);
    if (!temp_color || !temp_depth || !half_surface) return 23;
    xbox_HeapFree(temp_depth);
    xbox_HeapFree(temp_color);
    for (i = 0; i < 3u; ++i) {
        buffers[i] = xbox_HeapAllocPageRounded(2457600u, 16384u);
        if (!buffers[i]) return 24;
    }
    free((void *)g_memory_offset);
    return 0;
}
'''
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        with tempfile.TemporaryDirectory(prefix="mercs-heap-large-") as temp:
            source_file = Path(temp) / "test.c"
            executable = Path(temp) / "test.exe"
            source_file.write_text(
                prelude + structures + stubs + body + free_body + harness,
                encoding="utf-8",
            )
            build = subprocess.run(
                [compiler, "-std=c11", "-O2", str(source_file), "-o", str(executable)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(executable)], timeout=5)
            self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()

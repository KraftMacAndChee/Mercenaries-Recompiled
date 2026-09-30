"""Regression coverage for Xbox contiguous allocations owning full pages."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class KernelContiguousPageReservationTests(unittest.TestCase):
    def test_contiguous_bridges_reserve_every_touched_guest_page(self):
        bridge = (ROOT / "src/kernel/kernel_bridge.c").read_text(encoding="utf-8")
        memory = (ROOT / "src/kernel/xbox_memory_layout.c").read_text(encoding="utf-8")

        self.assertRegex(
            bridge,
            r"(?s)static void bridge_MmAllocateContiguousMemory\(void\).*?"
            r"xbox_HeapAllocPageRounded\(size, 4096\)",
        )
        self.assertRegex(
            bridge,
            r"(?s)static void bridge_MmAllocateContiguousMemoryEx\(void\).*?"
            r"if \(align < 4096\) align = 4096;.*?"
            r"xbox_HeapAllocPageRounded\(size, align\)",
        )

        helper = re.search(
            r"uint32_t xbox_HeapAllocPageRounded\(uint32_t size, uint32_t alignment\)\n"
            r"\{.*?\n\}",
            memory,
            re.S,
        )
        self.assertIsNotNone(helper)

        harness = r"""
#include <stdint.h>
#include <limits.h>

static uint32_t seen_size;
static uint32_t seen_alignment;
static unsigned call_count;
static uint8_t seen_class;

#define XBOX_HEAP_CLASS_PAGE_BACKED 1u

static uint32_t xbox_HeapAllocClass(
    uint32_t size, uint32_t alignment, uint8_t allocation_class)
{
    seen_size = size;
    seen_alignment = alignment;
    seen_class = allocation_class;
    ++call_count;
    return 0x038B4000u;
}

HELPER

static int expect(uint32_t request, uint32_t expected_size,
                  uint32_t expected_alignment)
{
    const unsigned before = call_count;
    const uint32_t result = xbox_HeapAllocPageRounded(request, expected_alignment);
    if (result != 0x038B4000u || call_count != before + 1u) return 10;
    if (seen_size != expected_size || seen_alignment != expected_alignment ||
        seen_class != XBOX_HEAP_CLASS_PAGE_BACKED) return 11;
    return 0;
}

int main(void)
{
    int status;
    status = expect(152u, 4096u, 16384u);
    if (status) return status;
    if (((0x038B4000u + seen_size + 15u) & ~15u) < 0x038B5000u) return 12;
    status = expect(4096u, 4096u, 4096u);
    if (status) return status;
    status = expect(4097u, 8192u, 4096u);
    if (status) return status;

    {
        const unsigned before = call_count;
        if (xbox_HeapAllocPageRounded(0u, 4096u) != 0u) return 13;
        if (xbox_HeapAllocPageRounded(UINT32_MAX, 4096u) != 0u) return 14;
        if (call_count != before) return 15;
    }
    return 0;
}
""".replace("HELPER", helper.group(0))

        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        with tempfile.TemporaryDirectory(prefix="mercs-contiguous-pages-") as temp:
            source_file = Path(temp) / "test.c"
            executable = Path(temp) / "test.exe"
            source_file.write_text(harness, encoding="utf-8")
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

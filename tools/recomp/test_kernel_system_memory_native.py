"""Pin the retail system-memory imports to guest-VA allocation and x86 ABI."""

from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


class KernelSystemMemoryTests(unittest.TestCase):
    def test_retail_imports_guest_heap_bridges_and_stack_cleanup(self):
        xbe = ROOT / "game_files/mercenaries-retail/default.xbe"
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        thunk_base = 0x002DBCE0
        ordinal_by_slot = [
            struct.unpack_from("<I", raw, config.va_to_file_offset(thunk_base + i * 4))[0]
            & 0x7FFFFFFF
            for i in range(111)
        ]
        self.assertEqual(ordinal_by_slot[52], 167)
        self.assertIn(172, ordinal_by_slot)

        for call_va, thunk_va in (
            (0x0022A958, 0x002DBDB0),
            (0x0022A9AB, 0x002DBDAC),
        ):
            call_offset = config.va_to_file_offset(call_va)
            self.assertEqual(
                raw[call_offset:call_offset + 2],
                bytes.fromhex("FF15"),
                "expected x86 CALL dword ptr [absolute]",
            )
            self.assertEqual(struct.unpack_from("<I", raw, call_offset + 2)[0], thunk_va)
        self.assertEqual(
            raw[
                config.va_to_file_offset(0x0022A93C):
                config.va_to_file_offset(0x0022A947)
            ],
            bytes.fromhex("6A048D45ECBB0000010053"),
        )

        source = (ROOT / "src/kernel/kernel_bridge.c").read_text(encoding="utf-8")
        args_body = re.search(
            r"static int stdcall_args_for_ordinal\(ULONG ordinal\)\n\{.*?\n\}",
            source,
            re.S,
        )[0]
        dispatch_body = re.search(
            r"static bridge_func_t bridge_for_ordinal\(ULONG ordinal\)\n\{.*?\n\}",
            source,
            re.S,
        )[0]
        self.assertIn("case 167: return  8;", args_body)
        self.assertIn("case 172: return  8;", args_body)
        self.assertIn("case 167: return bridge_MmAllocateSystemMemory;", dispatch_body)
        self.assertIn("case 172: return bridge_MmFreeSystemMemory;", dispatch_body)
        self.assertRegex(
            source,
            r"(?s)static void bridge_MmAllocateSystemMemory\(void\).*?"
            r"xbox_HeapAlloc\(size, 4096\).*?g_eax = xbox_va;",
        )
        self.assertRegex(
            source,
            r"(?s)static void bridge_MmFreeSystemMemory\(void\).*?"
            r"xbox_HeapGetAllocationSize\(xbox_va\).*?"
            r"xbox_HeapFree\(xbox_va\);.*?g_eax = freed_pages;",
        )

        old = args_body.replace("case 167: return  8;", "").replace(
            "case 172: return  8;", ""
        )
        prelude = "#include <stdint.h>\ntypedef uint32_t ULONG;\n"
        harness = r'''
int main(void) {
    const unsigned ordinals[2] = {167, 172};
    for (unsigned index = 0; index < 2; ++index) {
        uint32_t esp = 0x008BFFF0u;
        for (unsigned call = 0; call < 100000; ++call) {
            esp -= 12u; /* two retail arguments plus CALL return address */
            esp += 4u + (uint32_t)stdcall_args_for_ordinal(ordinals[index]);
            if (esp != 0x008BFFF0u) return 2;
        }
    }
    return 0;
}
'''
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        with tempfile.TemporaryDirectory(prefix="mercs-system-memory-") as temp:
            source_file = Path(temp) / "test.c"
            executable = Path(temp) / "test.exe"
            for label, body in (("fixed", args_body), ("old", old)):
                source_file.write_text(prelude + body + harness, encoding="utf-8")
                build = subprocess.run(
                    [compiler, "-std=c11", "-O2", str(source_file), "-o", str(executable)],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(build.returncode, 0, build.stderr)
                result = subprocess.run([str(executable)], timeout=5)
                if label == "fixed":
                    self.assertEqual(result.returncode, 0)
                else:
                    self.assertNotEqual(result.returncode, 0, "old ABI escaped regression")


if __name__ == "__main__":
    unittest.main()

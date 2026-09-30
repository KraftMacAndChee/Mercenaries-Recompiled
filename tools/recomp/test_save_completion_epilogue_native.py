"""Verify the retail async-save completion epilogue and guest stack ABI."""

from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config
from generated_test_utils import generated_text_containing


class SaveCompletionEpilogueTests(unittest.TestCase):
    def test_retail_bytes_state_and_stack(self) -> None:
        xbe = ROOT / "game_files/mercenaries-retail/default.xbe"
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        offset = config.va_to_file_offset(0x0018D435)
        retail = bytes.fromhex(
            "bf020000005d8bc75fc706000000005e59c3"
        )
        self.assertEqual(raw[offset:offset + len(retail)], retail)

        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patch = next(
            item for item in patcher["PATCHES"]
            if item.name ==
            "retail save completion shared epilogue restores its full frame"
        )
        owner = generated_text_containing("void sub_0018D2D0(void)")
        inline = patcher["TRANSLATOR_INLINED_PATCHES"][patch.name]
        self.assertIn(inline, owner)
        fixed = patch.after.rstrip()

        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint8_t memory[0x30000];
static uint32_t eax, ecx, esi, edi, esp, g_seh_ebp;
#define RECOMP_TRACE_FUNC(a) ((void)(a))
#define MEM32(a) (*(uint32_t *)(void *)(memory + (uint32_t)(a)))
#define PUSH32(s,v) do { uint32_t value_=(v); (s)-=4; MEM32(s)=value_; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
'''
        harness = r'''
int main(void) {
    for (uint32_t i = 0; i < 4096; ++i) {
        esp = 0x20000;
        ecx = 0x11110000u + i;
        esi = 0x1000;
        edi = 0x22220000u + i;
        g_seh_ebp = 0x33330000u + i;
        MEM32(esi) = 7;
        PUSH32(esp, 0);              /* caller return */
        PUSH32(esp, ecx);
        PUSH32(esp, 0x44440000u+i);  /* saved ESI */
        PUSH32(esp, edi);
        PUSH32(esp, g_seh_ebp);
        sub_0018D435();
        if (esp != 0x20000 || eax != 2 || MEM32(0x1000) != 0 ||
            ecx != 0x11110000u+i || esi != 0x44440000u+i ||
            edi != 0x22220000u+i || g_seh_ebp != 0x33330000u+i)
            return 2;
    }
    puts("4096 async-save completion epilogues preserved state and ESP");
    return 0;
}
'''
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        with tempfile.TemporaryDirectory(prefix="merc-save-epilogue-") as directory:
            path = Path(directory)
            source = path / "test.c"
            exe = path / "test.exe"
            source.write_text(prelude + fixed + harness, encoding="utf-8")
            build = subprocess.run(
                [compiler, "-O2", "-std=c11", str(source), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())

    def test_menu_dialog_shared_targets_preserve_dispatch_frame(self) -> None:
        """MenuDialogBase uses interior targets for every non-pressed event."""
        xbe = ROOT / "game_files/mercenaries-retail/default.xbe"
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        expected = {
            0x000D92F2: bytes.fromhex(
                "8b466485c074098b4e4c50e85eabffff5d5f5e5bc20800"
            ),
            0x000D9302: bytes.fromhex("5d5f5e5bc20800"),
            0x000D950E: bytes.fromhex(
                "84c07419688c01528a6a00e84265120083c4085d5f8946585e5bc20800"
                "6889693bfb6a00e82965120083c4085d5f8946585e5bc20800"
            ),
            0x000D9582: bytes.fromhex(
                "83ff030f8578fdffff3bdf0f8770fdffffff249dc0960d00"
            ),
        }
        for address, retail in expected.items():
            offset = config.va_to_file_offset(address)
            self.assertEqual(raw[offset:offset + len(retail)], retail)

        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patches = {item.name: item.after.rstrip()
                   for item in patcher["PATCHES"]}
        stubs = (
            ROOT / "ports/mercenaries/src/recomp/gen/recomp_stubs_unresolved.c"
        ).read_text(encoding="utf-8")
        names = {
            "retail MenuDialog previous-menu shared target restores its frame":
                "sub_000D92F2",
            "retail MenuDialog pressed-event shared epilogue restores its frame":
                "sub_000D9302",
            "retail MenuDialog control-sound shared target restores its frame":
                "sub_000D950E",
            "retail MenuDialog non-pressed dispatcher preserves its frame":
                "sub_000D9582",
        }
        bodies = []
        for patch_name, function_name in names.items():
            match = re.search(
                rf"void {function_name}\(void\)\n\{{.*?\n\}}", stubs, re.S
            )
            self.assertIsNotNone(match)
            self.assertEqual(match.group(0), patches[patch_name])
            bodies.append(match.group(0))

        epilogue = re.search(
            r"void sub_000D9303\(void\)\n\{.*?\n\}", stubs, re.S
        )
        self.assertIsNotNone(epilogue)
        bodies.append(epilogue.group(0))
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint8_t memory[0x30000];
static uint32_t eax, ebx, ecx, edx, esi, edi, esp, g_seh_ebp;
#define RECOMP_TRACE_FUNC(a) ((void)(a))
#define MEM32(a) (*(uint32_t *)(void *)(memory + (uint32_t)(a)))
#define PUSH32(s,v) do { uint32_t value_=(v); (s)-=4; MEM32(s)=value_; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
#define LO8(v) ((uint8_t)(v))
#define TEST_Z(a,b) ((((uint32_t)(a) & (uint32_t)(b)) == 0))
#define CMP_NE(a,b) ((uint32_t)(a) != (uint32_t)(b))
#define CMP_A(a,b) ((uint32_t)(a) > (uint32_t)(b))
#define RECOMP_ITAIL(a) do { (void)(a); abort(); } while(0)
static void sub_000D3E60(void) { esp += 8; }
static void sub_001FFA60(void) { eax=0x55667788u; esp += 4; }
static void pressed_frame(void) {
    esp=0x20000; PUSH32(esp,0x22222222); PUSH32(esp,0x11111111);
    PUSH32(esp,0xAAAAAAAA); PUSH32(esp,0xBBBBBBBB);
    PUSH32(esp,0xCCCCCCCC); PUSH32(esp,0xDDDDDDDD);
    PUSH32(esp,0xEEEEEEEE);
}
static void ordinary_frame(void) {
    esp=0x20000; PUSH32(esp,0x22222222); PUSH32(esp,0x11111111);
    PUSH32(esp,0xAAAAAAAA); PUSH32(esp,0xBBBBBBBB);
    PUSH32(esp,0xCCCCCCCC); PUSH32(esp,0xDDDDDDDD);
}
'''
        harness = r'''
int main(void) {
    pressed_frame(); sub_000D9302(); if(esp!=0x20000) return 2;
    pressed_frame(); esi=0x1000; MEM32(esi+0x64)=0;
    sub_000D92F2(); if(esp!=0x20000) return 3;
    pressed_frame(); esi=0x1000; eax=0;
    sub_000D950E(); if(esp!=0x20000 || MEM32(0x1058)!=0x55667788u) return 4;
    ordinary_frame(); edi=2; sub_000D9582(); if(esp!=0x20000) return 5;
    puts("MenuDialog pressed and ordinary input targets preserve ESP");
    return 0;
}
'''
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        with tempfile.TemporaryDirectory(prefix="merc-menu-dialog-") as directory:
            path = Path(directory)
            source = path / "test.c"
            exe = path / "test.exe"
            source.write_text(prelude + "\n".join(bodies) + harness,
                              encoding="utf-8")
            build = subprocess.run(
                [compiler, "-O2", "-std=c11", str(source), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())

    def test_pending_error_and_invalid_shared_epilogues(self) -> None:
        xbe = ROOT / "game_files/mercenaries-retail/default.xbe"
        config.configure_from_xbe(str(xbe))
        raw = xbe.read_bytes()
        expected = {
            0x0018D354: bytes.fromhex("8bc75f5e59c3"),
            0x0018D3FE: bytes.fromhex(
                "83f8070f854cffffffbf030000005d8bc75fc706000000005e59c3"
            ),
        }
        for address, retail in expected.items():
            offset = config.va_to_file_offset(address)
            self.assertEqual(raw[offset:offset + len(retail)], retail)

        patcher = runpy.run_path(
            str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
        )
        patches = {item.name: item.after.rstrip()
                   for item in patcher["PATCHES"]}
        owner = generated_text_containing("void sub_0018D2D0(void)")
        inline = patcher["TRANSLATOR_INLINED_PATCHES"]
        names = (
            "retail load save invalid operation shared epilogue restores its frame",
            "retail load save pending and error shared epilogue restores its frame",
        )
        bodies = []
        for patch_name in names:
            self.assertIn(inline[patch_name], owner)
            bodies.append(patches[patch_name])

        prelude = r'''
#include <stdint.h>
#include <stdio.h>
static uint8_t memory[0x30000];
static uint32_t eax, ecx, esi, edi, esp, g_seh_ebp;
#define RECOMP_TRACE_FUNC(a) ((void)(a))
#define MEM32(a) (*(uint32_t *)(void *)(memory + (uint32_t)(a)))
#define PUSH32(s,v) do { uint32_t value_=(v); (s)-=4; MEM32(s)=value_; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
static void frame(uint32_t with_ebp) {
    esp=0x20000; ecx=0x11111111; esi=0x1000; edi=0x22222222;
    g_seh_ebp=0x33333333; MEM32(esi)=8;
    PUSH32(esp,0); PUSH32(esp,ecx); PUSH32(esp,0x44444444);
    PUSH32(esp,edi); if(with_ebp) PUSH32(esp,g_seh_ebp);
}
'''
        harness = r'''
int main(void) {
    frame(0); sub_0018D354();
    if(esp!=0x20000 || eax!=0x22222222 || ecx!=0x11111111 ||
       esi!=0x44444444 || edi!=0x22222222 || MEM32(0x1000)!=8) return 2;
    frame(1); eax=0; sub_0018D3FE();
    if(esp!=0x20000 || eax!=0x22222222 || ecx!=0x11111111 ||
       esi!=0x44444444 || edi!=0x22222222 ||
       g_seh_ebp!=0x33333333 || MEM32(0x1000)!=8) return 3;
    frame(1); eax=7; sub_0018D3FE();
    if(esp!=0x20000 || eax!=3 || ecx!=0x11111111 ||
       esi!=0x44444444 || edi!=0x22222222 ||
       g_seh_ebp!=0x33333333 || MEM32(0x1000)!=0) return 4;
    puts("load/save shared epilogues preserve ESP and operation state");
    return 0;
}
'''
        compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
        with tempfile.TemporaryDirectory(prefix="merc-save-shared-") as directory:
            path = Path(directory)
            source = path / "test.c"
            exe = path / "test.exe"
            source.write_text(prelude + "\n".join(bodies) + harness,
                              encoding="utf-8")
            build = subprocess.run(
                [compiler, "-O2", "-std=c11", str(source), "-o", str(exe)],
                capture_output=True, text=True,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            print(run.stdout.strip())


if __name__ == "__main__":
    unittest.main()

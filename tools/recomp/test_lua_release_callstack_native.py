"""The retail release Lua callstack printer must remain diagnostic-only."""

from pathlib import Path
import hashlib
import sys
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0005.c"
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
sys.path.insert(0, str(ROOT))
from tools.recomp import config


def test_release_callstack_is_side_effect_free() -> None:
    generated = GENERATED.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")
    body = re.search(
        r"void sub_00112930\(void\)\n\{.*?\n\}", generated, re.S
    )
    assert body is not None
    body = body.group(0)

    assert "retail release Lua callstack walker has no observable side effects" in patcher
    assert '"sub_00112930"' in patcher
    assert "sub_001DE6A0" not in body
    assert "sub_001DDC70" not in body
    assert "esp += 4;" in body

    # Independently anchor this diagnostic replacement to the retail body.
    # The original walks Lua frames; our compatibility policy omits that
    # diagnostic work. This is not a claim that arbitrary debug calls are pure.
    xbe = ROOT / "game_files/mercenaries-retail/default.xbe"
    raw = xbe.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == "aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7"
    config.configure_from_xbe(str(xbe))
    offset = config.va_to_file_offset(0x00112930)
    assert offset is not None
    retail = raw[offset:offset + 0x51]
    assert hashlib.sha256(retail).hexdigest() == "1a7d3a943efa785d5d70e39a591fea7ae2221c90d0622a4541967ddeca4da3af"
    for call_offset, target in ((0x13, 0x001DDC70), (0x2E, 0x001DE6A0), (0x3E, 0x001DDC70)):
        assert retail[call_offset] == 0xE8
        displacement = int.from_bytes(retail[call_offset + 1:call_offset + 5], "little", signed=True)
        assert 0x00112930 + call_offset + 5 + displacement == target
    assert retail[-6:] == bytes.fromhex("5f 5e 83 c4 60 c3")

    compiler = shutil.which("gcc") or "C:/MinGW/bin/gcc.exe"
    harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t esp, eax, ebx, ecx, edx, esi, edi;
#define RECOMP_TRACE_FUNC(address) ((void)(address))
int main(void) {
    esp = 0x20000u;
    eax = 1u; ebx = 2u; ecx = 3u; edx = 4u; esi = 5u; edi = 6u;
    sub_00112930();
    if (esp != 0x20004u || eax != 1u || ebx != 2u || ecx != 3u ||
        edx != 4u || esi != 5u || edi != 6u) return 2;
    puts("release Lua callstack contract passed");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="merc-lua-callstack-") as directory:
        path = Path(directory)
        source_path = path / "test.c"
        exe_path = path / "test.exe"
        source_path.write_text(
            harness.split("int main", 1)[0] + body + "\nint main" +
            harness.split("int main", 1)[1], encoding="utf-8"
        )
        build = subprocess.run(
            [compiler, "-std=c11", str(source_path), "-o", str(exe_path)],
            capture_output=True, text=True
        )
        assert build.returncode == 0, build.stderr
        run = subprocess.run([str(exe_path)], capture_output=True, text=True)
        assert run.returncode == 0, run.stderr
        print(run.stdout.strip())


if __name__ == "__main__":
    test_release_callstack_is_side_effect_free()

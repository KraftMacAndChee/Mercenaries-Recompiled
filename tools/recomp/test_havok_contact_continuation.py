#!/usr/bin/env python3
"""Verify the omitted retail Havok contact callback tail stays restored."""

from hashlib import sha256
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0011.c"
RECOMPILE = ROOT / "ports/mercenaries/scripts/Recompile.ps1"
XBE = ROOT / "game_files/mercenaries-retail/default.xbe"


def main() -> None:
    generated = GENERATED.read_text(encoding="utf-8")
    recompile = RECOMPILE.read_text(encoding="utf-8")
    start = generated.index("void sub_002318A2(void)")
    end = generated.index("\n/**\n * sub_00231B06", start)
    function = generated[start:end]

    assert '"--function-range", "0x002318A2:0x00231B06"' in recompile
    assert "Original: 0x002318A2 - 0x00231B06 (612 bytes, 185 insns)" in generated
    assert "recomp_xmm_load(xmm2v, eax + 0x30)" in function
    assert "fp_top() += MEMF(edi + 4)" in function
    assert "sub_00230D24(); return;" in function
    assert "loc_00231B01: ;" in function
    assert "sub_00230D10(); return;" in function
    assert "havok_contact_tail_231a01.inc" not in generated
    assert "MEM32(ecx + 8)" not in function, "callback must not mask allocator state"

    retail_tail = XBE.read_bytes()[0x221A01:0x221B06]
    assert len(retail_tail) == 0x105
    assert sha256(retail_tail).hexdigest().upper() == (
        "99A2B7B0291B378C10D5B3ABFFE8F85EF18941E15783604494D6B03F2C379943"
    )
    assert retail_tail.endswith(bytes.fromhex("0F8523F2FFFFE90AF2FFFF"))

    print("Havok retail contact continuation checks passed")


if __name__ == "__main__":
    main()
#!/usr/bin/env python3
"""Retail regression for automatic bounty collectible pickup."""

from pathlib import Path
import struct
import sys


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.recomp import config
from tools.recomp.disasm import Disassembler
from tools.recomp.translator import FunctionTranslator


ROOT = Path(__file__).resolve().parents[2]
XBE = ROOT / "game_files/mercenaries-retail/default.xbe"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0000.c"


def test_retail_bounty_can_auto_use_reaches_shared_true_epilogue() -> None:
    xbe = XBE.read_bytes()
    config.configure_from_xbe(str(XBE))

    start, end = 0x00030FA0, 0x00031030
    offset = config.va_to_file_offset(start)
    assert offset is not None
    raw = xbe[offset : offset + end - start]
    insns = Disassembler().disassemble_function(raw, start, end)
    assert any(i.address == 0x00031015 and i.mnemonic == "jmp" for i in insns)

    table_offset = config.va_to_file_offset(0x00031030)
    assert table_offset is not None
    assert struct.unpack_from("<4I", xbe, table_offset) == (
        0x0003101C,
        0x0003101C,
        0x0003101C,
        0x0003101C,
    )

    func_db = {
        start: {
            "name": "sub_00030FA0",
            "start": f"0x{start:08X}",
            "end": end,
            "size": end - start,
        }
    }
    translated = FunctionTranslator(xbe, func_db).translate_function(
        start, func_db[start]
    )
    assert "switch: 4 entries, 1 targets" in translated
    assert "goto loc_0003101C;" in translated
    assert "loc_0003101C:" in translated


def test_current_generated_bounty_can_auto_use_is_locally_lowered() -> None:
    generated = GENERATED.read_text(encoding="utf-8")
    begin = generated.index("void sub_00030FA0(void)")
    finish = generated.index("\n/**\n * sub_00031040", begin)
    function = generated[begin:finish]
    assert "goto loc_0003101C;" in function
    assert "loc_0003101C:" in function
    assert "RECOMP_ITAIL(MEM32(eax * 4 + 0x31030))" not in function


if __name__ == "__main__":
    test_retail_bounty_can_auto_use_reaches_shared_true_epilogue()
    test_current_generated_bounty_can_auto_use_is_locally_lowered()
    print("ok  bounty_auto_use_jump_table")

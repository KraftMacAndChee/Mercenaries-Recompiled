"""Print XBE vtable entries and short disassemblies of their targets.

This is intentionally a read-only analysis helper. It understands XBE
section mappings, so callers can use guest virtual addresses directly.
"""

from __future__ import annotations

import argparse
import struct

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

from tools.xbe_parser.xbe_parser import XBEParser


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xbe")
    parser.add_argument("vtable", type=lambda value: int(value, 0))
    parser.add_argument("--start", type=int, default=0)
    parser.add_argument("--count", type=int, default=256)
    parser.add_argument("--instructions", type=int, default=5)
    args = parser.parse_args()

    xbe_parser = XBEParser(args.xbe)
    image = xbe_parser.parse()
    base = image.header.base_address
    vtable_offset = xbe_parser._va_to_file(args.vtable, base)
    disassembler = Cs(CS_ARCH_X86, CS_MODE_32)

    for index in range(args.start, args.start + args.count):
        entry_offset = vtable_offset + index * 4
        if entry_offset + 4 > len(image.raw_data):
            break
        target = struct.unpack_from("<I", image.raw_data, entry_offset)[0]
        try:
            target_offset = xbe_parser._va_to_file(target, base)
        except Exception:
            target_offset = -1
        if target_offset < 0 or target_offset >= len(image.raw_data):
            print(f"[{index:03d}] 0x{target:08X} <not mapped>")
            continue

        code = image.raw_data[target_offset : target_offset + 64]
        instructions = []
        for instruction in disassembler.disasm(code, target):
            instructions.append(
                f"{instruction.mnemonic} {instruction.op_str}".rstrip()
            )
            if (
                len(instructions) >= args.instructions
                or instruction.mnemonic.startswith("ret")
            ):
                break
        print(
            f"[{index:03d}] 0x{target:08X}  " + " ; ".join(instructions)
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
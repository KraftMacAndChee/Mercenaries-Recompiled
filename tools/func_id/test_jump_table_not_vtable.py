"""Regression: indexed switch tables are not callable pointer tables."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.func_id.vtable_scanner import scan_vtables


def main():
    image = bytearray(0x300)
    code_va = 0x002A0000
    table_va = 0x00300000
    methods = (code_va + 0x40, code_va + 0x68, code_va + 0x90)

    # jmp dword ptr [eax*4 + table_va]
    image[0:7] = b"\xFF\x24\x85" + struct.pack("<I", table_va)
    for method in methods:
        image[method - code_va] = 0xC3
    struct.pack_into("<III", image, 0x200, *methods)

    # A second, unreferenced table with the same valid entries remains eligible.
    legitimate_va = table_va + 0x20
    struct.pack_into("<III", image, 0x220, *methods)

    sections = [
        {"name": ".text", "va": code_va, "size": 0x200,
         "raw": 0, "raw_size": 0x200, "executable": True},
        {"name": ".data", "va": table_va, "size": 0x100,
         "raw": 0x200, "raw_size": 0x100, "executable": False},
    ]
    functions = [
        {"start": f"0x{method:08X}", "size": 1}
        for method in methods
    ]

    _, tables = scan_vtables(bytes(image), functions, {}, sections=sections)
    addresses = {table["address"] for table in tables}
    assert table_va not in addresses
    assert legitimate_va in addresses

    print("OK: indexed switch table is excluded without rejecting pointer tables")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

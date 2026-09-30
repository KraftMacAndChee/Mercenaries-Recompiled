"""Regression test for long, strongly anchored function-pointer tables."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.func_id.vtable_scanner import scan_vtables


def main():
    image = bytearray(0x700)
    anchored_offsets = (
        0x10, 0x30, 0x60, 0xA0, 0xF0, 0x130, 0x180, 0x1C0,
        0x210, 0x250, 0x2A0, 0x2E0, 0x330, 0x380, 0x3E0,
    )
    anchored_methods = tuple(0x002A0000 + offset
                             for offset in anchored_offsets)
    for offset in anchored_offsets:
        image[offset] = 0xC3  # ret

    # Twelve straight-line MOV stores put the first RET beyond both the
    # ordinary 48-byte and 16-instruction bounded-code checks.
    long_initializer = 0x002A0480
    image[0x480:0x480 + 60] = b"\xA3\0\0\0\0" * 12
    image[0x480 + 60] = 0xC3

    entries = anchored_methods + (long_initializer,)
    struct.pack_into("<" + "I" * len(entries), image, 0x600, *entries)

    sections = [
        {"name": ".text", "va": 0x002A0000, "size": 0x580,
         "raw": 0, "raw_size": 0x580, "executable": True},
        {"name": ".data", "va": 0x00300000, "size": 0x100,
         "raw": 0x600, "raw_size": 0x100, "executable": False},
    ]
    functions = [
        {"start": f"0x{address:08X}", "size": 1}
        for address in anchored_methods
    ]

    results, tables = scan_vtables(
        bytes(image), functions, {}, sections=sections)
    table = next(t for t in tables if t["address"] == 0x00300000)
    assert table["entries"] == list(entries)
    assert table["kind"] == "function_pointer_table"
    assert long_initializer in results
    assert results[long_initializer]["method"] == "pointer_table_entry"
    assert results[long_initializer]["category"] == "game_engine"

    # A large registration table may be discovered before most of its entries
    # have names. Overwhelming bounded-control-flow evidence must retain the
    # table even when fewer than half the entries are independently known.
    sparse_image = bytearray(0xA00)
    sparse_entries = tuple(
        0x002A0000 + 0x10 + index * 16 + (index % 2) * 4
        for index in range(99)
    ) + (0x002A0680,)
    for address in sparse_entries[:-1]:
        sparse_image[address - 0x002A0000] = 0xC3  # ret
    sparse_image[0x680:0x6B0] = b"\x90" * 0x30  # one straight-line callback
    struct.pack_into("<" + "I" * len(sparse_entries), sparse_image, 0x800,
                     *sparse_entries)
    sparse_sections = [
        {"name": ".text", "va": 0x002A0000, "size": 0x700,
         "raw": 0, "raw_size": 0x700, "executable": True},
        {"name": ".data", "va": 0x00300000, "size": 0x200,
         "raw": 0x800, "raw_size": 0x200, "executable": False},
    ]
    sparse_functions = [
        {"start": f"0x{address:08X}", "size": 1}
        for address in sparse_entries[:46]
    ]
    sparse_results, sparse_tables = scan_vtables(
        bytes(sparse_image), sparse_functions, {}, sections=sparse_sections)
    sparse_table = next(
        table for table in sparse_tables if table["address"] == 0x00300000)
    assert sparse_table["kind"] == "function_pointer_table"
    assert sparse_entries[46] in sparse_results
    assert sparse_entries[-1] in sparse_results
    print("OK: long anchored function-pointer tables retain initializer entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

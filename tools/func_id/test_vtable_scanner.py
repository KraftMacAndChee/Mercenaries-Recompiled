"""Regression checks for vtable targets in executable XBE sections.

    py -3 tools/func_id/test_vtable_scanner.py
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))

from tools.func_id.vtable_scanner import _looks_like_code_entry, scan_vtables


def main():
    image = bytearray(0x180)
    methods = (0x002A0010, 0x002A0040, 0x002A0090, 0x002A00B0)

    # One known method, two valid short compiler thunks, and one vtable-only
    # full MSVC method. The fourth method has a legitimate large-stack
    # prologue whose first five-byte CALL begins at byte 44. A 48-byte input
    # window truncates that CALL and falsely rejects the method even though its
    # entry is independently anchored by a referenced vtable.
    image[0x10:0x11] = b"\xC3"                  # ret
    image[0x40:0x47] = b"\x8B\xC1\xE9\0\0\0\0"  # mov eax,ecx; jmp
    image[0x90:0x95] = b"\x33\xC0\xC2\x04\x00"    # xor eax,eax; ret 4
    image[0xB0:0xE1] = (
        b"\x81\xEC\x54\x01\x00\x00"      # sub esp, 0x154
        b"\x8B\x84\x24\x5C\x01\x00\x00"  # mov eax, [esp+0x15c]
        b"\x53\x55\x56"                  # preserve nonvolatile registers
        b"\x8B\xB4\x24\x64\x01\x00\x00"  # mov esi, [esp+0x164]
        b"\x57\x33\xDB\x8B\xE9\x56"
        b"\x89\x45\x2C"
        b"\x88\x5C\x24\x23"
        b"\x88\x5C\x24\x22"
        b"\x89\x5C\x24\x44"
        b"\xE8\x00\x00\x00\x00"      # first control-flow instruction
    )
    struct.pack_into("<IIII", image, 0x100, *methods)

    # This resembles the Mercenaries XMV false positive: a pointer-like table
    # targets packed 0x1201 data which decodes indefinitely as ADDs and never
    # reaches a thunk RET/JMP. It must not seed pseudo-functions.
    false_methods = (0x002A0020, 0x002A0060, 0x002A0080)
    for start in (0x20, 0x60, 0x80):
        for offset in range(start, start + 0x10, 2):
            image[offset:offset + 2] = b"\x01\x12"
    struct.pack_into("<III", image, 0x114, *false_methods)

    # A fully indirect callback table can have no previously known entry. Its
    # targets are still valid when every entry has bounded code structure.
    indirect_methods = (methods[1], methods[2], methods[3])
    struct.pack_into("<III", image, 0x128, *indirect_methods)

    # Retail Xbox section flags can mark .rdata executable. A non-code field
    # after a real vtable can then look like an additional code pointer. The
    # scanner must terminate the table before that field without discarding
    # the valid methods that precede it.
    trailing_data_pointer = 0x00300070
    struct.pack_into("<IIIII", image, 0x140, *methods, trailing_data_pointer)

    # Small classes can have only a destructor and one virtual method.  The
    # constructor's immediate reference to this table is the extra evidence
    # that distinguishes it from a random pair of executable-looking words.
    short_method = 0x002A00E8
    image[0xE8:0xE9] = b"\xC3"
    struct.pack_into("<II", image, 0x158, methods[0], short_method)
    image[0x170:0x180] = b"\x00" * 0x10

    sections = [
        {"name": "DSOUND", "va": 0x002A0000, "size": 0x100,
         "raw": 0, "raw_size": 0x100, "executable": True},
        {"name": ".data", "va": 0x00300000, "size": 0x80,
         "raw": 0x100, "raw_size": 0x80, "executable": True},
    ]
    functions = [
        {"start": "0x002A0010", "size": 1},
        {"start": "0x002A0020", "size": 2},
    ]

    results, vtables = scan_vtables(
        bytes(image), functions, {
            0x00300040: [methods[0]],
            0x00300058: [methods[0]],
        },
        sections=sections)

    real = next(vt for vt in vtables if vt["address"] == 0x00300000)
    assert real["entries"] == list(methods)
    assert all(method in results for method in methods)
    assert results[methods[1]]["method"] == "vtable_thunk"

    # The boundary fix must remain narrow. This entry has no control flow in
    # the first 49 bytes; a CALL immediately after that boundary represents
    # the kind of packed executable-section data that the retail XPP section
    # exposed when the scan window was widened indiscriminately.
    late_image = b"\x90" * 49 + b"\xE8\x00\x00\x00\x00\xC3"
    late_sections = [{"name": "XPP", "va": 0x002D0000,
                      "size": len(late_image), "raw": 0,
                      "raw_size": len(late_image), "executable": True}]
    assert not _looks_like_code_entry(
        late_image, 0x002D0000, late_sections)

    assert all(vt["address"] != 0x00300014 for vt in vtables)
    assert false_methods[1] not in results
    assert false_methods[2] not in results

    indirect = next(vt for vt in vtables if vt["address"] == 0x00300028)
    assert indirect["entries"] == list(indirect_methods)

    terminated = next(vt for vt in vtables if vt["address"] == 0x00300040)
    assert terminated["entries"] == list(methods)
    assert trailing_data_pointer not in results

    short = next(vt for vt in vtables if vt["address"] == 0x00300058)
    assert short["entries"] == [methods[0], short_method]
    assert short_method in results

    # The same two pointers without an independent table-address reference
    # must remain rejected.
    _, unreferenced_vtables = scan_vtables(
        bytes(image), functions, {0x00300040: [methods[0]]},
        sections=sections)
    assert all(vt["address"] != 0x00300058 for vt in unreferenced_vtables)

    print("OK: vtable-only code entries are retained and packed data is rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())

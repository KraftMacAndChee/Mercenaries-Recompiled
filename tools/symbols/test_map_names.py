"""Self-check for the MAP parser. Synthetic data only -- no game files.

    py -3 tools/symbols/test_map_names.py
"""
import json
import tempfile
from pathlib import Path

from map_names import check_entry, parse_map, port_names, va_to_off

# A title with .text at 0x11000 and an XDK section at 0x50000, mirroring the
# real layout: MAP section 0001 -> XBE section 0.
SECTIONS = [
    (".text", 0x11000, 0x2000, 0x1000, 0x2000),
    ("DSOUND", 0x50000, 0x1000, 0x3000, 0x1000),
]

MAP_TEXT = """\
 Default

 Preferred load address is 00400000

 Start         Length     Name                   Class
 0001:00000000 00001000H .text                   CODE
 0002:00000000 00001000H DSOUND                  CODE

  Address         Publics by Value              Rva+Base     Lib:Object
 0000:00000000       ___safe_se_handler_count   00000000     <absolute>
 0001:00000000       ?first@@YAXXZ              00400700 f i A.obj
 0001:00000100       ?second@@YAXXZ             00400800 f i A.obj
 0002:00000040       _DirectSoundCreate@8       00420000 f i DSOUND.lib

 entry point at        0001:00000100
"""


def test_parse():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "t.map"
        p.write_text(MAP_TEXT)
        syms, entry = parse_map(p, SECTIONS)

    # section:offset resolves against the XBE section, NOT Rva+Base.
    assert syms[0x11000] == "?first@@YAXXZ", syms
    assert syms[0x11100] == "?second@@YAXXZ", syms
    # MAP section 0002 -> XBE section index 1 (DSOUND @ 0x50000).
    assert syms[0x50040] == "_DirectSoundCreate@8", syms
    # The <absolute> segment 0000 has no XBE section and must be dropped.
    assert not any(v < 0x11000 for v in syms), syms
    assert entry == 0x11100, hex(entry)
    # Rva+Base would have given 0x400800 -- make sure we never emit that.
    assert 0x400800 not in syms
    print("ok  parse_map: section:offset -> XBE VA, entry point, absolutes dropped")


def test_va_to_off():
    assert va_to_off(SECTIONS, 0x11000) == 0x1000
    assert va_to_off(SECTIONS, 0x11010) == 0x1010
    assert va_to_off(SECTIONS, 0x50040) == 0x3040
    assert va_to_off(SECTIONS, 0x99999) is None  # outside every section
    print("ok  va_to_off")


def test_port():
    # Donor DSOUND holds a distinctive function at 0x50040.
    body = bytes(range(0x40)) + b"\x55\x8b\xec\x83\xec\x10\x90\x91\x92\x93\x94\x95" + b"\x00" * 0x40
    donor_raw = b"\x00" * 0x3000 + body
    donor_syms = {0x50040: "_DirectSoundCreate@8"}

    # Target has the same code at a different address, 0x60040.
    tgt_secs = [("DSOUND", 0x60000, 0x1000, 0x100, 0x1000)]
    tgt_raw = b"\x00" * 0x100 + bytes(0x40) + body[0x40:] + b"\x00" * 0x100

    names, stats = port_names(donor_raw, SECTIONS, donor_syms, tgt_raw, tgt_secs,
                              {0x60040}, ["DSOUND"], 12)
    assert names == {0x60040: "_DirectSoundCreate@8"}, (names, stats)
    print("ok  port_names: matches library code at a different address")

    # Same match, but the target's detector never called it a function start:
    # it must be dropped rather than reported.
    names, stats = port_names(donor_raw, SECTIONS, donor_syms, tgt_raw, tgt_secs,
                              set(), ["DSOUND"], 12)
    assert names == {}, names
    assert stats["off_start"] == 1, stats
    print("ok  port_names: drops matches that aren't function starts")

    # An all-zero opening is padding and must never be matched on.
    names, stats = port_names(b"\x00" * 0x4000, SECTIONS, {0x50040: "pad"},
                              tgt_raw, tgt_secs, {0x60040}, ["DSOUND"], 12)
    assert names == {} and stats["unusable"] == 1, stats
    print("ok  port_names: rejects zero-filled signatures")


def test_check_entry():
    # Matched pair: MAP's entry resolves to the XBE header's entry.
    assert check_entry(0x11100, {"entry_point": "0x00011100"}) is True
    # Mismatched build -- the Xyanide case, which otherwise yields 20k wrong names.
    assert check_entry(0x21CDA2, {"entry_point": "0x00280724"}) is False
    # A MAP with no entry point line can't be checked, but isn't fatal.
    assert check_entry(None, {"entry_point": "0x00011100"}) is True
    print("ok  check_entry: accepts matched builds, rejects mismatched")


if __name__ == "__main__":
    test_parse()
    test_va_to_off()
    test_port()
    test_check_entry()
    print("\nall passed")

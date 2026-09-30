"""
Recover function names from an MSVC linker MAP file.

Two modes:

  resolve   MAP + analysis.json -> {va: name} for the title the MAP belongs to.

  port      Carry XDK *library* names from a title that has a MAP onto a title
            that does not, by matching library code byte-for-byte.

            Donors do NOT have to share the target's XDK version. Measured:
            ATV3 (XDK 5849) named 504 functions in Burnout 3 (also 5849), and
            Starcraft Ghost (XDK 5659) named 657 in the same binary at the same
            precision. Where both named the same address they agreed 99.1% of
            the time (422/426), so XDK library code is stable enough across
            versions to port names between them. Prefer several donors over one
            and merge the results; the union beat either alone (735 vs 504).

Why section:offset and not Rva+Base
-----------------------------------
A MAP's `Rva+Base` column is relative to the PE's preferred load address
(0x400000). imagebld discards that when it emits the XBE, and the XBE's section
layout is its own. The section:offset pair does survive: MAP section index N is
XBE section N-1, so

    xbe_va = xbe_sections[n - 1].virtual_addr + offset

Verified against ATV3 Lawless (XDK 5849), where the MAP's `entry point at
0001:0000d398` resolves to 0x0001E398 -- the XBE header's entry point -- and
all 11,189 publics land inside their claimed section.

Names are emitted raw (still MSVC-mangled). Sanitising to C identifiers is
tools/ghidra_naming/merge_names.py's job; this writes the same {addr: name}
shape it does, so the two are interchangeable downstream.
"""

import argparse
import json
import re
import sys
from pathlib import Path

# " 0001:00000000 00101896H .text                   CODE"
SEC_RE = re.compile(r"^\s*([0-9a-f]{4}):([0-9a-f]{8})\s+([0-9a-f]+)H\s+(\S+)\s+(\S+)\s*$", re.I)
# " 0001:00000000       ?foo@@YAXXZ   00400700 f i FOO.obj"
SYM_RE = re.compile(r"^\s*([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{8})\s", re.I)
ENTRY_RE = re.compile(r"entry point at\s+([0-9a-f]{4}):([0-9a-f]{8})", re.I)

# Sections holding statically-linked XDK library code, i.e. the code that is
# shared between titles built against the same XDK and so can be name-ported.
DEFAULT_XDK_SECTIONS = ["D3D", "D3DX", "DSOUND", "WMADEC", "XGRPH", "XPP",
                        "XONLINE", "XNET", "DOLBY", "XACTENG"]

# 12 bytes: long enough that ambiguous hits are rare, short enough to usually
# land before the first relocated operand. Tuned on ATV3 -> Burnout 3, where it
# gave the best precision at near-peak yield.
DEFAULT_SIG_LEN = 12


def load_sections(analysis_path):
    a = json.load(open(analysis_path))
    return [(s["name"], int(s["virtual_addr"], 16), s["virtual_size"],
             int(s["raw_addr"], 16), s["raw_size"]) for s in a["sections"]], a


def parse_map(map_path, sections):
    """MAP -> ({va: name}, entry_va or None). MAP section N is XBE section N-1."""
    lines = Path(map_path).read_text(errors="replace").splitlines()

    sec_va = {}
    for ln in lines:
        m = SEC_RE.match(ln)
        if m:
            idx = int(m.group(1), 16)
            if 0 < idx <= len(sections):
                sec_va.setdefault(idx, sections[idx - 1][1])

    syms = {}
    for ln in lines:
        m = SYM_RE.match(ln)
        if not m:
            continue
        idx, off, name = int(m.group(1), 16), int(m.group(2), 16), m.group(3)
        if idx in sec_va:
            syms[sec_va[idx] + off] = name

    entry = None
    for ln in lines:
        m = ENTRY_RE.search(ln)
        if m:
            idx, off = int(m.group(1), 16), int(m.group(2), 16)
            if idx in sec_va:
                entry = sec_va[idx] + off
            break
    return syms, entry


def check_entry(entry, xbe):
    """Does the MAP describe the build this XBE actually is?

    A MAP and an XBE sitting in the same drop are not necessarily the same
    build. Resolving the MAP's entry point and comparing it to the one in the
    XBE header is a cheap identity check: it agrees for a matched pair and
    diverges immediately for a mismatched one. Xyanide (2003-05-07) ships a MAP
    from 17:42 next to an XBE built 14 hours later, and without this it
    silently yields 20,642 wrong names at 10% recall.
    """
    if entry is None:
        print("note: no entry point in MAP; cannot verify it matches the XBE.")
        return True
    actual = int(xbe["entry_point"], 16)
    if entry == actual:
        print(f"entry point check: MATCH ({entry:#010x}) -- MAP describes this XBE")
        return True
    print(f"ERROR: entry point mismatch. MAP resolves to {entry:#010x}, "
          f"XBE header says {actual:#010x}.\n"
          f"       The MAP and XBE are different builds; the names would be "
          f"wrong. Find the XBE built alongside this MAP.", file=sys.stderr)
    return False


def va_to_off(sections, va):
    for _n, sva, vsz, ra, rsz in sections:
        if sva <= va < sva + vsz and (va - sva) < rsz:
            return ra + (va - sva)
    return None


def _find_all(blob, sig, limit=2):
    out, i = [], blob.find(sig)
    while i >= 0 and len(out) < limit:
        out.append(i)
        i = blob.find(sig, i + 1)
    return out


def port_names(donor_raw, donor_secs, donor_syms, tgt_raw, tgt_secs,
               tgt_starts, xdk_sections, sig_len):
    """Match donor XDK code into the target. Keeps only unambiguous matches
    that land on a function the target's own detector already found."""
    ranges = {n: (va, va + vsz) for n, va, vsz, _ra, _rsz in donor_secs
              if n in xdk_sections}
    blobs = [(sva, tgt_raw[ra:ra + rsz]) for n, sva, _vsz, ra, rsz in tgt_secs
             if n in xdk_sections]

    names, stats = {}, {"unique": 0, "ambiguous": 0, "absent": 0,
                        "unusable": 0, "off_start": 0}
    for va, name in donor_syms.items():
        if not any(lo <= va < hi for lo, hi in ranges.values()):
            continue
        off = va_to_off(donor_secs, va)
        if off is None:
            stats["unusable"] += 1
            continue
        sig = donor_raw[off:off + sig_len]
        # A mostly-zero opening is padding, not code: it matches everywhere.
        if len(sig) < sig_len or sig.count(0) > sig_len // 2:
            stats["unusable"] += 1
            continue

        found = []
        for sva, blob in blobs:
            for p in _find_all(blob, sig):
                found.append(sva + p)
            if len(found) > 1:
                break
        if not found:
            stats["absent"] += 1
        elif len(found) > 1:
            stats["ambiguous"] += 1
        else:
            stats["unique"] += 1
            # The check that makes this trustworthy: agree with the target's
            # own analysis, or drop it.
            if found[0] in tgt_starts:
                names[found[0]] = name
            else:
                stats["off_start"] += 1
    return names, stats


def _load_starts(functions_json):
    f = json.load(open(functions_json))
    if isinstance(f, dict):
        f = f.get("functions", list(f.values())[0])
    return {int(x["start"], 16) for x in f}


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="mode", required=True)

    r = sub.add_parser("resolve", help="MAP + analysis.json -> {va: name}")
    r.add_argument("map_file")
    r.add_argument("analysis_json")
    r.add_argument("-o", "--output", required=True)

    t = sub.add_parser("port", help="carry XDK names onto a title with no MAP")
    t.add_argument("--donor-map", required=True)
    t.add_argument("--donor-xbe", required=True)
    t.add_argument("--donor-analysis", required=True)
    t.add_argument("--target-xbe", required=True)
    t.add_argument("--target-analysis", required=True)
    t.add_argument("--target-functions", required=True,
                   help="functions.json from tools.disasm. Run it WITHOUT "
                        "--text-only or the XDK sections have no functions to "
                        "match against and nearly everything is discarded.")
    t.add_argument("--sig-len", type=int, default=DEFAULT_SIG_LEN)
    t.add_argument("--sections", default=",".join(DEFAULT_XDK_SECTIONS))
    t.add_argument("-o", "--output", required=True)

    a = p.parse_args()

    if a.mode == "resolve":
        secs, xbe = load_sections(a.analysis_json)
        syms, entry = parse_map(a.map_file, secs)
        print(f"resolved {len(syms)} symbols")
        if not check_entry(entry, xbe):
            sys.exit(2)
        json.dump({f"{v:#010x}": n for v, n in sorted(syms.items())},
                  open(a.output, "w"), indent=1)
        print(f"wrote {a.output}")
        return

    donor_secs, donor_a = load_sections(a.donor_analysis)
    tgt_secs, tgt_a = load_sections(a.target_analysis)

    dv, tv = donor_a.get("xdk_version"), tgt_a.get("xdk_version")
    if dv != tv:
        # Not a problem in practice: a 5659 donor named MORE of Burnout 3
        # (5849) than a 5849 donor did, at the same precision. Worth printing
        # only so a wildly mismatched pairing is visible.
        print(f"note: donor is XDK {dv}, target is XDK {tv}. Library code is "
              f"largely stable across versions; mismatched donors still work.")
    else:
        print(f"both titles are XDK {dv}")

    donor_syms, donor_entry = parse_map(a.donor_map, donor_secs)
    # The donor's MAP must describe the donor's XBE, or every signature is
    # taken from the wrong bytes.
    if not check_entry(donor_entry, donor_a):
        sys.exit(2)
    names, stats = port_names(
        Path(a.donor_xbe).read_bytes(), donor_secs, donor_syms,
        Path(a.target_xbe).read_bytes(), tgt_secs, _load_starts(a.target_functions),
        [s.strip() for s in a.sections.split(",")], a.sig_len)

    kept = len(names)
    print(f"  unique matches:  {stats['unique']}")
    print(f"  ambiguous:       {stats['ambiguous']}")
    print(f"  absent:          {stats['absent']}")
    print(f"  unusable sig:    {stats['unusable']}")
    print(f"  dropped (not a detected function start): {stats['off_start']}")
    if stats["unique"]:
        print(f"  kept {kept} names ({100*kept/stats['unique']:.1f}% of unique matches)")
    json.dump({f"{v:#010x}": n for v, n in sorted(names.items())},
              open(a.output, "w"), indent=1)
    print(f"wrote {a.output}")


if __name__ == "__main__":
    main()

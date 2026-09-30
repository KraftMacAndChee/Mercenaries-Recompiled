"""Assert the kernel thunk table agrees with the canonical export table.

The Xbox kernel export table contains DATA exports -- ExEventObjectType (16),
HalDiskModelNumber (41), IoFileObjectType (71), KdDebuggerEnabled (88) and
others -- that occupy ordinals but are variables, not callable functions.
Numbering only the callable exports and assigning them sequential ordinals
skips those slots and shifts everything after, so a game importing ordinal N
silently gets the function that belongs at N-1. It does not crash at the call
site; it corrupts the stack and fails somewhere else entirely.

That happened: 37 ordinals in src/kernel/kernel_thunks.c and 37 rows in
docs/formats/kernel-exports.md were shifted, binding 25 of Line of Contact's
144 imports to the wrong function.

Run this after touching either table.

    py -3 tools/validate_ordinals.py
"""
import re
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PARSER = os.path.join(ROOT, 'tools/xbe_parser/xbe_parser.py')
THUNKS = os.path.join(ROOT, 'src/kernel/kernel_thunks.c')
DOCS = os.path.join(ROOT, 'docs/formats/kernel-exports.md')


def canonical():
    src = open(PARSER, encoding='utf-8').read()
    return {int(o): n for o, n in
            re.findall(r'^\s*(\d+)\s*:\s*"(\w+)"\s*,', src, re.M)}


def thunk_cases():
    src = open(THUNKS, encoding='utf-8', errors='replace').read()
    body = src[src.index('xbox_resolve_ordinal'):]
    return {int(o): s for o, s in
            re.findall(r'case\s+(\d+)\s*:\s*return\s*\(ULONG_PTR\)\s*&?(\w+);', body)}


def doc_rows():
    src = open(DOCS, encoding='utf-8', errors='replace').read()
    out = {}
    for o, sig in re.findall(r'^\|\s*(\d+)\s*\|\s*`([^`]+)`', src, re.M):
        m = re.search(r'\b(\w+)\s*\(', sig) or re.search(r'(\w+)\s*$', sig)
        if m:
            out[int(o)] = m.group(1)
    return out


def main():
    table = canonical()
    errors = []

    for ordinal, sym in sorted(thunk_cases().items()):
        core = sym[5:] if sym.startswith('xbox_') else sym
        if core.startswith('Unknown_'):
            continue
        want = table.get(ordinal)
        if want is None:
            errors.append(f"thunks: ordinal {ordinal} ({sym}) is not in the export table")
        elif core != want and 'Xbox' + core != want:
            belongs = next((o for o, n in table.items()
                            if n == core or n == 'Xbox' + core), None)
            errors.append(
                f"thunks: ordinal {ordinal} is {want}, but returns {sym}"
                + (f" (which belongs at {belongs})" if belongs else ""))

    for ordinal, name in sorted(doc_rows().items()):
        want = table.get(ordinal)
        if want and name != want:
            errors.append(f"docs:   ordinal {ordinal} is {want}, but documented as {name}")

    if errors:
        print(f"FAIL: {len(errors)} ordinal mismatch(es)\n")
        for e in errors:
            print("  " + e)
        return 1

    print(f"OK: {len(thunk_cases())} thunk cases and {len(doc_rows())} doc rows "
          f"agree with the {len(table)}-entry export table")
    return 0


if __name__ == '__main__':
    sys.exit(main())

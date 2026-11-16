#!/usr/bin/env python3
"""Cross-platform port of ports/mercenaries/scripts/Merge-FunctionSeeds.ps1.

Merges classified function entry points with reviewed manual seeds, rejecting
classifier entries that land inside an already-known function body.
"""
import argparse
import bisect
import json
import os
import sys


def parse_addr(value):
    text = str(value).strip()
    if text.lower().startswith("0x"):
        digits = text[2:]
        if not digits or any(c not in "0123456789abcdefABCDEF" for c in digits):
            raise ValueError(f"Invalid hexadecimal Xbox address: '{text}'.")
        return int(digits, 16)
    if not text.isdigit():
        raise ValueError(f"Invalid decimal Xbox address: '{text}'.")
    return int(text, 10)


def load_json(path):
    with open(path, "r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--functions", required=True)
    parser.add_argument("--identified", required=True)
    parser.add_argument("--manual-seeds", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    functions = load_json(args.functions)
    identified = load_json(args.identified)
    manual_seeds = load_json(args.manual_seeds)

    ordered = sorted(parse_addr(fn["start"]) for fn in functions)
    starts = list(ordered)
    # End for the function that starts at index i.
    ends = [parse_addr(fn["end"]) for fn in functions]
    # Pair ends with starts by sorting both on start.
    pairs = sorted((parse_addr(fn["start"]), parse_addr(fn["end"])) for fn in functions)
    starts = [p[0] for p in pairs]
    ends = [p[1] for p in pairs]
    known_starts = set(starts)

    accepted = set()
    rejected_interior = 0
    for item in identified:
        address = parse_addr(item["start"])
        if address in known_starts:
            accepted.add(address)
            continue
        index = bisect.bisect_left(starts, address)
        prior = index - 1
        if prior >= 0 and starts[prior] < address < ends[prior]:
            rejected_interior += 1
            continue
        accepted.add(address)

    for item in manual_seeds:
        accepted.add(parse_addr(item["start"]))

    output = [{"start": f"0x{value:08X}"} for value in sorted(accepted)]

    parent = os.path.dirname(os.path.abspath(args.output))
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(output, handle, indent=2)
        handle.write("\n")

    print(f"Accepted {len(output)} seeds; rejected {rejected_interior} "
          f"classifier entries inside known functions.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Create a diagnostic generated-C tree with safe function-body overlays.

This never edits canonical generated output. It is intended for bisection:
copy a candidate tree, then replace non-structural changed functions with the
same-address bodies from a provenance baseline. Functions whose recovered
range changed, or which now subsume former entries, are deliberately skipped.
"""
from __future__ import annotations
import argparse
import bisect
import json
import shutil
from pathlib import Path
from audit_generated_function_parity import FUNCTION_RE, closing_brace, load_functions

def replace_body(text: str, address: int, replacement: str) -> str:
    for match in FUNCTION_RE.finditer(text):
        if int(match.group(1), 16) != address:
            continue
        end = closing_brace(text, text.find("{", match.start(), match.end()))
        return text[:match.start()] + replacement + text[end:]
    raise ValueError(f"sub_{address:08X} not found in copied candidate file")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--minimum", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--maximum", type=lambda value: int(value, 0), default=0xFFFFFFFF)
    args = parser.parse_args()
    baseline_root = args.baseline.resolve()
    candidate_root = args.candidate.resolve()
    output_root = args.output.resolve()
    if output_root.exists():
        raise ValueError(f"output already exists: {output_root}")
    baseline = load_functions(baseline_root)
    candidate = load_functions(candidate_root)
    candidate_starts = sorted(candidate)
    structural_owners: set[int] = set()
    for address in set(baseline) - set(candidate):
        owner_index = bisect.bisect_right(candidate_starts, address) - 1
        if owner_index < 0:
            continue
        owner = candidate_starts[owner_index]
        if owner < address < candidate[owner].original_end:
            structural_owners.add(owner)
    selected: list[int] = []
    skipped: list[dict[str, object]] = []
    for address in sorted(set(baseline) & set(candidate)):
        old, new = baseline[address], candidate[address]
        if not (args.minimum <= address <= args.maximum) or old.digest == new.digest:
            continue
        reason = None
        if old.original_end != new.original_end:
            reason = "range-changed"
        elif address in structural_owners:
            reason = "subsumes-baseline-entry"
        if reason:
            skipped.append({"address": f"0x{address:08X}", "reason": reason})
        else:
            selected.append(address)
    shutil.copytree(candidate_root, output_root)
    by_file: dict[str, list[int]] = {}
    for address in selected:
        by_file.setdefault(candidate[address].source, []).append(address)
    for relative, addresses in by_file.items():
        path = output_root / relative
        text = path.read_text(encoding="utf-8")
        for address in addresses:
            text = replace_body(text, address, baseline[address].body)
        path.write_text(text, encoding="utf-8", newline="\n")
    manifest = {
        "baseline": str(baseline_root), "candidate": str(candidate_root),
        "minimum": f"0x{args.minimum:08X}", "maximum": f"0x{args.maximum:08X}",
        "overlaid": [f"0x{address:08X}" for address in selected], "skipped": skipped,
    }
    (output_root / "overlay-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"overlaid={len(selected)} skipped={len(skipped)} output={output_root}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

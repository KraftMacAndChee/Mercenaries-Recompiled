#!/usr/bin/env python3
"""Compare generated recompiler trees at function granularity.

Generated file chunking changes between translator revisions, so file diffs are
noisy. This tool keys bodies by retail address and reports added, removed, and
changed functions. Optional canonical inputs are scanned for address references.
"""

from __future__ import annotations
import argparse
import bisect
import hashlib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

FUNCTION_RE = re.compile(r"(?m)^void\s+sub_([0-9A-Fa-f]{8})\s*\(void\)\s*\n\{")
ORIGINAL_RE = re.compile(
    r"\* Original:\s*0x([0-9A-Fa-f]{8})\s*-\s*0x([0-9A-Fa-f]{8})"
)
SPACE_RE = re.compile(r"[ \t]+")


@dataclass(frozen=True)
class FunctionBody:
    address: int
    source: str
    start_line: int
    body: str
    original_end: int

    @property
    def digest(self) -> str:
        normalized = "\n".join(
            SPACE_RE.sub(" ", line.rstrip()).strip()
            for line in self.body.replace("\r\n", "\n").splitlines()
            if line.strip()
        )
        return hashlib.sha256(normalized.encode()).hexdigest()


def closing_brace(text: str, opening: int) -> int:
    depth, state, index = 0, "code", opening
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and following == "/":
                state, index = "line", index + 1
            elif char == "/" and following == "*":
                state, index = "block", index + 1
            elif char == '"':
                state = "string"
            elif char == "'":
                state = "char"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index + 1
        elif state == "line" and char == "\n":
            state = "code"
        elif state == "block" and char == "*" and following == "/":
            state, index = "code", index + 1
        elif state in ("string", "char"):
            if char == "\\":
                index += 1
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        index += 1
    raise ValueError("unterminated generated function body")


def load_functions(root: Path) -> dict[int, FunctionBody]:
    result: dict[int, FunctionBody] = {}
    files = sorted(root.rglob("recomp_[0-9][0-9][0-9][0-9].c"))
    if not files:
        raise ValueError(f"no recomp_XXXX.c files under {root}")
    for path in files:
        text = path.read_text(encoding="utf-8")
        for match in FUNCTION_RE.finditer(text):
            address = int(match.group(1), 16)
            annotations = list(ORIGINAL_RE.finditer(text, max(0, match.start() - 512), match.start()))
            if not annotations or int(annotations[-1].group(1), 16) != address:
                raise ValueError(f"missing or mismatched Original range for sub_{address:08X} in {path}")
            original_end = int(annotations[-1].group(2), 16)
            end = closing_brace(text, text.find("{", match.start(), match.end()))
            if address in result:
                raise ValueError(f"duplicate sub_{address:08X}: {result[address].source} and {path}")
            result[address] = FunctionBody(
                address, str(path.relative_to(root)),
                text.count("\n", 0, match.start()) + 1,
                text[match.start():end], original_end,
            )
    return result


def scan_references(paths: Iterable[Path], addresses: Iterable[int]) -> dict[int, list[str]]:
    addresses = set(addresses)
    result = {address: [] for address in addresses}
    address_token = re.compile(
        r"(?i)(?:sub_|0x)([0-9a-f]{8})\b|(?<![0-9A-Fa-f])([0-9a-f]{8})(?![0-9A-Fa-f])"
    )
    for root in paths:
        files = [root] if root.is_file() else sorted(p for p in root.rglob("*") if p.is_file())
        for path in files:
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            referenced = {
                int(first or second, 16)
                for first, second in address_token.findall(text)
            }
            for address in addresses & referenced:
                result[address].append(str(path))
    return result


def entry(body: FunctionBody, references: dict[int, list[str]]) -> dict[str, object]:
    return {
        "address": f"0x{body.address:08X}", "source": body.source,
        "line": body.start_line, "sha256": body.digest,
        "original_end": f"0x{body.original_end:08X}",
        "canonical_references": references.get(body.address, []),
    }


def compare(baseline_root: Path, candidate_root: Path, reference_paths: list[Path]) -> dict[str, object]:
    baseline, candidate = load_functions(baseline_root), load_functions(candidate_root)
    baseline_keys, candidate_keys = set(baseline), set(candidate)
    removed_all = sorted(baseline_keys - candidate_keys)
    candidate_starts = sorted(candidate)
    subsumed: list[tuple[int, int]] = []
    removed: list[int] = []
    for address in removed_all:
        owner_index = bisect.bisect_right(candidate_starts, address) - 1
        owner = candidate_starts[owner_index] if owner_index >= 0 else None
        if owner is not None and owner < address < candidate[owner].original_end:
            subsumed.append((address, owner))
        else:
            removed.append(address)
    added = sorted(candidate_keys - baseline_keys)
    changed = sorted(
        address for address in baseline_keys & candidate_keys
        if baseline[address].digest != candidate[address].digest
    )
    references = scan_references(reference_paths, removed + added + changed)
    return {
        "baseline_root": str(baseline_root), "candidate_root": str(candidate_root),
        "baseline_function_count": len(baseline),
        "candidate_function_count": len(candidate),
        "unchanged_function_count": len(baseline_keys & candidate_keys) - len(changed),
        "removed": [entry(baseline[a], references) for a in removed],
        "subsumed": [{
            "address": f"0x{address:08X}",
            "baseline": entry(baseline[address], references),
            "candidate_owner": entry(candidate[owner], references),
        } for address, owner in subsumed],
        "added": [entry(candidate[a], references) for a in added],
        "changed": [{
            "address": f"0x{a:08X}",
            "baseline": entry(baseline[a], references),
            "candidate": entry(candidate[a], references),
            "canonical_references": references.get(a, []),
        } for a in changed],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--canonical-reference", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = compare(
        args.baseline.resolve(), args.candidate.resolve(),
        [path.resolve() for path in args.canonical_reference],
    )
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    print(
        f"generated parity: baseline={report['baseline_function_count']} "
        f"candidate={report['candidate_function_count']} "
        f"removed={len(report['removed'])} subsumed={len(report['subsumed'])} "
        f"added={len(report['added'])} "
        f"changed={len(report['changed'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
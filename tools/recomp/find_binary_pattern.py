#!/usr/bin/env python3
"""Find hexadecimal byte patterns with ``??`` wildcards in binary files."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_pattern(value: str) -> list[int | None]:
    tokens = value.replace("_", " ").split()
    if not tokens:
        raise argparse.ArgumentTypeError("pattern cannot be empty")
    parsed: list[int | None] = []
    for token in tokens:
        if token in {"?", "??"}:
            parsed.append(None)
        else:
            try:
                parsed.append(int(token, 16))
            except ValueError as exc:
                raise argparse.ArgumentTypeError(
                    f"invalid hexadecimal byte {token!r}"
                ) from exc
    if any(value < 0 or value > 0xFF for value in parsed if value is not None):
        raise argparse.ArgumentTypeError("pattern bytes must be in the range 00..FF")
    return parsed


def find_all(data: bytes, pattern: list[int | None]) -> list[int]:
    fixed = next((index for index, value in enumerate(pattern) if value is not None), None)
    if fixed is None:
        return list(range(len(data) - len(pattern) + 1))
    needle = bytes((pattern[fixed],))
    offsets: list[int] = []
    start = 0
    while True:
        hit = data.find(needle, start)
        if hit < 0:
            break
        candidate = hit - fixed
        if candidate >= 0 and candidate + len(pattern) <= len(data):
            if all(value is None or data[candidate + index] == value
                   for index, value in enumerate(pattern)):
                offsets.append(candidate)
        start = hit + 1
    return offsets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pattern", type=parse_pattern)
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()
    for path in args.paths:
        for offset in find_all(path.read_bytes(), args.pattern):
            print(f"{path}: file+0x{offset:X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

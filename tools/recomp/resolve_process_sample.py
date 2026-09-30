#!/usr/bin/env python3
"""Resolve ProcessSampler module addresses with an MSVC linker map."""

from __future__ import annotations

import argparse
import bisect
import pathlib
import re


PREFERRED_BASE_RE = re.compile(r"Preferred load address is ([0-9A-Fa-f]+)")
MAP_SYMBOL_RE = re.compile(
    r"^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}\s+"
    r"(\S+)\s+([0-9A-Fa-f]{16})(?:\s|$)"
)
ADDRESS_RE = re.compile(r"0x[0-9A-Fa-f]+")


def parse_int(value: str) -> int:
    return int(value, 0)


def load_map(path: pathlib.Path) -> tuple[int, list[tuple[int, str]]]:
    preferred_base = None
    symbols: dict[int, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if preferred_base is None:
            match = PREFERRED_BASE_RE.search(line)
            if match:
                preferred_base = int(match.group(1), 16)
        match = MAP_SYMBOL_RE.match(line)
        if not match:
            continue
        name, preferred_address = match.groups()
        address = int(preferred_address, 16)
        if preferred_base is not None and address >= preferred_base:
            symbols.setdefault(address - preferred_base, name)

    if preferred_base is None:
        raise ValueError(f"{path}: missing preferred load address")
    if not symbols:
        raise ValueError(f"{path}: no image symbols found")
    return preferred_base, sorted(symbols.items())


def resolve_text(text: str, module_base: int, symbols: list[tuple[int, str]]) -> str:
    rvas = [rva for rva, _ in symbols]
    # MSVC maps do not report the executable's SizeOfImage. The last public
    # symbol plus one 64 KiB allocation unit safely covers function tails while
    # preventing unrelated DLL addresses from being attributed to the EXE.
    maximum_image_rva = rvas[-1] + 0x10000

    def replace(match: re.Match[str]) -> str:
        absolute = int(match.group(0), 16)
        if absolute < module_base:
            return match.group(0)
        rva = absolute - module_base
        if rva > maximum_image_rva:
            return match.group(0)
        index = bisect.bisect_right(rvas, rva) - 1
        if index < 0:
            return match.group(0)
        symbol_rva, name = symbols[index]
        offset = rva - symbol_rva
        return name if offset == 0 else f"{name}+0x{offset:X}"

    return ADDRESS_RE.sub(replace, text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--sample", required=True, type=pathlib.Path)
    parser.add_argument("--map", required=True, dest="map_path", type=pathlib.Path)
    parser.add_argument("--module-base", required=True, type=parse_int)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    _, symbols = load_map(args.map_path)
    sample = args.sample.read_text(encoding="utf-8", errors="replace")
    resolved = resolve_text(sample, args.module_base, symbols)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(resolved, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
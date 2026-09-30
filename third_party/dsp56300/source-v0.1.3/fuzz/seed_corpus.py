#!/usr/bin/env python3
"""Extract fuzz corpus seeds from test files.

Usage: python3 fuzz/seed_corpus.py
"""

import re
import struct
import pathlib

ROOT = pathlib.Path(__file__).parent.parent
FUZZ = pathlib.Path(__file__).parent

HEADER_SIZE = 29

# Register name -> header offset and size
REG_HEADER_MAP = {
    "A1": (6, 3),
    "B1": (9, 3),
    "X0": (12, 3),
    "Y0": (15, 3),
    "R0": (18, 3),
    "N0": (21, 3),
    "M0": (24, 3),
}


def pack_u24_le(val: int) -> bytes:
    return struct.pack("<I", val & 0xFFFFFF)[:3]


def build_emu_seed(pram_words: dict[int, int], regs: dict[str, int] = {},
                   use_execute_one: bool = False) -> bytes:
    """Build a binary seed matching the emu fuzz target input format."""
    header = bytearray(HEADER_SIZE)

    # flags
    flags = 1 if use_execute_one else 0
    struct.pack_into("<H", header, 0, flags)

    # SR
    sr = regs.get("SR", 0) & 0x8CFF
    struct.pack_into("<I", header, 2, sr)

    # 24-bit register fields
    for name, (off, _size) in REG_HEADER_MAP.items():
        val = regs.get(name, 0) & 0xFFFFFF
        header[off] = val & 0xFF
        header[off + 1] = (val >> 8) & 0xFF
        header[off + 2] = (val >> 16) & 0xFF

    # SP, LC
    header[27] = regs.get("SP", 0) & 0x0F
    header[28] = regs.get("LC", 0) & 0xFF

    # PRAM words (up to 16, packed contiguously from index 0)
    if not pram_words:
        return bytes(header)
    max_idx = min(max(pram_words.keys()), 15)
    pram_bytes = bytearray()
    for i in range(max_idx + 1):
        pram_bytes += pack_u24_le(pram_words.get(i, 0))

    return bytes(header) + bytes(pram_bytes)


def extract_emu_seeds():
    """Extract pram/register assignments from emu.rs, grouped by test function."""
    src = (ROOT / "crates" / "emu" / "tests" / "emu.rs").read_text()
    corpus_dir = FUZZ / "corpus" / "emu"
    corpus_dir.mkdir(parents=True, exist_ok=True)

    # Split by #[test] fn name
    parts = re.split(r"#\[test\]\s*fn\s+(\w+)", src)

    count = 0
    for i in range(1, len(parts), 2):
        name = parts[i]
        body = parts[i + 1]

        # Extract pram assignments: pram[N] = 0xHEX
        pram_assigns = re.findall(r"pram\[(\d+)\]\s*=\s*(0x[0-9A-Fa-f]+)", body)
        if not pram_assigns:
            continue

        pram_words = {}
        for idx_str, val_str in pram_assigns:
            pram_words[int(idx_str)] = int(val_str, 16)

        # Extract register pre-sets: s.registers[reg::FOO] = val
        reg_assigns = re.findall(
            r"s\.registers\[reg::(\w+)\]\s*=\s*(0x[0-9A-Fa-f]+|\d+)", body
        )
        regs = {}
        for reg_name, val_str in reg_assigns:
            regs[reg_name] = int(val_str, 0)

        # Emit run() variant
        seed = build_emu_seed(pram_words, regs, use_execute_one=False)
        (corpus_dir / f"seed_run_{name}").write_bytes(seed)
        count += 1

        # Emit execute_one() variant
        seed = build_emu_seed(pram_words, regs, use_execute_one=True)
        (corpus_dir / f"seed_step_{name}").write_bytes(seed)
        count += 1

    print(f"emu: wrote {count} seeds to {corpus_dir}")


def extract_asm_seeds():
    """Extract roundtrip/assemble strings from asm.rs."""
    src = (ROOT / "crates" / "asm" / "tests" / "asm.rs").read_text()
    corpus_dir = FUZZ / "corpus" / "asm"
    corpus_dir.mkdir(parents=True, exist_ok=True)

    # roundtrip("...", N) and roundtrip_warning("...", N, ...)
    strings = re.findall(r'roundtrip(?:_warning)?\("([^"]+)"', src)

    # Also grab assemble_line("...", N) calls
    strings += re.findall(r'assemble_line\("([^"]+)"', src)

    # Deduplicate while preserving order
    seen = set()
    unique = []
    for s in strings:
        if s not in seen:
            seen.add(s)
            unique.append(s)

    count = 0
    for i, s in enumerate(unique):
        (corpus_dir / f"seed_{i:04d}").write_bytes(s.encode("utf-8"))
        count += 1

    print(f"asm: wrote {count} seeds to {corpus_dir}")


if __name__ == "__main__":
    extract_emu_seeds()
    extract_asm_seeds()

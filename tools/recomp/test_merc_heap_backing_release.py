#!/usr/bin/env python3
"""Regression coverage for large RtlHeap fallback backing releases."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANUAL = (ROOT / "ports" / "mercenaries" / "src" / "recomp_manual.c").read_text(encoding="utf-8")

assert "if (address < MERC_HEAP_ARENA_START || address >= MERC_HEAP_ARENA_END)" in MANUAL
assert "xbox_HeapFree(address);" in MANUAL
assert "merc_heap_remove_block((uint32_t)found);" in MANUAL

release = MANUAL.index("xbox_HeapFree(address);")
remove = MANUAL.index("merc_heap_remove_block((uint32_t)found);", release)
assert release < remove

print("ok merc_heap_backing_release")

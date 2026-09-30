#!/usr/bin/env python3
"""Verify the durable Havok heightfield temporary-array size repair."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0008.c"
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"


def check(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    save = "MEM32(esp + 0x2Cu) = ebx;"
    restore = "ebx = MEM32(esp + 0x2Cu);"
    assert text.count(save) == 1, f"missing or duplicate Havok size save in {path}"
    assert text.count(restore) == 1, f"missing or duplicate Havok size restore in {path}"
    if path == GENERATED:
        assert text.index(save) < text.index("loc_001A626A: ;")
        assert text.index(restore) > text.index("loc_001A63EA: ;")
        assert text.index(restore) < text.index("loc_001A63F4: ;")


def main() -> None:
    check(GENERATED)
    check(PATCHER)
    print("Havok heightfield allocation-size preservation checks passed")


if __name__ == "__main__":
    main()
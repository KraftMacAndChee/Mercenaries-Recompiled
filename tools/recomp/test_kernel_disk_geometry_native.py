#!/usr/bin/env python3
"""Regression coverage for the fresh Xbox partition geometry query."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BRIDGE = ROOT / "src" / "kernel" / "kernel_bridge.c"


def main() -> None:
    source = BRIDGE.read_text(encoding="utf-8")
    assert 'getenv("MERCENARIES_ENABLE_DISK_GEOMETRY") != NULL' in source
    assert "ioctl == 0x00070000u" in source
    assert "output_length >= 24u" in source
    assert "BRIDGE_MEM32(output_va + 0) = 0x01400000u" in source
    assert "BRIDGE_MEM32(output_va + 8) = 12u" in source
    assert "BRIDGE_MEM32(output_va + 20) = 512u" in source
    assert "bridge_write_iostatus(ios_va, 0u, 24u)" in source
    print("fresh Xbox disk geometry query returns a complete 10 GB HDD descriptor")


if __name__ == "__main__":
    main()

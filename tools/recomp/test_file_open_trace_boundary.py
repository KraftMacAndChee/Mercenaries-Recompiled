#!/usr/bin/env python3
"""Keep the hidden boot-boundary trace canonical and opt-in."""

from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    bridge = (root / "src/kernel/kernel_bridge.c").read_text(encoding="utf-8")
    runner = (root / "tools/recomp/Run-HiddenRetailManual.ps1").read_text(encoding="utf-8")
    variable = "MERCENARIES_TRACE_FILE_OPEN_DUMP_AT"
    assert variable in bridge
    assert variable in runner
    assert "[FILE-OPEN-TRACE-STATE]" in bridge
    assert "exit(90);" in bridge
    assert "[FILE-OPEN-TRACE]" in bridge
    assert "[ValidateRange(1, 512)]" in runner
    assert "MERCENARIES_TRACE_KERNEL_CALLS" in runner
    assert "MERCENARIES_TRACE_KERNEL_DUMP_AT" in bridge
    assert "MERCENARIES_TRACE_KERNEL_DUMP_AT" in runner
    assert "[KERNEL-BOUNDARY-TRACE]" in bridge
    print("file-open trace boundary test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
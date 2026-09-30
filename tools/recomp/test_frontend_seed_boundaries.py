"""Regression for reviewed retail seed boundaries required at startup.

    py -3 tools/recomp/test_frontend_seed_boundaries.py
"""
from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports" / "mercenaries"
RECOMPILE = PORT / "scripts" / "Recompile.ps1"
MANUAL_SEEDS = PORT / "manual-seeds.json"
FUNCTIONS = PORT / "analysis" / "disasm" / "functions.json"
GEN = PORT / "src" / "recomp" / "gen"

SHORT_BOUNDARIES = {0x00034AD0, 0x00208F30, 0x00209B80}


def main() -> int:
    script = RECOMPILE.read_text(encoding="utf-8")
    assert re.search(
        r'"--function-range",\s*"0x000DFF60:0x000E0097"', script
    ), "front-end shared continuation is not pinned in Recompile.ps1"

    seeds = {
        int(item["start"], 16)
        for item in json.loads(MANUAL_SEEDS.read_text(encoding="utf-8"))
    }
    required = SHORT_BOUNDARIES | {0x000DFF60}
    assert required <= seeds, f"missing reviewed manual seeds: {required - seeds}"

    # After canonical regeneration, every protected short boundary must be a
    # real function and the front-end owner must include its complete retail
    # continuation rather than ending at 0x000E000A.
    functions = {
        int(item["start"], 16): int(item["end"], 16)
        for item in json.loads(FUNCTIONS.read_text(encoding="utf-8"))
    }
    if (GEN / "recomp_0000.c").exists():
        assert SHORT_BOUNDARIES <= functions.keys(), (
            f"generated analysis lost protected seeds: "
            f"{SHORT_BOUNDARIES - functions.keys()}"
        )

    print("OK: startup function range and protected retail seeds are pinned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

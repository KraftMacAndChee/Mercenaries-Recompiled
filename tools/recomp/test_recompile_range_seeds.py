"""Every reviewed Recompile.ps1 function range must have a durable seed."""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RECOMPILE = ROOT / "ports" / "mercenaries" / "scripts" / "Recompile.ps1"
SEEDS = ROOT / "ports" / "mercenaries" / "manual-seeds.json"


def test_every_function_range_owner_is_manually_seeded() -> None:
    script = RECOMPILE.read_text(encoding="utf-8")
    range_starts = {
        int(start, 16)
        for start in re.findall(
            r'"--function-range",\s*"(0x[0-9A-Fa-f]+):0x[0-9A-Fa-f]+"',
            script,
        )
    }
    seed_starts = {
        int(entry["start"], 16)
        for entry in json.loads(SEEDS.read_text(encoding="utf-8"))
    }

    missing = sorted(range_starts - seed_starts)
    assert not missing, (
        "Recompile.ps1 function-range owners missing from manual-seeds.json: "
        + ", ".join(f"0x{address:08X}" for address in missing)
    )


if __name__ == "__main__":
    test_every_function_range_owner_is_manually_seeded()
    print("ok  recompile_range_seeds")

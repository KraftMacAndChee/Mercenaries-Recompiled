"""Regression checks for the retail RsAi ObjectiveNone factory branch."""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEEDS = ROOT / "ports" / "mercenaries" / "manual-seeds.json"
GEN = ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen"


def test_objective_none_branch_is_seeded_and_translated() -> None:
    seeds = json.loads(SEEDS.read_text(encoding="utf-8"))
    assert any(int(item["start"], 0) == 0x00086AF8 for item in seeds)

    generated = (GEN / "recomp_0002.c").read_text(encoding="utf-8")
    stubs = (GEN / "recomp_stubs_unresolved.c").read_text(encoding="utf-8")
    assert "void sub_00086AF8(void)" in generated
    assert "MEM32(esp + 4) = edx;" in generated
    assert "ecx = eax;" in generated
    assert "sub_00067990(); return; /* tail jmp 0x00067990 */" in generated
    assert "00086AF8: not detected" not in stubs


if __name__ == "__main__":
    test_objective_none_branch_is_seeded_and_translated()
    print("AI ObjectiveNone branch checks passed")
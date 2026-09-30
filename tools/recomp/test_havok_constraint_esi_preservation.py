from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)
TYPES = (ROOT / "ports/mercenaries/src/recomp/recomp_types.h").read_text(
    encoding="utf-8"
)


def test_havok_constraint_builder_preserves_nonvolatile_esi() -> None:
    assert '"Havok constraint builder nonvolatile ESI guard"' in PATCHER
    assert "const uint32_t _constraint_esi = esi;" in PATCHER
    assert "recomp_havok_constraint_esi_checkpoint" in PATCHER
    assert "esi = _constraint_esi;" in PATCHER
    assert "void recomp_havok_constraint_esi_checkpoint" in TYPES


if __name__ == "__main__":
    test_havok_constraint_builder_preserves_nonvolatile_esi()
    print("havok constraint ESI preservation tests passed")
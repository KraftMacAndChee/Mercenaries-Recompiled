from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0006.c"
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"


def test_full_duplicate_vehicle_event_ring_has_equivalent_fast_path() -> None:
    generated = GENERATED.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")
    marker = "Vehicle physics full duplicate event ring fast path"
    assert marker in patcher
    assert "_event_i < 10u" in generated
    assert "MEM32(ebx + 0xCu + _event_i * 4u) != edi" in generated
    assert "if (_all_duplicate_events != 0u) goto loc_0014FAC3;" in generated


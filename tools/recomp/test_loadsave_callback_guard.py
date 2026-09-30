"""Regression coverage for invalid RsLoadSaveGame callback pointers."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANUAL = ROOT / "ports" / "mercenaries" / "src" / "recomp_manual.c"
TYPES = ROOT / "ports" / "mercenaries" / "src" / "recomp" / "recomp_types.h"
GENERATED = ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_0007.c"
PATCHER = ROOT / "ports" / "mercenaries" / "scripts" / "Patch-Generated.py"


def test_loadsave_optional_callback_is_validated_before_vtable_read() -> None:
    manual = MANUAL.read_text(encoding="utf-8")
    types = TYPES.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")

    assert "recomp_loadsave_callback_sanitize" in types
    assert "callback_address > 0x04000000u - 4u" in manual
    assert "vtable > 0x04000000u - 12u" in manual
    assert "[LOADSAVE-CALLBACK-GUARD]" in manual
    guarded = "eax = recomp_loadsave_callback_sanitize(esi, eax);"
    assert guarded in generated
    assert guarded in patcher
    assert generated.index(guarded) < generated.index(
        "if (TEST_Z(eax, eax)) goto loc_001874A6"
    )


if __name__ == "__main__":
    test_loadsave_optional_callback_is_validated_before_vtable_read()
    print("Load/save callback guard checks passed")

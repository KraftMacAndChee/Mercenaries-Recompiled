from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
        encoding="utf-8"
    )
    generated = (
        ROOT / "ports/mercenaries/src/recomp/gen/recomp_0008.c"
    ).read_text(encoding="utf-8")
    patcher = (
        ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
    ).read_text(encoding="utf-8")

    assert "uint32_t recomp_havok_callback_count_checkpoint" in manual
    assert "count <= 0x1000u" in manual
    assert "[HAVOK-CALLBACK-LIST]" in manual
    assert "if (!owner_valid || !array_valid || !count_sane)" in manual
    needle = "esi = recomp_havok_callback_count_checkpoint("
    assert generated.count(needle) == 1
    assert "Havok callback collection corruption guard" in patcher
    assert patcher.count(needle) == 1
    print("Havok callback-list corruption guard test passed")


if __name__ == "__main__":
    main()

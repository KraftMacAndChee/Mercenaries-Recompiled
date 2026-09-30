from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCH_SCRIPT = ROOT / "ports" / "mercenaries" / "scripts" / "Patch-Generated.py"
GENERATED = ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" / "recomp_0000.c"


def test_property_update_preserves_nonvolatile_esi() -> None:
    patch_text = PATCH_SCRIPT.read_text(encoding="utf-8")
    assert "retail property updater nonvolatile ESI snapshot" in patch_text
    assert "retail property updater nonvolatile ESI restore" in patch_text

    generated = GENERATED.read_text(encoding="utf-8")
    start = generated.index("void sub_00012520(void)")
    end = generated.index("\nvoid sub_00012710(void)", start)
    body = generated[start:end]

    assert body.count("const uint32_t _saved_esi_nonvolatile = esi;") == 1
    restore = body.index("recomp_update_esi_checkpoint(0x00012520u")
    pop = body.index("POP32(esp, esi);", restore)
    repair = body.index("esi = _saved_esi_nonvolatile;", pop)
    frame_restore = body.index("esp = ebp;", repair)
    assert restore < pop < repair < frame_restore

def test_vehicle_transition_preserves_nonvolatile_esi() -> None:
    patch_text = PATCH_SCRIPT.read_text(encoding="utf-8")
    assert "retail vehicle transition nonvolatile ESI snapshot" in patch_text
    assert "retail vehicle transition nonvolatile ESI restore" in patch_text

    generated = GENERATED.read_text(encoding="utf-8")
    start = generated.index("void sub_0002E4D0(void)")
    end = generated.index("\nvoid sub_0002E680(void)", start)
    body = generated[start:end]

    assert body.count("const uint32_t _saved_esi_nonvolatile = esi;") == 1
    first_pop = body.index("POP32(esp, edi);")
    checkpoint = body.index("recomp_update_esi_checkpoint(0x0002E4D0u", first_pop)
    pop = body.index("POP32(esp, esi);", checkpoint)
    repair = body.index("esi = _saved_esi_nonvolatile;", pop)
    frame_restore = body.index("esp = ebp;", repair)
    assert first_pop < checkpoint < pop < repair < frame_restore

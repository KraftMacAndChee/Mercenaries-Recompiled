from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_roadblock_model_trace_is_targeted_and_opt_in() -> None:
    manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
        encoding="utf-8"
    )
    patches = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
        encoding="utf-8"
    )

    assert 'getenv("MERCENARIES_TRACE_ROADBLOCK_MODEL")' in manual
    assert "value0 != roadblock_trace_hash" in manual
    assert "loc_001F5020" in patches
    assert "loc_001F505D" in patches
    assert "loc_00220984" in patches
    assert "loc_00220E2D" in patches
    assert "loc_00220FC1" in patches
    assert "loc_0022135A" in patches
    assert "loc_002214AE" in patches
    assert "loc_002100E0" in patches
    assert "loc_00216DA0" in patches
    assert "loc_00216DBE" in patches
    assert "loc_00216DD0" in patches
    assert "_render_queue_target" in patches
    assert "recomp_redmodel_queue_checkpoint" in manual
    assert "recomp_redprimitive_draw_checkpoint" in manual
    assert "[ROADBLOCK-PRIMITIVE]" in manual
    assert patches.count("recomp_redmodel_render_checkpoint") >= 11
    assert patches.count("recomp_redprimitive_draw_checkpoint") >= 6


if __name__ == "__main__":
    test_roadblock_model_trace_is_targeted_and_opt_in()
    print("roadblock model render trace checks passed")
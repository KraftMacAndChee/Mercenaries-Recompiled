"""Keep final-flare diagnostics bounded, gated, and after draw submission."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
RUNNER = (ROOT / "tools/recomp/Run-HiddenRetailManual.ps1" ).read_text(encoding="utf-8")


def test_flare_composite_capture_is_post_draw_and_opt_in() -> None:
    draw = SOURCE.index("dev->lpVtbl->DrawPrimitiveUP(dev")
    marker = SOURCE.index("MERCENARIES_CAPTURE_FLARE_COMPOSITE_PREFIX", draw)
    probe = SOURCE.index("MERCENARIES_CAPTURE_FLARE_PROBE_PREFIX", marker)
    assert draw < marker < probe
    block = SOURCE[draw:probe]
    assert "captured_flare_composites < 32u" in block
    assert "MERCENARIES_CAPTURE_FLARE_COMPOSITE_GATE_FILE" in block
    assert "debug_trace_flare_sample_source" in block
    assert "d3d8_DebugCaptureTextureToPath" in block
    assert "bounds=(%.6g,%.6g)..(%.6g,%.6g)" in block
    assert "composite_vertex < num_verts" in block
    assert "[switch]$CaptureFlareComposite" in RUNNER
    assert "MERCENARIES_CAPTURE_FLARE_COMPOSITE_PREFIX" in RUNNER
    assert "MERCENARIES_TRACE_SUSPICIOUS_FLARE_BOUNDS" in SOURCE
    assert "[FLARE-SUSPICIOUS]" in SOURCE
    assert "suspicious_count < 32u" in SOURCE
    assert "debug_project_br3d_flare_bounds" in SOURCE
    assert "projected_suspicious" in SOURCE
    assert "projected=%d/" in SOURCE
    assert "C_OBJ_VIEW_M0: -73 + 96" in SOURCE
    assert "C_PROJ_SCREEN_0: -70 + 96" in SOURCE
    assert "[switch]$TraceSuspiciousFlare" in RUNNER


if __name__ == "__main__":
    test_flare_composite_capture_is_post_draw_and_opt_in()
    print("ok nv2a_flare_composite_capture")

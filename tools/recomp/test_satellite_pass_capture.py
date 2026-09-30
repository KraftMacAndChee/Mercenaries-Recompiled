from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c"
RUNNER = ROOT / "tools" / "recomp" / "Run-HiddenRetailRoute.ps1"
MANUAL_RUNNER = ROOT / "tools" / "recomp" / "Run-HiddenRetailManual.ps1"


def main() -> None:
    renderer = RENDERER.read_text(encoding="utf-8")
    runner = RUNNER.read_text(encoding="utf-8")
    manual_runner = MANUAL_RUNNER.read_text(encoding="utf-8")
    assert "debug_capture_satellite_pass_before" in renderer
    assert "debug_capture_satellite_pass_after" in renderer
    assert "debug_capture_satellite_draw_before" in renderer
    assert "debug_capture_satellite_draw_after" in renderer
    assert "[SATELLITE-DRAW]" in renderer
    assert "g_debug_satellite_array_capture_count < 8u" in renderer
    assert "ordinal % 512u >= 511u" in renderer
    assert "pre-target-%08X.bmp" in renderer
    assert "pre-source-%08X.bmp" in renderer
    assert 'debug_capture_satellite_draw_after("array"' in renderer
    assert 'debug_capture_satellite_draw_after("immediate"' in renderer
    for shader_hash in ("CE41C616", "1730DD1A", "B0828BA6"):
        assert shader_hash in renderer
    assert "MERCENARIES_CAPTURE_SATELLITE_PASS_GATE_FILE" in renderer
    assert "[switch]$CaptureSatellitePasses" in runner
    assert "satellite-pass.ready" in runner
    assert "[switch]$CaptureSatellitePasses" in manual_runner
    assert "satellite-pass.ready" in manual_runner
    for marker in ("[SATELLITE-DEPTH-DECISION]", "[SATELLITE-SAMPLED]",
                   "transient-pre", "transient-post", "live-zeta",
                   "CompareFileTime", "capture_count >= 9u", "reports >= 16u",
                   "PSGetShaderResources(context, stage", "debug_capture_satellite_raw"):
        assert marker in renderer
    print("satellite pass capture guard: ok")


if __name__ == "__main__":
    main()

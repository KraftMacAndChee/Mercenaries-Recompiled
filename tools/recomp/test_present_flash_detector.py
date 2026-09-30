from pathlib import Path
import runpy


ROOT = Path(__file__).resolve().parents[2]
DEVICE = ROOT / "src" / "d3d" / "d3d8_device.c"
RUNNER = ROOT / "tools" / "recomp" / "Run-HiddenRetailManual.ps1"
ROUTE_RUNNER = ROOT / "tools" / "recomp" / "Run-HiddenRetailRoute.ps1"
PGRAPH = ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c"


def test_present_flash_detector_is_opt_in_and_bounded():
    text = DEVICE.read_text(encoding="utf-8")
    assert '"MERCENARIES_TRACE_PRESENT_FLASH"' in text
    assert '"MERCENARIES_TRACE_PRESENT_FLASH_GATE_FILE"' in text
    assert '"MERCENARIES_CAPTURE_PRESENT_FLASH_PREFIX"' in text
    assert "coherent * 100u >= PRESENT_FLASH_SAMPLE_COUNT * 72u" in text
    assert "capture_limit > 64u" in text
    assert "[PRESENT-FLASH]" in text


def test_hidden_runner_exposes_present_flash_capture():
    text = RUNNER.read_text(encoding="utf-8-sig")
    assert "[switch]$TracePresentFlash" in text
    assert "$settings.MERCENARIES_TRACE_PRESENT_FLASH = '1'" in text
    assert "present-flash.gate" in text
    assert "$PresentFlashCaptureCount" in text


def test_scanout_surface_trace_is_opt_in_and_timestamped():
    device = DEVICE.read_text(encoding="utf-8")
    pgraph = PGRAPH.read_text(encoding="utf-8")
    runner = RUNNER.read_text(encoding="utf-8-sig")
    route_runner = ROUTE_RUNNER.read_text(encoding="utf-8-sig")
    assert '"[PRESENT-FLASH] tick=%llu elapsed=%llu' in device
    assert '"MERCENARIES_TRACE_SCANOUT_SURFACE"' in pgraph
    assert '"[PGRAPH-SCANOUT] tick=%llu' in pgraph
    assert 'MERCENARIES_TRACE_SCANOUT_GUEST_DWORD' in pgraph
    assert 'extern ptrdiff_t g_xbox_mem_offset;' in pgraph
    assert '$ScanoutGuestDword' in runner
    assert '$ScanoutGuestDword' in route_runner
    assert 'MERCENARIES_TRACE_SCANOUT_GUEST_DWORD' in route_runner
    assert "$settings.MERCENARIES_TRACE_SCANOUT_SURFACE = '1'" in route_runner
    assert "[switch]$TraceScanoutSurface" in runner
    assert "$settings.MERCENARIES_TRACE_SCANOUT_SURFACE = '1'" in runner
    assert "[switch]$TraceFlips" in runner
    assert "$settings.MERCENARIES_TRACE_FLIPS = '1'" in runner


def test_fullscreen_immediate_trace_is_opt_in_bounded_and_route_gated():
    pgraph = PGRAPH.read_text(encoding="utf-8")
    route_runner = ROUTE_RUNNER.read_text(encoding="utf-8-sig")
    assert '"MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE"' in pgraph
    assert '"MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE_GATE_FILE"' in pgraph
    assert "fullscreen_trace_count < 64u" in pgraph
    assert "[PGRAPH-FULLSCREEN-IMM]" in pgraph
    assert "[PGRAPH-FULLSCREEN-ARRAY]" in pgraph
    assert "[PGRAPH-DIMMER-ARRAY]" in pgraph
    assert "[PGRAPH-COLORED-OVERLAY]" in pgraph
    assert "colored_overlay_trace_count < 64u" in pgraph
    assert "count == 4u && stride == 20u && g_pg.blend_enable" in pgraph
    assert "(red != green || green != blue)" in pgraph
    assert "read_le32(vertex_bytes + 8u) == 0x20000000u" in pgraph
    assert "0.8f *" in pgraph
    assert "[switch]$TraceFullscreenImmediate" in route_runner
    assert "$settings.MERCENARIES_TRACE_FULLSCREEN_IMMEDIATE = '1'" in route_runner
    assert "two-clubs-support.bmp" in route_runner
    assert "MERCENARIES_TEST_SURVIVOR_FLASH_FILE" in route_runner
    assert "survivor-flash.trigger" in route_runner
    assert "MERCENARIES_CAPTURE_COLORED_OVERLAY_PREFIX" in route_runner
    assert "[PGRAPH-COLORED-OVERLAY-CAPTURE]" in pgraph
    assert 'd3d8_states_debug_trace_blend("colored-overlay-after")' in pgraph
    assert "dimmer-object color=%08X" in pgraph
    assert "MERCENARIES_TRACE_SCREEN_FLASH" in route_runner
    patcher = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
        encoding="utf-8"
    )
    assert "def trace_screen_flash_setter(text: str) -> str:" in patcher
    assert "recomp_screen_flash_checkpoint(ecx" in patcher
    assert "def trace_survivor_state(text: str) -> str:" in patcher
    assert "recomp_survivor_state_checkpoint(3u" in patcher
    assert "legacy_raw_checkpoint" in patcher
    trace_survivor = runpy.run_path(
        str(ROOT / "ports/mercenaries/scripts/Patch-Generated.py")
    )["trace_survivor_state"]
    generated = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0002.c").read_text(
        encoding="utf-8"
    )
    traced = trace_survivor(generated)
    assert "recomp_survivor_state_checkpoint(0u" not in traced
    assert trace_survivor(traced) == traced
    manual = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
        encoding="utf-8"
    )
    assert "static void recomp_apply_test_survivor_flash(void)" in manual
    assert "0x80FFFF00u" in manual
    assert "0x3FD47AE1u" in manual
    assert "[TEST-SURVIVOR-FLASH]" in manual
    assert "void recomp_screen_flash_checkpoint(" in manual
    assert "[SCREEN-FLASH-SET]" in manual
    assert "time_rate=%g" in manual
    assert "survivor_sample=%d/%u age_ms=%llu" in manual
    assert "ratio=%g last_hp=%g" in manual
    assert "static int recomp_screen_flash_trace_enabled(void)" in manual
    assert "void recomp_survivor_state_checkpoint(" in manual
    assert "[SURVIVOR-STATE]" in manual
    assert "recomp_apply_test_survivor_flash();" in manual

if __name__ == "__main__":
    test_present_flash_detector_is_opt_in_and_bounded()
    test_hidden_runner_exposes_present_flash_capture()
    test_scanout_surface_trace_is_opt_in_and_timestamped()
    test_fullscreen_immediate_trace_is_opt_in_bounded_and_route_gated()
    print("ok present_flash_detector")

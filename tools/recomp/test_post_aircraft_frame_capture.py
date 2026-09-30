"""Regression coverage for deterministic post-aircraft frame capture."""

from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[2] / "ports" / "mercenaries" /
          "src" / "recomp_manual.c").read_text(encoding="utf-8")
start = SOURCE.index(
    "if (g_test_post_aircraft_capture_deadline_ms != 0u &&")
end = SOURCE.index(
    "g_test_post_aircraft_capture_deadline_ms = 0u;", start)

block = SOURCE[start:end]
assert 'getenv("MERCENARIES_CAPTURE_POST_AIRCRAFT_PATH")' in block
assert "d3d8_DebugCaptureFrameToPath(capture_path);" in block
assert "d3d8_DebugArmFlipCapture" not in block
assert "d3d8_PresentFrame" not in block

print("ok post_aircraft_frame_capture")
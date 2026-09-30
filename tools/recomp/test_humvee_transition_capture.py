"""Regression coverage for opt-in, dynamically armed Humvee-transition capture."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEVICE = (ROOT / "src/d3d/d3d8_device.c").read_text(encoding="utf-8")
HEADER = (ROOT / "src/d3d/d3d8_internal.h").read_text(encoding="utf-8")
MANUAL = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(encoding="utf-8")
RUNNER = (ROOT / "tools/recomp/Run-HiddenRetailRoute.ps1").read_text(encoding="utf-8")


assert "void d3d8_DebugStartDisplayCapture(" in HEADER
start = DEVICE.index("void d3d8_DebugStartDisplayCapture(")
body = DEVICE[start:DEVICE.index("\n}\n", start) + 3]
assert "g_debug_display_capture_count = 0u;" in body
assert "g_debug_display_capture_next_ms = g_debug_display_capture_started_ms;" in body
assert "limit > 256u" in body

route = MANUAL[MANUAL.index("static void recomp_apply_test_aircraft_pickup_route("):
               MANUAL.index("static void recomp_trace_roadblock_actor_census(")]
assert '"MERCENARIES_CAPTURE_HUMVEE_TRANSITION_PREFIX"' in route
assert "d3d8_DebugStartDisplayCapture(" in route
assert "transition_prefix, 100u, 256u);" in route
assert "[switch]$CaptureHumveeTransition" in RUNNER
assert "MERCENARIES_CAPTURE_HUMVEE_TRANSITION_PREFIX" in RUNNER

print("Humvee transition capture regression passed")

"""Regression coverage for NV2A line-smoothing state propagation."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PGRAPH = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
STATES = (ROOT / "src/d3d/d3d8_states.c").read_text(encoding="utf-8")
INTERNAL = (ROOT / "src/d3d/d3d8_internal.h").read_text(encoding="utf-8")

assert "case NV097_SET_LINE_SMOOTH_ENABLE:" in PGRAPH
assert "g_pg.line_smooth_enable = param;" in PGRAPH
assert "d3d8_states_set_line_smooth(g_pg.line_smooth_enable != 0u);" in PGRAPH
assert "MERCENARIES_TRACE_LINE_EFFECTS_GAMEPLAY_ONLY" in PGRAPH
assert "void d3d8_states_set_line_smooth(BOOL enable)" in STATES
assert "rd.AntialiasedLineEnable = g_line_smooth_enable;" in STATES
assert "AuthenticLineHaze" not in STATES
assert "void    d3d8_states_set_line_smooth(BOOL enable);" in INTERNAL

print("nv2a line smooth regression test: PASS")

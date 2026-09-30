"""Regression coverage for opt-in multi-stage texture edge diagnostics."""

from pathlib import Path


SOURCE = Path("src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")


assert '"MERCENARIES_TRACE_TEXTURE_EDGES"' in SOURCE
assert "[PGRAPH-TEXTURE-EDGE-STAGE] stage=%u off=%08X" in SOURCE
assert "for (i = 0u; i < 4u; ++i)" in SOURCE
assert "if (!g_pg.tex[i].enabled)" in SOURCE
assert "get_guest_texture_dimensions(stage_format" in SOURCE
assert "resolved_texture_offset(i)" in SOURCE

print("NV2A texture edge multi-stage trace checks passed")

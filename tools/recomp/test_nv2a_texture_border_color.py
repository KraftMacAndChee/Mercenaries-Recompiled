from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PGRAPH = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
STATES = (ROOT / "src/d3d/d3d8_states.c").read_text(encoding="utf-8")

assert "case NV097_SET_TEXTURE_BORDER_COLOR:" in PGRAPH
assert "g_pg.tex[stage].border_color = param;" in PGRAPH
assert PGRAPH.count("D3DTSS_BORDERCOLOR") >= 2
assert "tss[D3DTSS_BORDERCOLOR] >> 16" in STATES
assert "tss[D3DTSS_BORDERCOLOR] >> 8" in STATES
assert "tss[D3DTSS_BORDERCOLOR] & 0xFFu" in STATES
assert "tss[D3DTSS_BORDERCOLOR] >> 24" in STATES

print("NV2A texture border color is tracked and converted from ARGB to RGBA")
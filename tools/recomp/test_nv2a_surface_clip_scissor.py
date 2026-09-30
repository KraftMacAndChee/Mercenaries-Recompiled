from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)

assert "surface_x = g_pg.surface_clip_h & 0xFFFFu" in SOURCE
assert "surface_y = g_pg.surface_clip_v & 0xFFFFu" in SOURCE
assert "surface_width = g_pg.surface_clip_h >> 16" in SOURCE
assert "surface_height = g_pg.surface_clip_v >> 16" in SOURCE
assert "xmax = surface_x + surface_width" in SOURCE
assert "ymax = surface_y + surface_height" in SOURCE

assert "g_pg.window_clip_type == 0u" in SOURCE
assert "if (window_xmin > xmin) xmin = window_xmin" in SOURCE
assert "if (window_xmax < xmax) xmax = window_xmax" in SOURCE
assert "if (window_ymin > ymin) ymin = window_ymin" in SOURCE
assert "if (window_ymax < ymax) ymax = window_ymax" in SOURCE
assert "((g_pg.window_clip_h[0] >> 16) & 0x0FFFu) + 1u" in SOURCE
assert "((g_pg.window_clip_v[0] >> 16) & 0x0FFFu) + 1u" in SOURCE

obsolete = """if (!g_pg.window_clip_h_valid || !g_pg.window_clip_v_valid ||
        g_pg.window_clip_type != 0u) {
        d3d8_states_set_scissor(FALSE, 0, 0, 0, 0);"""
assert obsolete not in SOURCE
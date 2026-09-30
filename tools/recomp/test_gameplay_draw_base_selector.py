#!/usr/bin/env python3
"""Regression coverage for stable address-based gameplay draw selection."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")

assert 'MERCENARIES_CAPTURE_GAMEPLAY_DRAW_EXACT_BASE' in SOURCE
assert 'vertex_base == (uint32_t)strtoul(exact_base_env, NULL, 0)' in SOURCE
assert 'MERCENARIES_CAPTURE_GAMEPLAY_DRAW_DUMP_BASE' in SOURCE
assert '(index == dump_index || vertex_base == dump_base)' in SOURCE

assert 'MERCENARIES_TRACE_GAMEPLAY_DRAWS' in SOURCE
assert '(draw_prefix || draw_trace)' in SOURCE
assert '[PGRAPH-GAMEPLAY-DRAW-TRACE]' in SOURCE
assert 'g_pg.bound_color->logical_width == 640u' in SOURCE
assert 'g_pg.bound_color->logical_height == 480u' in SOURCE

array_path = SOURCE[
    SOURCE.index("static void submit_array_draw(void)"):
    SOURCE.index("static void submit_draw(void)")
]
assert "ID3D11Texture2D *feedback_textures[4]" in array_path
assert "stage_surface == g_pg.bound_color" in array_path
assert "snapshot_bound_color_surface(" in array_path
assert 'debug_capture_gameplay_draw_after("array")' in array_path
assert "d3d8_BindExternalTexture(i, NULL);" in array_path
assert "ID3D11ShaderResourceView_Release(feedback_srvs[i]);" in array_path
assert "ID3D11Texture2D_Release(feedback_textures[i]);" in array_path

print("gameplay draw base selector regression passed")
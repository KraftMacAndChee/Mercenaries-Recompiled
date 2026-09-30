from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)

assert "const int use_flare_combiners = immediate_source_surface" in SOURCE
assert (
    "g_pg.blend_sfactor == NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA" in SOURCE
)
assert "g_pg.blend_dfactor == NV097_SET_BLEND_FUNC_DFACTOR_V_ONE" in SOURCE
gate = SOURCE.index("const int use_immediate_combiners")
assert "const int use_immediate_combiners = g_pg.combiner_valid;" in SOURCE
vertex_gate = SOURCE.index("if ((g_pg.transform_execution_mode & 3u) == 2u")
assert SOURCE.index("g_pg.immediate_vertex_count == num_verts", vertex_gate) > vertex_gate
assert "MERCENARIES_EXPERIMENT_IMMEDIATE_VSH" not in SOURCE
print("ok nv2a_flare_surface_combiner")

from pathlib import Path


SOURCE = (Path(__file__).parents[2] / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)


probe = SOURCE.index("const int use_flare_probe_vsh")
assert SOURCE.index("g_pg.transform_program_start == 64u", probe) > probe
assert SOURCE.index("g_pg.immediate_vertex_count == num_verts", probe) > probe
vertex_gate = SOURCE.index("if ((g_pg.transform_execution_mode & 3u) == 2u", probe)
assert SOURCE.index("g_pg.immediate_vertex_count == num_verts", vertex_gate) > vertex_gate
assert "MERCENARIES_EXPERIMENT_IMMEDIATE_VSH" not in SOURCE
assert "replaces Z/W with 0/1 and makes every probe visible" in SOURCE
print("ok nv2a_flare_depth_probe")

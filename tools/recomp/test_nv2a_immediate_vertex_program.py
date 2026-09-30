from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)


comment = SOURCE.index(
    "Persistent NV097 immediate attributes are object-space vertex inputs"
)
gate = SOURCE.index(
    "if ((g_pg.transform_execution_mode & 3u) == 2u", comment
)
end = SOURCE.index("/* NV2A PROJECT2D texture modes", gate)
vertex_path = SOURCE[comment:end]

assert "g_pg.immediate_vertex_count == num_verts" in vertex_path
assert "program_vertices =" in vertex_path
assert "g_pg.immediate_vertices" in vertex_path
assert "MERCENARIES_" not in vertex_path
assert "use_dot_st_combiners" not in vertex_path
assert "use_flare_combiners" not in vertex_path
assert "use_flare_probe_vsh" not in vertex_path
assert SOURCE.index("prepare_transform_program(", end) > end

print("ok nv2a_immediate_vertex_program")

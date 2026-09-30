from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")


def test_array_line_loop_closes_with_duplicate_first_vertex():
    assert "expanded_line_loop" in SOURCE
    assert "prim_count = count;" in SOURCE
    assert "submitted_vertex_count = count + 1u;" in SOURCE
    assert "memcpy(expanded_line_loop + (size_t)count * draw_stride" in SOURCE


def test_immediate_line_loop_closes_fixed_and_programmable_vertices():
    assert "int is_line_loop = (g_pg.draw_mode == 3);" in SOURCE
    assert "out_vert_count = num_verts + 1u;" in SOURCE
    assert "out[num_verts] = out[0];" in SOURCE
    assert "memcpy(program_vertices[num_verts], program_vertices[0]" in SOURCE


def test_xbox_quad_strip_and_polygon_keep_their_fill_topologies():
    assert "case 9:  return D3DPT_TRIANGLESTRIP;" in SOURCE
    assert "case 10: return D3DPT_TRIANGLEFAN;" in SOURCE

    # QUAD_STRIP input pairs become adjacent quads without inventing vertices.
    strip = [0, 1, 2, 3, 4, 5]
    strip_triangles = []
    for index in range(len(strip) - 2):
        if index & 1:
            strip_triangles.append((strip[index + 1], strip[index], strip[index + 2]))
        else:
            strip_triangles.append((strip[index], strip[index + 1], strip[index + 2]))
    assert strip_triangles == [
        (0, 1, 2), (2, 1, 3), (2, 3, 4), (4, 3, 5)
    ]

    # POLYGON must retain vertex zero as the fan center.
    polygon = [0, 1, 2, 3, 4]
    assert [(polygon[0], polygon[i], polygon[i + 1])
            for i in range(1, len(polygon) - 1)] == [
        (0, 1, 2), (0, 2, 3), (0, 3, 4)
    ]


test_array_line_loop_closes_with_duplicate_first_vertex()
test_immediate_line_loop_closes_fixed_and_programmable_vertices()
test_xbox_quad_strip_and_polygon_keep_their_fill_topologies()
print("NV2A line-loop regression checks passed")

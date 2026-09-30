from pathlib import Path


def main() -> None:
    root = Path(__file__).parents[2]
    vsh = (root / "src" / "d3d" / "d3d8_vsh.c").read_text(encoding="utf-8")
    pgraph = (root / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")

    assert "g_vsh_input_bgra_mask" in vsh
    assert "float4(input.v%d.zyxw)" in vsh
    assert "hash ^= bgra_mask" in vsh
    assert "converted_vertex_colors" not in pgraph
    assert "NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D" in pgraph
    assert "input_components, input_bgra_mask" in pgraph

    print("ok vsh_d3dcolor_bgra")


if __name__ == "__main__":
    main()
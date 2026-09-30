from pathlib import Path


def main() -> None:
    source = (
        Path(__file__).parents[2]
        / "src"
        / "nv2a"
        / "nv2a_pgraph_d3d11.c"
    ).read_text(encoding="utf-8")

    assert "post_shadow_depth_restore_window" in source
    assert "MERCENARIES_DEBUG_FORCE_DEPTH_POST_SHADOW" not in source
    assert "NV097_SET_BLEND_FUNC_SFACTOR_V_ZERO" in source
    assert "NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_ALPHA" in source
    assert "g_pg.blend_sfactor == D3DBLEND_ZERO" not in source
    assert "g_pg.blend_dfactor == D3DBLEND_SRCALPHA" not in source
    assert "g_pg.draw_mode == 6u" in source
    assert "g_pg.depth_write" in source
    assert "g_pg.depth_func == D3DCMP_LESSEQUAL" in source
    assert "host_depth_test = 1" in source

    print("ok post_shadow_depth_restore")


if __name__ == "__main__":
    main()

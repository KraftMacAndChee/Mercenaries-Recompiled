from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c"


def test_feedback_guard_uses_guest_allocation_identity() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    helper = text[text.index("static int guest_color_surface_aliases_bound") :]
    helper = helper[: helper.index("static GuestDepthSurface")]
    assert "surface->offset == bound->offset" in helper
    assert "surface->texture == bound->texture" in helper
    assert "guest_color_surface_aliases_bound(stage_surface)" in text
    assert "snapshot_bound_color_surface(\n                    g_pg.bound_color" in text
    assert "snapshot_bound_color_surface(\n                        g_pg.bound_color" in text


def test_linear_texture_dump_honors_exact_offset_filter() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    function = text[text.index("static void dump_guest_linear_bgra_texture") :]
    function = function[: function.index("static IDirect3DTexture8 *prepare_guest_texture")]
    assert 'pgraph_cached_getenv("MERCENARIES_DUMP_TEXTURE_OFFSET")' in function
    assert "offset_filter && offset !=" in function


if __name__ == "__main__":
    test_feedback_guard_uses_guest_allocation_identity()
    test_linear_texture_dump_honors_exact_offset_filter()
    print("feedback surface alias guard tests passed")

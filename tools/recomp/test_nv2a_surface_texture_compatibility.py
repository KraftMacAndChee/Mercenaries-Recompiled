from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)


def main() -> None:
    assert "guest_color_surface_texture_format_compatible" in SOURCE
    assert "guest_color_surface_texture_shape_compatible" in SOURCE
    assert SOURCE.count("get_guest_surface_texture(") == 3
    # Texture shapes describe the Xbox allocation after guest AA expansion,
    # never the host internal-resolution-scaled D3D texture.
    assert "backing_width = surface->logical_width" in SOURCE
    assert "backing_height = surface->logical_height" in SOURCE
    assert "backing_width *= 2u" in SOURCE
    assert "backing_height *= 2u" in SOURCE
    assert "backing_width != width || backing_height != height" in SOURCE
    assert "surface->resolve_source_width != width" in SOURCE
    assert "surface->resolve_source_height != height" in SOURCE
    assert "surface->pitch != pitch && surface->resolve_source_pitch != pitch" in SOURCE
    assert "guest_surface_backing_dimensions(source->logical_width" in SOURCE
    assert "target->resolve_source_pitch = source->pitch" in SOURCE
    # Partial scene/HUD draws after the AA resolve must retain the backing
    # declaration.  Only explicit replacement paths invalidate provenance.
    array_tail = SOURCE.split('debug_capture_post_pda_pending_write("array")', 1)[1]
    array_tail = array_tail.split('note_guest_surface_resolve("array")', 1)[0]
    immediate_tail = SOURCE.split(
        'debug_capture_post_pda_pending_write("immediate")', 1)[1]
    immediate_tail = immediate_tail.split(
        'note_guest_surface_resolve("immediate")', 1)[0]
    assert "resolve_source_width = 0u" not in array_tail
    assert "resolve_source_width = 0u" not in immediate_tail
    assert SOURCE.count("resolve_source_width = 0u;") >= 3
    assert "alias_color->resolve_source_width = 0u;" in SOURCE
    assert "NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE" in SOURCE
    assert "get_guest_texture_levels(texture_format, color_format, control0) > 1u" in SOURCE
    compatibility = SOURCE.split(
        "static int guest_color_surface_texture_format_compatible", 1
    )[1].split("static int guest_color_surface_texture_shape_compatible", 1)[0]
    assert "COLOR_LC_IMAGE_CR8YB8CB8YA8" not in compatibility
    assert "COLOR_LC_IMAGE_YB8CR8YA8CB8" not in compatibility
    assert "COLOR_LU_IMAGE_A8R8G8B8" in compatibility
    assert "COLOR_LU_IMAGE_X8R8G8B8" in compatibility
    print("ok nv2a_surface_texture_compatibility")


if __name__ == "__main__":
    main()

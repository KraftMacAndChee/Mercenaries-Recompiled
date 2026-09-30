#!/usr/bin/env python3
"""Regression checks for exact-value vertex constant-buffer uploads."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "d3d" / "d3d8_vsh.c").read_text(encoding="utf-8")


def test_guest_constants_only_dirty_on_exact_value_change() -> None:
    start = SOURCE.index("void d3d8_vsh_set_constant")
    end = SOURCE.index("void d3d8_vsh_set_input_layout", start)
    block = SOURCE[start:end]
    assert "memcmp(&g_vsh_constants.c[start_reg][0], data, byte_count) != 0" in block
    assert "memcpy(&g_vsh_constants.c[start_reg][0], data, byte_count);" in block
    assert block.index("memcmp(") < block.index("g_vsh_constants_dirty = TRUE;")


def test_host_constants_are_cached_as_complete_exact_snapshot() -> None:
    assert "static NV2AVSHostConstants g_vsh_host_constants;" in SOURCE
    assert "static BOOL g_vsh_host_constants_valid = FALSE;" in SOURCE
    start = SOURCE.index("BOOL d3d8_vsh_prepare_draw")
    block = SOURCE[start:]
    compare = block.index("memcmp(&host_constants, &g_vsh_host_constants,")
    mapping = block.index("ID3D11DeviceContext_Map", compare)
    cache = block.index("g_vsh_host_constants = host_constants;", mapping)
    valid = block.index("g_vsh_host_constants_valid = TRUE;", cache)
    assert compare < mapping < cache < valid


def test_inline_and_surface_changes_invalidate_or_compare_host_snapshot() -> None:
    inline_start = SOURCE.index("void d3d8_vsh_set_inline_values")
    inline_end = SOURCE.index("BOOL d3d8_vsh_is_programmable", inline_start)
    inline = SOURCE[inline_start:inline_end]
    assert "memcmp(g_vsh_inline_values, values," in inline
    assert "g_vsh_host_constants_valid = FALSE;" in inline
    assert "host_constants.surface_width = (float)d3d8_GetRenderTargetWidth();" in SOURCE
    assert "host_constants.surface_height = (float)d3d8_GetRenderTargetHeight();" in SOURCE


def test_buffer_lifetime_invalidates_host_snapshot() -> None:
    init = SOURCE[SOURCE.index("HRESULT d3d8_vsh_init"):
                  SOURCE.index("void d3d8_vsh_shutdown")]
    shutdown = SOURCE[SOURCE.index("void d3d8_vsh_shutdown"):
                      SOURCE.index("HRESULT d3d8_vsh_create_shader")]
    assert "g_vsh_host_constants_valid = FALSE;" in init
    assert "g_vsh_host_constants_valid = FALSE;" in shutdown


if __name__ == "__main__":
    test_guest_constants_only_dirty_on_exact_value_change()
    test_host_constants_are_cached_as_complete_exact_snapshot()
    test_inline_and_surface_changes_invalidate_or_compare_host_snapshot()
    test_buffer_lifetime_invalidates_host_snapshot()
    print("PASS: vertex shader constant uploads use exact-value caches")

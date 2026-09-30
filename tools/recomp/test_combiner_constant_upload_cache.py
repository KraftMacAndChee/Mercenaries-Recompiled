#!/usr/bin/env python3
"""Regression checks for exact-value combiner constant-buffer uploads."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "d3d" / "d3d8_combiners.c"


def test_combiner_constants_are_zero_initialized_and_compared_exactly():
    text = SOURCE.read_text(encoding="utf-8")
    assert "static NV2APSConstants g_last_constants;" in text
    assert "static BOOL g_last_constants_valid = FALSE;" in text
    assert "memset(&constants, 0, sizeof(constants));" in text
    assert "memcmp(&constants, &g_last_constants, sizeof(constants)) != 0" in text


def test_upload_cache_only_advances_after_a_successful_map():
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("constants_changed = !g_last_constants_valid ||")
    end = text.index("/* Bind the constant buffer to PS slot 0 */", start)
    block = text[start:end]
    success = block.index("if (SUCCEEDED(hr))")
    cache_write = block.index("g_last_constants = constants;")
    cache_valid = block.index("g_last_constants_valid = TRUE;")
    assert success < cache_write < cache_valid
    assert "memcpy(mapped.pData, &constants, sizeof(constants));" in block


def test_upload_cache_is_invalidated_for_each_buffer_lifetime():
    text = SOURCE.read_text(encoding="utf-8")
    init = text[text.index("HRESULT d3d8_combiners_init"):
                text.index("void d3d8_combiners_shutdown")]
    shutdown = text[text.index("void d3d8_combiners_shutdown"):
                    text.index("void d3d8_combiners_set_pixel_shader")]
    assert "g_last_constants_valid = FALSE;" in init
    assert "g_last_constants_valid = FALSE;" in shutdown


def test_cache_effectiveness_trace_is_opt_in_and_low_volume():
    text = SOURCE.read_text(encoding="utf-8")
    assert 'getenv("MERCENARIES_TRACE_D3D_DRAW_PERF")' in text
    assert '"[D3D-COMBINER-CB] prepares=%llu uploads=%llu "' in text
    assert "now - g_constant_trace_start_ms >= 1000u" in text


if __name__ == "__main__":
    test_combiner_constants_are_zero_initialized_and_compared_exactly()
    test_upload_cache_only_advances_after_a_successful_map()
    test_upload_cache_is_invalidated_for_each_buffer_lifetime()
    test_cache_effectiveness_trace_is_opt_in_and_low_volume()
    print("combiner constant upload cache regression checks passed")

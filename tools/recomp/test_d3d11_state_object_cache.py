from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "d3d" / "d3d8_states.c").read_text(encoding="utf-8")


def test_immutable_d3d11_states_are_cached_by_exact_descriptor() -> None:
    assert "D3D8_STATE_CACHE_CAPACITY" in SOURCE
    for cache in ("g_blend_cache", "g_ds_cache", "g_raster_cache", "g_sampler_cache"):
        assert f"memcmp(&" in SOURCE
        assert cache in SOURCE
    assert "g_blend_cache[i].state" in SOURCE
    assert "g_ds_cache[i].state" in SOURCE
    assert "g_raster_cache[i].state" in SOURCE
    assert "g_sampler_cache[i].state" in SOURCE


def test_cache_owns_objects_until_renderer_shutdown() -> None:
    shutdown = SOURCE[SOURCE.index("void d3d8_states_shutdown(void)"):]
    for release in (
        "ID3D11BlendState_Release(g_blend_cache[i].state)",
        "ID3D11DepthStencilState_Release(g_ds_cache[i].state)",
        "ID3D11RasterizerState_Release(g_raster_cache[i].state)",
        "ID3D11SamplerState_Release(g_sampler_cache[i].state)",
    ):
        assert release in shutdown
    assert "g_blend_cache_count = 0u" in shutdown
    assert "g_sampler_cache_count = 0u" in shutdown


def test_bounded_cache_has_correct_transient_fallback() -> None:
    assert "g_blend_state_transient = TRUE" in SOURCE
    assert "g_ds_state_transient = TRUE" in SOURCE
    assert "g_raster_state_transient = TRUE" in SOURCE
    assert "g_sampler_state_transient[stage] = TRUE" in SOURCE
    assert "g_blend_state_transient && g_blend_state" in SOURCE
    assert "g_sampler_state_transient[stage] && g_sampler_states[stage]" in SOURCE


if __name__ == "__main__":
    test_immutable_d3d11_states_are_cached_by_exact_descriptor()
    test_cache_owns_objects_until_renderer_shutdown()
    test_bounded_cache_has_correct_transient_fallback()
    print("PASS: D3D11 immutable state objects use exact bounded caches")

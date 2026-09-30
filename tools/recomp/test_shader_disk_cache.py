from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CACHE = (ROOT / "src" / "d3d" / "d3d8_shader_cache.c").read_text(
    encoding="utf-8"
)
COMBINERS = (ROOT / "src" / "d3d" / "d3d8_combiners.c").read_text(
    encoding="utf-8"
)
VSH = (ROOT / "src" / "d3d" / "d3d8_vsh.c").read_text(encoding="utf-8")


def test_cache_key_tracks_exact_translator_output_and_profile() -> None:
    assert "for (i = 0; i < source_size; ++i)" in CACHE
    assert "while (*profile != '\\0')" in CACHE
    assert "hash ^= SHADER_CACHE_VERSION" in CACHE
    assert 'd3d8_shader_source_hash(hlsl, (size_t)len, "ps_5_0")' in COMBINERS
    assert 'd3d8_shader_source_hash(hlsl_buf, (size_t)hlsl_len,' in VSH
    assert '"vs_5_0"' in VSH


def test_cache_file_is_validated_and_bad_bytecode_is_invalidated() -> None:
    assert "header.magic != SHADER_CACHE_MAGIC" in CACHE
    assert "header.version != SHADER_CACHE_VERSION" in CACHE
    assert "header.source_hash != source_hash" in CACHE
    assert "fgetc(file) != EOF" in CACHE
    assert "MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH" in CACHE
    assert 'd3d8_shader_cache_invalidate("ps", source_hash)' in COMBINERS
    assert 'd3d8_shader_cache_invalidate("vs", source_hash)' in VSH


def test_dynamic_constants_are_not_persisted() -> None:
    assert "ID3DBlob" in CACHE
    assert "NV2ACombinerConstants" not in CACHE
    assert "constants" not in CACHE.lower()


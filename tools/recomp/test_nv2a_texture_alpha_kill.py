from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_texture_alpha_kill_reaches_both_pixel_shader_paths() -> None:
    pgraph = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
    shaders = (ROOT / "src" / "d3d" / "d3d8_shaders.c").read_text(encoding="utf-8")
    combiners = (ROOT / "src" / "d3d" / "d3d8_combiners.c").read_text(encoding="utf-8")
    combiner_header = (ROOT / "src" / "d3d" / "d3d8_combiners.h").read_text(encoding="utf-8")

    assert "D3DTSS_ALPHAKILL" in pgraph
    assert "NV_PGRAPH_TEXCTL0_0_ALPHAKILLEN" in pgraph
    assert "PSFlags & (8u << i)" in shaders
    assert shaders.count("pc->ps_flags |= 8u << stage;") == 2
    assert "alpha_kill_mask" in combiner_header
    assert "r_t%d.a == 0.0) discard" in combiners
    assert "tss[D3DTSS_ALPHAKILL]" in combiners


def test_alpha_test_uses_nv2a_byte_precision() -> None:
    shaders = (ROOT / "src" / "d3d" / "d3d8_shaders.c").read_text(encoding="utf-8")

    # Xemu rounds final combiner alpha to an 8-bit integer before applying
    # NV097_SET_ALPHA_FUNC.  Floating-point comparison changes coverage at
    # filtered foliage edges and produces visible halos.
    assert '"    uint   AlphaRef;\\n"' in shaders
    assert 'fragAlpha = (uint)round(saturate(current.a) * 255.0)' in shaders
    assert shaders.count("pc->alpha_ref = rs[D3DRS_ALPHAREF] & 0xFFu;") == 2
    assert "current.a < AlphaRef" not in shaders

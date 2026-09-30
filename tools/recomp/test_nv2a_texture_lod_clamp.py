from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_nv2a_minimum_mip_level_reaches_d3d11_sampler() -> None:
    pgraph = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
    states = (ROOT / "src" / "d3d" / "d3d8_states.c").read_text(encoding="utf-8")

    assert "uint32_t filter, uint32_t control0" in pgraph
    assert "NV097_SET_TEXTURE_CONTROL0_MIN_LOD_CLAMP" in pgraph
    assert ">> 18" in pgraph
    assert "D3DTSS_MAXMIPLEVEL" in pgraph
    assert pgraph.count("g_pg.tex[i].control0") >= 2
    assert "sd.MinLOD = (FLOAT)tss[D3DTSS_MAXMIPLEVEL];" in states

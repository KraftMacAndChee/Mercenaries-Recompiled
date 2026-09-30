from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]


def test_nv2a_dot_mapping_reaches_generated_hlsl():
    header = (ROOT / "src/d3d/d3d8_combiners.h").read_text(encoding="utf-8")
    source = (ROOT / "src/d3d/d3d8_combiners.c").read_text(encoding="utf-8")
    pgraph = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")

    assert "int dot_map[NV2A_MAX_TEXTURES];" in header
    assert "DWORD dot_mapping, DWORD other_stage_input" in header
    assert "DWORD dot_mapping = rs[D3DRS_PSDOTMAPPING];" in source
    assert "state->dot_map[3] = (dot_mapping >> 8) & 0xf;" in source
    calls = re.findall(
        r"d3d8_combiners_from_nv2a_registers\((.*?)\);", pgraph, re.DOTALL
    )
    # Diagnostic conversion sites may grow; every site must forward the mapping.
    assert calls, "No NV2A conversion sites found"
    assert all("g_pg.shader_dot_mapping" in call for call in calls)

    # All defined NV2A mappings must be represented, and both dot texture
    # modes must consume the mapped vector rather than the raw sampled RGB.
    assert "MINUS1_TO_1_D3D" in source
    assert "MINUS1_TO_1_GL" in source
    assert "case 3: /* MINUS1_TO_1 */" in source
    assert "case 4: /* HILO_1 */" in source
    assert "float dot%d = dot(input.tc%d.xyz, dotmap%d);" in source
    assert "float dot%d = dot(input.tc%d.xyz, r_t%d.rgb);" not in source


if __name__ == "__main__":
    test_nv2a_dot_mapping_reaches_generated_hlsl()
    print("combiner dot mapping: ok")

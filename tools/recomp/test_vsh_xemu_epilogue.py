from pathlib import Path


def main() -> None:
    source = (Path(__file__).parents[2] / "src" / "d3d" / "d3d8_vsh.c").read_text()

    assert '"float4 nv2a_nan_to_one(float4 v) {\\n"' in source
    assert '"    o.oD0  = %s;\\n"' in source
    assert '"saturate(nv2a_nan_to_one(oD0))"' in source
    assert '"    o.oD1  = saturate(nv2a_nan_to_one(oD1));\\n"' in source
    assert '"    o.oB0  = saturate(nv2a_nan_to_one(oB0));\\n"' in source
    assert '"    o.oB1  = saturate(nv2a_nan_to_one(oB1));\\n"' in source
    for register in ("oT0", "oT1", "oT2", "oT3"):
        assert f'"    float4 {register}  = float4(0,0,0,1);\\n"' in source
    assert '"    float4 oPts = float4(0,0,0,1);\\n"' in source

    for register in ("oB0", "oB1"):
        assert f'"    float4 {register}  = float4(0,0,0,1);\\n"' in source
    assert '"float4 nv2a_mul(float4 a, float4 b) {\\n"' in source
    assert 'sb_append(&expr, "nv2a_mul(");' in source
    assert 'sb_append(&expr, "(nv2a_mul(");' in source
    assert '"    oPos.xy = trunc(oPos.xy * 16.0) / 16.0;\\n"' in source
    assert '"    if (abs(oPos.x + 0.5) < 0.03125) oPos.x = 0.0;\\n"' in source

    print("ok vsh_xemu_epilogue")


if __name__ == "__main__":
    main()

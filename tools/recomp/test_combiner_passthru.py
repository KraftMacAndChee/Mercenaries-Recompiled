from pathlib import Path


def main() -> None:
    root = Path(__file__).parents[2]
    header = (root / "src" / "d3d" / "d3d8_combiners.h").read_text()
    source = (root / "src" / "d3d" / "d3d8_combiners.c").read_text()

    assert "NV2A_TEXMODE_PASSTHRU   = 0x04" in header
    assert "state->tex_mode[i] != NV2A_TEXMODE_PASSTHRU" in source
    assert "state->tex_mode[i] == NV2A_TEXMODE_PASSTHRU" in source
    assert 'EMIT("    float4 r_t%d = input.tc%d;\\n", i, i);' in source

    print("ok combiner_passthru")


if __name__ == "__main__":
    main()

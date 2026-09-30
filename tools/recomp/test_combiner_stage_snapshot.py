from pathlib import Path


def main() -> None:
    root = Path(__file__).parents[2]
    source = (root / "src" / "d3d" / "d3d8_combiners.c").read_text()

    assert "stage_input_reg_name" in source
    for register in ("v0", "v1", "t0", "t1", "t2", "t3", "r0", "r1"):
        assert f'float4 stage_{register} = r_{register};' in source
    assert "stage_idx >= 0 ? stage_input_reg_name(input->reg)" in source

    print("ok combiner_stage_snapshot")


if __name__ == "__main__":
    main()
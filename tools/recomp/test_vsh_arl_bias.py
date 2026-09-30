from pathlib import Path


def main() -> None:
    source = (
        Path(__file__).parents[2] / "src" / "d3d" / "d3d8_vsh.c"
    ).read_text()

    # Xemu adds this bias before ARL floor because normalized vertex inputs
    # can reconstruct an intended integral bone index just below the integer.
    assert 'sb_append(sb, ".x + 0.001);\\n");' in source
    assert 'sb_append(sb, ".x);\\n");' not in source
    assert "hardware-compatible" in source

    print("ok vsh_arl_bias")


if __name__ == "__main__":
    main()
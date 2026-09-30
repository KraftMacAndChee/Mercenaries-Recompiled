"""Verify every immediate draw honors valid NV2A register-combiner state."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def main() -> None:
    source = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(
        encoding="utf-8"
    )
    selection = "const int use_immediate_combiners = g_pg.combiner_valid;"
    assert source.count(selection) == 1
    assert "MERCENARIES_EXPERIMENT_IMMEDIATE_COMBINERS" not in source

    selection_at = source.index(selection)
    conversion_at = source.index(
        "d3d8_combiners_from_nv2a_registers(", selection_at
    )
    draw_at = source.index("DrawPrimitiveUP", conversion_at)
    assert selection_at < conversion_at < draw_at
    print("ok nv2a_immediate_combiner_selection")


if __name__ == "__main__":
    main()
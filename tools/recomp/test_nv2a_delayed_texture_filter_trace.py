from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")

def test_texture_filter_trace_can_start_after_draw():
    assert "MERCENARIES_TRACE_TEXTURE_FILTERS_AFTER_DRAW" in SRC
    assert "g_pg.stats.draw_calls >= trace_after_draw" in SRC
    for field in ("off=%08X", "addr=%08X", "rect=%08X"):
        assert field in SRC

if __name__ == "__main__":
    test_texture_filter_trace_can_start_after_draw()
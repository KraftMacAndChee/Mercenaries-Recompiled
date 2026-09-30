from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)


def main() -> None:
    assert "MERCENARIES_TRACE_MOVIE_PGRAPH" in SOURCE
    assert "[PGRAPH-MOVIE-METHOD]" in SOURCE
    assert "[PGRAPH-MOVIE-DRAW]" in SOURCE
    assert "pgraph_texture_format_is_packed_yuv" in SOURCE
    assert "trace_count >= 256u" in SOURCE
    assert "movie_draw_trace_count++ < 128u" in SOURCE
    print("ok nv2a_movie_pgraph_trace")


if __name__ == "__main__":
    main()

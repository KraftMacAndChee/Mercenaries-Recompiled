from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c"


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static uint64_t hash_guest_rows64")
    end = source.index("static void dump_locked_bgra_texture", start)
    block = source[start:end]

    assert "x + sizeof(uint64_t) <= row_bytes" in block
    assert "memcpy(&word, row + x, sizeof(word));" in block
    assert "for (; x < row_bytes; ++x)" in block
    assert "for (x = 0; x < row_bytes; ++x)" not in block
    print("Texture hash word-mixing test passed")


if __name__ == "__main__":
    main()

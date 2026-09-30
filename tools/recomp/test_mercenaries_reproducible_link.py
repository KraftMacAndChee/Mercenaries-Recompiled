from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "ports" / "mercenaries" / "CMakeLists.txt"


def test_release_link_is_reproducible():
    text = CMAKE.read_text(encoding="utf-8")
    assert "/Brepro" in text
    assert "add_compile_options(/utf-8)" in text


if __name__ == "__main__":
    test_release_link_is_reproducible()
    print("ok mercenaries reproducible link")
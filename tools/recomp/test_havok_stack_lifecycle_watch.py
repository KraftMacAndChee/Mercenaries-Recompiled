from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ports" / "mercenaries" / "src" / "main.c"


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    assert "g_havok_watchpoint_lifecycle_reset" in text
    assert "g_recomp_current_func == 0x001F2A90u" in text
    assert "value == 0u && old_value == floor" in text
    assert '"[HAVOK-STACK-WATCH] lifecycle memset-zero "' in text
    assert "guest register globals" in text
    assert '"[HAVOK-STACK-WATCH] reinitialized current=%08X\\n"' in text
    assert text.index("value == 0u && old_value == floor") < text.index(
        '"[HAVOK-STACK-UNDERFLOW-WATCH]'
    )
    print("Havok stack lifecycle-watch checks passed")


if __name__ == "__main__":
    main()

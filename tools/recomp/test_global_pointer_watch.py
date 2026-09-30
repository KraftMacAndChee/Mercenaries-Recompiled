from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ports" / "mercenaries" / "src" / "main.c"


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    assert "g_global_pointer_watchpoint_guest_va" in text
    assert "arm_global_pointer_watchpoint(0x00643844u);" in text
    assert 'getenv("MERCENARIES_TRACE_GLOBAL_POINTER_WATCH")' in text
    assert '"[GLOBAL-POINTER-WATCH] guest=%08X old=%08X value=%08X "' in text
    assert "value == 0xFFFFFFFFu" in text
    print("Global pointer provenance-watch checks passed")


if __name__ == "__main__":
    main()
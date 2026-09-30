from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ports" / "mercenaries" / "src" / "recomp_manual.c"


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("uint32_t recomp_xinput_get_state(")
    end = text.index("uint32_t recomp_xinput_set_state(", start)
    body = text[start:end]

    assert 'getenv("MERCENARIES_CAPTURE_INPUT_POLL_PATH")' in body
    assert 'getenv("MERCENARIES_CAPTURE_INPUT_POLL_DELAY_MS")' in body
    assert "d3d8_DebugCaptureFrameToPath(capture_path);" in body
    assert "input_poll_capture_done = 1;" in body
    assert body.index("d3d8_DebugCaptureFrameToPath(capture_path);") < body.index(
        "result = xbox_InputGetState(port, &state);"
    )
    print("Frontend input-poll capture checks passed")


if __name__ == "__main__":
    main()

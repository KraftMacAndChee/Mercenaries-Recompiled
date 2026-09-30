"""Late guest frames must never receive an additional frame-slot sleep."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "d3d" / "d3d8_device.c").read_text(encoding="utf-8")


def test_late_frames_rebase_without_waiting() -> None:
    block = SOURCE[
        SOURCE.index("void d3d8_WaitForGuestFrameSlot(void)"):
        SOURCE.index("void d3d8_PresentFrame(void)")
    ]
    late = "else if (now.QuadPart >= next_flip)"
    assert late in block
    late_block = block[block.index(late):block.index("while (now.QuadPart < next_flip)")]
    assert "next_flip = now.QuadPart + interval;" in late_block
    assert "return;" in late_block
    assert "next_flip + frequency.QuadPart / 4" not in block
    assert "SwitchToThread();" in block
    assert block.index("while (now.QuadPart < next_flip)") < block.index(
        "next_flip += interval;"
    )


def test_deadline_model_caps_fast_frames_without_penalizing_slow_frames() -> None:
    interval = 100
    deadline = 0
    now = 0
    waits: list[int] = []
    for render_time in (0, 20, 20, 140, 140, 20, 20):
        now += render_time
        if deadline == 0:
            deadline = now + interval
        elif now >= deadline:
            deadline = now + interval
            waits.append(0)
            continue
        wait = deadline - now
        now = deadline
        waits.append(wait)
        deadline += interval
    assert waits[:2] == [100, 80]
    assert waits[2:4] == [80, 0]
    assert waits[4] == 0
    assert all(wait >= 0 for wait in waits)


if __name__ == "__main__":
    test_late_frames_rebase_without_waiting()
    test_deadline_model_caps_fast_frames_without_penalizing_slow_frames()
    print("D3D8 frame pacing deadline regression passed")
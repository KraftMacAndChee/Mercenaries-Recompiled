from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_xaudio2_queue_watermark_pacing() -> None:
    header = (ROOT / "src" / "apu" / "apu_xaudio2.h").read_text(encoding="utf-8")
    backend = (ROOT / "src" / "apu" / "apu_xaudio2.c").read_text(encoding="utf-8")
    core = (ROOT / "src" / "apu" / "apu_core.c").read_text(encoding="utf-8")

    assert "int xa2_get_queued_buffers(void);" in header
    assert "int xa2_get_queue_capacity(void);" in header
    assert "g_xa2_underruns" in backend
    assert "queued_buffers >= high" in core
    assert "#define XA2_NUM_BUFS      8" in backend
    assert "queue_low_watermark = high / 2;" in core
    assert "queued_buffers <= queue_low_watermark" in core
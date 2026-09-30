from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_apu_voice_rates_are_not_ignored() -> None:
    shim = (ROOT / "src" / "apu" / "apu_shim.h").read_text(encoding="utf-8")
    vp = (ROOT / "src" / "apu" / "apu_vp.c").read_text(encoding="utf-8")

    assert "typedef struct SRC_STATE" in shim
    assert "const double step = ratio > 1.0e-9 ? 1.0 / ratio : 1.0;" in shim
    assert "state->position += step;" in shim
    assert "voice_resample_callback" in vp
    assert "src_callback_read(filter->resampler, rate" in vp
    assert "(void)rate" not in vp


def test_zero_length_segment_transition_is_retried() -> None:
    vp = (ROOT / "src" / "apu" / "apu_vp.c").read_text(encoding="utf-8")

    callback = vp.split("static long voice_resample_callback", 1)[1].split(
        "static int voice_resample", 1
    )[0]
    process = vp.split("static void voice_process", 1)[1].split(
        "void mcpx_apu_vp_frame", 1
    )[0]
    assert "if (count < 0)" in callback
    assert "if (count <= 0)" not in callback
    assert "if (count < 0) break;" in process

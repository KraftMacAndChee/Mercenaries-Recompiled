from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DSP = (ROOT / "src" / "apu" / "apu_dsp.c").read_text(encoding="utf-8")


def test_speaker_downmix_is_default_and_folds_physical_and_3d_destinations():
    assert 'getenv("MERCENARIES_DISABLE_APU_SPEAKER_DOWNMIX") == NULL' in DSP
    assert 'getenv("MERCENARIES_ENABLE_APU_SPEAKER_DOWNMIX")' not in DSP
    for index in range(10):
        assert f"mixbins[{index}][sample]" in DSP
    fallback = DSP.split("static void fallback_speaker_sample", 1)[1].split(
        "static int dsp_enabled", 1
    )[0]
    assert "hrtf_submix" not in fallback
    assert "mixbins[6][sample]" in fallback
    assert "mixbins[7][sample]" in fallback
    assert "0.70710678f * mixbins[8][sample]" in fallback
    assert "0.70710678f * mixbins[9][sample]" in fallback


def test_vertical_3d_source_survives_without_direct_center_send():
    # A vertical source may have no useful physical center send while retaining
    # strong 3D-front energy. The DSP fallback must still produce both ears.
    bins = [0.0] * 10
    bins[6] = 0.25
    bins[7] = 0.25
    left = bins[0] + 0.70710678 * bins[2] + 0.5 * bins[3] + \
        0.70710678 * bins[4] + bins[6] + 0.70710678 * bins[8]
    right = bins[1] + 0.70710678 * bins[2] + 0.5 * bins[3] + \
        0.70710678 * bins[5] + bins[7] + 0.70710678 * bins[9]
    assert left == 0.25
    assert right == 0.25


if __name__ == "__main__":
    test_speaker_downmix_is_default_and_folds_physical_and_3d_destinations()
    test_vertical_3d_source_survives_without_direct_center_send()

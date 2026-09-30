from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VP = (ROOT / "src" / "apu" / "apu_vp.c").read_text(encoding="utf-8")


def test_hrtf_trace_is_opt_in_and_observational() -> None:
    assert 'getenv("MERCENARIES_TRACE_APU_HRTF")' in VP
    assert "[APU-HRTF-TABLE]" in VP
    assert "[APU-HRTF-TARGET]" in VP
    assert "[APU-HRTF-VOICE]" in VP
    assert "now_us - last_trace_us < 1000000" in VP
    assert "previous_handle == HRTF_NULL_HANDLE" in VP
    assert "handle == HRTF_NULL_HANDLE" in VP
    target = VP.split("case NV1BA0_PIO_SET_VOICE_TAR_HRTF:", 1)[1].split(
        "case NV1BA0_PIO_SET_VOICE_TAR_VOLA:", 1
    )[0]
    assert "previous_handle != handle" in target
    assert "voice_set_mask" in target
    assert "hrtf_filter_set_target_params" in target


if __name__ == "__main__":
    test_hrtf_trace_is_opt_in_and_observational()

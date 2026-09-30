from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VP = ROOT / "src" / "apu" / "apu_vp.c"


def test_full_hrtf_is_explicit_opt_in_with_disable_override() -> None:
    body = VP.read_text(encoding="utf-8")
    assert "d->vp.hrtf_enabled" in body
    assert 'getenv("MERCENARIES_ENABLE_APU_HRTF") != NULL' in body
    assert 'getenv("MERCENARIES_TEST_DISABLE_HRTF") != NULL' in body


if __name__ == '__main__':
    test_full_hrtf_is_explicit_opt_in_with_disable_override()

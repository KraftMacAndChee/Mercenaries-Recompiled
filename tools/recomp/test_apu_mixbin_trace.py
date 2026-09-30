from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VP = (ROOT / "src" / "apu" / "apu_vp.c").read_text(encoding="utf-8")


def test_mixbin_trace_is_opt_in_and_preserves_every_mixbin():
    assert 'getenv("MERCENARIES_TRACE_APU_MIXBINS")' in VP
    assert "for (int bin = 0; bin < NUM_MIXBINS; ++bin)" in VP
    assert '"[APU-MIXBINS]' in VP


def test_voice_start_trace_includes_mixbin_routing():
    assert 'getenv("MERCENARIES_TRACE_APU_VOICE_ON")' in VP
    assert r'"vbin=%08X vol=%08X/%08X/%08X\n"' in VP
    assert "NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFFu" in VP
    assert "NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFFu" in VP
    assert "NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFFu" in VP
    assert "NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFFu" in VP


if __name__ == "__main__":
    test_mixbin_trace_is_opt_in_and_preserves_every_mixbin()
    test_voice_start_trace_includes_mixbin_routing()
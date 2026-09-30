from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VP = (ROOT / 'src' / 'apu' / 'apu_vp.c').read_text(encoding='utf-8')

def test_vp_monitor_excludes_multipass_source_bin():
    assert 'static int peek_ahead_multipass_bin' in VP
    assert 'voice_list == NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_MP_TOP - 1' in VP
    assert 'if (bin[b] == mp_bin)' in VP

if __name__ == '__main__':
    test_vp_monitor_excludes_multipass_source_bin()

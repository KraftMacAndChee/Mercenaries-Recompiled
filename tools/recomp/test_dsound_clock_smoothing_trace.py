from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / 'ports' / 'mercenaries' / 'src' / 'recomp_manual.c').read_text(encoding='utf-8')


def test_dsound_clock_ab_trace_is_opt_in():
    assert 'MERCENARIES_TRACE_DSOUND_CLOCK' in SRC
    assert 'MERCENARIES_TEST_DSOUND_CLOCK_NO_FORWARD_REBASE' in SRC
    assert '[DSOUND-CLOCK]' in SRC
    assert 'guest_ahead > 0 && !suppress_forward_rebase' in SRC
    assert 'clock_rate_hz != rate_hz' in SRC
    assert '(now - clock_base_ms) * (ULONGLONG)clock_rate_hz' in SRC
    assert 'rate_hz = guest_u32(device + 0x2448u) == 0x01312D00u' in SRC
    assert 'milliseconds makes the movie clock 16.67x too fast at 60 Hz' in SRC


if __name__ == '__main__':
    test_dsound_clock_ab_trace_is_opt_in()
    print('ok dsound_clock_ab_trace')


from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / 'ports' / 'mercenaries' / 'src' / 'recomp_manual.c').read_text(encoding='utf-8')
PATCHES = (ROOT / 'ports' / 'mercenaries' / 'scripts' / 'Patch-Generated.py').read_text(encoding='utf-8')
GEN = (ROOT / 'ports' / 'mercenaries' / 'src' / 'recomp' / 'gen' / 'recomp_0012.c').read_text(encoding='utf-8')


def test_xmv_decode_timing_trace_is_opt_in_and_aggregated():
    assert 'MERCENARIES_TRACE_XMV_FRAME_DECODE_TIMING' in SRC
    assert '[XMV-DECODE-TIMING]' in SRC
    assert 'QueryPerformanceCounter(&timing_decode_start)' not in SRC
    assert 'timing_decode_start = now;' in SRC
    assert 'timing_total_ticks += elapsed;' in SRC
    assert 'XMV full-frame decoder timing entry' in PATCHES
    assert 'XMV full-frame decoder timing exit' in PATCHES
    assert 'recomp_xmv_frame_decode_checkpoint(0u, MEM32(esp + 4u));' in GEN
    assert 'recomp_xmv_frame_decode_checkpoint(1u, 0u);' in GEN


if __name__ == '__main__':
    test_xmv_decode_timing_trace_is_opt_in_and_aggregated()
    print('ok xmv_decode_timing_trace')


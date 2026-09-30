from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = (ROOT / 'src' / 'apu' / 'apu_xaudio2.c').read_text(encoding='utf-8')

def test_xaudio_queue_has_lightweight_trace_flag():
    assert 'MERCENARIES_TRACE_XAUDIO_QUEUE' in SRC
    assert '[XA2-QUEUE]' in SRC

if __name__ == '__main__':
    test_xaudio_queue_has_lightweight_trace_flag()

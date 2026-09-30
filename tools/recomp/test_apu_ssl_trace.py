from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "apu" / "apu_vp.c").read_text(encoding="utf-8")

assert 'getenv("MERCENARIES_TRACE_APU_SSL")' in SOURCE
assert '[APU-SSL] event=%s voice=%u ssl=%d seg=%d' in SOURCE
assert 'trace_apu_ssl_event("set-list"' in SOURCE
assert '"set-length" : "set-offset"' in SOURCE
assert 'trace_apu_ssl_consume_if_changed' in SOURCE
assert 'trace_apu_ssl_event("advance-list"' in SOURCE
assert 'trace_apu_ssl_event("notify"' in SOURCE
assert 'g_trace_apu_ssl_events >= 8192u' in SOURCE

assert 'getenv("MERCENARIES_TRACE_APU_PITCH")' in SOURCE
assert '[APU-PITCH] voice=%u argument=%08X pitch=%d rate=%.7g' in SOURCE
assert 'g_trace_apu_pitch_events >= 4096u' in SOURCE
assert 'g_trace_apu_pitch_last_valid[voice]' in SOURCE
assert 'g_trace_apu_pitch_last[voice] == pitch' in SOURCE
assert 'trace_apu_pitch_write(d, current_voice, argument);' in SOURCE

print("ok apu ssl and pitch trace")
"""Regression coverage for opt-in XMV scheduler timing diagnostics."""

from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[2] / "ports" / "mercenaries" /
          "src" / "recomp_manual.c").read_text(encoding="utf-8")

assert 'getenv("MERCENARIES_TRACE_XMV_SCHEDULER")' in SOURCE
assert 'const uint32_t pts = guest_u32(decoder + 0xE4u);' in SOURCE
assert 'const uint32_t rate = guest_u32(decoder + 0xBCu);' in SOURCE
assert 'const uint32_t clock = device >= 0x00010000u' in SOURCE
assert '"[XMV-SCHED] frames=%u target_delta=%llu/%u/%u ' in SOURCE
assert 'scheduler_target_large_deltas' in SOURCE
print("ok xmv_scheduler_timing_trace")
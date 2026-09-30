from pathlib import Path


RUNNER = Path("tools/recomp/Run-HiddenRetailRoute.ps1").read_text(encoding="utf-8")
MANUAL = Path("ports/mercenaries/src/recomp_manual.c").read_text(encoding="utf-8")


assert "[switch]$TraceHqAudio" in RUNNER
assert "$settings.MERCENARIES_TRACE_HQ_AUDIO = '1'" in RUNNER
assert "g_mercenaries_hq_briefing_active != 0u" in MANUAL
assert 'getenv("MERCENARIES_TRACE_HQ_AUDIO")' in MANUAL

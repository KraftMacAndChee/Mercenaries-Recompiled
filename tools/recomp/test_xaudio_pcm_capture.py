"""Regression coverage for opt-in post-mix XAudio2 PCM capture."""

from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[2] / "src" / "apu" /
          "apu_xaudio2.c").read_text(encoding="utf-8")
MANUAL_RUNNER = (Path(__file__).resolve().parent /
                 "Run-HiddenRetailManual.ps1").read_text(encoding="utf-8")

assert 'getenv("MERCENARIES_DUMP_XAUDIO_PCM_PATH")' in SOURCE
assert '_fsopen(pcm_dump_path, "wb", _SH_DENYWR)' in SOURCE
assert 'if (g_xa2_pcm_dump != NULL)' in SOURCE
assert "const size_t written = fwrite(samples," in SOURCE
assert "g_xa2_pcm_dump_bytes +=" in SOURCE
assert "fclose(g_xa2_pcm_dump);" in SOURCE
assert "MERCENARIES_DUMP_XAUDIO_PCM_PATH" not in (
    Path(__file__).resolve().parents[2] / "artifacts" / "user-preview" /
    "Run-Mercenaries-Preview.cmd").read_text(encoding="utf-8")

assert "[switch]$CapturePcm" in MANUAL_RUNNER
assert "[switch]$TraceApuHeadroom" in MANUAL_RUNNER
assert "host-output-s16le-48k-stereo.pcm" in MANUAL_RUNNER
assert "$settings.MERCENARIES_TRACE_APU_HEADROOM = '1'" in MANUAL_RUNNER

print("ok xaudio_pcm_capture")

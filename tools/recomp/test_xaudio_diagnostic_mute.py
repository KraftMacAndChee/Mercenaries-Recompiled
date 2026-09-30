from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "apu" / "apu_xaudio2.c").read_text(encoding="utf-8")

assert 'getenv("MERCENARIES_TEST_MUTE_HOST_AUDIO")' in SOURCE
assert "IXAudio2SourceVoice_SetVolume(g_xa2_source, 0.0f" in SOURCE
assert "IXAudio2SourceVoice_SubmitSourceBuffer" in SOURCE
assert "fwrite(samples" in SOURCE

print("ok xaudio diagnostic mute")

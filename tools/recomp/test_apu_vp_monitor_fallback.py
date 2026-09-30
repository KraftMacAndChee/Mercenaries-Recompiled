from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "src" / "apu" / "apu_dsp.c").read_text(encoding="utf-8")

# Match Xemu's stable default while full title DSP execution remains
# incomplete. A GP-only monitor is not a valid final speaker mix, so it must
# never become the normal path merely because GP happens to complete.
assert "MCPX_APU_DEBUG_MON_GP_OR_EP" in source
assert 'enabled = getenv("MERCENARIES_ENABLE_APU_DSP") != NULL &&' in source
assert 'getenv("MERCENARIES_DISABLE_APU_DSP") == NULL' in source
assert 'getenv("MERCENARIES_ENABLE_APU_EP_DSP") != NULL' in source
assert 'getenv("MERCENARIES_DISABLE_APU_SPEAKER_DOWNMIX") == NULL' in source
assert 'getenv("MERCENARIES_ENABLE_APU_SPEAKER_DOWNMIX")' not in source
assert "dsp_use_fallback" in source
assert "d->gp.realtime = false;" in source
assert "d->ep.realtime = false;" in source

vp_output = (ROOT / "src" / "apu" / "apu_vp.c").read_text(encoding="utf-8")
assert "if (d->monitor.point == MCPX_APU_DEBUG_MON_VP)" in vp_output
assert "d->monitor.frame_buf[off + i][0] += isamp[2 * i];" in vp_output
assert "d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];" in vp_output

print("ok apu_vp_monitor_fallback")

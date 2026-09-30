from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)
MANUAL = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
    encoding="utf-8"
)
RUNNER = (ROOT / "tools/recomp/Run-HiddenRetailRoute.ps1").read_text(
    encoding="utf-8"
)


def test_vehicle_audio_probe_is_default_off_and_read_only():
    assert "def trace_vehicle_audio_update" in PATCHER
    assert "recomp_vehicle_audio_update_checkpoint(ecx);" in PATCHER
    assert 'getenv("MERCENARIES_TRACE_VEHICLE_AUDIO_STATE")' in MANUAL
    body = MANUAL.split("void recomp_vehicle_audio_update_checkpoint", 1)[1]
    body = body.split("void recomp_find_culprit_checkpoint", 1)[0]
    assert "guest_u32(effect + 0x08u)" in body
    assert "guest_u8(effect + 0x140u)" in body
    assert "guest_f32(effect + 0xB4u)" in body
    assert "guest_f32(effect + 0xCCu)" in body
    assert "guest_f32(effect + 0xD0u)" in body
    assert "guest_f32(effect + 0xD4u)" in body
    assert "guest_u32(effect + 0xD8u)" in body
    assert "engine_handle = guest_u32(effect + 0xE8u);" in body
    assert "brake_handle = guest_u32(effect + 0xECu);" in body
    assert "road_handle = guest_u32(effect + 0xE0u);" in body
    assert "accel_handle = guest_u32(effect + 0xF0u);" in body
    assert "reverse_handle = guest_u32(effect + 0xF4u);" in body
    assert "skid_handle = guest_u32(effect + 0xFCu);" in body
    assert "slide_handle = guest_u32(effect + 0x100u);" in body
    assert "MEM32(" not in body
    assert "MEM8(" not in body
    assert "[switch]$TraceVehicleAudioState" in RUNNER
    assert "$settings.MERCENARIES_TRACE_VEHICLE_AUDIO_STATE = '1'" in RUNNER
    assert "$settings.MERCENARIES_TRACE_XACT_PLAY = '1'" in RUNNER


if __name__ == "__main__":
    test_vehicle_audio_probe_is_default_off_and_read_only()
    print("Vehicle audio state trace checks passed")
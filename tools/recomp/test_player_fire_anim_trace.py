"""Keep the opt-in player firing animation trace at the retail aim site."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANUAL = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(encoding="utf-8")
HEADER = (ROOT / "ports/mercenaries/src/recomp/recomp_types.h").read_text(encoding="utf-8")
GENERATED = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0001.c").read_text(encoding="utf-8")
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(encoding="utf-8")

start = GENERATED.index("void sub_000512F0(void)")
end = GENERATED.index("void sub_00051D30(void)", start)
function = GENERATED[start:end]

for source in (MANUAL, HEADER, function):
    assert "recomp_human_fire_anim_checkpoint" in source
    assert "recomp_human_fire_play_checkpoint" in source
assert 'getenv("MERCENARIES_TRACE_PLAYER_FIRE_ANIM")' in MANUAL
assert "shoot_state == 0u" in MANUAL
assert "guest_u32(human) != 0x002E32B8u" in MANUAL
assert "MEM32(ebx + 0x6C4)" in function
assert "_human_fire_play_handle" in function
assert "_human_fire_play_params" in function
assert '_human_fire_play_params = MEM32(esp + 4);' in function
assert '"[PLAYER-FIRE-PLAY]' in MANUAL
assert '"player firing vertical-aim diagnostic"' in PATCHER
assert '"player firing upper-body Play diagnostic"' in PATCHER
assert "recomp_human_weapon_update_checkpoint" in MANUAL
assert "recomp_human_weapon_update_checkpoint" in HEADER
weapon_start = GENERATED.index("void sub_0004DDB0(void)")
weapon_end = GENERATED.index("void sub_0004E490(void)", weapon_start)
weapon_function = GENERATED[weapon_start:weapon_end]
for stage in range(10, 18):
    assert f"recomp_human_weapon_update_checkpoint({stage}u" in weapon_function
assert '"player weapon fire-event diagnostic"' in PATCHER
assert '"player weapon use result diagnostic"' in PATCHER
assert "const uint32_t _weapon_use_esi = esi;" in weapon_function
assert "const uint32_t _weapon_use_esp = g_esp;" in weapon_function
assert "0x0004E2E2u" in weapon_function
assert "0x0004E43Du" in weapon_function
assert "esi = _weapon_use_esi" in weapon_function
assert "esp = _weapon_use_esp" in weapon_function
assert '"player must-aim weapon Use nonvolatile guard"' in PATCHER
assert '"player simple weapon Use nonvolatile guard"' in PATCHER

print("player firing animation trace: ok")
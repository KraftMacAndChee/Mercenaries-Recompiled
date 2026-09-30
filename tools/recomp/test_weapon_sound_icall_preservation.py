"""Keep the retail weapon Fire cue lookup ABI-correct."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GENERATED = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0006.c").read_text(
    encoding="utf-8")
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8")

start = GENERATED.index("void sub_00150C30(void)")
end = GENERATED.index("void sub_00150CA0(void)", start)
function = GENERATED[start:end]

assert "PUSH32(esp, 0x8EAB16D9u);" in function
assert "const uint32_t _weapon_fire_esi = esi;" in function
assert "const uint32_t _weapon_fire_edi = edi;" in function
assert "recomp_nonvolatile_icall_checkpoint(" in function
assert "0x00150C84u" in function
assert "esi = _weapon_fire_esi;" in function
assert "edi = _weapon_fire_edi;" in function
assert "_icall_esp - 8u, g_esp" in function
assert "esp = _icall_esp - 8u;" in function
assert '"weapon fire cue lookup nonvolatile guard"' in PATCHER
assert "const uint32_t _weapon_play_esi = esi;" in function
assert "0x00150C8Eu" in function
assert "_weapon_play_esp - 8u, g_esp" in function
assert "recomp_weapon_fire_sound_checkpoint(" in function
assert '"weapon PlayManaged call nonvolatile guard and trace"' in PATCHER

print("weapon Fire cue nonvolatile preservation: ok")
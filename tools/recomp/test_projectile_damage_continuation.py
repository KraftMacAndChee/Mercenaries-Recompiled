from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GENERATED = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0000.c").read_text(
    encoding="utf-8"
)
start = GENERATED.index("void sub_0001FBB0(void)")
end = GENERATED.index("\n/**", start + 1)
body = GENERATED[start:end]

assert "/* retail projectile continuation repair */" in body
assert "goto loc_00020235;" in body
assert "loc_00020235: ;" in body
assert "recomp_bullet_hit_checkpoint(1u" in body
assert "recomp_bullet_damage_result_checkpoint(esi, edi, (float)fp_top())" in body
assert "MEM32(eax + 0xA8)" in body
assert "sub_00020235();" not in body

PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)
assert "retail projectile continuation repair" in PATCHER
assert "recomp_bullet_damage_result_checkpoint(esi, edi, (float)fp_top())" in PATCHER

print("retail projectile actor hits retain the ApplyModifiedDamage continuation")
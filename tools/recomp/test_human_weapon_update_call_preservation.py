"""Keep the retail human weapon-update call ABI-correct."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GENERATED = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_0001.c").read_text(encoding="utf-8")
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(encoding="utf-8")

start = GENERATED.index("void sub_000591B0(void)")
end = GENERATED.index("void sub_000593A0(void)", start)
function = GENERATED[start:end]

assert "const uint32_t _human_update_esi = esi;" in function
assert "const uint32_t _human_update_esp = g_esp;" in function
assert "PUSH32(esp, 0); sub_0004DDB0();" in function
assert "0x0005921Du" in function
assert "esi = _human_update_esi;" in function
assert "esp = _human_update_esp;" in function
assert '"retail human weapon update call nonvolatile guard"' in PATCHER

print("human weapon-update call preservation: ok")
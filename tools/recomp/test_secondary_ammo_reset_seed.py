import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports/mercenaries"


seeds = json.loads((PORT / "manual-seeds.json").read_text(encoding="utf-8"))
matches = [seed for seed in seeds if seed.get("start") == "0x00101AA0"]
assert len(matches) == 1, matches
assert "RsHudSecondaryAmmo::ResetTime" in matches[0].get("reason", "")

generated = "\n".join(
    path.read_text(encoding="utf-8")
    for path in sorted((PORT / "src/recomp/gen").glob("*.c"))
)
start = generated.index("void sub_00101AA0(void)")
end = generated.index("\n}\n", start) + len("\n}\n")
body = generated[start:end]

assert "RECOMP_TRACE_FUNC(0x00101AA0u);" in body
assert body.count("sub_000F2950();") == 2
assert "MEM32(ecx + 0xC) = 0;" in body
assert "MEM32(ecx + 0x10) = 0x80808080u;" in body
assert "sub_000F2A50(); return; /* tail jmp 0x000F2A50 */" in body
assert "0x00101AA0: not detected; minimal guest ret" not in generated

print("ok secondary_ammo_reset_seed")
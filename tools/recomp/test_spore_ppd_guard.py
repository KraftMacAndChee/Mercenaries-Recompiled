from pathlib import Path

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(encoding="utf-8")
HEADER = (ROOT / "ports/mercenaries/src/recomp/recomp_types.h").read_text(encoding="utf-8")
MANUAL = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(encoding="utf-8")
GENERATED = generated_text_containing(
    "recomp_spore_ppd_is_valid(0x001ECD0Eu, esi, ecx, edi)"
)
GENERATED_LOOKUP = generated_text_containing(
    "recomp_spore_ppd_is_valid(0x001EC900u, 0u, ecx, edx)"
)

assert '"Invalid Spore permanent-property pointer returns not found"' in PATCHER
assert "int recomp_spore_ppd_is_valid(uint32_t site, uint32_t spore" in HEADER
assert "[SPORE-PPD-INVALID]" in MANUAL
assert "ppd >= 0x00010000u && ppd <= 0x03FFFFF0u" in MANUAL
assert "recomp_spore_ppd_is_valid(0x001ECD0Eu, esi, ecx, edi)" in GENERATED
assert GENERATED.count("recomp_spore_ppd_is_valid(0x001ECD0Eu, esi, ecx, edi)") == 1
assert '"Invalid permanent-property lookup root returns not found"' in PATCHER
assert "recomp_spore_ppd_is_valid(0x001EC900u, 0u, ecx, edx)" in GENERATED_LOOKUP
assert "MEM8(eax) = 0u;" in GENERATED_LOOKUP
assert "esp += 12; return; /* ret 8 */" in GENERATED_LOOKUP
print("Spore permanent-property validity guard tests passed")

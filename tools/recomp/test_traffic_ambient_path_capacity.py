from pathlib import Path

from generated_test_utils import generated_text_containing


ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)
GENERATED = generated_text_containing("MEM32(esi + 0x22CA4) >= 0x1Eu")

OLD_COMPARE = "CMP_GE(MEM32(esi + 0x22CA4), 0x50)"
CAPACITY_COMPARE = "MEM32(esi + 0x22CA4) >= 0x1Eu"
SIGNED_COMPARE = "CMP_GE(MEM32(esi + 0x22CA4), 0x1E)"

assert '"Ambient civilian path list uses its 30-entry capacity"' in PATCHER
assert "RsTrafficManager::_AmbientCivPaths has 30 entries" in PATCHER
assert CAPACITY_COMPARE in PATCHER
assert CAPACITY_COMPARE in GENERATED
assert SIGNED_COMPARE not in GENERATED
assert OLD_COMPARE not in GENERATED

# The source count lives in a uint32_t guest word.  Every value outside the
# valid 0..29 index range, including values whose high bit would make a signed
# comparison look negative, must take the capacity exit.
for count in (0, 1, 29):
    assert not (count >= 0x1E)
for count in (30, 31, 80, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF):
    assert count >= 0x1E

print("ok traffic_ambient_path_capacity")

"""Regression coverage for the XACT engine critical-section predicate."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)
GENERATED = (
    ROOT / "ports/mercenaries/src/recomp/gen/recomp_0012.c"
).read_text(encoding="utf-8")
SECONDARY = (
    ROOT / "ports/mercenaries/src/recomp/gen/recomp_0013.c"
).read_text(encoding="utf-8")

MARKER = "XACT engine critical-section predicate preserves cmp carry"
FIX = "_cf = ((uint8_t)(LO8(eax)) < (uint8_t)(LO8(ecx)));"
EXTEND = "eax = _cf ? 0xFFFFFFFF : 0; /* sbb self (CF extend) */"

assert ('"' + MARKER + '"') in PATCHER
assert FIX in PATCHER
assert FIX in GENERATED

start = GENERATED.index("void sub_0027DB23(void)")
end = GENERATED.index("void sub_0027DB45(void)", start)
lock_function = GENERATED[start:end]
assert EXTEND in lock_function
assert "RECOMP_ICALL_SAFE(MEM32(0x2DBCE8)" in lock_function

secondary_start = SECONDARY.index("void sub_0029CBB0(void)")
secondary_end = SECONDARY.index("void sub_0029CBD2(void)", secondary_start)
secondary_lock = SECONDARY[secondary_start:secondary_end]
assert "Secondary engine critical-section predicate preserves cmp carry" in PATCHER
assert FIX in secondary_lock
assert EXTEND in secondary_lock
assert "RECOMP_ICALL_SAFE(MEM32(0x2DBCE8)" in secondary_lock

print("XACT engine critical-section predicate test passed")

from pathlib import Path

from generated_test_utils import generated_text_containing


ROOT = Path(__file__).resolve().parents[2]
GENERATED = generated_text_containing("void sub_001DC1F0(void)")
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)


def function(name: str, next_name: str) -> str:
    start = GENERATED.index(f"void {name}(void)")
    end = GENERATED.index(f"void {next_name}(void)", start)
    return GENERATED[start:end]


allocation = function("sub_001DC1F0", "sub_001DC230")
assert "_cf = ((uint32_t)(edx) < (uint32_t)(eax));" in allocation
assert "eax = _cf ? 0xFFFFFFFF : 0;" in allocation

for name, next_name in (
    ("sub_001DCC50", "sub_001DCCC0"),
    ("sub_001DCCC0", "sub_001DCD30"),
):
    block = function(name, next_name)
    assert "_cf = ((uint32_t)(eax) < (uint32_t)(edx));" in block
    assert "edi = _cf ? 0xFFFFFFFF : 0;" in block
    assert "_cf = ((uint32_t)(ecx) < (uint32_t)(MEM32(esi + 8)));" in block
    assert "eax = _cf ? 0xFFFFFFFF : 0;" in block
    assert "flags set for next jcc */\n    edi = _cf" not in block
    assert "flags set for next jcc */\n    eax = _cf" not in block

assert '"RedWorld allocation result preserves cmp carry"' in PATCHER
assert '"RedWorld paired-container first-index carry (second helper)"' in PATCHER
assert '"RedWorld paired-container second-index carry (second helper)"' in PATCHER
print("RedWorld container carry tests passed")

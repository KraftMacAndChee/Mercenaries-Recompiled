#!/usr/bin/env python3
"""Guard the retail Store next-item accessor's inherited EFLAGS snapshot."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0003.c"


def main() -> None:
    source = GENERATED.read_text(encoding="utf-8")
    start = source.index("void sub_000C8DC0(void)")
    end = source.index("\nvoid sub_", start + 1)
    function = source[start:end]

    compare = function.index("/* cmp eax, esi - flags set for next jcc */", 1)
    snapshot = function.index(
        "_flags = (CMP_L(eax, esi)); /* preserve inherited cmp flags", compare
    )
    restore_edi = function.index("POP32(esp, edi);", snapshot)
    restore_esi = function.index("POP32(esp, esi);", restore_edi)
    consume = function.index("if (_flags != 0) goto loc_000C8E12", restore_esi)

    assert compare < snapshot < restore_edi < restore_esi < consume
    assert "if (CMP_L(eax, esi)) goto loc_000C8E12" not in function
    print("ok mercenaries_store_accessor_flags")


if __name__ == "__main__":
    main()
#!/usr/bin/env python3
"""Guard the active retail RsLight flag snapshot used by the checked-in build."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0007.c"


def main() -> None:
    source = GENERATED.read_text(encoding="utf-8")
    start = source.index("void sub_00177300(void)")
    end = source.index("\nvoid sub_", start + 1)
    function = source[start:end]

    compare = function.index("/* ucomiss xmm0, xmm1 - sets EFLAGS */")
    snapshot = function.index("preserve ucomiss flags for lahf", compare)
    reload_default = function.index("recomp_xmm_loadss(xmm0v, 0x2F3F14)", snapshot)
    consume = function.index("lahf from snapshotted ucomiss", reload_default)

    assert compare < snapshot < reload_default < consume
    print("ok retail_light_fade_flags")


if __name__ == "__main__":
    main()
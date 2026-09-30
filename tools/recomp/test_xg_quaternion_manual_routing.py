"""Regression checks for the retail XG quaternion manual-call routing.

Run: py -3 tools/recomp/test_xg_quaternion_manual_routing.py

PblMatrix's Xbox quaternion constructor directly calls these two XG routines.
Registering them only in the indirect-call lookup is insufficient: the lift
must omit their generated bodies so direct calls link to recomp_manual.c.
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports" / "mercenaries"
RECOMPILE = PORT / "scripts" / "Recompile.ps1"
MANUAL = PORT / "src" / "recomp_manual.c"
GENERATED = PORT / "src" / "recomp" / "gen"

ROUTINES = {
    "0x0022F9BC": "sub_0022F9BC",
    "0x0022FA9E": "sub_0022FA9E",
}


def main() -> None:
    recompile = RECOMPILE.read_text(encoding="utf-8")
    manual = MANUAL.read_text(encoding="utf-8")
    generated_sources = list(GENERATED.glob("recomp_*.c"))
    assert generated_sources, "retail generated sources are missing"
    generated_text = "\n".join(
        path.read_text(encoding="utf-8") for path in generated_sources
    )

    for address, symbol in ROUTINES.items():
        assert recompile.count(f'"--manual-function", "{address}"') == 1
        definition = rf"(?m)^void {symbol}\(void\)\s*\{{"
        assert len(re.findall(definition, manual)) == 1
        assert not re.search(definition, generated_text), (
            f"{symbol} has a generated body which would bypass the manual routine"
        )
        assert f"{symbol}();" in generated_text, (
            f"no direct retail call sites remain for {symbol}"
        )

    header = (GENERATED / "recomp_funcs.h").read_text(encoding="utf-8")
    for symbol in ROUTINES.values():
        assert f"void {symbol}(void);" in header

    normalize_start = manual.index("void sub_0022FA9E(void)")
    matrix_start = manual.index("void sub_0022F9BC(void)", normalize_start)
    normalize = manual[normalize_start:matrix_start]
    assert "length_squared >= 0.99999f" in normalize
    assert "length_squared <= 1.00001f" in normalize
    assert "length_squared > 1.0e-10f" in normalize
    assert "isfinite(length_squared)" not in normalize

    print("Retail XG quaternion direct-call routing checks passed")


if __name__ == "__main__":
    main()
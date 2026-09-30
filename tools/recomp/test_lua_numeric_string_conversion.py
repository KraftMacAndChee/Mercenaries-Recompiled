from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "ports/mercenaries/src/recomp/recomp_types.h"
MANUAL = ROOT / "ports/mercenaries/src/recomp_manual.c"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0011.c"
FORMATTER_GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0009.c"
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"


def main() -> None:
    header = HEADER.read_text(encoding="utf-8")
    manual = MANUAL.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8")
    formatter_generated = FORMATTER_GENERATED.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")

    assert "double recomp_native_strtod" in header
    assert "result = strtod(source, &end);" in manual
    assert "guest_string + (uint32_t)consumed" in manual
    assert 'getenv("MERCENARIES_TRACE_NATIVE_STRTOD")' in manual
    assert 'snprintf(destination, 32u, "%.14g", value)' in manual
    assert "recomp_native_format_lua_number" in header
    assert "recomp_native_format_lua_number(eax, MEMD(esp))" in formatter_generated
    assert "fp_push(recomp_native_strtod(MEM32(esp + 4), MEM32(esp + 8)))" in generated
    assert '"retail Lua number formatter native boundary"' in patcher
    assert '"retail CRT strtod native boundary"' in patcher
    print("Lua numeric-string conversion bridge regression checks passed")


if __name__ == "__main__":
    main()
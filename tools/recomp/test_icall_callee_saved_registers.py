"""Indirect translated calls must enforce the Xbox x86 callee-saved ABI."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADERS = (
    ROOT / "templates/runtime/recomp_types.h",
    ROOT / "ports/mercenaries/src/recomp/recomp_types.h",
)


for header in HEADERS:
    text = header.read_text(encoding="utf-8")
    for macro_name in ("RECOMP_ICALL", "RECOMP_ICALL_SAFE"):
        start = text.index(f"#define {macro_name}(")
        end = text.index("} while(0)", start)
        macro = text[start:end]
        for register in ("ebx", "esi", "edi"):
            assert f"_callee_saved_{register} = g_{register}" in macro
            assert f"g_{register} = _callee_saved_{register}" in macro

print("indirect-call callee-saved register preservation: ok")

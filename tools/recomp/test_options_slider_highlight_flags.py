'Regression coverage for the options-slider selected-row EFLAGS fix.'

from pathlib import Path
import runpy

ROOT = Path(__file__).resolve().parents[2]
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0004.c"


def test_options_slider_highlight_flags():
    patches = runpy.run_path(str(PATCHER))["PATCHES"]
    patch = next(
        item for item in patches
        if item.name == (
            "Options slider selection retains TEST BL flags across coordinate work"
        )
    )
    assert "TEST_Z(LO8(ebx), LO8(ebx))" in patch.before
    assert "_flags = (TEST_Z(LO8(ebx), LO8(ebx)))" in patch.after
    assert "if (_flags != 0) goto loc_000E954A" in patch.after

    generated = GENERATED.read_text(encoding="utf-8")
    assert patch.after in generated
    assert patch.before not in generated


if __name__ == "__main__":
    test_options_slider_highlight_flags()
    print("ok  options_slider_highlight_flags")

from pathlib import Path

from generated_test_utils import generated_text_containing


ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)
GENERATED = generated_text_containing("loc_0016B105: ;")

MARKER = "preserve first ambient intersection COMISS before xmm0 reload"
PREDICATE = "_ambient_first_intersection_nonpositive"
NATIVE_MARKER = "/* preserve comiss flags across 17 instruction(s) */"

assert MARKER in PATCHER
if NATIVE_MARKER in GENERATED:
    compare_at = GENERATED.index(NATIVE_MARKER)
    branch_at = GENERATED.index(
        "if (_flags != 0) goto loc_0016B105", compare_at
    )
    reload_at = GENERATED.index(
        "recomp_xmm_loadss(xmm0v, 0x2DC08C)", compare_at
    )
    assert compare_at < reload_at < branch_at
else:
    assert MARKER in GENERATED
    predicate_at = GENERATED.index(PREDICATE)
    reload_at = GENERATED.index(
        "recomp_xmm_loadss(xmm0v, 0x2DC08C)", predicate_at
    )
    assert predicate_at < reload_at
    assert (
        "if (_ambient_first_intersection_nonpositive) "
        "goto loc_0016B105" in GENERATED
    )

print("ok traffic_ambient_intersection_flags")
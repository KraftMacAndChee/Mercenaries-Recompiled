"""Regression coverage against premature XACT stream release."""

from pathlib import Path

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]
MANUAL = (ROOT / "ports" / "mercenaries" / "src" /
          "recomp_manual.c").read_text(encoding="utf-8")
TYPES = (ROOT / "ports" / "mercenaries" / "src" / "recomp" /
         "recomp_types.h").read_text(encoding="utf-8")
GENERATED = generated_text_containing(
    "PUSH32(esp, 0); sub_002874EF(); /* call 0x002874EF */"
)
PATCHER = (ROOT / "ports" / "mercenaries" / "scripts" /
           "Patch-Generated.py").read_text(encoding="utf-8")

for source in (MANUAL, TYPES, GENERATED, PATCHER):
    assert "recomp_xact_release_deferred_streams" not in source
assert "XACT deferred stream release after initial scheduler pass" not in PATCHER
assert "PUSH32(esp, 0); sub_002874EF(); /* call 0x002874EF */" in GENERATED

print("ok no_premature_xact_stream_release")
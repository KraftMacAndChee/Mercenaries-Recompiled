#!/usr/bin/env python3
"""Regression coverage for invalid retail SetTexture resource hardening."""

from pathlib import Path

from generated_test_utils import generated_text_containing

ROOT = Path(__file__).resolve().parents[2]
MANUAL = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(encoding="utf-8")
HEADER = (ROOT / "ports/mercenaries/src/recomp/recomp_types.h").read_text(encoding="utf-8")
GENERATED = generated_text_containing(
    "MEM32(esp + 8) = recomp_validate_texture_bind("
)
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(encoding="utf-8")

assert "uint32_t recomp_validate_texture_bind" in MANUAL
assert "uint32_t recomp_validate_texture_bind" in HEADER
assert '"[TEXTURE-BIND-INVALID]' in MANUAL
assert "texture >= 0x00010000u && texture <= 0x03FFFFE0u" in MANUAL
assert "MEM32(esp + 8) = recomp_validate_texture_bind(" in GENERATED
assert "g_recomp_current_func, MEM32(esp + 4), MEM32(esp + 8)" in GENERATED
assert '"retail SetTexture invalid guest-resource guard"' in PATCHER

print("texture bind guard: ok")

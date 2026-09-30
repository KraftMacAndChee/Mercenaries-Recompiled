from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PATCHER = (ROOT / "ports/mercenaries/scripts/Patch-Generated.py").read_text(
    encoding="utf-8"
)
MANUAL = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
    encoding="utf-8"
)
TYPES = (ROOT / "ports/mercenaries/src/recomp/recomp_types.h").read_text(
    encoding="utf-8"
)

assert "recomp_secondary_ammo_checkpoint(0u, esi, 0u);" in PATCHER
assert "recomp_secondary_ammo_checkpoint(1u, esi, 0u);" in PATCHER
assert "recomp_secondary_ammo_checkpoint(2u, esi, eax);" in PATCHER
assert 'getenv("MERCENARIES_TRACE_SECONDARY_AMMO")' in MANUAL
assert "ammo=%d max=%d" in MANUAL
assert "flash_alpha=%.9g" in MANUAL
assert "void recomp_secondary_ammo_checkpoint" in TYPES
RUNNER = (ROOT / "tools/recomp/Run-HiddenRetailManual.ps1").read_text(
    encoding="utf-8"
)
assert "[switch]$TraceSecondaryAmmo" in RUNNER
assert "MERCENARIES_TRACE_SECONDARY_AMMO" in RUNNER
print("ok secondary_ammo_trace")
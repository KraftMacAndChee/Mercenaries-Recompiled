"""Regression contract for Mercenaries retail-analysis fixed-point discovery."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RECOMPILE = ROOT / "ports" / "mercenaries" / "scripts" / "Recompile.ps1"


def main():
    source = RECOMPILE.read_text(encoding="utf-8")
    assert "[ValidateRange(2, 64)]" in source
    assert "[int]$MaxPasses = 32" in source
    assert "$inputSeedHash = (Get-FileHash" in source
    assert "$outputSeedHash = (Get-FileHash" in source
    assert "if ($outputSeedHash -eq $inputSeedHash)" in source
    assert "$functionCount -eq $previousCount" not in source
    print("ok  mercenaries_seed_fixed_point")


if __name__ == "__main__":
    main()
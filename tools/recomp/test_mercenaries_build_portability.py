"""Mercenaries build pins tool versions without pinning machine-local paths."""

from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[2] /
          "ports/mercenaries/scripts/Build.ps1").read_text(encoding="utf-8")

assert '[string]$Generator = "Visual Studio 17 2022"' in SOURCE
assert '"x64,version=10.0.26100.0"' in SOURCE
assert '"v143,version=14.44.35207,host=x64"' in SOURCE
assert "Program Files" not in SOURCE
assert "C:\\Users\\" not in SOURCE
assert '& $cmake @configureArguments' in SOURCE

print("ok mercenaries_build_portability")

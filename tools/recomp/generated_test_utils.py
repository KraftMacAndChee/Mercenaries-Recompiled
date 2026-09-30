"""Chunk-independent helpers for regression tests over disposable generated C."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GEN = ROOT / "ports/mercenaries/src/recomp/gen"


def generated_path_containing(marker: str) -> Path:
    matches = []
    for path in sorted(GEN.glob("recomp_*.c")):
        if marker in path.read_text(encoding="utf-8"):
            matches.append(path)
    if len(matches) != 1:
        raise AssertionError(
            f"expected one generated file containing {marker!r}, "
            f"found {[path.name for path in matches]}"
        )
    return matches[0]


def generated_text_containing(marker: str) -> str:
    return generated_path_containing(marker).read_text(encoding="utf-8")
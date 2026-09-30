"""The ISO-to-port source pipeline must not depend on one developer machine."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
SCANNED_ROOTS = (
    ROOT / "ports/mercenaries/scripts",
    ROOT / "tools/recomp",
    ROOT / "tools/diagnostics",
)
SOURCE_SUFFIXES = {".py", ".ps1", ".cmake", ".json", ".toml"}
SHIPPING_PIPELINE = tuple((ROOT / "ports/mercenaries/scripts").glob("*"))


def test_portability_has_no_personal_absolute_paths() -> None:
    # Assemble signatures so this regression does not flag its own source.
    signatures = (
        re.compile(re.escape("C:" + "/" + "Users" + "/"), re.IGNORECASE),
        re.compile(re.escape("C:" + "\\" + "Users" + "\\"), re.IGNORECASE),
        re.compile(re.escape("F:" + "/" + "Downloads" + "/"), re.IGNORECASE),
        re.compile(re.escape("F:" + "\\" + "Downloads" + "\\"), re.IGNORECASE),
    )
    violations = []
    for source_root in SCANNED_ROOTS:
        for path in source_root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for line_number, line in enumerate(text.splitlines(), 1):
                if any(signature.search(line) for signature in signatures):
                    violations.append(f"{path.relative_to(ROOT)}:{line_number}: {line.strip()}")
    assert not violations, "Personal absolute paths found:\n" + "\n".join(violations)


def test_iso_pipeline_has_no_external_engineering_tree_dependency() -> None:
    forbidden = ("references/xemu", "references\\xemu")
    violations = []
    for path in SHIPPING_PIPELINE:
        if not path.is_file() or path.suffix.lower() not in {".py", ".ps1"}:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for token in forbidden:
            if token.lower() in text.lower():
                violations.append(f"{path.relative_to(ROOT)}: {token}")
    assert not violations, (
        "External engineering-tree dependency found:\n" + "\n".join(violations)
    )


if __name__ == "__main__":
    test_portability_has_no_personal_absolute_paths()
    test_iso_pipeline_has_no_external_engineering_tree_dependency()
    print("portable source paths passed")
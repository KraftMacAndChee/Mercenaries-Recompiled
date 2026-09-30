"""Export maintained source as a clean GitHub-import directory and verified ZIP."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.recomp.source_package_policy import verify_source_zip

SOURCE_ROOTS = (
    "src", "include", "templates", "docs", "tools", ".github",
    "third_party/dsp56300/msvc-x64-v0.1.3",
    "third_party/dsp56300/source-v0.1.3", "third_party/lua-5.0.3",
)
ROOT_FILES = (
    "CMakeLists.txt", "LICENSE", "README.md", "CONTRIBUTING.md",
    ".gitignore", ".gitattributes", "ARCHITECTURE.md", "CODE_MAP.md",
    "SUBSYSTEMS.md", "DATA_FLOW.md", "COMPATIBILITY.md", "WORKAROUNDS.md",
    "MAINTENANCE.md", "ARCHITECTURE_EVIDENCE.md", "DEV_HISTORY_ASTRA.md",
    "DEV_HISTORY_SOL.md", "DOCUMENTATION_AUDIT.md",
)
EXCLUDED = (
    "third_party/dsp56300/source-v0.1.3/target",
    "tools/disasm/output", "tools/func_id/output", "tools/recomp/output",
)
MAX_GIT_FILE_BYTES = 100 * 1024 * 1024


def archive_helpers():
    path = ROOT / "ports/mercenaries/scripts/Archive-GeneratedProvenance.py"
    spec = importlib.util.spec_from_file_location("source_archive_helpers", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def source_files(root: Path, helper) -> list[tuple[str, Path]]:
    files = []
    for name in SOURCE_ROOTS:
        if not (root / name).is_dir():
            raise ValueError(f"Required source directory missing: {name}")
        files.extend(helper.collect(root / name, name, excluded=EXCLUDED))
    files.extend(helper.collect_port_sources(root / "ports/mercenaries"))
    for name in ROOT_FILES:
        if not (root / name).is_file():
            raise ValueError(f"Required source file missing: {name}")
        files.append((name, root / name))
    files.sort(key=lambda item: item[0])
    names = [name.casefold() for name, _ in files]
    if len(names) != len(set(names)):
        raise ValueError("Duplicate or case-colliding export paths")
    helper.validate_source_paths(files, root)
    for name, path in files:
        if path.stat().st_size >= MAX_GIT_FILE_BYTES:
            raise ValueError(f"File exceeds normal GitHub size limit: {name}")
        if (path.suffix.lower() in {".dmp", ".mdmp", ".pem", ".key"}
                or path.name.lower() == ".env"
                or path.name.lower().startswith(".env.")):
            raise ValueError(f"Review local capture/credential file before export: {name}")
    return files


def package(output: Path) -> dict:
    output = output.absolute()
    if output.exists():
        raise ValueError(f"Output already exists; choose a new directory: {output}")
    helper = archive_helpers()
    files = source_files(ROOT, helper)
    # Keep a failed export for diagnosis; never delete or replace a prior export.
    output.mkdir(parents=True, exist_ok=False)
    repository = output / "repository"
    repository.mkdir()
    entries = []
    copied = []
    for name, source in files:
        target = repository / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        source_hash = helper.sha256_file(source)
        if helper.sha256_file(target) != source_hash:
            raise ValueError(f"Source changed during export: {name}")
        entries.append({"path": name, "bytes": target.stat().st_size,
                        "sha256": source_hash})
        copied.append((name, target))
    manifest = {
        "schema": 1,
        "description": "Maintained source snapshot; original Git history is not included.",
        "hashScope": "Payload files only; excludes SOURCE_MANIFEST.json itself.",
        "payloadTreeSha256": helper.tree_hash(copied),
        "files": entries,
    }
    manifest_path = repository / "SOURCE_MANIFEST.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    copied.append(("SOURCE_MANIFEST.json", manifest_path))
    copied.sort(key=lambda item: item[0])
    archive = output / "Mercenaries-Recompiled-source.zip"
    helper.write_zip(archive, copied)
    checked = verify_source_zip(archive)
    import zipfile
    with zipfile.ZipFile(archive) as z:
        bad = z.testzip()
        if bad:
            raise ValueError(f"ZIP CRC verification failed: {bad}")
        for name, source in copied:
            if hashlib.sha256(z.read(name)).hexdigest().upper() != helper.sha256_file(source):
                raise ValueError(f"ZIP content differs from staged file: {name}")
    archive_hash = helper.sha256_file(archive)
    (output / "SHA256SUMS.txt").write_text(
        f"{archive_hash}  {archive.name}\n", encoding="ascii")
    result = {
        "archive": archive.name, "sha256": archive_hash,
        "archiveBytes": archive.stat().st_size, "fileCount": checked,
        "payloadBytes": sum(item["bytes"] for item in entries),
        "payloadTreeSha256": manifest["payloadTreeSha256"],
        "largestFile": max(entries, key=lambda item: item["bytes"]),
        "checks": ["source paths and links", "GitHub file-size ceiling",
                   "game/archive signatures and reviewed fixture hash",
                   "ZIP CRCs", "staged/ZIP SHA-256 equality"],
    }
    (output / "PACKAGE_VALIDATION.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="New directory for repository, ZIP, and verification records")
    args = parser.parse_args()
    try:
        print(json.dumps(package(args.output), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, f"Source export failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

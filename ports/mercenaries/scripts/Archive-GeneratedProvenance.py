"""Content-addressed provenance archive created before generated C is replaced."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid
import zipfile
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[3]))
from tools.recomp.source_package_policy import forbidden_source_path, verify_source_zip


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


# Build products and investigation scratch files must not enter source archives.
# Pinned third-party binaries are retained; locally compiled tool executables
# are excluded only from the tools/ tree below.
SOURCE_OUTPUT_PATTERNS = (
    "*.log", "*.obj", "*.pdb", "*.ilk", "*.map", "*.pyc", "*.pyo",
    "*.bak", "*.orig", "*.rej", "*.tmp", "*.before-*", "*~",
)


def is_source_output(archive_name: str) -> bool:
    if forbidden_source_path(archive_name):return True
    path = Path(archive_name)
    if any(part in {"__pycache__", ".pytest_cache"} for part in path.parts):
        return True
    if any(fnmatch.fnmatch(path.name.lower(), pattern) for pattern in SOURCE_OUTPUT_PATTERNS):
        return True
    return archive_name.startswith("tools/") and path.suffix.lower() == ".exe"


def collect(root: Path, prefix: str, excluded: tuple[str, ...] = ()) -> list[tuple[str, Path]]:
    result: list[tuple[str, Path]] = []
    if not root.is_dir():
        return result
    # Do not follow source links/junctions into local retail data or backups.
    # A ZIP writer dereferences links, so checking ZIP attributes alone is late.
    resolved_root = root.resolve()
    if root.is_symlink() or root.is_junction():
        raise ValueError(f"Linked source directory: {root}")
    for directory, names, files in os.walk(root, followlinks=False):
        parent = Path(directory)
        for name in list(names):
            child = parent / name
            relative = child.relative_to(root).as_posix()
            archive_name = f"{prefix}/{relative}"
            if (any(archive_name == item or archive_name.startswith(item + "/") for item in excluded)
                    or (prefix != "generated" and is_source_output(archive_name))):
                names.remove(name)
                continue
            if child.is_symlink() or child.is_junction():
                raise ValueError(f"Linked source directory: {child}")
        for name in files:
            path = parent / name
            relative = path.relative_to(root).as_posix()
            archive_name = f"{prefix}/{relative}"
            if any(archive_name == item or archive_name.startswith(item + "/") for item in excluded):
                continue
            if prefix != "generated" and is_source_output(archive_name):
                continue
            if path.is_symlink() or not path.resolve().is_relative_to(resolved_root):
                raise ValueError(f"Linked or escaping source file: {path}")
            result.append((archive_name, path))
    return sorted(result, key=lambda item: item[0])


def validate_source_paths(files: list[tuple[str, Path]], repo_root: Path) -> None:
    """Also cover individually selected source files and linked parent folders."""
    root = repo_root.resolve()
    for name, source in files:
        if not source.resolve().is_relative_to(root):
            raise ValueError(f"Source outside repository: {name}")
        for part in (source, *source.parents):
            if part == repo_root:
                break
            if part.is_symlink() or part.is_junction():
                raise ValueError(f"Linked source input: {name}")


def tree_hash(files: list[tuple[str, Path]]) -> str:
    manifest = "\n".join(f"{name}\t{sha256_file(path)}" for name, path in files)
    return hashlib.sha256(manifest.encode("utf-8")).hexdigest().upper()


def write_zip(path: Path, files: list[tuple[str, Path]]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, source in files:
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            with source.open("rb") as stream:
                archive.writestr(info, stream.read())


def git_output(repo: Path, *arguments: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(repo), *arguments],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip() or None


def collect_port_sources(port_root: Path) -> list[tuple[str, Path]]:
    # Historical gen-* snapshots are disposable outputs just like active gen/.
    # Keep all of them outside the maintained source archive; the active output
    # is preserved separately by generated-tree.zip.
    snapshots = tuple(
        "ports/mercenaries/src/recomp/" + path.name
        for path in (port_root / "src/recomp").glob("gen-*")
        if path.is_dir()
    )
    return collect(port_root, "ports/mercenaries", excluded=(
        "ports/mercenaries/analysis", "ports/mercenaries/src/recomp/gen",
    ) + snapshots)


def archive_provenance(generated_root: Path, repo_root: Path, port_root: Path,
                       xbe_path: Path, archive_root: Path) -> Path:
    generated = collect(generated_root, "generated")
    if not generated:
        raise RuntimeError(f"generated tree is empty: {generated_root}")

    toolchain: list[tuple[str, Path]] = []
    for relative in (
        "src", "include", "templates", "docs", "tools/disasm", "tools/func_id",
        "tools/recomp", "tools/xbe_parser",
        "third_party/dsp56300/msvc-x64-v0.1.3", "third_party/lua-5.0.3",
    ):
        toolchain.extend(collect(repo_root / relative, relative))
    toolchain.extend(collect(repo_root / "third_party/dsp56300/source-v0.1.3",
                             "third_party/dsp56300/source-v0.1.3", excluded=(
                                 "third_party/dsp56300/source-v0.1.3/target",
                                 "third_party/dsp56300/source-v0.1.3/.git",
                             )))
    for relative in ("CMakeLists.txt", "tools/__init__.py",
                     "tools/diagnostics/inspect_retail_script.py",
                     "tools/diagnostics/retail_box_collision.py",
                     "tools/diagnostics/inspect_retail_templates.py",
                     "tools/diagnostics/inspect_retail_lua_snapshot.py",
                     "tools/diagnostics/inspect_retail_model_bounds.py",
                     "tools/diagnostics/compare_human_snapshots.py"):
        source = repo_root / relative
        if not source.is_file():
            raise RuntimeError(f"required toolchain source is missing: {source}")
        toolchain.append((relative, source))
    for relative in (
        "LICENSE", "README.md", "CONTRIBUTING.md", ".gitignore", ".gitattributes",
        "tools/README.md",
    ):
        source = repo_root / relative
        if source.is_file():
            toolchain.append((relative, source))
    toolchain.extend(collect_port_sources(port_root))
    toolchain.sort(key=lambda item: item[0])
    validate_source_paths(toolchain, repo_root)

    generated_hash = tree_hash(generated)
    toolchain_hash = tree_hash(toolchain)
    xbe_hash = sha256_file(xbe_path)
    archive_id = hashlib.sha256(
        f"{generated_hash}\n{toolchain_hash}\n{xbe_hash}\n".encode("ascii")
    ).hexdigest().upper()
    destination = archive_root / archive_id
    if destination.exists():
        manifest = json.loads((destination / "provenance.json").read_text(encoding="utf-8"))
        if (manifest.get("generatedTreeSha256") != generated_hash or
                manifest.get("toolchainTreeSha256") != toolchain_hash or
                manifest.get("retailXbeSha256") != xbe_hash):
            raise RuntimeError(f"existing provenance archive is inconsistent: {destination}")
        verify_source_zip(destination / 'toolchain-source.zip')
        return destination

    archive_root.mkdir(parents=True, exist_ok=True)
    staging = archive_root / f".{archive_id}.staging-{uuid.uuid4().hex}"
    staging.mkdir()
    try:
        generated_zip = staging / "generated-tree.zip"
        toolchain_zip = staging / "toolchain-source.zip"
        write_zip(generated_zip, generated)
        write_zip(toolchain_zip, toolchain)
        verify_source_zip(toolchain_zip)
        revision = git_output(repo_root, "rev-parse", "HEAD")
        dirty = git_output(repo_root, "status", "--porcelain", "--untracked-files=all")
        manifest = {
            "schema": 1,
            "archiveId": archive_id,
            "sourceRevision": revision,
            "workingTreeDirty": bool(dirty),
            "retailXbeSha256": xbe_hash,
            "generatedFileCount": len(generated),
            "generatedTreeSha256": generated_hash,
            "toolchainFileCount": len(toolchain),
            "toolchainTreeSha256": toolchain_hash,
            "generatedArchiveSha256": sha256_file(generated_zip),
            "toolchainArchiveSha256": sha256_file(toolchain_zip),
        }
        (staging / "provenance.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        os.replace(staging, destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated-root", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--port-root", required=True, type=Path)
    parser.add_argument("--xbe", required=True, type=Path)
    parser.add_argument("--archive-root", required=True, type=Path)
    args = parser.parse_args()
    destination = archive_provenance(
        args.generated_root.resolve(), args.repo_root.resolve(),
        args.port_root.resolve(), args.xbe.resolve(), args.archive_root.resolve()
    )
    print(destination)


if __name__ == "__main__":
    main()

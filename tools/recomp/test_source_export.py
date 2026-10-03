"""GitHub source export boundaries and byte identity."""
import hashlib
import importlib.util
import json
from pathlib import Path
import zipfile

import pytest

ROOT = Path(__file__).resolve().parents[2]


def exporter():
    path = ROOT / "ports/mercenaries/scripts/Package-Source.py"
    spec = importlib.util.spec_from_file_location("source_export", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fixture_tree(module, root):
    for name in module.SOURCE_ROOTS:
        directory = root / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "fixture.txt").write_text("maintained source\n")
    for name in module.ROOT_FILES:
        (root / name).write_text("source document\n")
    port = root / "ports/mercenaries/src/recomp"
    port.mkdir(parents=True)
    (port / "manual.c").write_text("void hook(void) {}\n")
    for name in ("game_files/default.xbe", ".git/objects/local",
                 "artifacts/private.txt", "tools/recomp/output/analysis.json",
                 "ports/mercenaries/src/recomp/gen/game.c",
                 "ports/mercenaries/src/recomp/gen-old/game.c"):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"local data")


def test_export_manifest_and_archive_match_without_local_outputs(tmp_path, monkeypatch):
    module = exporter()
    helper = module.archive_helpers()
    root = tmp_path / "source"
    root.mkdir()
    fixture_tree(module, root)
    monkeypatch.setattr(module, "ROOT", root)
    monkeypatch.setattr(module, "archive_helpers", lambda: helper)
    output = tmp_path / "export"
    result = module.package(output)
    with zipfile.ZipFile(output / result["archive"]) as archive:
        manifest = json.loads(archive.read("SOURCE_MANIFEST.json"))
        assert len(archive.namelist()) == len(manifest["files"]) + 1
        for item in manifest["files"]:
            data = archive.read(item["path"])
            assert data == (output / "repository" / item["path"]).read_bytes()
            assert len(data) == item["bytes"]
            assert hashlib.sha256(data).hexdigest().upper() == item["sha256"]
        assert "ports/mercenaries/src/recomp/manual.c" in archive.namelist()
        assert not any("game.c" in name or "private.txt" in name or
                       "/output/" in name or name.startswith(".git/")
                       for name in archive.namelist())
    before = (output / result["archive"]).read_bytes()
    with pytest.raises(ValueError, match="already exists"):
        module.package(output)
    assert (output / result["archive"]).read_bytes() == before


def test_export_rejects_renamed_game_content(tmp_path, monkeypatch):
    module = exporter()
    helper = module.archive_helpers()
    root = tmp_path / "source"
    root.mkdir()
    fixture_tree(module, root)
    (root / "src/innocent.dat").write_bytes(b"XBEH" + bytes(100))
    monkeypatch.setattr(module, "ROOT", root)
    monkeypatch.setattr(module, "archive_helpers", lambda: helper)
    with pytest.raises(ValueError, match="Xbox executable header"):
        module.package(tmp_path / "export")
    assert not (tmp_path / "export/SHA256SUMS.txt").exists()


def test_export_rejects_capture_before_writing_output(tmp_path, monkeypatch):
    module = exporter()
    helper = module.archive_helpers()
    root = tmp_path / "source"
    root.mkdir()
    fixture_tree(module, root)
    (root / "src/crash.dmp").write_bytes(b"capture")
    monkeypatch.setattr(module, "ROOT", root)
    monkeypatch.setattr(module, "archive_helpers", lambda: helper)
    with pytest.raises(ValueError, match="capture/credential"):
        module.package(tmp_path / "export")
    assert not (tmp_path / "export").exists()


@pytest.mark.parametrize("include_history", [False, True])
def test_export_optional_history_and_required_build_inputs(tmp_path, include_history):
    module = exporter()
    helper = module.archive_helpers()
    fixture_tree(module, tmp_path)
    if include_history:
        for name in module.OPTIONAL_ROOT_FILES:
            (tmp_path / name).write_text("historical record\n")
    names = {name for name, _ in module.source_files(tmp_path, helper)}
    for name in module.OPTIONAL_ROOT_FILES:
        assert (name in names) == include_history
    (tmp_path / "CMakeLists.txt").unlink()
    with pytest.raises(ValueError, match="Required source file missing: CMakeLists"):
        module.source_files(tmp_path, helper)

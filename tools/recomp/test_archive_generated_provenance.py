from pathlib import Path
import importlib.util
import json
import tempfile
import zipfile


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "ports/mercenaries/scripts/Archive-GeneratedProvenance.py"


def load_module():
    spec = importlib.util.spec_from_file_location("archive_generated_provenance", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_content_addressed_archive_is_complete_and_idempotent():
    module = load_module()
    with tempfile.TemporaryDirectory(prefix="merc-provenance-") as directory:
        repo = Path(directory) / "repo"
        port = repo / "ports/mercenaries"
        generated = port / "src/recomp/gen"
        archives = repo / "artifacts/provenance/generated-trees"
        for path, data in {
            repo / "CMakeLists.txt": b"root build\n",
            repo / "src/runtime.c": b"runtime\n",
            repo / "LICENSE": b"source-license\n",
            repo / "docs/README.md": b"documentation\n",
            repo / ".gitignore": b"local-output\n",
            repo / "tools/recomp/probe.exe": b"local-tool\n",
            repo / "tools/recomp/probe.obj": b"local-object\n",
            port / "runtime-test.log": b"local-log\n",
            port / "src/runtime.c.before-trace": b"scratch-backup\n",
            port / "src/recomp/gen-old/recomp_0000.c": b"old-generated\n",
            port / "third_party/xdvdfs/xdvdfs.exe": b"pinned-extractor\n",
            repo / "third_party/dsp56300/msvc-x64-v0.1.3/lib/dsp56300_emu_ffi.lib": b"pinned-dsp\n",
            repo / "third_party/dsp56300/source-v0.1.3/Cargo.lock": b"locked-public-dependencies",
            repo / "third_party/dsp56300/source-v0.1.3/crates/emu/src/core.rs": b"patched-public-source",
            repo / "third_party/dsp56300/source-v0.1.3/target/release/cache.bin": b"not-source",
            repo / "tools/__init__.py": b"\n",
            repo / "tools/recomp/translator.py": b"translator\n",
            repo / "tools/diagnostics/inspect_retail_script.py": b"script-reader\n",
            repo / "tools/diagnostics/retail_box_collision.py": b"retail-collision\n",
            repo / "tools/diagnostics/inspect_retail_templates.py": b"retail-templates\n",
            repo / "tools/diagnostics/inspect_retail_lua_snapshot.py": b"retail-lua-snapshot\n",
            repo / "tools/diagnostics/inspect_retail_model_bounds.py": b"retail-model-bounds\n",
            repo / "tools/diagnostics/compare_human_snapshots.py": b"human-snapshot-reader\n",
            repo / "tools/diagnostics/unreviewed.py": b"not-selected\n",
            repo / "third_party/lua-5.0.3/src/ldo.c": b"public-lua\n",
            repo / "third_party/lua-5.0.3/COPYRIGHT": b"public-lua-notice\n",
            repo / "artifacts/private-backup/example.h": b"private-backup\n",
            repo / "tools/xbe_parser/xbe_parser.py": b"parser\n",
            port / "scripts/Patch-Generated.py": b"patch\n",
            port / "analysis/temporary.json": b"excluded\n",
            generated / "recomp_0000.c": b"generated-a\n",
            generated / "recomp_dispatch.c": b"generated-b\n",
            repo / "retail/default.xbe": b"retail-xbe\n",
        }.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

        first = module.archive_provenance(generated, repo, port, repo / "retail/default.xbe", archives)
        second = module.archive_provenance(generated, repo, port, repo / "retail/default.xbe", archives)
        assert first == second
        assert len(list(archives.iterdir())) == 1
        manifest = json.loads((first / "provenance.json").read_text(encoding="utf-8"))
        assert manifest["generatedFileCount"] == 2
        assert manifest["toolchainFileCount"] == 21
        with zipfile.ZipFile(first / "generated-tree.zip") as archive:
            assert archive.namelist() == ["generated/recomp_0000.c", "generated/recomp_dispatch.c"]
        with zipfile.ZipFile(first / "toolchain-source.zip") as archive:
            names = archive.namelist()
            assert "third_party/dsp56300/source-v0.1.3/Cargo.lock" in names
            assert "third_party/dsp56300/source-v0.1.3/crates/emu/src/core.rs" in names
            assert not any("source-v0.1.3/target/" in name for name in names)
            assert "CMakeLists.txt" in names
            assert "docs/README.md" in names
            assert ".gitignore" in names
            assert "ports/mercenaries/third_party/xdvdfs/xdvdfs.exe" in names
            assert "tools/recomp/probe.exe" not in names
            assert "tools/recomp/probe.obj" not in names
            assert "ports/mercenaries/runtime-test.log" not in names
            assert "ports/mercenaries/src/runtime.c.before-trace" not in names
            assert not any("gen-old/" in name for name in names)
            assert "LICENSE" in names
            assert "third_party/dsp56300/msvc-x64-v0.1.3/lib/dsp56300_emu_ffi.lib" in names
            assert "tools/diagnostics/inspect_retail_script.py" in names
            assert "tools/diagnostics/retail_box_collision.py" in names
            assert "tools/diagnostics/inspect_retail_templates.py" in names
            assert "tools/diagnostics/inspect_retail_lua_snapshot.py" in names
            assert "tools/diagnostics/inspect_retail_model_bounds.py" in names
            assert "tools/diagnostics/unreviewed.py" not in names
            assert "third_party/lua-5.0.3/src/ldo.c" in names
            assert "third_party/lua-5.0.3/COPYRIGHT" in names
            assert not any(name.startswith("artifacts/") for name in names)
            assert "tools/__init__.py" in names
            assert "tools/xbe_parser/xbe_parser.py" in names
            assert "ports/mercenaries/analysis/temporary.json" not in names
            assert not any(name.startswith("ports/mercenaries/src/recomp/gen/") for name in names)

        (repo / "src/runtime.c").write_bytes(b"runtime-changed\n")
        third = module.archive_provenance(generated, repo, port, repo / "retail/default.xbe", archives)
        assert third != first
        assert len(list(archives.iterdir())) == 2


if __name__ == "__main__":
    test_content_addressed_archive_is_complete_and_idempotent()
    print("archive generated provenance test: PASS")


def test_collection_does_not_follow_links(tmp_path):
    import pytest
    module = load_module()
    root = tmp_path / "src"
    root.mkdir()
    outside = tmp_path / "local-game"
    outside.mkdir()
    (outside / "renamed.dat").write_bytes(b"local retail data")
    link = root / "linked-data"
    try:
        link.symlink_to(outside, target_is_directory=True)
    except OSError as error:
        pytest.skip(f"Creating test symlink is unavailable: {error}")
    with pytest.raises(ValueError, match="Linked source directory"):
        module.collect(root, "src")
    link.unlink()
    link.symlink_to(outside / "renamed.dat")
    with pytest.raises(ValueError, match="Linked or escaping source file"):
        module.collect(root, "src")


def test_individual_source_selection_cannot_escape_repository(tmp_path):
    import pytest
    module = load_module()
    repo = tmp_path / "repo"
    repo.mkdir()
    outside = tmp_path / "outside.c"
    outside.write_text("local-only data")
    with pytest.raises(ValueError, match="Source outside repository"):
        module.validate_source_paths([("src/example.c", outside)], repo)


def test_excluded_game_directories_are_not_walked(tmp_path):
    module = load_module()
    root = tmp_path / "src"
    (root / "game_files").mkdir(parents=True)
    (root / "game_files/renamed.dat").write_bytes(b"retail data")
    (root / "keep.c").write_text("source")
    assert [name for name, _ in module.collect(root, "src")] == ["src/keep.c"]


def test_collection_rejects_windows_junction(tmp_path):
    import os
    import subprocess
    import pytest
    if os.name != "nt":
        pytest.skip("Windows junction test")
    module = load_module()
    root = tmp_path / "src"
    root.mkdir()
    outside = tmp_path / "local-game"
    outside.mkdir()
    link = root / "linked-data"
    made = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(outside)],
                          capture_output=True, text=True)
    assert made.returncode == 0, made.stderr
    try:
        assert link.is_junction()
        with pytest.raises(ValueError, match="Linked source directory"):
            module.collect(root, "src")
    finally:
        # rmdir removes the junction itself, never the target or its contents.
        os.rmdir(link)
    assert outside.is_dir()

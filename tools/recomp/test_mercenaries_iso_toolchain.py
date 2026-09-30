from pathlib import Path
import hashlib


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "ports" / "mercenaries" / "scripts"


def test_xdvdfs_bootstrap_is_offline_bundled_and_hash_pinned():
    text = (SCRIPTS / "Setup-Tools.ps1").read_text(encoding="utf-8")
    expected = "DF5F19954EF706C130C256546BF67CF7725611BDB30D1815577187007A6F907A"
    bundle_root = ROOT / "ports" / "mercenaries" / "third_party" / "xdvdfs"
    bundled = bundle_root / "xdvdfs.exe"
    assert bundled.is_file()
    assert (bundle_root / "LICENSE").is_file()
    assert hashlib.sha256(bundled.read_bytes()).hexdigest().upper() == expected
    assert expected in text
    assert "Invoke-WebRequest" not in text
    assert "http://" not in text and "https://" not in text
    assert 'Join-Path $portRoot "third_party\\xdvdfs"' in text
    assert text.index("$stagedHash") < text.index("Move-Item -LiteralPath $stagingRoot")


def test_iso_entry_point_runs_pipeline_in_order_and_records_provenance():
    text = (SCRIPTS / "Build-From-Iso.ps1").read_text(encoding="utf-8")
    ordered = [
        '"Setup-Tools.ps1"',
        '"Setup-Python.ps1"',
        '"Extract-Game.ps1"',
        '"Recompile.ps1"',
        '"Build.ps1"',
    ]
    positions = [text.index(item) for item in ordered]
    assert positions == sorted(positions)
    for field in (
        "inputIsoSha256",
        "retailXbeSha256",
        "generatedTreeSha256",
        "patchPipelineSha256",
        "toolchainTreeSha256",
        "toolchainArchiveSha256",
        "cmakeGenerator",
        "compilerVersion",
        "compilerExeSha256",
        "linkerExeSha256",
        "visualStudioPlatformToolset",
        "windowsSdkVersion",
        "executableSha256",
        "launcherSha256",
        "bundledRuntimeExtractorSha256",
        "sdl2RuntimeSha256",
    ):
        assert field in text

    assert "Archive-GeneratedProvenance.py" in text
    assert "schema = 3" in text
    # Quoted CMake paths commonly contain Program Files (x86). The quoted
    # branch must therefore accept parentheses instead of truncating at (x86).
    assert r'(?:"([^"]*)"|([^\)]+))' in text
    assert "match.Groups[1].Success" in text
    assert '"generated/$relative' in text
    assert "Generated-tree identity disagrees" in text


def test_native_build_pins_generator_toolset_and_sdk():
    text = (SCRIPTS / "Build.ps1").read_text(encoding="utf-8")
    assert '"Visual Studio 17 2022"' in text
    assert '"x64,version=10.0.26100.0"' in text
    assert '"v143,version=14.44.35207,host=x64"' in text


def test_isolated_build_directory_flows_into_native_build_and_provenance():
    native = (SCRIPTS / "Build.ps1").read_text(encoding="utf-8")
    pipeline = (SCRIPTS / "Build-From-Iso.ps1").read_text(encoding="utf-8")
    for script in (native, pipeline):
        assert "[string]$BuildDirectory" in script
        assert "[IO.Path]::GetFullPath($BuildDirectory)" in script
        assert 'Join-Path $repoRoot "build\\mercenaries"' in script
    assert "BuildDirectory = $buildRoot" in pipeline
    for name in ("$executable", "$launcher", "$runtimeExtractor", "$sdl2Runtime", "$cachePath"):
        assert name + " = Join-Path $buildRoot " in pipeline
    assert '"-B", $buildRoot' in native


if __name__ == "__main__":
    test_xdvdfs_bootstrap_is_offline_bundled_and_hash_pinned()
    test_iso_entry_point_runs_pipeline_in_order_and_records_provenance()
    test_native_build_pins_generator_toolset_and_sdk()
    test_isolated_build_directory_flows_into_native_build_and_provenance()
    print("ok mercenaries ISO toolchain")

def test_maintained_archive_excludes_all_generated_snapshots(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("archive_sources", SCRIPTS / "Archive-GeneratedProvenance.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    port = tmp_path / "mercenaries"
    for name in ("src/recomp/gen/recomp_0000.c", "src/recomp/gen-old/recomp_0000.c",
                 "analysis/old.json", "src/recomp_manual.c", "src/turret_camera_roll.h",
                 "scripts/Patch-Generated.py", "src/recomp/recomp_types.h"):
        target = port / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("fixture")
    names = {name for name, _ in module.collect_port_sources(port)}
    assert names == {"ports/mercenaries/" + name for name in (
        "src/recomp_manual.c", "src/turret_camera_roll.h", "scripts/Patch-Generated.py",
        "src/recomp/recomp_types.h")}
    assert module.collect(port / "src/recomp/gen", "generated")

"""First-launch ISO setup is offline, transactional, and instant after install."""

from pathlib import Path
import hashlib


ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports" / "mercenaries"
LAUNCHER = (PORT / "src" / "setup_launcher.c").read_text(encoding="utf-8")
CMAKE = (PORT / "CMakeLists.txt").read_text(encoding="utf-8")
BUILD = (PORT / "scripts" / "Build.ps1").read_text(encoding="utf-8")
ISO_BUILD = (PORT / "scripts" / "Build-From-Iso.ps1").read_text(encoding="utf-8")


def test_setup_identity_and_offline_distribution() -> None:
    assert 'L"Mercenaries Recompiled Setup"' in LAUNCHER
    assert 'L"tools\\\\xdvdfs.exe"' in LAUNCHER
    mod_loader = (PORT / "src" / "mod_loader.cpp").read_text(encoding="utf-8")
    assert "CREATE_NO_WINDOW" in LAUNCHER  # ISO extraction
    assert "CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT" in mod_loader  # game child
    assert "mercenaries_mod_launch(g_state.window, g_state.base_dir, g_state.game_dir)" in LAUNCHER
    assert "PBS_MARQUEE" not in LAUNCHER
    assert "PBM_SETMARQUEE" not in LAUNCHER
    assert "WM_INSTALL_PROGRESS" in LAUNCHER
    assert "g_state.view.progress=" in LAUNCHER
    assert "GetProcessIoCounters(process.hProcess, &counters)" in LAUNCHER
    assert "counters.WriteTransferCount * 85ULL" in LAUNCHER
    extractor = LAUNCHER[
        LAUNCHER.index("static BOOL run_extractor") :
        LAUNCHER.index("static BOOL write_install_marker")
    ]
    assert "scan_install_tree" not in extractor
    assert "WaitForSingleObject(process.hProcess, 200)" in extractor
    assert 'L"CANCEL",5,TRUE' in LAUNCHER
    assert "refresh_setup();" in LAUNCHER
    for stage in (92, 96, 98, 100):
        assert f"WM_INSTALL_PROGRESS, {stage}, 0" in LAUNCHER
    assert "Invoke-WebRequest" not in LAUNCHER
    assert "http://" not in LAUNCHER and "https://" not in LAUNCHER
    assert "xemu" not in LAUNCHER.lower()
    assert "add_executable(mercenaries_launcher WIN32" in CMAKE
    assert 'OUTPUT_NAME "Mercenaries Recompiled"' in CMAKE
    assert "/xdvdfs.exe" in CMAKE
    assert "/xdvdfs-LICENSE.txt" in CMAKE
    assert "/SDL2.dll" in CMAKE
    assert "/SDL2-LICENSE.txt" in CMAKE
    sdl = PORT / "third_party" / "SDL2" / "SDL2.dll"
    assert sdl.is_file()
    assert hashlib.sha256(sdl.read_bytes()).hexdigest().upper() == (
        "47A3B27654832D9F7E5719C88078210A3DBFD6407B559C5A3C4EB8AB7D0EBDB6"
    )


def test_normal_launch_checks_only_completion_marker() -> None:
    marker_branch = LAUNCHER.index("if (require_marker) {")
    immediate_success = LAUNCHER.index("return TRUE;", marker_branch)
    required_scan = LAUNCHER.index(
        "for (index = 0; index < ARRAYSIZE(required_files); ++index)"
    )
    assert marker_branch < immediate_success < required_scan
    assert "validate_install(g_state.game_dir, TRUE" in LAUNCHER
    assert "validate_install(g_state.staging_dir, FALSE" in LAUNCHER


def test_one_time_install_is_fully_validated_and_transactional() -> None:
    assert "#define EXPECTED_RETAIL_FILE_COUNT 434ULL" in LAUNCHER
    assert "#define EXPECTED_RETAIL_TOTAL_BYTES 2506857692ULL" in LAUNCHER
    assert "scan_install_tree(game_dir, &file_count, &total_bytes)" in LAUNCHER
    assert (
        "AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7"
        in LAUNCHER
    )
    assert 'L"game_files\\\\.mercenaries-installing"' in LAUNCHER
    assert "MoveFileExW(g_state.staging_dir, g_state.game_dir" in LAUNCHER
    assert "write_install_marker(g_state.staging_dir)" in LAUNCHER
    assert LAUNCHER.index("validate_install(g_state.staging_dir, FALSE") < (
        LAUNCHER.index("write_install_marker(g_state.staging_dir)")
    )


def test_reproducible_build_emits_and_hashes_first_launch_files() -> None:
    assert "Mercenaries Recompiled.exe" in BUILD
    assert "tools\\xdvdfs.exe" in BUILD
    assert "launcherSha256" in ISO_BUILD
    assert "bundledRuntimeExtractorSha256" in ISO_BUILD
    assert "sdl2RuntimeSha256" in ISO_BUILD
    assert "schema = 3" in ISO_BUILD


if __name__ == "__main__":
    test_setup_identity_and_offline_distribution()
    test_normal_launch_checks_only_completion_marker()
    test_one_time_install_is_fully_validated_and_transactional()
    test_reproducible_build_emits_and_hashes_first_launch_files()
    print("ok mercenaries first-launch setup")

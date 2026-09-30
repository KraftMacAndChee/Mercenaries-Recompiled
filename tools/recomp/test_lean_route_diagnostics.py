from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RUNNER = (ROOT / "tools" / "recomp" / "Run-HiddenRetailRoute.ps1").read_text(
    encoding="utf-8"
)
MANUAL = (ROOT / "ports" / "mercenaries" / "src" / "recomp_manual.c").read_text(
    encoding="utf-8"
)


def test_lean_route_removes_high_volume_observers() -> None:
    lean = RUNNER[RUNNER.index("if ($LeanDiagnostics)") :]
    for setting in (
        "MERCENARIES_TRACE_VEHICLE_REWARD",
        "MERCENARIES_TRACE_ROADBLOCK_ACTORS",
        "MERCENARIES_TRACE_NOTIFICATION_LIST",
        "MERCENARIES_TRACE_XACT_FAILURES",
        "MERCENARIES_TRACE_XACT_INTERNAL_FAILURES",
    ):
        assert f"$settings.Remove('{setting}')" in lean


def test_icall_mismatch_report_is_cached_opt_in() -> None:
    start = MANUAL.index("void recomp_nonvolatile_icall_checkpoint(")
    end = MANUAL.index("\n}\n", start)
    helper = MANUAL[start:end]
    assert "static int trace_enabled = -1;" in helper
    assert 'getenv("MERCENARIES_TRACE_ICALL_NONVOLATILE")' in helper
    assert "if (!trace_enabled ||" in helper


if __name__ == "__main__":
    test_lean_route_removes_high_volume_observers()
    test_icall_mismatch_report_is_cached_opt_in()
    print("PASS: lean route disables high-volume observers")

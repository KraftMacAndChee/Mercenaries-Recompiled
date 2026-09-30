from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ports" / "mercenaries" / "src" / "recomp_manual.c"


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    # Route completion is now a shared helper and its log includes stage and
    # vehicle information. Inspect the behavior, not the obsolete log spelling.
    match = re.search(
        r'static void recomp_complete_test_aircraft_route\(.*?\n\}',
        text, re.S,
    )
    assert match is not None, "Missing shared aircraft completion helper"
    block = match.group(0)

    assert re.search(
        r'if \(getenv\("MERCENARIES_CAPTURE_POST_AIRCRAFT_DRAW_PREFIX"\) != NULL\)\s*'
        r'pgraph_d3d11_reset_gameplay_capture_series\(\);', block,
    ), "Capture reset must remain conditional on the diagnostic prefix"
    assert block.index("g_recomp_aircraft_route_complete = 1;") < block.index(
        "pgraph_d3d11_reset_gameplay_capture_series();"
    )
    assert block.index("pgraph_d3d11_reset_gameplay_capture_series();") < block.index(
        'getenv("MERCENARIES_TRACE_HAVOK_STACK_WATCH")'
    )
    print("Post-aircraft draw capture reset checks passed")


if __name__ == "__main__":
    main()

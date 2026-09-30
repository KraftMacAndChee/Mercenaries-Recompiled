#!/usr/bin/env python3
import tempfile
from pathlib import Path

from audit_generated_function_parity import compare, load_functions


def write_tree(root: Path, bodies: list[tuple[int, str]]) -> None:
    content = '#include "recomp_funcs.h"\n\n'
    for address, statement in bodies:
        content += (
            f"/*\n * Original: 0x{address:08X} - 0x{address + 0x10:08X} "
            "(16 bytes, 1 insns)\n */\n"
            f"void sub_{address:08X}(void)\n{{\n    {statement}\n}}\n\n"
        )
    (root / "recomp_0000.c").write_text(content, encoding="utf-8")


def main() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        baseline, candidate = root / "baseline", root / "candidate"
        baseline.mkdir()
        candidate.mkdir()
        write_tree(baseline, [(0x1000, "eax = 1;"), (0x2000, "eax = 2;")])
        write_tree(candidate, [(0x1000, "eax   = 1;"), (0x2000, "eax = 3;"), (0x3000, "return;")])
        reference = root / "Patch-Generated.py"
        reference.write_text("target = 'sub_00002000'\n", encoding="utf-8")
        assert len(load_functions(baseline)) == 2
        report = compare(baseline, candidate, [reference])
        assert report["unchanged_function_count"] == 1
        assert not report["removed"]
        assert not report["subsumed"]
        assert [item["address"] for item in report["added"]] == ["0x00003000"]
        assert [item["address"] for item in report["changed"]] == ["0x00002000"]
        assert report["changed"][0]["canonical_references"] == [str(reference)]

        # A former function entry inside a newly coalesced candidate range is
        # represented explicitly instead of being reported as missing code.
        coalesced = root / "coalesced"
        coalesced.mkdir()
        write_tree(coalesced, [(0x1000, "eax = 1;")])
        source = coalesced / "recomp_0000.c"
        source.write_text(
            source.read_text(encoding="utf-8").replace(
                "0x00001010 (16 bytes", "0x00002010 (4096 bytes"
            ),
            encoding="utf-8",
        )
        coalesced_report = compare(baseline, coalesced, [])
        assert not coalesced_report["removed"]
        assert [item["address"] for item in coalesced_report["subsumed"]] == ["0x00002000"]
    print("audit generated function parity test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
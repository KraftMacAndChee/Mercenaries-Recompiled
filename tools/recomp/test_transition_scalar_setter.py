import json
import re
from pathlib import Path


ROOT = Path(__file__).parents[2]
SEEDS = ROOT / "ports" / "mercenaries" / "manual-seeds.json"
STUB = (
    ROOT
    / "ports"
    / "mercenaries"
    / "src"
    / "recomp"
    / "gen"
    / "recomp_stubs_unresolved.c"
)


def main() -> None:
    seeds = json.loads(SEEDS.read_text(encoding="utf-8"))
    assert any(entry.get("start") == "0x0021AB90" for entry in seeds)

    # The discovered setter now lives in an ordinary generated translation
    # unit, not the unresolved-stub file. Require one actual implementation.
    implementations = []
    for path in STUB.parent.glob('*.c'):
        text = path.read_text(encoding='utf-8')
        if re.search(r'void sub_0021AB90\(void\)\s*\{', text):
            implementations.append(text)
    assert len(implementations) == 1
    source = implementations[0]
    start = source.index("void sub_0021AB90(void)")
    end = source.index("\n}", start)
    body = source[start:end]
    assert "recomp_xmm_loadss(xmm0v, esp + 4)" in body
    assert "MEMF(ecx + 0xDC) = xmm0" in body
    assert "esp += 8" in body
    assert "minimal guest ret" not in body

    print("Transition scalar setter checks passed")


if __name__ == "__main__":
    main()

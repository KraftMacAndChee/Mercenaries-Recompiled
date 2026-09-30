#!/usr/bin/env python3
"""Keep the retail RsSoundEffectCar2::SetEffectSet routine whole.

Function discovery mistakes 0x00150014 for an independent vtable entry.  The
real routine begins at 0x0014FF00 and falls through that address to initialize
all three running states, the reverse ratio, restore its stack, and return.
"""

from hashlib import sha256
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0006.c"
RECOMPILE = ROOT / "ports/mercenaries/scripts/Recompile.ps1"
XBE = ROOT / "game_files/mercenaries-retail/default.xbe"
MANUAL_SEEDS = ROOT / "ports/mercenaries/manual-seeds.json"


def main() -> None:
    generated = GENERATED.read_text(encoding="utf-8")
    recompile = RECOMPILE.read_text(encoding="utf-8")
    manual_seeds = json.loads(MANUAL_SEEDS.read_text(encoding="utf-8"))

    # The car sound vtable at 0x002E1670 dispatches SetDetailLevel through
    # slot 5 to 0x0014EBA0. If that indirect entry is not seeded, ignition is
    # enabled but LOD stays eNone and every authored vehicle cue is suppressed.
    assert any(int(entry["start"], 16) == 0x0014EBA0 for entry in manual_seeds)
    lod_start = generated.index("void sub_0014EBA0(void)")
    lod_end = generated.index("\n/**\n * sub_0014ECD0", lod_start)
    lod_function = generated[lod_start:lod_end]
    assert "MEM32(esi + 0xFC) = edi" in lod_function
    assert "MEM32(esi + 0x100) = edi" in lod_function
    assert "sub_0014EAE0();" in lod_function

    start = generated.index("void sub_0014FF00(void)")
    end = generated.index("\n/**\n * sub_001500A0", start)
    function = generated[start:end]

    assert '"--function-range", "0x0014FF00:0x0015009E"' in recompile
    assert "Original: 0x0014FF00 - 0x0015009E (414 bytes, 103 insns)" in generated
    assert "loc_00150028: ;" in function
    assert "loc_00150058: ;" in function
    assert "loc_00150088: ;" in function
    assert function.count("sub_0014F080();") == 3
    assert "MEMF(esi + 0xB0) = xmm1" in function
    assert "esp += 8; return; /* ret 4 */" in function

    retail = XBE.read_bytes()[0x13FF00:0x14009E]
    assert len(retail) == 0x19E
    assert sha256(retail).hexdigest().upper() == (
        "70EABD47F25B0E455D7E35906AE105250B5B101977B3A4CDD18389758A542B67"
    )
    assert retail.endswith(bytes.fromhex("5F5E83C414C20400"))

    print("Vehicle sound-effect retail range checks passed")


if __name__ == "__main__":
    main()

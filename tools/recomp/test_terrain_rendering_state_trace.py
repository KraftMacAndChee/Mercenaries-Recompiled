import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCHER_PATH = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0005.c"
TYPES = ROOT / "ports/mercenaries/src/recomp/recomp_types.h"
MANUAL = ROOT / "ports/mercenaries/src/recomp_manual.c"


def load_patcher():
    spec = importlib.util.spec_from_file_location(
        "merc_patch_generated", PATCHER_PATH
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class TerrainRenderingStateTraceTests(unittest.TestCase):
    def test_retail_lua_binding_is_instrumented_idempotently(self):
        module = load_patcher()
        generated = GENERATED.read_text(encoding="utf-8")

        request_patch = next(
            patch
            for patch in module.PATCHES
            if patch.name
            == "RedTerrain EnableRendering request checkpoint"
        )
        if request_patch.after in generated:
            patched = generated
        else:
            self.assertIn(request_patch.before, generated)
            patched = generated.replace(
                request_patch.before, request_patch.after, 1
            )

        body = patched.split("void sub_0011CC60(void)", 1)[1]
        body = body.split("\n}\n", 1)[0]
        self.assertEqual(
            body.count("recomp_terrain_rendering_request("), 1
        )
        self.assertIn("MEM32(0x643384u)", body)
        self.assertIn("LO8(eax) != 0u", body)

    def test_trace_is_opt_in_and_tracks_the_terrain_active_bit(self):
        types = TYPES.read_text(encoding="utf-8")
        manual = MANUAL.read_text(encoding="utf-8")

        self.assertIn("void recomp_terrain_rendering_request(", types)
        self.assertIn("void recomp_terrain_rendering_request(", manual)
        self.assertIn("MERCENARIES_TRACE_TERRAIN_STATE", manual)
        self.assertIn("guest_u32(0x00643384u)", manual)
        self.assertIn("[TERRAIN-STATE]", manual)
        self.assertIn("[TERRAIN-REQUEST]", manual)
        self.assertIn("[TERRAIN-%s]", manual)
        self.assertIn("subject == 0x00643370u", manual)


if __name__ == "__main__":
    unittest.main()

import importlib.util
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCHER_PATH = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"
GENERATED = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0005.c"


def load_patcher():
    spec = importlib.util.spec_from_file_location("merc_patch_generated", PATCHER_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class PathSpawnDeathAccountingTests(unittest.TestCase):
    def test_one_departure_retires_one_positive_matching_slot(self):
        module = load_patcher()
        generated = GENERATED.read_text(encoding="utf-8")
        patched = module.patch_path_spawn_death_accounting(generated)

        marker = "/* RsPath departure accounting: retire one live slot */"
        self.assertEqual(patched.count(marker), 1)
        self.assertEqual(
            patched.count("if ((int16_t)MEM16(edx + esi * 2) > 0)"), 1
        )
        self.assertIn("goto loc_00122D2C;", patched)
        self.assertEqual(module.patch_path_spawn_death_accounting(patched), patched)

        body = patched.split("void sub_00122CF0(void)", 1)[1]
        body = body.split("\n}\n", 1)[0]
        self.assertEqual(
            body.count("MEM16(edx + esi * 2) = MEM16(edx + esi * 2) - 1;"),
            1,
        )


if __name__ == "__main__":
    unittest.main()

"""Verify the fixed-function shader uses the Xbox D3DFOG numeric ABI."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class D3D8FogModeTests(unittest.TestCase):
    def test_shader_matches_xbox_fog_enum(self) -> None:
        header = (ROOT / "src/d3d/d3d8_xbox.h").read_text(encoding="utf-8")
        shader = (ROOT / "src/d3d/d3d8_shaders.c").read_text(encoding="utf-8")
        self.assertIn("#define D3DFOG_EXP              1", header)
        self.assertIn("#define D3DFOG_EXP2             2", header)
        self.assertIn("#define D3DFOG_LINEAR           3", header)
        self.assertIn('"    if (mode == 3u) {\\n"  /* D3DFOG_LINEAR */', shader)
        self.assertIn('"    } else if (mode == 1u) {\\n"  /* D3DFOG_EXP */', shader)
        self.assertIn('"    } else if (mode == 2u) {\\n"  /* D3DFOG_EXP2 */', shader)


if __name__ == "__main__":
    unittest.main()

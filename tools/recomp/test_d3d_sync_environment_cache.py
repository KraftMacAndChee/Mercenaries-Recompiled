"""Keep disabled D3D synchronization diagnostics off the render hot path."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class D3DSyncEnvironmentCacheTests(unittest.TestCase):
    def test_trace_gate_is_cached(self):
        source = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
            encoding="utf-8"
        )
        body = re.search(
            r"void recomp_d3d_sync_checkpoint\(uint32_t phase\)\n\{.*?\n\}",
            source,
            re.S,
        )[0]
        self.assertIn("static int trace_enabled = -1;", body)
        self.assertEqual(body.count('getenv("MERCENARIES_TRACE_D3D_SYNC")'), 1)
        self.assertRegex(
            body,
            r"if \(trace_enabled < 0\)\s+"
            r'trace_enabled = getenv\("MERCENARIES_TRACE_D3D_SYNC"\) != NULL;\s+'
            r"if \(!trace_enabled\)\s+return;",
        )


if __name__ == "__main__":
    unittest.main()
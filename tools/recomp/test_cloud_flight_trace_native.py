from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
PGRAPH = ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c"


class CloudFlightTraceTests(unittest.TestCase):
    def test_trace_matches_authored_reset_strip_vertex_count(self):
        source = PGRAPH.read_text(encoding="utf-8")
        marker = 'pgraph_cached_getenv("MERCENARIES_TRACE_CLOUD_FLIGHT")'
        start = source.index(marker)
        predicate = source[start : start + 900]
        self.assertIn("count == 58u", predicate)
        self.assertIn("stride == 24u", predicate)
        self.assertNotIn("g_pg.transform_program_load == 12u", predicate)
        self.assertIn("read_le32(vertex_bytes + 12u) == 0x00808080u", predicate)
        self.assertIn("read_le32(vertex_bytes + stride + 12u) == 0x80808080u", predicate)
        self.assertIn("vertex_bytes + stride * 4u, 16u", predicate)
        self.assertIn("vertex_bytes + stride * 6u, 16u", predicate)
        self.assertNotIn("count == 4u", predicate)
        trace = source[start : start + 2400]
        self.assertIn("filter=%08X address=%08X texprog=%08X", trace)
        self.assertIn("cloud-comb0", trace)


if __name__ == "__main__":
    unittest.main()

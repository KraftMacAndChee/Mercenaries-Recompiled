from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
PGRAPH = ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c"


class TextureCacheRevalidationTests(unittest.TestCase):
    def test_static_texture_fast_path_accepts_same_binding_or_frame_validation(self):
        source = PGRAPH.read_text(encoding="utf-8")
        fast_path = source.index(
            "cache_entry->source_hash_valid && (compressed || swizzled)"
        )
        return_path = source.index("return cache_entry->texture;", fast_path)
        block = source[fast_path:return_path]

        self.assertIn("same_stage_binding", block)
        self.assertIn("last_validation_generation == g_pg.surface_generation", block)

    def test_static_texture_hash_records_current_render_generation(self):
        source = PGRAPH.read_text(encoding="utf-8")
        hash_call = source.index("source_hash = levels > 1u ?")
        next_section = source.index("if (packed_yuv", hash_call)
        block = source[hash_call:next_section]

        self.assertIn(
            "cache_entry->last_validation_generation = g_pg.surface_generation",
            block,
        )

    def test_binding_identity_covers_complete_texture_cache_key(self):
        source = PGRAPH.read_text(encoding="utf-8")
        start = source.index("same_stage_binding =")
        end = source.index("/* Xbox swizzled", start)
        block = source[start:end]

        for expression in (
            "guest_texture[stage]",
            "guest_texture_offset[stage]",
            "guest_texture_format[stage]",
            "guest_texture_rect[stage]",
            "guest_texture_pitch[stage]",
        ):
            self.assertIn(expression, block)


if __name__ == "__main__":
    unittest.main()

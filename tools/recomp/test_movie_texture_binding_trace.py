"""Regression coverage for the deterministic opening-movie texture trace."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "ports" / "mercenaries" / "src" /
          "recomp_manual.c").read_text(encoding="utf-8")
PATCHES = (ROOT / "ports" / "mercenaries" / "scripts" /
           "Patch-Generated.py").read_text(encoding="utf-8")


def test_movie_texture_trace_is_opt_in_bounded_and_reproducible():
    assert 'getenv("MERCENARIES_TRACE_MOVIE") == NULL' in SOURCE
    assert "movie_texture_samples++ < 128u" in SOURCE
    assert "guest_u32(object + 0x58u) == 0x1D6FB11Au" in SOURCE
    assert "[RETAIL-MOVIE-TEXTURE]" in SOURCE
    assert '"RedMovie brush texture setter checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(10u, esi, edi, eax,' in PATCHES
    assert '"RsFrontEnd movie brush commit checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(11u, ebp, MEM32(ebp + 0x44u),' in PATCHES
    assert '"RedBrush2D movie renderer-context checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(12u, esi, MEM32(esi + 0x44u),' in PATCHES
    assert 'recomp_movie_checkpoint(13u, esi, eax, MEM32(esi + 0x24u),' in PATCHES
    assert '"RedBrush2D renderer queue insertion checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(14u, edi, esi, MEM32(edi + 0x24u), eax);' in PATCHES
    assert 'stage >= 10u && stage <= 24u' in SOURCE
    assert '"RsFrontEnd movie paint-state checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(15u, esi, esi + 0x34u,' in PATCHES
    assert '"Red UI movie brush lookup entry checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(16u, ecx, eax, MEM32(0x793514u),' in PATCHES
    assert '"Red UI movie brush lookup result checkpoints"' in PATCHES
    assert 'recomp_movie_checkpoint(17u, eax, edi, MEM32(0x793514u),' in PATCHES
    assert '"Red UI movie brush lookup failure checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(18u, 0u, edi, MEM32(0x793514u),' in PATCHES
    assert '"Red UI movie command creation checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(19u, ecx, eax, MEM32(ecx),' in PATCHES
    assert '"Red UI movie command consumption checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(20u, edi, MEM32(esi + 8u),' in PATCHES
    assert '"Red renderer movie texture-cache checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(21u, esi, MEM32(esi + 0x44u), edi,' in PATCHES
    assert '"Red renderer movie texture-bind checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(22u, esi, eax, edi,' in PATCHES
    assert '"Retail movie SetTexture resource checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(23u, MEM32(esp + 8u), MEM32(esp + 4u),' in PATCHES
    assert '"Red renderer movie texture-bind completion checkpoint"' in PATCHES
    assert 'recomp_movie_checkpoint(24u, esi, MEM32(esi + 0x44u), edi,' in PATCHES


if __name__ == "__main__":
    test_movie_texture_trace_is_opt_in_bounded_and_reproducible()
    print("ok movie_texture_binding_trace")
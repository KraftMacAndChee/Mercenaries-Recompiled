"""Regression coverage for the shared NV2A PVIDEO scanout overlay."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PGRAPH = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
D3D = (ROOT / "src" / "d3d" / "d3d8_device.c").read_text(encoding="utf-8")
CORE = (ROOT / "src" / "nv2a" / "nv2a_core.c").read_text(encoding="utf-8")


def test_pvideo_registers_are_decoded_by_shared_runtime() -> None:
    block = PGRAPH[PGRAPH.index("static BOOL composite_pvideo_overlay") :]
    for register in (
        "NV_PVIDEO_BUFFER", "NV_PVIDEO_BASE", "NV_PVIDEO_LIMIT",
        "NV_PVIDEO_OFFSET", "NV_PVIDEO_SIZE_IN", "NV_PVIDEO_POINT_IN",
        "NV_PVIDEO_DS_DX", "NV_PVIDEO_DT_DY", "NV_PVIDEO_POINT_OUT",
        "NV_PVIDEO_SIZE_OUT", "NV_PVIDEO_FORMAT", "NV_PVIDEO_COLOR_KEY",
    ):
        assert register in block
    assert "relative_end > limit" in block
    assert "absolute_end > memory_region_size(d->vram)" in block
    assert "convert_packed_yuv_pixel(source, x, 0" in block
    assert "NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8" in block


def test_pvideo_composes_every_scanout_path() -> None:
    assert PGRAPH.count("composite_pvideo_overlay(surface);") == 2
    assert "composite_pvideo_overlay(NULL);" in PGRAPH
    assert "d3d8_CompositeVideoOverlay(" in PGRAPH
    assert "d3d8_CompositeVideoOverlay(" in D3D


def test_pvideo_scale_and_color_key_match_xemu_semantics() -> None:
    assert "floor(calculated_in / 1048576.0 + 0.5)" in PGRAPH
    assert "(calculated_in + 1.0) / (double)output_size" in PGRAPH
    assert "background_texture.Load" in D3D
    assert "uint3 actual" in D3D
    assert "if (any(actual != wanted)) discard" in D3D
    assert "DXGI_FORMAT_B8G8R8A8_UNORM" in D3D


def test_stop_disables_the_overlay() -> None:
    start = CORE.index("void pvideo_write")
    stop = CORE[start : CORE.index("/* ============================================================", start)]
    assert "d->pvideo.regs[NV_PVIDEO_BUFFER] = 0" in stop
#!/usr/bin/env python3
"""Regression guards for scaled render/depth surfaces reused as textures."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")

# Guest allocations must never use internally scaled host dimensions. A 640x480
# pitch-2560 target ends at offset + 0x12c000 regardless of a 4K host target.
assert "guest_surface_backing_dimensions(candidate->logical_width" in SOURCE
assert "(uint64_t)candidate->pitch * guest_height" in SOURCE
assert "(uint64_t)candidate->pitch *\n                                      candidate->height" not in SOURCE

# Like Xemu, a texture fallback must synchronize any dirty GPU depth allocation
# before hashing or uploading its guest-memory bytes.
prepare = SOURCE[SOURCE.index("static IDirect3DTexture8 *prepare_guest_texture") :]
sync = "download_guest_surfaces_in_range_if_dirty(offset, source_length);"
hash_start = "source_hash = levels > 1u ?"
assert sync in prepare
assert prepare.index(sync) < prepare.index(hash_start)
assert '"MERCENARIES_ENABLE_SURFACE_DEPTH_READBACK"' in prepare[: prepare.index(sync)]

download = SOURCE[
    SOURCE.index("static int download_guest_depth_surface_to_vram") :
    SOURCE.index("static void download_guest_surfaces_in_range_if_dirty")
]
assert "desc.Width /\n                                                     guest_width" in download
assert "desc.Height /\n                                             guest_height" in download
assert "guest_value = (host_value << 8) | (host_value >> 24);" in download
assert "surface->downloaded_serial = surface->write_serial;" in download

array_draw = SOURCE[
    SOURCE.index("static void submit_array_draw(void)") :
    SOURCE.index("static void submit_draw(void)")
]
assert "g_pg.bound_depth->write_serial = ++g_pg.surface_write_serial;" in array_draw

print("scaled surface texture synchronization regression passed")
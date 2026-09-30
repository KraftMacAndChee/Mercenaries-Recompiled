from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PGRAPH = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
RESOURCES = (ROOT / "src/d3d/d3d8_resources.c").read_text(encoding="utf-8")
INTERNAL = (ROOT / "src/d3d/d3d8_internal.h").read_text(encoding="utf-8")


assert "NV097_SET_TEXTURE_FORMAT_CUBEMAP_ENABLE" in PGRAPH
assert "cube_face_stride = (cube_face_length + 127u) & ~127ull" in PGRAPH
assert "source_length = cube_face_stride * 6u" in PGRAPH
assert "d3d8_CreateCubeTextureImpl" in PGRAPH
assert "for (face = 0u; face < 6u; ++face)" in PGRAPH
assert "d3d8_UpdateCubeTextureFace" in PGRAPH

assert "D3D11_RESOURCE_MISC_TEXTURECUBE" in RESOURCES
assert "D3D11_SRV_DIMENSION_TEXTURECUBE" in RESOURCES
assert "td.ArraySize = cubemap ? 6 : 1" in RESOURCES
assert "subresource = Level + Face * tex->levels" in RESOURCES
assert "BOOL                    cubemap" in INTERNAL

print("nv2a cubemap upload regression test: PASS")

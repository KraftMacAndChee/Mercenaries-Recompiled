from pathlib import Path


SOURCE = (Path(__file__).parents[2] / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)


assert '"MERCENARIES_CAPTURE_FLARE_TEXTURE_PREFIX"' in SOURCE
assert 'g_pg.blend_sfactor == NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA' in SOURCE
assert 'g_pg.blend_dfactor == NV097_SET_BLEND_FUNC_DFACTOR_V_ONE' in SOURCE
assert 'd3d8_DebugCaptureTextureToPath(source->texture, path);' in SOURCE
assert 'target=%08X mask=%08X' in SOURCE

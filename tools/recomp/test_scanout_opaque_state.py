#!/usr/bin/env python3
"""Regression guard for opaque, state-isolated PCRTC scanout."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "d3d" / "d3d8_device.c").read_text(encoding="utf-8")

start = SOURCE.index("BOOL d3d8_CopyTextureToBackbuffer")
end = SOURCE.index("typedef struct D3D8PvideoConstants", start)
scanout = SOURCE[start:end]

# Xbox scanout consumes RGB; the render target's alpha remains available to
# in-frame effects but must not make the host presentation transparent/black.
assert 'return float4(c.rgb,1.0);' in SOURCE

# The resolve draw must not inherit guest flare/UI blend or depth state, and
# every state object acquired from D3D11 must be restored and released.
for call in (
    "ID3D11DeviceContext_OMGetBlendState",
    "ID3D11DeviceContext_OMGetDepthStencilState",
    "NULL, NULL, ~0u",
    "NULL, 0u",
    "old_blend, old_blend_factor",
    "old_depth,",
    "old_stencil_ref",
    "ID3D11BlendState_Release(old_blend)",
    "ID3D11DepthStencilState_Release(old_depth)",
):
    assert call in scanout, call

assert scanout.index("NULL, NULL, ~0u") < scanout.index("ID3D11DeviceContext_Draw")
assert scanout.index("ID3D11DeviceContext_Draw") < scanout.rindex("old_blend, old_blend_factor")

print("opaque scanout state isolation regression passed")
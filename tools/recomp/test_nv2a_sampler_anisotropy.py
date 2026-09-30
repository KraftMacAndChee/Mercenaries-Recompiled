#!/usr/bin/env python3
"""Static regression coverage for NV2A anisotropic sampler state."""

from pathlib import Path

SOURCE = (
    Path(__file__).resolve().parents[2]
    / "src"
    / "nv2a"
    / "nv2a_pgraph_d3d11.c"
).read_text(encoding="utf-8")

assert "NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY" in SOURCE
assert "1u << ((control0 & NV_PGRAPH_TEXCTL0_0_MAX_ANISOTROPY) >> 4)" in SOURCE
assert "host_min = D3DTEXF_ANISOTROPIC;" in SOURCE
assert "D3DTSS_MAXANISOTROPY" in SOURCE

print("PASS: NV2A max anisotropy is forwarded to the D3D11 sampler")

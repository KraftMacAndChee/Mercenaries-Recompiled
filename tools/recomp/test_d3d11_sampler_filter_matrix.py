"""Static regression check for the complete D3D11 sampler filter matrix."""
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[2] / "src/d3d/d3d8_states.c").read_text(
    encoding="utf-8"
)

expected = {
    "D3D11_FILTER_MIN_MAG_MIP_POINT",
    "D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR",
    "D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT",
    "D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR",
    "D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT",
    "D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR",
    "D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT",
    "D3D11_FILTER_MIN_MAG_MIP_LINEAR",
}
missing = sorted(name for name in expected if name not in SOURCE)
assert not missing, f"missing D3D11 filter mappings: {missing}"
assert "switch ((min_linear ? 4u : 0u)" in SOURCE
print("PASS: complete point/linear min-mag-mip filter matrix is mapped")
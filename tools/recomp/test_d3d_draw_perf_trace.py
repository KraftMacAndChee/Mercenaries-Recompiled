from pathlib import Path


SOURCE = (Path(__file__).parents[2] / "src/d3d/d3d8_device.c").read_text(
    encoding="utf-8"
)

assert 'd3d8_cached_getenv("MERCENARIES_TRACE_D3D_DRAW_PERF")' in SOURCE
assert "[D3D-DRAW-PERF] calls=%llu bytes=%llu upload_ms=%.3f" in SOURCE
assert "perf_after_upload.QuadPart - perf_begin.QuadPart" in SOURCE
assert "perf_after_vertex.QuadPart - perf_after_upload.QuadPart" in SOURCE
assert "perf_after_pixel.QuadPart - perf_after_vertex.QuadPart" in SOURCE
assert "perf_after_state.QuadPart - perf_after_pixel.QuadPart" in SOURCE
assert "perf_after_issue.QuadPart - perf_after_state.QuadPart" in SOURCE
assert "perf_after_issue.QuadPart - perf_begin.QuadPart" in SOURCE

print("D3D DrawPrimitiveUP performance trace checks passed")
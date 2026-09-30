"""Regression coverage for opt-in static-texture validation timing."""

from pathlib import Path


SOURCE = Path("src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")


assert '"MERCENARIES_TRACE_TEXTURE_PERF"' in SOURCE
assert '"[PGRAPH-TEXTURE-PERF] hashes=%llu bytes=%llu hash_ms=%.3f "' in SOURCE
assert "static int texture_validation_perf_enabled(void)" in SOURCE
assert "if (trace_perf)" in SOURCE
assert "QueryPerformanceCounter(&hash_begin);" in SOURCE
assert "hash_end.QuadPart - hash_begin.QuadPart" in SOURCE
assert "if (texture_validation_perf_enabled())" in SOURCE
assert "trace_texture_validation_perf(0u, 0, 1);" in SOURCE
assert "pgraph_cached_getenv" in SOURCE

print("NV2A texture validation performance trace checks passed")

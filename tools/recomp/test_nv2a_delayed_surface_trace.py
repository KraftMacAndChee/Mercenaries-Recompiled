"""Surface tracing can be deferred until retail flare/effect passes."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)

assert '"MERCENARIES_TRACE_SURFACE_DELAY_MS"' in SOURCE
assert "GetTickCount64() - trace_start_ms >= delay_ms" in SOURCE
assert SOURCE.count("pgraph_trace_surface_textures_enabled()") >= 6
assert SOURCE.count('pgraph_cached_getenv("MERCENARIES_TRACE_SURFACE_ALIASES")') == 3
assert SOURCE.count("trace_count++ < 128u") >= 1
assert "if (trace_count++ >= 128u)" in SOURCE
assert "g_debug_surface_method_trace_count++ < 1024u" in SOURCE
assert SOURCE.count("g_debug_surface_method_trace_count++ < 1024u") == 4

print("delayed surface trace: ok")

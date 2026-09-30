"""The texture-edge trace can be deferred until the gameplay scene."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(
    encoding="utf-8"
)

assert '"MERCENARIES_TRACE_TEXTURE_EDGES_DELAY_MS"' in SOURCE
assert "GetTickCount64() - trace_start_ms < delay_ms" in SOURCE
assert SOURCE.index("GetTickCount64() - trace_start_ms < delay_ms") < SOURCE.index(
    "candidate_count >= 256u"
)

print("delayed texture-edge trace: ok")

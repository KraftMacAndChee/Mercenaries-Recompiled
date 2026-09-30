#!/usr/bin/env python3
"""Static regression checks for generic NV2A texture color-key handling."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PGRAPH = (ROOT / "src/nv2a/nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
COMBINER_C = (ROOT / "src/d3d/d3d8_combiners.c").read_text(encoding="utf-8")
COMBINER_H = (ROOT / "src/d3d/d3d8_combiners.h").read_text(encoding="utf-8")

checks = {
    "four consecutive stage registers": all(
        token in PGRAPH for token in (
            "case NV097_SET_COLOR_KEY_COLOR:",
            "case NV097_SET_COLOR_KEY_COLOR + 4:",
            "case NV097_SET_COLOR_KEY_COLOR + 8:",
            "case NV097_SET_COLOR_KEY_COLOR + 12:",
        )
    ),
    "per-stage register storage": "g_pg.color_key[stage] = param;" in PGRAPH,
    "control0 color-key mode forwarding":
        "NV_PGRAPH_TEXCTL0_0_COLORKEYMODE" in PGRAPH and
        PGRAPH.count("d3d8_combiners_set_color_key(") >= 2,
    "X-format alpha mask": PGRAPH.count("return 0x00FFFFFFu;") >= 1,
    "constant-buffer aligned state": all(
        token in COMBINER_H for token in (
            "UINT  color_key_mode_bits;",
            "UINT  _color_key_pad[3];",
            "UINT  color_key[NV2A_MAX_TEXTURES];",
            "UINT  color_key_mask[NV2A_MAX_TEXTURES];",
        )
    ),
    "shader compares packed ARGB":
        "uint sampled_key = (key_c.a << 24) | (key_c.r << 16) |" in COMBINER_C,
    "mode 1 kills alpha": "if (key_mode == 1u) r_t%d.a = 0.0;" in COMBINER_C,
    "mode 2 kills color and alpha": "else r_t%d = float4(0, 0, 0, 0);" in COMBINER_C,
    "mode 3 discards": "if (key_mode == 3u) discard;" in COMBINER_C,
    "constant upload": all(
        token in COMBINER_C for token in (
            "constants.color_key_mode_bits = g_color_key_mode_bits;",
            "memcpy(constants.color_key, g_color_key, sizeof(constants.color_key));",
            "memcpy(constants.color_key_mask, g_color_key_mask,",
        )
    ),
}

failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("FAIL: " + ", ".join(failed))
print("PASS: NV2A texture color-key register, mask, and shader semantics are wired")

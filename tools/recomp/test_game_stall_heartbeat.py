"""The hidden route watchdog must not depend on full entry tracing."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "ports/mercenaries/src/recomp/recomp_types.h").read_text(
    encoding="utf-8"
)
STATE = (ROOT / "src/kernel/xbox_memory_layout.c").read_text(encoding="utf-8")
MANUAL = (ROOT / "ports/mercenaries/src/recomp_manual.c").read_text(
    encoding="utf-8"
)

assert "if (g_recomp_watchdog_heartbeat_enabled)" in HEADER
assert "++g_recomp_watchdog_heartbeat" in HEADER
assert "volatile uint32_t g_recomp_watchdog_heartbeat = 0" in STATE
assert "last_heartbeat = g_recomp_watchdog_heartbeat" in MANUAL
assert "heartbeat = g_recomp_watchdog_heartbeat" in MANUAL
assert "uint32_t last_index = g_recomp_recent_game_func_idx" not in MANUAL

print("hidden-route stall heartbeat: ok")

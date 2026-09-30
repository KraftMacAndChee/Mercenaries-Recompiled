"""Regression coverage for retail guest KTIMER/DPC delivery.

Mercenaries' XACT scheduler queues delayed cue events through KeSetTimer.
The bridge must defer the timer on the host tick and invoke its retail DPC
only at a translated game-thread safe point.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BRIDGE = (ROOT / "src" / "kernel" / "kernel_bridge.c").read_text(
    encoding="utf-8")
TYPES = (ROOT / "ports" / "mercenaries" / "src" / "recomp" /
         "recomp_types.h").read_text(encoding="utf-8")
DISPATCH = (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" /
            "recomp_dispatch.c").read_text(encoding="utf-8")
GENERATED = (ROOT / "ports" / "mercenaries" / "src" / "recomp" / "gen" /
             "recomp_0012.c").read_text(encoding="utf-8")
SEEDS = (ROOT / "ports" / "mercenaries" /
         "manual-seeds.json").read_text(encoding="utf-8")

assert "static void bridge_KeSetTimer(void)" in BRIDGE
assert "static void bridge_KeSetTimerEx(void)" in BRIDGE
assert "static void bridge_KeCancelTimer(void)" in BRIDGE
assert "static int bridge_deliver_guest_timers(void)" in BRIDGE
assert "xbox_kernel_service_guest_timers();" in TYPES
assert "g_kernel_pending_guest_timers != 0" in TYPES
assert "case  97: return bridge_KeCancelTimer;" in BRIDGE
assert "case 149: return bridge_KeSetTimer;" in BRIDGE
assert "case 150: return bridge_KeSetTimerEx;" in BRIDGE
assert "Timer functionality is not needed for basic execution" not in BRIDGE

assert "void sub_002830EC(void)" in GENERATED
assert "esp += 20; return; /* ret 16 */" in GENERATED
assert "{ 0x002830ECu, (recomp_func_t)sub_002830EC }" in DISPATCH
assert '"start": "0x002830EC"' in SEEDS

# XACT calls this method only through its engine interface vtable. If
# function discovery drops the entry, cues can be accepted while the APU mix
# remains silent. Keep the retail entry and its stdcall-style ret 12 cleanup
# reproducible from the seed manifest.
assert "void sub_00283884(void)" in GENERATED
assert "esp += 16; return; /* ret 12 */" in GENERATED
assert "{ 0x00283884u, (recomp_func_t)sub_00283884 }" in DISPATCH
assert '"start": "0x00283884"' in SEEDS

print("ok retail_guest_timer_dpc_and_xact_vtable")
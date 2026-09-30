"""Keep the retail traffic-path reader reachable after XBE regeneration."""

from pathlib import Path

from generated_test_utils import generated_text_containing


ROOT = Path(__file__).resolve().parents[2]
GENERATED = generated_text_containing("RECOMP_TRACE_FUNC(0x00124870u)")
DISPATCH = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_dispatch.c").read_text(
    encoding="utf-8"
)
FUNCS = (ROOT / "ports/mercenaries/src/recomp/gen/recomp_funcs.h").read_text(
    encoding="utf-8"
)

# RsPath::_Read is the virtual deserializer that registers authored vehicle and
# pedestrian paths with RsTrafficManager.  If function discovery trims its
# vtable entry, all path objects still allocate but ambient traffic and NPCs
# disappear from the world.
assert "void sub_00124870(void)" in GENERATED
assert "void sub_00124870(void);" in FUNCS
assert "{ 0x00124870u, (recomp_func_t)sub_00124870 }," in DISPATCH
assert "sub_0016A7C0(); /* call 0x0016A7C0 */" in GENERATED
assert "sub_0016A7E0(); /* call 0x0016A7E0 */" in GENERATED

print("ok traffic_path_reader_dispatch")

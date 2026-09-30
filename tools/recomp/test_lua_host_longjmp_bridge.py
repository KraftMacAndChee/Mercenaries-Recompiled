"""Regression coverage for Lua protected-call host setjmp/longjmp bridging."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TYPES = ROOT / "ports/mercenaries/src/recomp/recomp_types.h"
MANUAL = ROOT / "ports/mercenaries/src/recomp_manual.c"
LUA_GEN = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0009.c"
CRT_GEN = ROOT / "ports/mercenaries/src/recomp/gen/recomp_0011.c"
PATCHER = ROOT / "ports/mercenaries/scripts/Patch-Generated.py"


def test_lua_host_longjmp_bridge() -> None:
    types = TYPES.read_text(encoding="utf-8")
    manual = MANUAL.read_text(encoding="utf-8")
    lua_gen = LUA_GEN.read_text(encoding="utf-8")
    crt_gen = CRT_GEN.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")

    assert "#include <setjmp.h>" in types
    assert "RECOMP_LUA_HOST_SETJMP" in types
    assert "void *recomp_lua_host_jmp_register" in manual
    assert "uint32_t recomp_lua_host_longjmp" in manual
    assert "g_esp = *(volatile uint32_t *)(base + guest_buffer + 0x10u) + 4u;" in manual
    assert "*(volatile uint32_t *)(base + 0u)" in manual
    assert "RECOMP_LUA_HOST_SETJMP(_lua_jmpbuf)" in lua_gen
    assert lua_gen.count("recomp_lua_host_jmp_pop(ebp + -68);") == 2
    assert "recomp_lua_host_longjmp(MEM32(esp + 4), MEM32(esp + 8))" in crt_gen
    assert "retail Lua protected-call host setjmp bridge" in patcher
    assert "retail Lua longjmp host-frame transfer" in patcher


if __name__ == "__main__":
    test_lua_host_longjmp_bridge()
    print("Lua host longjmp bridge checks passed")
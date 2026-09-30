"""Regression coverage for translated guest RtlUnwind dispatch."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BRIDGE = ROOT / "src" / "kernel" / "kernel_bridge.c"


def test_rtl_unwind_is_observable_and_dispatched() -> None:
    source = BRIDGE.read_text(encoding="utf-8")

    assert "static void bridge_RtlUnwind(void)" in source
    assert "[KERNEL-RTL-UNWIND]" in source
    assert "case 312: return bridge_RtlUnwind;" in source
    assert "g_eax = return_value;" in source


if __name__ == "__main__":
    test_rtl_unwind_is_observable_and_dispatched()
    print("RtlUnwind bridge checks passed")

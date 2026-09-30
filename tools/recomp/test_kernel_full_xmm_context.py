"""Regression test for full XMM preservation across guest callbacks."""

from pathlib import Path


def main() -> None:
    source = (
        Path(__file__).parents[2] / "src" / "kernel" / "kernel_bridge.c"
    ).read_text(encoding="utf-8")

    assert "float xmm[8][4];" in source
    for index in range(8):
        assert (
            f"memcpy(context->xmm[{index}], g_xmm{index}, "
            f"sizeof(context->xmm[{index}]));"
        ) in source
        assert (
            f"memcpy(g_xmm{index}, context->xmm[{index}], "
            f"sizeof(context->xmm[{index}]));"
        ) in source

    assert "uint32_t save_eax = g_eax" not in source
    assert "bridge_guest_cpu_context saved_context;" in source

    print("ok full XMM guest callback context")


if __name__ == "__main__":
    main()
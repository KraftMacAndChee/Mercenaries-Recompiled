#!/usr/bin/env python3
"""Regression checks for the shared-runtime DSP56300 integration."""

from __future__ import annotations

import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
APU = ROOT / "src" / "apu"
VENDOR = ROOT / "third_party" / "dsp56300" / "msvc-x64-v0.1.3"

EXPECTED = {
    VENDOR / "include" / "dsp56300.h":
        "56CC51CF3F72616102077DF74B4A220D9D6717D7FD0F5F5A261703954DD8EB6C",
    VENDOR / "lib" / "dsp56300_emu_ffi.lib":
        "73C8C0373BFCB6A6EF97DCE70CD979AE0897D07441A1C0A552BF8C87A05AE625",
    VENDOR / "LICENSE":
        "73F2274B38A798D9FD1B08956BCD8691C0CF7549AFE6704B9337DF9F2E14CB57",
}


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise AssertionError(f"missing {label}: {needle}")


def main() -> None:
    for path, expected in EXPECTED.items():
        actual = hashlib.sha256(path.read_bytes()).hexdigest().upper()
        if actual != expected:
            raise AssertionError(f"hash mismatch for {path}: {actual}")

    cmake = (APU / "CMakeLists.txt").read_text(encoding="utf-8")
    dsp = (APU / "apu_dsp.c").read_text(encoding="utf-8")
    core = (APU / "apu_core.c").read_text(encoding="utf-8")
    state = (APU / "apu_state.h").read_text(encoding="utf-8")
    route = (ROOT / "tools" / "recomp" /
             "Run-HiddenRetailRoute.ps1").read_text(encoding="utf-8")

    require(cmake, "msvc-x64-v0.1.3", "pinned vendor path")
    require(cmake, "dsp56300_emu_ffi.lib", "DSP static library")
    for library in ("ws2_32", "ntdll", "userenv", "bcrypt", "advapi32"):
        require(cmake, library, f"Rust Windows dependency {library}")

    require(dsp, "dsp56300_create", "JIT creation")
    require(dsp, "dsp56300_run", "JIT execution")
    for memory in ("xram", "yram", "pram"):
        require(dsp, f"memset(dsp->core.{memory}, 0xCA,",
                f"Xemu-compatible {memory} power-on fill")
    require(dsp, "x_regions[1].start = 0x1400", "mix-buffer alias start")
    require(dsp, "x_regions[1].data.buffer.offset = 0x0c00",
            "mix-buffer XRAM alias")
    require(dsp, "Dsp56300MemoryRegion y_regions[2]",
            "MCPX Y-memory views")
    require(dsp, "y_regions[0].start = 0; y_regions[0].end = 0x0800",
            "Xemu-compatible MCPX YRAM range")
    require(dsp, "y_regions[1].start = 0x0800; y_regions[1].end = 0x1800",
            "MCPX high Y-space program-memory alias")
    require(dsp, "y_regions[1].data.buffer.base = dsp->core.pram",
            "bounded MCPX high Y-space backing")
    require(dsp, "info.memory_map.y_count = 2",
            "bounded MCPX Y-space region count")
    require(dsp, "info.address_register_mask = 0x0000ffffu",
            "MCPX 16-bit address-register execution profile")
    require(dsp, "dsp_dma_run", "DSP DMA engine")
    require(dsp, "value = (uint32_t)v16 << 8;",
            "MCPX 16-bit to 24-bit DMA expansion")
    require(dsp, "memory_address + i, value);",
            "untruncated DSP DMA writeback")
    if "memory_address + i, value & item_mask" in dsp:
        raise AssertionError(
            "16-bit DSP DMA writeback must retain the shifted high byte"
        )
    peripheral = dsp[dsp.index("static uint32_t dsp_read_peripheral"):
                     dsp.index("static void dsp_write_peripheral")]
    require(peripheral, "case 0xffffb3u:", "MCPX DSP timing peripheral")
    require(peripheral, "return 0u;", "Xemu-compatible timing value")
    if "dsp56300_cycle_count" in peripheral:
        raise AssertionError(
            "X:$FFFFB3 must not expose the host JIT execution counter"
        )
    xemu_dsp = (ROOT / "references" / "xemu" / "hw" / "xbox" / "mcpx" /
                "apu" / "dsp" / "dsp.c").read_text(encoding="utf-8")
    xemu_peripheral = xemu_dsp[xemu_dsp.index("uint32_t read_peripheral"):
                               xemu_dsp.index("void write_peripheral")]
    require(xemu_peripheral, "case 0xFFFFB3:", "Xemu timing peripheral")
    require(xemu_peripheral, "v = 0;", "Xemu timing value")
    require(dsp, "dsp_bootstrap", "DSP scratch bootstrap")
    require(dsp, "MCPX_APU_DEBUG_MON_GP_OR_EP", "real DSP monitor")
    require(dsp, "MERCENARIES_ENABLE_APU_EP_DSP", "experimental EP opt-in")
    require(dsp, "MERCENARIES_ENABLE_APU_DSP", "full DSP opt-in")
    require(dsp, "MERCENARIES_DISABLE_APU_DSP", "diagnostic fallback override")
    require(dsp, 'enabled = getenv("MERCENARIES_ENABLE_APU_DSP") != NULL &&',
            "full DSP guarded default")
    require(dsp, "d->monitor.point = MCPX_APU_DEBUG_MON_GP;",
            "GP output while EP is unavailable")
    require(dsp, "EP X:063C change", "EP loop-control provenance trace")
    require(dsp, "static void dsp_reset_mcpx", "MCPX-compatible DSP reset helper")
    require(dsp, "state.registers[DSP56300_REG_OMR] = 0x000002u",
            "Xemu-compatible OMR reset state")
    require(dsp, "state.registers[DSP56300_REG_M0 + i] = 0x0000ffffu",
            "MCPX-width linear modulo reset state")
    require(dsp, "state.interrupts.ipl[interrupt_slots[i]] = 3",
            "MCPX exception IPL reset state")
    require(dsp, "MERCENARIES_DUMP_APU_DSP_STATE_PATH",
            "one-shot maintained DSP state diagnostic")
    require(dsp, "MERCENARIES_DUMP_APU_DSP_FAILURE_STATE_PATH",
            "late DSP failure-state diagnostic")
    require(dsp, "static void dsp_dma_fail", "one-shot DSP DMA failure reason")
    require(dsp, '"invalid-fifo-parameters"',
            "DSP FIFO failure attribution")
    require(route, "ep-preloop-state.bin", "DSP state diagnostic route output")
    require(route, "ep-failure-state.bin",
            "DSP failure-state diagnostic route output")
    if dsp.count("dsp56300_reset(") != 1:
        raise AssertionError("all DSP reset sites must route through dsp_reset_mcpx")
    require(route, "[switch]$TraceApuDspState", "DSP state route switch")
    require(route, "MERCENARIES_TRACE_APU_DSP_N5", "DSP state trace wiring")
    require(core, "mcpx_apu_dsp_reset_write", "reset edge routing")
    require(core, "mcpx_apu_dsp_finalize", "DSP lifetime cleanup")
    require(core, "GP_DSP_MIXBUF_BASE", "MMIO mix-buffer alias")
    require(state, "Dsp56300Jit *jit", "opaque JIT state")

    forbidden = ("ports/mercenaries/src/recomp/gen",)
    for path, text in ((APU / "apu_dsp.c", dsp),
                       (APU / "CMakeLists.txt", cmake)):
        for marker in forbidden:
            if marker in text:
                raise AssertionError(f"non-portable marker in {path}: {marker}")

    print("APU DSP56300 integration regression checks passed")


if __name__ == "__main__":
    main()

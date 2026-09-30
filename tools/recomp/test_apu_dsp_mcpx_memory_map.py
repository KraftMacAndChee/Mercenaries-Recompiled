from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DSP = (ROOT / "src" / "apu" / "apu_dsp.c").read_text(encoding="utf-8")


def test_mcpx_high_y_window_explicitly_aliases_program_store():
    """Keep the retail EP's high-Y table reads independent of host layout."""
    assert "Dsp56300MemoryRegion y_regions[2]" in DSP
    assert "y_regions[1].start = 0x0800" in DSP
    assert "y_regions[1].end = 0x1800" in DSP
    assert "y_regions[1].data.buffer.base = dsp->core.pram" in DSP
    assert "y_regions[1].data.buffer.offset = 0" in DSP
    assert "info.memory_map.y_count = 2" in DSP


def test_mcpx_alias_documents_the_retail_firmware_probe():
    assert "reads Y:$fc9 and receives P:$7c9" in DSP


if __name__ == "__main__":
    test_mcpx_high_y_window_explicitly_aliases_program_store()
    test_mcpx_alias_documents_the_retail_firmware_probe()

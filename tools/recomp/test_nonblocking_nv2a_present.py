"""Static regression check for nonblocking NV2A FLIP_STALL presentation."""
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[2] / "src/d3d/d3d8_device.c").read_text(
    encoding="utf-8"
)
start = SOURCE.index("void d3d8_PresentFrame(void)")
end = SOURCE.index("void d3d8_DebugCaptureFrameNow(void)", start)
block = SOURCE[start:end]
assert "preview_present(g_device_state.swap_chain, 0, 0);" in block
assert "preview_present(g_device_state.swap_chain, 1, 0);" not in block
assert "d3d8_WaitForGuestFrameSlot();" in block
assert "deferred AA resolve uses the same clock" in block
assert "frequency.QuadPart / g_frame_cap_fps" in SOURCE
assert 'getenv("MERCENARIES_DISABLE_FLIP_PACING")' in SOURCE
print("PASS: direct NV2A FLIP_STALL uses the shared guest-frame clock without DXGI VSync")
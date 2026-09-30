"""Guard Xemu-compatible TV encoder field-pin semantics for XMV pacing."""
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[2] / "src/kernel/kernel_hal.c").read_text()
start = SOURCE.index("static ULONG xbox_av_current_field(void)")
end = SOURCE.index("\n}\n", start) + 3
function = SOURCE[start:end]

assert "static ULONG field_pin;" in function
assert "field_pin = (field_pin + 1u) & 1u;" in function
assert "return field_pin;" in function
assert "QueryPerformanceCounter" not in function
assert "GetTickCount" not in function
print("ok xemu_field_pin")
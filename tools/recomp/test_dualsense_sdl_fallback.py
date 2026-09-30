from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/input/dualsense_sdl.c").read_text(encoding="utf-8")
INPUT = (ROOT / "src/input/xinput_device.c").read_text(encoding="utf-8")
CMAKE = (ROOT / "src/input/CMakeLists.txt").read_text(encoding="utf-8")

assert 'LoadLibraryA("SDL2.dll")' in SOURCE
assert "SDL_CONTROLLER_TYPE_PS5_VALUE = 7" in SOURCE
assert "SDL_CONTROLLER_TYPE_PS4_VALUE = 4" in SOURCE
assert "type == SDL_CONTROLLER_TYPE_PS4_VALUE" in SOURCE
assert "type == SDL_CONTROLLER_TYPE_PS5_VALUE" in SOURCE
assert "open_first_playstation_controller" in SOURCE
assert "now - g_last_rescan_ms >= 1000u" in SOURCE
assert "if (port != 0u)" in SOURCE
assert "sdl_result = dualsense_sdl_get_state(dwPort, &sdl_state);" in INPUT
assert 'getenv("MERCENARIES_TRACE_CONTROLLER")' in INPUT
assert 'getenv("MERCENARIES_TRACE_CONTROLLER_AXES")' in INPUT
assert '"[INPUT-AXES] source=%s raw=%d,%d extrema=%d..%d,%d..%d "' in INPUT
assert 'sdl_result == ERROR_SUCCESS ? "sdl-playstation"' in INPUT
assert "#define HOST_STICK_OUTER_RIM_ENTER 30000" in INPUT
assert "#define HOST_STICK_OUTER_RIM_RELEASE 29500" in INPUT
assert "static void normalize_host_stick_outer_rim(" in INPUT
assert "magnitude_sq < enter_sq" in INPUT
assert "magnitude_sq < release_sq" in INPUT
assert "*latched = TRUE" in INPUT
assert "*latched = FALSE" in INPUT
assert "sqrt((double)magnitude_sq)" in INPUT
assert INPUT.index("trace_controller_axes(dwPort,") < INPUT.index(
    "normalize_host_stick_outer_rims(dwPort, pState);")
assert '"[INPUT-SDL] PlayStation controller active on Xbox port %lu "' in INPUT
assert "result != ERROR_SUCCESS && sdl_result != ERROR_SUCCESS" in INPUT
assert "pState->Gamepad.sThumbRX = sdl_state.Gamepad.sThumbRX;" in INPUT
assert "dualsense_sdl_set_vibration(dwPort, pVibration) == ERROR_SUCCESS" in INPUT
assert "XBOX_BUTTON_BLACK] = g_sdl.get_button(controller, SDL_BUTTON_RIGHTSHOULDER)" in SOURCE
assert "XBOX_BUTTON_WHITE] = g_sdl.get_button(controller, SDL_BUTTON_LEFTSHOULDER)" in SOURCE
assert "XBOX_BUTTON_BLACK] =\n            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)" in INPUT
assert "XBOX_BUTTON_WHITE] =\n            (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)" in INPUT
assert INPUT.index("XInputGetState(dwPort, &xi_state)") < INPUT.index(
    "sdl_result = dualsense_sdl_get_state(dwPort, &sdl_state);")
assert "dualsense_sdl.c" in CMAKE


def normalize_outer_rim(x: int, y: int, latched: bool):
    magnitude_sq = x * x + y * y
    if not latched:
        if magnitude_sq < 30000 * 30000:
            return x, y, False
        latched = True
    elif magnitude_sq < 29500 * 29500:
        return x, y, False
    if 0 < magnitude_sq < 32767 * 32767:
        scale = 32767 / (magnitude_sq ** 0.5)
        x = round(x * scale)
        y = round(y * scale)
    return x, y, latched


# Measured full-forward DualSense Y (~30197) fell below Mercenaries' retail
# squared-magnitude run threshold before endpoint normalization. Pure forward
# now clears it immediately after centering, while non-rim analog values remain
# untouched; a diagonal is no longer needed to seed the running state.
raw_forward_sq = (30197 * 30197) / float(32768 * 32768)
fixed_x, fixed_y, latched = normalize_outer_rim(0, 30197, False)
fixed_forward_sq = (fixed_x * fixed_x + fixed_y * fixed_y) / float(32768 * 32768)
assert raw_forward_sq < 0.95 < fixed_forward_sq
assert latched
assert normalize_outer_rim(0, 29999, False) == (0, 29999, False)
# A held physical rim can jitter just below the entry point without changing
# the guest input, which covers the captured post-jump 29,950-equivalent poll.
assert normalize_outer_rim(0, 29950, latched) == (0, 32767, True)
assert normalize_outer_rim(0, 29499, latched) == (0, 29499, False)

# Exact cardinal directions and diagonals all cross the unchanged retail
# squared-magnitude threshold after host-rim normalization. This is the user
# reproduction: center -> forward/backward must classify the same as diagonal.
def guest_magnitude_squared(x: int, y: int) -> float:
    nx, ny, _ = normalize_outer_rim(x, y, False)
    return (nx * nx + ny * ny) / float(32768 * 32768)


for x, y in ((0, 30197), (0, -30197), (30197, 0), (-30197, 0)):
    assert guest_magnitude_squared(x, y) > 0.95
for x, y in ((23000, 23000), (-23000, 23000),
             (23000, -23000), (-23000, -23000)):
    assert guest_magnitude_squared(x, y) > 0.95
print("ok dualsense_sdl_player_one_merge")
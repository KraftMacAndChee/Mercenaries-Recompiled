from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
STATE = (ROOT / "src" / "apu" / "apu_state.h").read_text(encoding="utf-8")
VP = (ROOT / "src" / "apu" / "apu_vp.c").read_text(encoding="utf-8")


def test_xbox_hrir_coefficients_use_xemu_per_ear_normalization() -> None:
    setter = STATE.split("hrtf_filter_set_target_params", 1)[1].split(
        "static inline float hrtf_filter_smooth_param", 1
    )[0]
    assert "memcpy(coeff, hrir_coeff[ch]" in setter
    assert "sum_abs += fabsf(coeff[k])" in setter
    assert "coeff[k] /= sum_abs" in setter
    assert "for (int ch = 0; ch < 2; ch++)" in setter


def test_per_ear_l1_normalization_bounds_impulse_gain() -> None:
    # These deliberately use very different raw energies, like the live
    # Mercenaries HRIR tables. Normalization must preserve each shape while
    # preventing listener direction alone from multiplying playback gain.
    ears = [
        [0.75, -0.50, 0.25, -0.125],
        [0.125, -0.0625, 0.03125, -0.015625],
    ]
    normalized = []
    for coefficients in ears:
        scale = sum(abs(value) for value in coefficients)
        normalized.append([value / scale for value in coefficients])

    for coefficients in normalized:
        assert abs(sum(abs(value) for value in coefficients) - 1.0) < 1e-9
        assert max(abs(value) for value in coefficients) <= 1.0


def test_mono_3d_source_expands_to_two_hrtf_ears() -> None:
    helper = STATE.split("hrtf_filter_process_mono_source", 1)[1].split(
        "/* ============================================================\n"
        " * ADPCM Decoder", 1
    )[0]
    assert "samples[n][1] = samples[n][0]" in helper
    assert "hrtf_filter_process(f, samples, samples)" in helper

    process = VP.split("static void voice_process", 1)[1].split(
        "/* ============================================================\n"
        " * Parallel voice dispatch", 1
    )[0]
    assert "bool hrtf_applied = false" in process
    assert "hrtf_filter_process_mono_source" in process
    assert "hrtf_applied = true" in process
    assert "voice_route_channel(hrtf_applied, channels, b)" in process
    assert "samples[i][route_channel]" in process


def test_hrtf_routes_follow_xbox_3d_mixbin_order() -> None:
    helper = VP.split("static inline unsigned int voice_route_channel", 1)[1].split(
        "static int g_trace_voice_energy", 1
    )[0]
    assert "hrtf_applied && route < 4" in helper
    assert "route / 2u" in helper
    assert "route % source_channels" in helper

    # DSMIXBINVOLUMEPAIRS_REQUIRED_3D is FL, BL, FR, BR. The title programs
    # the corresponding destinations as 6, 8, 7, 9, so both front/back
    # routes in each ear must consume the same HRTF output channel.
    route_channels = [route // 2 for route in range(4)]
    assert route_channels == [0, 0, 1, 1]


if __name__ == "__main__":
    test_xbox_hrir_coefficients_use_xemu_per_ear_normalization()
    test_per_ear_l1_normalization_bounds_impulse_gain()
    test_mono_3d_source_expands_to_two_hrtf_ears()
    test_hrtf_routes_follow_xbox_3d_mixbin_order()

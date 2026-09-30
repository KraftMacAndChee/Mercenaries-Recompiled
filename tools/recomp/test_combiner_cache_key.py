from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/d3d/d3d8_combiners.c").read_text(encoding="utf-8")


def test_animated_combiner_constants_are_not_shader_cache_identity():
    """C0/C1 values live in the per-draw constant buffer, not generated HLSL."""
    hash_body = SOURCE.split(
        "static uint32_t combiner_state_hash", 1
    )[1].split("static BOOL combiner_state_equal", 1)[0]
    equal_body = SOURCE.split(
        "static BOOL combiner_state_equal", 1
    )[1].split("/* ================================================================", 1)[0]

    # The structural spans end immediately before c0 and resume at tex_mode,
    # excluding c0/c1 and final_c0/final_c1 without a per-draw state copy.
    for body in (hash_body, equal_body):
        assert "offsetof(NV2ACombinerState, c0)" in body
        assert "offsetof(NV2ACombinerState, tex_mode)" in body
    for field in ("state->c0", "state->c1", "state->final_c0",
                  "state->final_c1"):
        assert field not in hash_body


def test_draw_time_constants_are_still_uploaded_each_draw():
    assert "d3dcolor_to_float4(g_combiner_state.c0[i], constants.c0[i])" in SOURCE
    assert "d3dcolor_to_float4(g_combiner_state.c1[i], constants.c1[i])" in SOURCE
    assert "d3dcolor_to_float4(g_combiner_state.final_c0, constants.final_c0)" in SOURCE
    assert "d3dcolor_to_float4(g_combiner_state.final_c1, constants.final_c1)" in SOURCE


def test_unchanged_external_structure_reuses_the_owned_cache_shader():
    setter = SOURCE.split(
        "void d3d8_combiners_set_nv2a_state", 1
    )[1].split("void d3d8_combiners_set_texture_scale", 1)[0]
    prepare = SOURCE.split(
        "BOOL d3d8_combiners_prepare_draw", 1
    )[1]

    assert "combiner_state_equal(&g_combiner_state, state)" in setter
    assert "g_current_shader = NULL" in setter
    assert "ps = g_current_shader" in prepare
    assert "g_current_shader = ps" in prepare

"""Regression coverage for the diagnostic post-aircraft gate route."""

from pathlib import Path

from generated_test_utils import generated_path_containing


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ports" / "mercenaries" / "src" / "recomp_manual.c"
PATCHER = ROOT / "ports" / "mercenaries" / "scripts" / "Patch-Generated.py"
RECOMP_TYPES = (
    ROOT / "ports" / "mercenaries" / "src" / "recomp" / "recomp_types.h"
)
GENERATED_GAMEPLAY = generated_path_containing("0x0004E5ECu")
GENERATED = generated_path_containing(
    "recomp_camera_ray_callsite_checkpoint(0x00069A44u"
)
GENERATED_CAMERA = generated_path_containing(
    "recomp_actor_query_entry_checkpoint(0xFFFFFFFFu, 0u, eax)"
)
GENERATED_EFFECT = generated_path_containing(
    "const uint32_t _effect_saved_esi = esi;"
)
GENERATED_QUERY_PRODUCER = generated_path_containing(
    "const uint32_t _mopp_query_saved_ebx = ebx;"
)
GENERATED_QUERY_CONSUMER = generated_path_containing(
    "recomp_actor_query_count_checkpoint(0x001707A8u, eax)"
)
GENERATED_NOTIFICATION = generated_path_containing(
    "uint32_t notification_next = 0u;"
)
GENERATED_REDSCENE = generated_path_containing(
    "recomp_redscene_collected_item_checkpoint"
)

def test_gate_route_is_opt_in_and_uses_authored_waypoints() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("static int recomp_apply_test_post_aircraft_gate_route(")
    end = text.index("static void recomp_input_ensure_initialized", start)
    route = text[start:end]

    assert 'getenv("MERCENARIES_TEST_AUTO_DRIVE_GATE_ROUTE")' in route
    assert "{1548.554f, 1779.954f}" in route  # loc_playerstart
    assert "{1548.554f, 1758.000f}" in route  # beyond low stone barrier
    assert "{1555.000f, 1740.000f}" in route  # beyond dmz_skgate
    assert "{1609.340f, 1666.298f}" in route  # allies0_battle1-entry
    assert "{1594.624f, 1528.525f}" in route  # roadblock centreline
    assert "{1630.000f, 1560.000f}" in route  # stay south of encounter
    assert "{1630.000f, 1510.000f}" in route  # clear encounter east edge
    assert "{1630.000f, 1463.000f}" in route  # return north of encounter
    assert "SWN_allies0_enc_roadblock.lyr" in route
    assert "wedging it at (1582,1494)" in route
    assert "{1584.096f, 1406.873f}" in route  # allies0_cliff-battle-entry
    assert "{1498.223f, 1220.721f}" in route  # north end of t_dmz-south_02a
    assert "{1490.000f, 1138.000f}" in route  # measured cornering arc
    assert "{1485.000f, 1128.000f}" in route
    assert "{1476.143f, 1122.390f}" in route
    assert "{1454.708f, 1122.390f}" in route
    assert "{1438.000f, 1122.390f}" in route  # beyond roadblock, east of fence
    assert "{1438.000f, 1135.000f}" in route  # turn north before adjacent fence
    assert "{1415.000f, 1135.000f}" in route  # cross west north of fence bounds
    assert "{1392.827f, 1106.409f}" in route
    assert "{1381.988f, 852.346f}" in route  # t_dmz-hq_03a north end
    assert "{1365.000f, 865.000f}" in route  # west-side U-turn entry
    assert "{1345.000f, 855.000f}" in route
    assert "{1345.000f, 842.766f}" in route
    assert "{1365.000f, 842.766f}" in route  # eastbound alignment begins
    assert "{1380.000f, 842.766f}" in route
    assert "{1421.873f, 842.766f}" in route  # far side of destructible props
    assert "{1434.000f, 840.500f}" in route  # clear parking approach
    assert "stage >= 35u && stage <= 41u ? 5.0f" in route
    assert "stage == 40u ? 7.0f" in route
    assert "stage >= 61u && stage <= 68u ? 4.0f" in route
    assert "stage == 14u ? 4.0f" in route
    assert "Reach the actual X=1630 clearance point before turning" in route
    assert "Do not drive directly from" in route
    assert "hillside/chain-fence boundary" in route
    assert "stage == 38u ? 3.0f" not in route
    assert "successful retail" in route and "destroys it in one impact" in route
    assert "distance <= (stage < 4u ? 50.0f : 35.0f)" in route
    assert "beside a solid wing wall about 37 metres" in route
    assert "stage + 1u == sizeof(targets) / sizeof(targets[0]) ?" in route
    assert "if (stage == 2u && wait_until_ms" in route
    assert "if (stage == 1u)" in route
    assert "wait_until_ms = now + 3500u;" in route
    assert "bAnalogButtons[XBOX_BUTTON_X] = 255" in route
    assert "forward_x = -guest_f32(actor + 0xD0u);" in route
    assert "right_x = guest_f32(actor + 0xB0u);" in route
    assert "stage >= 4u && forward_dot < 0.15f" in route
    assert "stage < 4u ? 7.0f :" in route
    assert "Runs 457/458 approached obliquely" in route
    assert "SWN.wld supplies the exact" in route
    assert "15.0f" in route
    assert "now - last_motion_ms >= 3000u" in route
    assert "route_started_ms" in route
    assert "now - route_started_ms >= 900000u" in route
    assert '"[GATE-ROUTE-ABORT] reason=timeout' in route
    assert "recovery_count >= 3u &&" in route
    assert "(stage < 35u || stage > 41u)" in route
    assert "after three failed recovery cycles" in route
    assert "accepting blocked final test" in route
    assert "distance <= 50.0f" in route
    assert "g_recomp_allied_hq_route_arrived = 1;" in route
    assert "this changes neither guest collision nor mission state" in route
    assert "g_recomp_allied_hq_from_checkpoint = 2;" in route
    assert "g_recomp_allied_hq_from_checkpoint = 3;" in route
    assert "g_recomp_allied_hq_from_checkpoint == 2" in route
    assert "vehicle fallback begins walk" in route
    assert "checkpoint_walk_targets[27]" in route
    assert "checkpoint_walk_targets[29]" in route
    assert "stage == 63u" in route
    assert "accepting south-road test" in route
    assert "px >= 1340.0f && px <= 1405.0f" in route
    assert "pz >= 815.0f && pz <= 845.0f" in route
    assert "walk_stage = g_recomp_allied_hq_from_checkpoint == 3 ? 29u : 27u;" in route
    assert "checkpoint exit side selected" in route
    assert "exit_x > 1390.0f && exit_z < 845.0f" in route
    assert "walk_stage = 29u;" in route
    assert "if (stage == 36u || stage == 37u)" not in route
    assert "XBOX_BUTTON_RTRIGGER] = 255" not in route
    assert "recovery_count >= 6u && distance > 100.0f" in route
    assert '"[GATE-ROUTE-ABORT] reason=off-route' in route
    assert "recovery_reverse_until_ms = now + 3500u;" in route
    assert "recovery_until_ms = now + 7000u;" in route
    assert "consumed entirely" in route and "by braking" in route
    assert "crate/barrel barricade" in route
    assert "stage >= 38u && stage <= 39u" in route
    assert "forward_dot >= 0.985f" in route
    assert "dmz_fence.msh's authored bounds" in route
    assert "4,500-mass destructible global_roadblock01" in route
    assert "following authored bend then passes north of the fence" in route
    assert "stage >= 64u" in route
    assert "run 461 entered the pile at about twenty" in route
    assert "Stages 35-37 retain full steering" in route
    assert "stage == 39u && px <= targets[stage][0]" in route
    assert "fabsf(pz - targets[stage][1]) <= 12.0f" in route
    assert "Crossing the westbound road plane" in route
    assert "stage >= 64u" in route
    assert "stage == 14u ||" not in route
    assert "three dmz_fence sections" in route and "two roadblock" in route
    assert "bAnalogButtons[XBOX_BUTTON_X] = 200" in route
    assert 'getenv("MERCENARIES_CAPTURE_BEYOND_GATE_PATH")' in route
    assert '"MERCENARIES_CAPTURE_AUTHORED_NORTH_ROUTE_PATH"' in route


def test_gate_route_tracks_only_the_players_occupied_vehicle() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    helper_start = text.index("static uint32_t recomp_find_player_occupied_vehicle(")
    helper_end = text.index("static int recomp_apply_test_roadblock_on_foot(", helper_start)
    helper = text[helper_start:helper_end]
    wheel_start = text.index("void recomp_vehicle_wheel_actor_checkpoint(")
    wheel_end = text.index("void recomp_", wheel_start + 5)
    wheel = text[wheel_start:wheel_end]

    assert "g_recomp_live_player_ai + 0x998u" in helper
    assert "human + 0x768u" in helper
    assert "manager = guest_u32(seat);" in helper
    assert "vehicle = guest_u32(manager);" in helper
    assert "recomp_find_player_occupied_vehicle() == actor" in wheel
    assert "g_recomp_live_vehicle_actor = actor;" in wheel

    control_start = text.index("void recomp_player_control_checkpoint(")
    control_end = text.index("void recomp_player_vehicle_exit_checkpoint(", control_start)
    control = text[control_start:control_end]
    assert "g_recomp_live_player_ai = player;" in control
    assert control.index("g_recomp_live_player_ai = player;") < control.index(
        'getenv("MERCENARIES_TRACE_INPUT")'
    )


def test_allied_hq_continuation_exits_walks_and_uses_authored_bouncer() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("static int recomp_apply_test_allied_hq_entry(")
    end = text.index("static void recomp_input_ensure_initialized", start)
    route = text[start:end]

    assert 'getenv("MERCENARIES_TEST_AUTO_ENTER_ALLIED_HQ")' in route
    # The helper is called by every XInput poll. Normal play must hit its
    # disabled-route gate before the fallback HumanPlayer RAM scan.
    assert route.index('getenv("MERCENARIES_TEST_AUTO_ENTER_ALLIED_HQ")') < route.index(
        "recomp_find_live_player_human()"
    )
    assert "g_recomp_allied_hq_route_arrived" in text
    assert "g_recomp_allied_hq_route_arrived_ms = now;" in text
    assert "{1452.000f, 865.000f}" in route
    assert "{1428.000f, 865.000f}" in route
    assert "{1428.000f, 848.000f}" in route
    assert "{1472.000f, 848.000f}" in route
    assert "{1485.606f, 844.731f}" in route
    assert "{1485.976f, 840.913f}" in route
    assert "{1489.324f, 836.714f}" in route  # exact SW.wld bouncer marker
    assert "SW_Mafia5.PTH and" in route
    assert "SW_poi_dmz_peds.PTH" in route
    assert "actionRange to only 1 unit" in route
    assert "1.30f : 3.0f" in route
    assert "continuous east-west divider" in route
    assert "guest_u32(g_recomp_live_player_ai + 0x998u)" in route
    assert "guest_u32(human) != 0x002E32B8u" in route
    assert "bAnalogButtons[XBOX_BUTTON_X] = 255" in route
    assert "bAnalogButtons[XBOX_BUTTON_Y] = 255" in route
    assert "recomp_find_named_actor(0x9A21CF9Fu)" in route
    assert "guest_u32(bouncer_actor + 4u) != 0x9A21CF9Fu" in route
    assert "reason=bouncer-use-timeout" in route
    assert "reason=walk-timeout" in route
    assert "reason=walk-stuck" in route
    assert "now - last_walk_motion_ms >= 20000u" in route
    assert "dmz_bld_nkguardtower" in route
    assert "{1455.000f, 1142.000f}, {1455.000f, 1113.000f}" in route
    assert "{1436.000f, 1100.000f}, {1420.000f, 1098.000f}" in route
    # The old diagonal hits SW.wld's guardrail. Run525's ordinary walking
    # detour is the active route and was exercised again by run543.
    assert "{1386.000f, 846.000f}, {1370.000f, 833.000f}" in route
    assert "{1412.000f, 822.000f}, {1428.000f, 848.000f}" in route
    assert "{1488.000f, 879.000f}, {1488.000f, 852.000f}" in route
    assert "now - phase_started_ms" in route
    assert "g_recomp_allied_hq_from_checkpoint ? 300000u : 120000u" in route
    assert 'getenv(\n                "MERCENARIES_CAPTURE_ALLIED_HQ_WALK_ABORT_PATH")' in route
    assert "marker_dx * marker_dx + marker_dz * marker_dz <= 2.25f" in route
    assert 'getenv("MERCENARIES_CAPTURE_ALLIED_HQ_SETTLED_PATH")' in route
    assert "name-layout-check" in route
    assert 'getenv(\n                    "MERCENARIES_CAPTURE_ALLIED_HQ_APPROACH_PATH")' in route
    assert "briefing_scripts.lua gives the playable-intro" in route
    assert "recomp_find_named_actor(0x7D454737u)" in route
    assert "reason=briefing-use-timeout" in route
    assert "requesting stand-up via Y" in route
    assert "now - hq_standup_requested_ms < 350u" in route
    assert "now - hq_standup_requested_ms < 3500u" in route
    assert "elapsed >= 47000u" in route
    assert 'getenv(\n                "MERCENARIES_CAPTURE_ALLIED_MISSION_ACCEPTED_PATH")' in route
    assert "recomp_find_player_occupied_vehicle() == 0u" in route
    assert "reason=vehicle-exit-timeout" in route
    assert "recomp_find_named_actor(0x9239CAACu)" in route
    assert '"render=%08X flags=%08X model=%08X' in route
    assert "hx - guest_f32(red_camera + 0x40u)" in route
    assert "The playable-intro script exposes the bouncer" in route
    assert 'getenv("MERCENARIES_CAPTURE_ALLIED_HQ_INTERIOR_PATH")' in route
    assert "{1487.683f, 786.920f}" in route
    assert "{1507.163f, 656.927f}" in route
    assert "CardEncRunners02X" in route
    assert "recomp_find_named_actor(0x3A69C438u)" in route
    assert 'getenv("MERCENARIES_TEST_AUTO_TWO_CLUBS")' in route
    assert "distance <= 1.25f" in route
    assert "clubs2_actor + 0x6B9u" in route
    assert "in_use_range = isfinite(distance) && distance < 2.0f" in route
    assert "if (!captured_subdue_probe && subdued)" in route
    assert "retail subdued flag observed" in route
    assert '"through normal input; extraction and verification "' in route
    assert '"NOT YET TESTED\\n"' in route
    assert "state->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN" in route
    assert "substate == 0xC739FD0Fu" in route
    assert "if (support_cancel_sent)" in route
    assert "reason=support-menu-open-timeout" in route
    assert "reason=support-menu-close-timeout" in route
    assert "reason=support-menu-closed-without-cancel" in route
    assert "stable after subdue probe" not in route
    assert "[TWO-CLUBS-ABORT]" in route
    assert "sizeof(walk_targets)" not in route
    runner = (ROOT / "tools" / "recomp" / "Run-HiddenRetailRoute.ps1").read_text(
        encoding="utf-8"
    )
    assert "MERCENARIES_CAPTURE_TWO_CLUBS_SUBDUE_PATH" in runner
    assert "[switch]$TwoClubs" in runner
    assert "MERCENARIES_TEST_AUTO_TWO_CLUBS" in runner
    assert "MERCENARIES_TEST_HQ_WAYPOINT_FILE" in runner
    assert "[int]$ResolutionScaleOverride = -1" in runner
    assert "MERCENARIES_TEST_RESOLUTION_SCALE" in runner
    assert "[switch]$TraceD3DDrawPerf" in runner
    assert "MERCENARIES_TRACE_D3D_DRAW_PERF" in runner
    assert "phase == 99u && walk_aborted" in route
    assert "recomp_test_hq_waypoint_override(walk_stage, now" in route
    override_start = text.index("static int recomp_test_hq_waypoint_override(")
    override = text[override_start:start]
    assert "now - last_poll_ms >= 500u" in override
    assert "lines++ < 128u" in override
    assert "requested_stage != stage || !isfinite(x) || !isfinite(z)" in override
    assert "sequence != cached_sequence" in override
    assert 'fopen(path, "r")' in override
    assert "guest_ptr" not in override and "guest_u32" not in override


def test_aircraft_rifle_probe_is_bounded_and_opt_in() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("static void recomp_apply_test_aircraft_pickup_route(")
    end = text.index("static void recomp_trace_roadblock_actor_census(", start)
    route = text[start:end]

    assert 'getenv("MERCENARIES_TEST_AUTO_FIRE_AFTER_AIRCRAFT_RIFLE")' in route
    assert '"[AIRCRAFT-ROUTE] post-rifle firing probe started' in route
    assert "stage == 2u" in route
    assert "cooldown_until_ms - now <= 3000u" in route


def test_aircraft_route_uses_reachable_humvee_action_radius() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("static void recomp_apply_test_aircraft_pickup_route(")
    end = text.index("static void recomp_trace_roadblock_actor_census(", start)
    route = text[start:end]

    assert "stage == 4u ? 0.81f : 1.0f" in route
    assert "distance_squared > arrival_radius_squared" in route
    assert "default as exactly one metre" in route
    assert "Only the retail player-vehicle ownership handoff proves" in route
    assert "recomp_find_player_occupied_vehicle();" in route
    assert "g_recomp_live_vehicle_actor = occupied_vehicle;" in route
    assert "recomp_complete_test_aircraft_route(now, stage);" in route
    assert "[AIRCRAFT-ROUTE-ABORT]" in route
    assert 'getenv("MERCENARIES_CAPTURE_AIRCRAFT_ROUTE_ABORT_PATH")' in route
    assert "elapsed >= 90000u" in route
    assert "cooldown_until_ms = now + (stage == 3u ? 8000u : 4500u)" not in route


def test_gate_route_acknowledges_driving_tutorial_before_vehicle_is_live() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("static int recomp_apply_test_post_aircraft_gate_route(")
    end = text.index("static int recomp_apply_test_allied_hq_entry", start)
    route = text[start:end]

    assert "((elapsed - delay_ms) % 1500u) < 300u" in route
    assert "state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;" in route
    assert "tutorial-A=%u" in route


def test_roadblock_handoff_exits_vehicle_and_uses_authored_encounter_edge() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("static int recomp_apply_test_roadblock_on_foot(")
    end = text.index("static int recomp_apply_test_post_aircraft_gate_route(", start)
    route = text[start:end]

    assert 'getenv("MERCENARIES_TEST_AUTO_ROADBLOCK_ON_FOOT")' in route
    assert 'getenv("MERCENARIES_TEST_AUTO_FIRE_AFTER_ROADBLOCK")' in route
    assert "[ROADBLOCK-ON-FOOT] firing probe started" in route
    assert "[ROADBLOCK-ON-FOOT] firing probe complete;" in route
    assert "Once the detour (and optional finite firing probe)" in route
    assert 'getenv("MERCENARIES_TRACE_ROADBLOCK_ACTORS")' in text
    assert "vtable != 0x002E2818u" in text
    assert "vtable != 0x002E2CE0u" in text
    assert "vtable != 0x002E32B8u" in text
    assert "guest_u8(actor + 0x950u)" in text
    assert "guest_f32(actor + 0x154u)" in text
    assert "guest_u32(actor + 0x50u)" in text
    assert "guest_u32(actor + 0x58u)" in text
    assert "guest_u32(g_recomp_live_player_ai + 0x998u)" in route
    assert "1595.198f - vx" in route  # authored roadblock centreline
    assert "100.0f * 100.0f" in route
    assert "bAnalogButtons[XBOX_BUTTON_X] = 255" in route
    assert "bAnalogButtons[XBOX_BUTTON_Y] = 255" in route
    assert "movement_squared <= 0.01f" in route
    assert "Transition immediately on the first low-speed" in route
    assert "now - phase_started_ms >= 12000u" in route
    assert "vehicle stopped at" in route
    exit_start = route.index("if (phase == 2u)")
    exit_phase = route[exit_start : route.index("if (human <", exit_start)]
    assert "bAnalogButtons[XBOX_BUTTON_X] = 255" not in exit_phase
    assert "after stopping it is reverse" in exit_phase
    assert "now - phase_started_ms >= 3000u" in exit_phase
    assert "rider_seat + 0x118u" in exit_phase
    assert "{1608.000f, 1530.000f}" in route  # back away from the Humvee
    assert "{1622.000f, 1510.000f}" in route  # east edge of encounter
    assert "{1622.000f, 1475.000f}" in route  # north of obstruction
    assert "{1591.465f, 1462.935f}" in route  # authored road centreline
    assert 'getenv("MERCENARIES_CAPTURE_ROADBLOCK_EXIT_PATH")' in route
    assert 'getenv("MERCENARIES_CAPTURE_ROADBLOCK_BYPASS_PATH")' in route
    assert "hx - guest_f32(red_camera + 0x40u)" in route
    assert "now - last_moved_ms >= 6000u" in route
    assert "selecting alternate waypoint" in route

    post_start = text.index("static int recomp_apply_test_post_aircraft_gate_route(")
    post_end = text.index("static void recomp_input_ensure_initialized", post_start)
    post = text[post_start:post_end]
    assert "recomp_apply_test_roadblock_on_foot(state, now, actor)" in post
    assert "recomp_trace_roadblock_actor_census();" in post
    assert "45.0f * 45.0f" in post


def test_player_vehicle_exit_ownership_transition_is_traceable() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    types = (
        ROOT / "ports" / "mercenaries" / "src" / "recomp" / "recomp_types.h"
    ).read_text(encoding="utf-8")

    assert "recomp_player_vehicle_exit_checkpoint" in types
    assert "_trace_va == 0x00091D90u" in types
    assert "_trace_va == 0x0008C810u" in types
    assert 'getenv("MERCENARIES_TRACE_PLAYER_VEHICLE_EXIT")' in text
    assert "player_ai + 0xCD4u" in text
    assert "player_ai + 0x55Du" in text
    assert "human + 0x768u" in text
    assert "seat + 0x118u" in text


def test_normal_preview_input_is_unchanged_when_route_is_disabled() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    call = text.index(
        "if (!recomp_apply_test_post_aircraft_gate_route(&state, now) &&"
    )
    legacy = text.index(
        'getenv("MERCENARIES_TEST_AUTO_DRIVE_AFTER_AIRCRAFT")', call
    )
    assert call < legacy


def test_post_gate_vehicle_trace_is_opt_in() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    assert 'getenv("MERCENARIES_TRACE_POST_GATE_VEHICLE")' in text
    assert '"[POST-GATE-VEHICLE] actor=%08X pos=' in text


def test_process_stimuli_ray_count_is_retail_bounded_and_written_back() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    start = text.index("uint32_t recomp_camera_ray_callsite_checkpoint(")
    end = text.index("uint32_t recomp_camera_ray_begin_checkpoint(", start)
    guard = text[start:end]
    expected = """edi = recomp_camera_ray_callsite_checkpoint(0x00069A44u, ebp, edi, esp);
    MEM32(esp + 0x18u) = edi;"""

    # Retail 0x69A0B compares the appended count with 15 and 0x69A1E clears
    # the candidate flag. Direct block execution is checked separately by
    # test_retail_loop_limits.py; the guarded call loads the same count.
    assert "site == 0x00069A44u" in guard
    assert "capacity = 15u;" in guard
    assert "if (site == 0x00069A44u)" in guard
    assert "sanitized_count = initialized;" in guard
    assert "if (CMP_LE((edi & edi), 0)) goto loc_00069A9C;" in GENERATED.read_text(encoding="utf-8")
    assert expected in GENERATED.read_text(encoding="utf-8")
    assert expected in PATCHER.read_text(encoding="utf-8")
    assert "if (CMP_LE((edi & edi), 0)) goto loc_00069A9C;" in PATCHER.read_text(encoding="utf-8")


def test_ai_update_lifetime_guard_is_narrow_and_persistent() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    generated = GENERATED.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")
    shared_epilogue = generated_path_containing(
        "void sub_00087B90(void)"
    ).read_text(encoding="utf-8")
    start = source.index("uint32_t recomp_ai_update_vtable_checkpoint(")
    end = source.index("void recomp_spore_predicate_checkpoint(", start)
    guard = source[start:end]

    assert 'getenv("MERCENARIES_TEST_REPAIR_AI_VTABLE")' in guard
    assert "if (current != expected_vtable)" in guard
    assert "*(volatile uint32_t *)guest_ptr(object) = expected_vtable;" in guard
    assert "[AI-STIMULUS-DIFF]" in guard
    assert "object + 0x4B8u" in guard
    assert "recomp_ai_process_stimuli_esi_checkpoint" in source
    assert "[AI-CALLEE-SAVED] ProcessStimuli ESI" in source
    assert "recomp_ai_process_stimuli_esp_checkpoint" in source
    assert "[AI-CALLEE-SAVED] ProcessStimuli ESP" in source
    assert "recomp_ai_process_stimuli_frame_checkpoint" in source
    assert "[AI-PROCESS-STACK]" in source
    assert "g_ai_stimulus_snapshot_lock" in guard
    assert "g_ai_stimulus_snapshots[256]" in guard
    assert "current_depth <= 4u" in guard
    assert "guard-exit=%u" in guard
    assert "if (!recomp_ai_stimulus_boundary_checkpoint(" in generated
    assert "POP32(esp, edi);" in generated
    assert "goto loc_0006B584;" in generated
    assert "ai_update_entry_vtable < 0x00200000u" in generated
    assert "ai_update_pre_stimulus_object = esi;" in generated
    assert "ai_process_frame_stack = esp;" in generated
    assert "0x0006982Du, ai_process_frame_stack, esp" in generated
    assert "0x000699F5u, ai_process_frame_stack, esp" in generated
    assert "ai_process_percept_call_active = 1u;" in generated
    assert "esi = ai_process_percept_esi;" in generated
    assert "ProcessStimuli fixed-frame ABI guards" in patcher
    assert "ProcessStimuli percept-call nonvolatile guard" in patcher
    assert "retail AI action shared epilogue interior entries" in patcher
    assert "RedSpace collected item pointer integrity boundary" in patcher
    assert "retail actor query result pointer integrity boundary" in patcher
    assert "retail actor query consumer pointer integrity boundary" in patcher
    assert "retail actor query cleanup nonvolatile result guard" in patcher
    assert "retail actor query returned count capacity boundary" in patcher
    assert "retail actor query consumer table capacity boundary" in patcher
    assert "retail actor query candidate position nonvolatile guard" in patcher
    assert "retail actor query best-result pointer boundary" in patcher
    assert "retail global notification callback nonvolatile guard" in patcher
    assert "retail Havok OBB MOPP query callback nonvolatile guard" in patcher

    notification = GENERATED_NOTIFICATION.read_text(encoding="utf-8")
    assert "uint32_t notification_next = 0u;" in notification
    assert "notification_next = MEM32(esi);" in notification
    assert "esi = notification_next;" in notification
    assert "cursor lifetime guard" in patcher
    assert "snapshot next cursor" in patcher
    assert "PblThread callback advances validated live cursor" in patcher
    assert "const uint32_t _notification_esi = esi;" in notification
    assert "recomp_update_esi_checkpoint(0x001FA110u" in notification
    assert "esi = _notification_esi;" in notification
    assert "edi = _notification_edi;" in notification
    assert "ebp = _notification_ebp;" in notification
    assert "recomp_redscene_collected_item_checkpoint" in GENERATED_REDSCENE.read_text(encoding="utf-8")
    producer = GENERATED_QUERY_PRODUCER.read_text(encoding="utf-8")
    assert "const uint32_t _mopp_query_saved_ebx = ebx;" in producer
    assert "const uint32_t _mopp_query_saved_esi = esi;" in producer
    assert "const uint32_t _mopp_query_saved_edi = edi;" in producer
    assert "0x00129CB2u, _mopp_query_saved_esi, esi" in producer
    assert "ebx = _mopp_query_saved_ebx;" in producer
    assert "esi = _mopp_query_saved_esi;" in producer
    assert "edi = _mopp_query_saved_edi;" in producer
    assert "recomp_actor_query_entry_checkpoint" in producer
    assert "const uint32_t _actor_query_result_count = esi;" in producer
    assert "recomp_update_esi_checkpoint(" in producer
    assert "0x0012E3E0u, _actor_query_result_count, esi" in producer
    assert "esi = _actor_query_result_count;" in producer
    consumer = GENERATED_QUERY_CONSUMER.read_text(encoding="utf-8")
    assert "recomp_actor_query_count_checkpoint(0x001707A8u, eax)" in consumer
    assert "CMP_AE(ebp, 0x37A2FCu)" in consumer
    assert "(ebp - 0x378EFCu) / 0x28u" in consumer
    assert "MEM32(ebp - 4u), esi" in consumer
    assert "const uint32_t _actor_query_candidate = esi;" in consumer
    for site in ("0x001707F2u", "0x0017080Cu", "0x0017081Cu", "0x0017082Au"):
        assert site in consumer
    assert consumer.count("recomp_nonvolatile_icall_checkpoint(") == 4
    assert consumer.count("esp = _icall_esp;") >= 4
    assert "esi = _actor_query_candidate;" in consumer
    assert "[ICALL-NONVOLATILE]" in source
    gameplay = GENERATED_GAMEPLAY.read_text(encoding="utf-8")
    for site in ("0x0004E5ECu", "0x0004E62Eu", "0x0004E693u"):
        assert site in gameplay
    assert gameplay.count("recomp_nonvolatile_icall_checkpoint(") >= 3
    assert "retail gameplay interpolation dispatch nonvolatile guard" in patcher
    assert "retail actor query eligibility call nonvolatile guard" in patcher
    camera = GENERATED_CAMERA.read_text(encoding="utf-8")
    assert "recomp_actor_query_entry_checkpoint(0xFFFFFFFFu, 0u, eax)" in camera
    assert "recomp_actor_query_count_checkpoint" in source
    assert "[ACTOR-QUERY-COUNT]" in source
    assert "static int recomp_trace_xact_enabled(void)" in source
    assert 'getenv("MERCENARIES_TRACE_XACT")' not in source
    assert "void sub_00087B90(void)" in shared_epilogue
    assert "ecx = MEM32(esp + 0x2EC);" in shared_epilogue
    assert "esp = esp + 0x2F0;" in shared_epilogue
    assert "ai_update_pre_stimulus_stack = esp;" in generated
    assert "esp = recomp_ai_process_stimuli_esp_checkpoint(" in generated
    assert "esi = recomp_ai_process_stimuli_esi_checkpoint(" in generated
    assert "ai_update_pre_stimulus_object = esi;" in patcher
    assert "retail AI pre-stimulus object snapshot" in patcher
    assert "0x0006B216u" in generated  # immediately after ProcessStimuli
    assert "0x0006B368u" in generated  # immediately before crashing virtual call
    assert "ai_update_entry_vtable" in generated
    assert "retail AI update post-stimulus vtable checkpoint" in patcher
    assert "retail AI update pre-anchor vtable checkpoint" in patcher
    camera_tilt = GENERATED_CAMERA.read_text(encoding="utf-8")
    assert "uint32_t camera_tilt_saved_edi = 0u;" in camera_tilt
    assert "0x000902D9u, camera_tilt_saved_edi, edi" in camera_tilt
    assert "edi = camera_tilt_saved_edi;" in camera_tilt
    assert "esp = camera_tilt_saved_esp;" in camera_tilt
    assert "camera tilt loop ABI guard" in patcher
    assert "[CAMERA-TILT-LOOP-ABI]" in source

def test_player_update_lifetime_trace_is_narrow_and_persistent() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")
    generated = GENERATED_CAMERA.read_text(encoding="utf-8")

    assert 'getenv("MERCENARIES_TRACE_PLAYER_UPDATE_LIFETIME")' in source
    assert "[PLAYER-UPDATE-LIFETIME]" in source
    assert patcher.count("recomp_player_update_lifetime_checkpoint") >= 9
    assert generated.count("recomp_player_update_lifetime_checkpoint") >= 9
    assert "loc_000917C0" in generated
    assert "loc_00091AC1" in generated
    assert '"retail player action callback nonvolatile guard"' in patcher
    assert "const uint32_t _player_action_saved_esi = esi;" in generated
    assert "recomp_update_esi_checkpoint(0x00091A32u" in generated
    assert '"retail player movement callback nonvolatile guard"' in patcher
    assert "const uint32_t _player_movement_saved_ebx = ebx;" in generated
    assert "const uint32_t _player_movement_saved_esi = esi;" in generated
    assert "const uint32_t _player_movement_saved_edi = edi;" in generated
    assert "recomp_update_esi_checkpoint(0x00091AADu" in generated


def test_effect_update_tail_calls_restore_nonvolatile_esi() -> None:
    generated = GENERATED_EFFECT.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")

    for site in (
        "00096DC9", "00096DF3", "00096E02",
        "00096E0D", "00096E18", "00096E4B",
    ):
        checkpoint = (
            f"recomp_update_esi_checkpoint(0x{site}u, "
            "_effect_saved_esi, esi);\n    esi = _effect_saved_esi;"
        )
        assert checkpoint in generated
        assert f"retail effect tail call {site} nonvolatile ESI guard" in patcher

def test_notification_list_mutation_trace_is_targeted_and_persistent() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    patcher = PATCHER.read_text(encoding="utf-8")
    notification_generated = GENERATED_NOTIFICATION.read_text(encoding="utf-8")

    assert 'getenv("MERCENARIES_TRACE_NOTIFICATION_LIST")' in source
    assert "[NOTIFICATION-LIST]" in source
    assert "[GUEST-PURECALL]" in source
    assert "_target_trace_va == 0x002376FCu" in RECOMP_TYPES.read_text(encoding="utf-8")
    assert "target != 0x002376FCu" in source
    assert 'target == 0x002376FCu ? "purecall" :' in source
    assert '!cursor_valid ? "invalid-pointer" :' in source
    assert '!links_valid ? "broken-links" : "invalid-entry"' in source
    assert "loc_001FA102" in patcher
    assert "_notification_entry" in patcher
    assert "_notification_target" in patcher
    assert patcher.count("recomp_notification_list_checkpoint") >= 3
    assert "recomp_notification_cursor_sanitize" in source
    assert "[NOTIFICATION-CURSOR-GUARD]" in source
    assert "next_previous == cursor && previous_next == cursor" in source
    assert "retail notification callback validates saved successor" in patcher
    assert "retail PblThread callback advances validated live cursor" in patcher
    assert "recomp_pbl_thread_next_after_update" in source
    assert "Retail 0x1FA10D calls the update callback" in source
    assert (
        "recomp_pbl_thread_next_after_update(esi, notification_next);" in
        notification_generated
    )
    assert "esi = recomp_notification_cursor_sanitize(esi);" in notification_generated
    assert "if (esi == 0u) goto loc_001FA155;" in notification_generated
    assert "g_esp = _icall_esp;" in notification_generated
    assert (
        "_notification_target, __FUNCTION__, __LINE__" in
        notification_generated
    )


def test_game_stall_watchdog_covers_long_route_and_reports_host_and_guest_state() -> None:
    source = SOURCE.read_text(encoding="utf-8")

    assert '"MERCENARIES_TRACE_GAME_STALL_MS", value' in source
    assert 'getenv("MERCENARIES_TEST_AUTO_DRIVE_GATE_ROUTE") == NULL' in source
    assert 'threshold_ms = 15000u;' in source
    assert 'g_recomp_post_aircraft_gate_route_started = 1;' in source
    assert 'if (g_recomp_post_aircraft_gate_route_started ||' in source
    assert 'getenv("MERCENARIES_TRACE_GAME_STALL_MS") != NULL' in source
    assert 'g_recomp_watchdog_heartbeat_enabled = 1u;' in source
    assert 'last_heartbeat = g_recomp_watchdog_heartbeat' in source
    assert 'heartbeat = g_recomp_watchdog_heartbeat' in source
    assert 'distance <= best_waypoint_distance - 1.0f' in source
    assert 'recomp_dump_game_stall_snapshot("GAME-STALL", unchanged_ms)' in source
    assert "recomp_dump_suspended_native_stack(tag, context);" in source
    assert "RtlLookupFunctionEntry(" in source
    assert "RtlVirtualUnwind(UNW_FLAG_NHANDLER" in source
    assert "__except (EXCEPTION_EXECUTE_HANDLER)" in source
    assert "[unwind-fault=%08lX]" in source
    snapshot = source[
        source.index("static void recomp_dump_game_stall_snapshot"):
        source.index("static DWORD WINAPI recomp_game_stall_watchdog_thread")
    ]
    assert snapshot.index("GetThreadContext(game_thread, &context)") < (
        snapshot.index("recomp_dump_suspended_native_stack(tag, context)")
    ) < snapshot.index("ResumeThread(game_thread)")
    assert 'fprintf(stderr, "[%s-STACK]", tag);' in source
    assert 'recomp_arm_game_stall_watchdog();' in source
    assert 'THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT' in source
    assert 'g_recomp_recent_game_funcs' in source
    assert '[GATE-ROUTE-RECOVERY]' in source
    assert 'recovery_reverse_until_ms' in source

if __name__ == "__main__":
    test_gate_route_is_opt_in_and_uses_authored_waypoints()
    test_gate_route_tracks_only_the_players_occupied_vehicle()
    test_allied_hq_continuation_exits_walks_and_uses_authored_bouncer()
    test_aircraft_rifle_probe_is_bounded_and_opt_in()
    test_aircraft_route_uses_reachable_humvee_action_radius()
    test_gate_route_acknowledges_driving_tutorial_before_vehicle_is_live()
    test_roadblock_handoff_exits_vehicle_and_uses_authored_encounter_edge()
    test_player_vehicle_exit_ownership_transition_is_traceable()
    test_normal_preview_input_is_unchanged_when_route_is_disabled()
    test_post_gate_vehicle_trace_is_opt_in()
    test_process_stimuli_ray_count_is_source_bounded_and_written_back()
    test_ai_update_lifetime_guard_is_narrow_and_persistent()
    test_player_update_lifetime_trace_is_narrow_and_persistent()
    test_effect_update_tail_calls_restore_nonvolatile_esi()
    test_notification_list_mutation_trace_is_targeted_and_persistent()
    test_game_stall_watchdog_covers_long_route_and_reports_host_and_guest_state()
    print("Post-aircraft gate-route checks passed")

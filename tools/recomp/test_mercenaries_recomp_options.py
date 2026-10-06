"""Recomp Options remain source-controlled, reproducible, and functional."""

from pathlib import Path
import runpy


ROOT = Path(__file__).resolve().parents[2]
PORT = ROOT / "ports" / "mercenaries"
OPTIONS = (PORT / "src" / "recomp_options.c").read_text(encoding="utf-8")
OPTIONS_HEADER = (PORT / "src" / "recomp_options.h").read_text(encoding="utf-8")
MENU = (PORT / "src" / "recomp_options_menu.c").read_text(encoding="utf-8")
MAIN = (PORT / "src" / "main.c").read_text(encoding="utf-8")
MANUAL = (PORT / "src" / "recomp_manual.c").read_text(encoding="utf-8")
CMAKE = (PORT / "CMakeLists.txt").read_text(encoding="utf-8")
DEVICE = (ROOT / "src" / "d3d" / "d3d8_device.c").read_text(encoding="utf-8")
PGRAPH = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.c").read_text(encoding="utf-8")
PGRAPH_HEADER = (ROOT / "src" / "nv2a" / "nv2a_pgraph_d3d11.h").read_text(encoding="utf-8")
NV2A_HOOK = (ROOT / "src" / "nv2a" / "nv2a_mmio_hook.c").read_text(encoding="utf-8")
STATES = (ROOT / "src" / "d3d" / "d3d8_states.c").read_text(encoding="utf-8")
KERNEL = (ROOT / "src" / "kernel" / "kernel_xbox.c").read_text(encoding="utf-8")
PATCHER = PORT / "scripts" / "Patch-Generated.py"
RECOMP_TYPES = (PORT / "src" / "recomp" / "recomp_types.h").read_text(encoding="utf-8")
RUNNER = (ROOT / "tools" / "recomp" / "Run-HiddenRetailManual.ps1").read_text(encoding="utf-8")


def test_options_are_source_controlled_and_portable() -> None:
    assert "src/recomp_options.c" in CMAKE
    assert "src/recomp_options_menu.c" in CMAKE
    combined = "\n".join((OPTIONS, MENU, MAIN, CMAKE))
    assert "xemu.exe" not in combined.lower()
    assert "C:\\Users\\trent" not in combined
    assert "F:\\" not in combined


def test_requested_menu_shape_and_placement() -> None:
    assert "MENU_MAIN_HASH" in MENU and "MENU_PAUSE_HASH" in MENU
    assert "MENU_SHELL_OPTIONS_HASH" in MENU
    assert "insert_item_before(menu,RECOMP_OPTIONS_MENU_HASH,0x00DFD9ADu)" in MENU
    assert "OPTIONS_ITEM_COUNT 16u" in MENU
    assert "RECOMP_OPTIONS_PS2_UPGRADES_HASH" in MENU
    assert "RECOMP_OPTIONS_BOIDS_HASH" not in MENU
    assert "RECOMP_OPTIONS_OBJECT_DISTANCE_HASH" not in MENU
    assert "RECOMP_OPTIONS_NPC_DISTANCE_HASH" not in MENU
    assert "(input == 2u || input == 3u) && event == 1u" in MENU
    assert "event == 1u || event == 4u" not in MENU
    assert "if (selected == MENU_BACK_HASH) return 2u" in MENU
    assert "if (eax == 2u)" in PATCHER.read_text(encoding="utf-8")
    assert "MEM32(esp + 4u) = 4u" in PATCHER.read_text(encoding="utf-8")
    assert "guest(menu + 0x69u) = 1u" not in MENU
    for text in (
        "FPS CAP: %s",
        "V-SYNC: %s",
        "ASPECT RATIO: %s",
        "NPC WAKE DISTANCE: %s",
        "NPC DRAW DISTANCE: %s",
        "NPC LOD: %s",
        "RESOLUTION SCALE: %s",
        "ANISOTROPIC FILTERING: %s",
        "HAZE: %s",
        'return changed_options() ? "APPLY" : "APPLY (NO CHANGES)";',
        "DISPLAY: %s",
        'return "RECOMP OPTIONS"',
    ):
        assert text in OPTIONS
    assert '{ "WINDOWED", "BORDERLESS", "FULLSCREEN" }' in OPTIONS


def test_selected_custom_label_is_redrawn_above_retail_highlight() -> None:
    patches = runpy.run_path(str(PATCHER))["PATCHES"]
    by_name = {patch.name: patch for patch in patches}
    style = by_name[
        "front-end BrushMenu selected custom label localization style"
    ]
    replacement = by_name[
        "front-end BrushMenu selected custom label replacement"
    ]
    assert "loc_000E6C4A" in style.before
    assert "edx = recomp_options_localization_hash(edx);" in style.after
    assert "loc_000E6C61" in replacement.before
    assert "recomp_options_replace_label" in replacement.after
    assert "MEM32(esi + 0x38u)" in replacement.after


def test_settings_persist_beside_the_executable() -> None:
    assert "GetModuleFileNameA(NULL, path, MAX_PATH)" in OPTIONS
    assert '"mercenaries_recomp.ini"' in OPTIONS
    assert "WritePrivateProfileStringA" in OPTIONS
    for key in (
        "60FPS", "AspectRatio", "DrawDistance", "ResolutionScale",
        "Anisotropic16x", "DisplayMode", "AuthenticHaze",
        "NpcWakeDistance", "NpcDrawDistance", "NpcLOD", "FixedXboxPrompts",
    ):
        assert f'"{key}"' in OPTIONS


def test_renderer_settings_have_real_backing_behavior() -> None:
    assert "d3d8_SetFrameCap(recomp_options_fps_cap())" in MAIN
    assert MAIN.count("d3d8_SetVSync(recomp_options_vsync())") == 2
    assert "RECOMP_OPTIONS_VSYNC_HASH" in MENU
    assert "d3d8_SetForceAnisotropic16x" in MAIN
    assert "d3d8_SetAuthenticLineHazeEnabled" not in MAIN
    assert "d3d8_SetPresentationAspect" in MAIN
    assert "xbox_set_widescreen_enabled" in MAIN
    assert "WS_OVERLAPPEDWINDOW : WS_POPUP" in MAIN
    assert "SM_CXSCREEN" in MAIN and "SM_CYSCREEN" in MAIN
    assert "present.Windowed = display_mode != RECOMP_DISPLAY_FULLSCREEN" in MAIN
    assert "g_force_anisotropic_16x" in STATES
    assert "rd.AntialiasedLineEnable = g_line_smooth_enable;" in STATES
    assert "D3D11_FILTER_ANISOTROPIC" in STATES
    assert "MaxAnisotropy = 16" in STATES
    assert "g_presentation_aspect_width" in DEVICE
    assert "frequency.QuadPart / g_frame_cap_fps" in DEVICE
    assert "void d3d8_WaitForGuestFrameSlot(void)" in DEVICE
    assert "completed_scanout = nv2a_hook_service_scanout()" in MAIN
    assert "if (completed_scanout)\n        d3d8_WaitForGuestFrameSlot();" in MAIN
    assert "scanout_tick" not in DEVICE
    assert "int pgraph_d3d11_service_scanout(void);" in PGRAPH_HEADER
    scanout = PGRAPH[PGRAPH.index("static int service_pending_scanout(int force)\n{"):]
    scanout = scanout[:scanout.index("int pgraph_d3d11_service_scanout(void)")]
    assert "return service_pending_scanout(0);" in PGRAPH
    assert scanout.count("return 0;") == 2
    assert "g_pg.pending_scanout = NULL;" in scanout
    assert scanout.rstrip().endswith("return 1;\n}")
    assert "return pgraph_d3d11_service_scanout() != 0;" in NV2A_HOOK
    assert "ClearRenderTargetView" in DEVICE
    assert "xbox_set_widescreen_enabled" in KERNEL


def test_authentic_haze_gates_only_the_retail_xbox_glow_pass() -> None:
    assert "recomp_options_action_marker_outer_radius" not in OPTIONS
    assert "recomp_options_action_marker_outer_color" not in OPTIONS
    assert "AuthenticLineHaze" not in STATES
    assert "int recomp_options_authentic_haze(void);" in RECOMP_TYPES
    patches = runpy.run_path(str(PATCHER))["PATCHES"]
    by_name = {patch.name: patch for patch in patches}
    glow = by_name["FilterShader Render authentic glow option"]
    assert "loc_0015B0EE" in glow.before
    assert "sub_0015AB40" in glow.before
    assert "recomp_options_authentic_haze()" in glow.after
    assert "goto loc_0015B0F3" in glow.after
    assert "sub_0015A6E0" not in glow.after


def test_resolution_scale_is_true_internal_rendering() -> None:
    assert "recomp_options_internal_resolution_size(&internal_width" in MAIN
    assert "pgraph_d3d11_set_internal_resolution(internal_width" in MAIN
    assert "static const uint32_t heights[] = { 480u, 720u, 900u, 1080u, 1440u, 2160u }" in OPTIONS
    assert "w = (uint32_t)(((uint64_t)h * 4u + 1u) / 3u);" in OPTIONS
    assert "Xbox render targets are 4:3" in OPTIONS
    assert "*physical_width = (uint32_t)(((uint64_t)w * g_pg.internal_width + 639u)" in PGRAPH
    assert "*physical_height = (uint32_t)(((uint64_t)h * g_pg.internal_height + 479u)" in PGRAPH
    assert "desc.Width = width;" in PGRAPH
    assert "desc.Height = height;" in PGRAPH
    assert "d3d8_BindRenderTargets(color->rtv, depth ? depth->dsv : NULL," in PGRAPH
    assert "color->width, color->height" in PGRAPH
    assert "d3d8_SetLogicalRenderTargetSize(color->logical_width," in PGRAPH
    assert "viewport.Width = (FLOAT)width;" in DEVICE
    assert "viewport.Height = (FLOAT)height;" in DEVICE
    assert 'GetEnvironmentVariableA("MERCENARIES_TEST_RESOLUTION_SCALE"' in OPTIONS
    override = OPTIONS[
        OPTIONS.index("static void apply_test_overrides("):
        OPTIONS.index("void recomp_options_init(void)")
    ]
    assert "strtol(text, &end, 10)" in override
    assert "clamp_setting((int)value, 5)" in override
    assert "write_setting" not in override


def test_supersampling_scales_only_the_internal_render_target() -> None:
    assert "RECOMP_OPTIONS_SSAA_HASH" in OPTIONS_HEADER
    assert "RECOMP_OPTIONS_CHANGE_SSAA" in OPTIONS_HEADER
    assert "RECOMP_OPTIONS_SSAA_HASH" in MENU
    assert '"Supersampling", RECOMP_OPTIONS_SSAA_DEFAULT' in OPTIONS
    assert "RECOMP_OPTIONS_SSAA_HASH: g_options.pending.ssaa = cycle" in OPTIONS
    assert "static const char *ssaa_modes[]" in OPTIONS
    assert "SUPERSAMPLING: %s" in OPTIONS
    assert "int recomp_options_supersampling(void);" in OPTIONS_HEADER
    internal = OPTIONS[
        OPTIONS.index("void recomp_options_internal_resolution_size"):
        OPTIONS.index("void recomp_options_presentation_aspect")
    ]
    assert "ssaa_numerators[]" in internal and "ssaa_denominators[]" in internal
    assert "RECOMP_OPTIONS_SSAA_MAX" in internal
    # Applied when the option changes, and it must not touch output size.
    assert "RECOMP_OPTIONS_CHANGE_RESOLUTION |\n                   RECOMP_OPTIONS_CHANGE_SSAA" in MAIN
    assert "recomp_options_resolution_size" not in internal


def test_live_resolution_change_preserves_render_target_history() -> None:
    migration = PGRAPH[
        PGRAPH.index("static int prepare_color_surface_migrations("):
        PGRAPH.index("void pgraph_d3d11_set_internal_resolution(")
    ]
    setter = PGRAPH[
        PGRAPH.index("void pgraph_d3d11_set_internal_resolution("):
        PGRAPH.index("static GuestColorSurface *find_guest_color_surface(")
    ]
    assert "d3d8_CopyTextureToRenderTarget(surface->srv, migration->rtv" in migration
    assert "surface->logical_width * internal_width" in migration
    assert "surface->logical_height * internal_height" in migration
    assert "if (!prepare_color_surface_migrations(width, height, migrations))" in setter
    assert "surface->texture = migration->texture;" in setter
    assert "surface->rtv = migration->rtv;" in setter
    assert "surface->srv = migration->srv;" in setter
    assert "release_guest_color_surface(&g_pg.color_surfaces[i])" not in setter
    assert "g_pg.last_drawn_color = NULL" not in setter
    assert "g_pg.last_resolved_color = NULL" not in setter
    assert "g_pg.pending_scanout = NULL" not in setter

def test_live_resolution_transition_regression_is_opt_in() -> None:
    assert 'getenv("MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS")' in MAIN
    assert "MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS_DELAY_MS" in MAIN
    assert "MERCENARIES_TEST_INTERNAL_RESOLUTION_CAPTURE_PREFIX" in MAIN
    transition = MAIN[MAIN.index("if (test_resolution_transitions && completed_scanout"):MAIN.index("if (refreshed && g_xbox_mem_offset")]
    assert "MERCENARIES_CAPTURE_DISPLAY_PREFIX" not in transition
    assert "d3d8_DebugStartDisplayCapture(prefix, 500u, 64u)" in MAIN
    assert "test_resolution_frames == 10u" in MAIN
    assert "pgraph_d3d11_set_internal_resolution(2880u, 2160u)" in MAIN
    assert "test_resolution_frames == 30u" in MAIN
    assert "pgraph_d3d11_set_internal_resolution(640u, 480u)" in MAIN
    assert "[switch]$TestInternalResolutionTransitions" in RUNNER
    assert "MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS = '1'" in RUNNER
    assert "InternalResolutionTransitionDelayMs" in RUNNER
    assert "MERCENARIES_TEST_INTERNAL_RESOLUTION_CAPTURE_PREFIX" in RUNNER
    assert '"MERCENARIES_TEST_PRESENTATION_RESIZE_TRANSITIONS"' in MAIN
    assert "d3d8_ResizePresentation((UINT)GetSystemMetrics(SM_CXSCREEN)" in transition
    assert "[switch]$TestPresentationResizeTransitions" in RUNNER
    assert "MERCENARIES_TEST_PRESENTATION_RESIZE_TRANSITIONS = '1'" in RUNNER

def test_aspect_and_draw_distance_use_deterministic_generated_hooks() -> None:
    patches = runpy.run_path(str(PATCHER))["PATCHES"]
    names = {patch.name for patch in patches}
    assert "RedCamera SetDrawDistance recomp option" in names
    assert "RedCamera SetPerspective recomp aspect option" in names
    generated = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((PORT / "src" / "recomp" / "gen").glob("recomp_*.c"))
    )
    assert "recomp_options_scale_draw_distance" in generated
    assert "recomp_options_perspective_fov" in generated
    assert "recomp_options_perspective_aspect" in generated
    assert "static const float multipliers[] = { 1.0f, 1.25f, 1.5f, 2.0f }" in OPTIONS
    assert "tanf(horizontal_fov * 0.5f) * guest_aspect / target_aspect" in OPTIONS


def test_npc_wake_distance_is_optional_and_resolution_independent() -> None:
    assert "RECOMP_OPTIONS_WAKE_HASH" in OPTIONS_HEADER
    assert "RECOMP_OPTIONS_CHANGE_WAKE" in OPTIONS_HEADER
    assert "RECOMP_OPTIONS_WAKE_HASH" in MENU
    assert "RECOMP_OPTIONS_DISTANCE_HASH" not in MENU
    assert '"NpcWakeDistance", 0, g_options.path), 3' in OPTIONS
    assert 'static const char *wake_distances[] = { "ORIGINAL", "150%", "200%", "300%" };' in OPTIONS
    assert "static const float multipliers[] = { 1.0f, 1.5f, 2.0f, 3.0f };" in OPTIONS

    front = OPTIONS[
        OPTIONS.index("float recomp_options_scale_ai_visibility_threshold"):
        OPTIONS.index("float recomp_options_scale_ai_behind_distance_squared")
    ]
    behind = OPTIONS[
        OPTIONS.index("float recomp_options_scale_ai_behind_distance_squared"):
        OPTIONS.index("static float selected_camera_aspect")
    ]
    assert "threshold / npc_wake_distance_multiplier()" in front
    assert "resolution" not in front.lower()
    assert "distance_squared * multiplier * multiplier" in behind

    patches = runpy.run_path(str(PATCHER))["PATCHES"]
    by_name = {patch.name: patch for patch in patches}
    front_patch = by_name[
        "human AI LOD applies the user-selected front-camera wake distance"
    ]
    behind_patch = by_name[
        "human AI LOD applies the user-selected behind-camera wake distance"
    ]
    assert "loc_00050640" in front_patch.before
    assert "recomp_options_scale_ai_visibility_threshold(xmm0)" in front_patch.after
    assert "loc_0005071E" in behind_patch.before
    assert "recomp_options_scale_ai_behind_distance_squared" in behind_patch.after

    generated = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((PORT / "src" / "recomp" / "gen").glob("recomp_*.c"))
    )
    assert "recomp_options_scale_ai_visibility_threshold" in generated
    assert "recomp_options_scale_ai_behind_distance_squared" in generated


def test_npc_draw_distance_and_lod_preserve_original_defaults() -> None:
    assert "RECOMP_OPTIONS_NPC_DISTANCE_HASH" in OPTIONS_HEADER
    assert "RECOMP_OPTIONS_NPC_LOD_HASH" in OPTIONS_HEADER
    assert "loaded.npc_draw_distance = 0;" in OPTIONS
    assert '"NpcLOD", 0, g_options.path), 1' in OPTIONS
    assert "lod_adjustment_type != 5" in OPTIONS
    assert "lod_adjustment_type == 5" in OPTIONS
    assert "npc_draw_distance" in OPTIONS
    patches = runpy.run_path(str(PATCHER))["PATCHES"]
    by_name = {patch.name: patch for patch in patches}
    cull = by_name["RedModel human far cull applies NPC draw distance option"]
    civ_center = by_name[
        "ambient civilian spawn center applies NPC draw distance option"
    ]
    civ_radius = by_name[
        "ambient civilian spawn radius applies NPC draw distance option"
    ]
    lod = by_name["RedModel human LOD can remain high while preserving far cull"]
    assert "recomp_options_scale_npc_draw_distance" in cull.after
    assert civ_center.after.count("recomp_options_scale_ambient_civ_distance") == 2
    assert civ_radius.after.count("recomp_options_scale_ambient_civ_distance") == 6
    assert "0x22CAC" in civ_center.before and "0x22CA8" in civ_center.before
    assert "0x22CB4" in civ_radius.before and "0x22CB0" in civ_radius.before
    assert "recomp_options_force_high_npc_lod" in lod.after
    assert "recomp_options_high_npc_lod_mask" in lod.after
    assert "MEMF(esp + 0x20)" not in lod.after
    assert "loc_00220CA2" in lod.before


def test_hor_plus_preserves_retail_projected_lod_visibility() -> None:
    helper = OPTIONS[
        OPTIONS.index("float recomp_options_scale_camera_visibility"):
        OPTIONS.index("static float npc_wake_distance_multiplier")
    ]
    assert "4.0f / 3.0f" in helper
    assert "6.0f / 5.0f" in helper
    assert "7.0f / 4.0f" in helper
    assert "g_options.applied.aspect" in helper
    assert "resolution" in helper.lower()
    assert "g_options.applied.resolution" not in helper
    assert "corrected < 1.0f ? corrected : 1.0f" in helper

    patches = runpy.run_path(str(PATCHER))["PATCHES"]
    patch = {
        item.name: item for item in patches
    }["RsActor camera visibility preserves retail LOD under Hor+ aspect"]
    assert "loc_00015243" in patch.before
    assert "recomp_options_scale_camera_visibility(xmm0)" in patch.after

    generated = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted((PORT / "src" / "recomp" / "gen").glob("recomp_*.c"))
    )
    assert "recomp_options_scale_camera_visibility(xmm0)" in generated


def test_live_aspect_apply_refreshes_the_retail_camera_projection() -> None:
    assert "recomp_options_refresh_retail_camera_projection();" in MAIN
    assert "RECOMP_OPTIONS_CHANGE_ASPECT | RECOMP_OPTIONS_CHANGE_FOV" in MAIN
    assert "void recomp_options_refresh_retail_camera_projection(void);" in OPTIONS_HEADER
    assert "void recomp_options_refresh_retail_camera_projection(void)" in MANUAL
    refresh = MANUAL[
        MANUAL.index("void recomp_options_refresh_retail_camera_projection(void)"):
        MANUAL.index("static void recomp_print_red_lookup_string")
    ]
    assert "0x006437B4u" in refresh
    assert "0x006437D0u" in refresh
    assert "0x00300DC4u" in refresh
    assert "0x00300DC8u" in refresh and "0x00300DCCu" in refresh
    assert "recomp_lookup(0x001F8F10u)" in refresh
    assert "recomp_options_perspective_fov(fov, aspect)" in refresh
    assert "recomp_options_perspective_aspect(aspect)" in refresh
    assert "recomp_save_guest_cpu_context(&saved);" in refresh
    assert "recomp_restore_guest_cpu_context(&saved);" in refresh
    assert "0x3E4CCCCDu" in refresh


if __name__ == "__main__":
    test_options_are_source_controlled_and_portable()
    test_requested_menu_shape_and_placement()
    test_selected_custom_label_is_redrawn_above_retail_highlight()
    test_settings_persist_beside_the_executable()
    test_renderer_settings_have_real_backing_behavior()
    test_resolution_scale_is_true_internal_rendering()
    test_supersampling_scales_only_the_internal_render_target()
    test_live_resolution_change_preserves_render_target_history()
    test_live_resolution_transition_regression_is_opt_in()
    test_aspect_and_draw_distance_use_deterministic_generated_hooks()
    test_npc_wake_distance_is_optional_and_resolution_independent()
    test_npc_draw_distance_and_lod_preserve_original_defaults()
    test_hor_plus_preserves_retail_projected_lod_visibility()
    test_live_aspect_apply_refreshes_the_retail_camera_projection()
    print("ok mercenaries recomp options")

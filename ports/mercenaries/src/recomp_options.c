#include "recomp_controls.h"
#include "recomp_options.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct recomp_option_values {
    int fps_cap, aspect, resolution, anisotropic, display, haze,
        wake_distance, npc_draw_distance, npc_lod, fixed_xbox_prompts, object_distance, og_bugs, ps2_upgrades;
} recomp_option_values;

typedef struct recomp_options_state {
    int initialized;
    recomp_option_values applied;
    recomp_option_values pending;
    recomp_options_apply_callback callback;
    char path[MAX_PATH];
} recomp_options_state;

static recomp_options_state g_options;
/* Choice order is shared by the menu and persistence; zero FPS means uncapped. */
static const int g_fps_caps[] = {30, 60, 90, 120, 0};
static const char *g_fps_cap_labels[] = {"30", "60", "90", "120", "UNCAPPED"};
/* Keep the active world consistent until its next full load. */
static int g_object_distance_for_world;
static int clamp_setting(int value, int maximum) { return value < 0 ? 0 : (value > maximum ? maximum : value); }
static void build_config_path(char path[MAX_PATH]) {
    DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH); char *separator;
    if (length == 0u || length >= MAX_PATH) { strcpy_s(path, MAX_PATH, "mercenaries_recomp.ini"); return; }
    separator = strrchr(path, '\\');
    if (separator != NULL) separator[1] = '\0'; else path[0] = '\0';
    strcat_s(path, MAX_PATH, "mercenaries_recomp.ini");
}
static void write_setting(const char *name, int value) {
    char text[16]; _snprintf_s(text, sizeof(text), _TRUNCATE, "%d", value);
    WritePrivateProfileStringA("RecompOptions", name, text, g_options.path);
}
static void write_all_settings(void) {
    write_setting("FPSCap", g_fps_caps[g_options.applied.fps_cap]);
    write_setting("60FPS", g_options.applied.fps_cap != 0); /* older-build rollback */
    write_setting("AspectRatio", g_options.applied.aspect);
    write_setting("DrawDistance", 0); /* Clear legacy overrides when settings are saved. */
    write_setting("ResolutionScale", g_options.applied.resolution);
    write_setting("Anisotropic16x", g_options.applied.anisotropic);
    write_setting("DisplayMode", g_options.applied.display);
    write_setting("AuthenticHaze", g_options.applied.haze != 0); /* older-build rollback */
    write_setting("HazeMode", g_options.applied.haze);
    write_setting("ObjectDrawDistance", g_options.applied.object_distance);
    write_setting("NpcWakeDistance", g_options.applied.wake_distance);
    write_setting("NpcDrawDistance", g_options.applied.npc_draw_distance);
    write_setting("NpcLOD", g_options.applied.npc_lod);
    write_setting("FixedXboxPrompts", g_options.applied.fixed_xbox_prompts);
    write_setting("OGBugs", g_options.applied.og_bugs);
    write_setting("PS2Upgrades", g_options.applied.ps2_upgrades);
    /* Retired persistent setting: flock creation is an F9 action. */
    WritePrivateProfileStringA("RecompOptions", "BoidSimulation", NULL, g_options.path);
}
static void apply_test_overrides(recomp_option_values *values) {
    char text[16];
    char *end;
    long value;
    DWORD length;
    if (!values) return;
    length = GetEnvironmentVariableA("MERCENARIES_TEST_RESOLUTION_SCALE",
                                     text, sizeof(text));
    if (length == 0u || length >= sizeof(text)) return;
    value = strtol(text, &end, 10);
    if (end == text || *end != '\0') return;
    values->resolution = clamp_setting((int)value, 5);
}
static int load_fps_cap(void) {
    char text[32], *end;
    long fps;
    int legacy = clamp_setting(GetPrivateProfileIntA(
        "RecompOptions", "60FPS", 0, g_options.path), 1);
    GetPrivateProfileStringA("RecompOptions", "FPSCap", "", text, sizeof(text), g_options.path);
    if (!text[0]) return legacy;
    fps = strtol(text, &end, 10);
    if (end != text && *end == '\0') {
        for (unsigned i = 0; i < sizeof(g_fps_caps) / sizeof(g_fps_caps[0]); ++i)
            if (fps == g_fps_caps[i]) return (int)i;
    }
    return legacy; /* Malformed values must not accidentally select uncapped. */
}
void recomp_options_init(void) {
    recomp_option_values loaded;
    if (g_options.initialized) return;
    build_config_path(g_options.path);
    loaded.fps_cap = load_fps_cap();
    loaded.aspect = clamp_setting(GetPrivateProfileIntA("RecompOptions", "AspectRatio", 0, g_options.path), 3);
    /* Legacy DrawDistance is deliberately ignored: the retail terrain cache
     * has a fixed budget and extending camera distance can exhaust it. */
    loaded.resolution = clamp_setting(GetPrivateProfileIntA("RecompOptions", "ResolutionScale", 0, g_options.path), 5);
    loaded.anisotropic = clamp_setting(GetPrivateProfileIntA("RecompOptions", "Anisotropic16x", 0, g_options.path), 1);
    loaded.display = clamp_setting(GetPrivateProfileIntA("RecompOptions", "DisplayMode", 0, g_options.path), 2);
    /* Preserve existing enabled haze as Unfiltered; new installations use
     * Authentic. The legacy key remains readable for rollback builds. */
    int legacy_haze = (int)GetPrivateProfileIntA("RecompOptions", "AuthenticHaze", -1, g_options.path);
    loaded.haze = clamp_setting(GetPrivateProfileIntA("RecompOptions", "HazeMode",
        legacy_haze < 0 ? 1 : (legacy_haze ? 2 : 0), g_options.path), 2);
    /* Disabled until scenery and terrain can share a validated extended range.
     * Ignore persisted overrides too: the retail terrain cache has 64 embedded
     * entries, and extending only the frustum exposes its near cutoff. */
    loaded.object_distance = 0;
    loaded.wake_distance = clamp_setting(GetPrivateProfileIntA(
        "RecompOptions", "NpcWakeDistance", 0, g_options.path), 3);
    /* Retired option: old INI values must not extend human culling or civilian spawning. */
    loaded.npc_draw_distance = 0;
    loaded.npc_lod = clamp_setting(GetPrivateProfileIntA(
        "RecompOptions", "NpcLOD", 0, g_options.path), 1);
    loaded.fixed_xbox_prompts = clamp_setting(GetPrivateProfileIntA(
        "RecompOptions", "FixedXboxPrompts", 0, g_options.path), 1);
    /* Preserve original behavior until the player opts into original-game fixes. */
    loaded.og_bugs = clamp_setting(GetPrivateProfileIntA(
        "RecompOptions", "OGBugs", 1, g_options.path), 1);
    /* Diagnostic runs need isolated native/scaled A/B comparisons without
     * rewriting the user's persisted menu choices. The override is read once
     * at startup and never written back to mercenaries_recomp.ini. */
    loaded.ps2_upgrades = clamp_setting(GetPrivateProfileIntA(
        "RecompOptions", "PS2Upgrades", 0, g_options.path), 1);
    apply_test_overrides(&loaded);
    g_object_distance_for_world = loaded.object_distance;
    g_options.applied = loaded;
    g_options.pending = loaded;
    g_options.initialized = 1;
}
void recomp_options_set_apply_callback(recomp_options_apply_callback callback) {
    recomp_options_init();
    g_options.callback = callback;
}
void recomp_options_begin_edit(void) {
    recomp_options_init();
    g_options.pending = g_options.applied;
}
void recomp_options_cancel_edit(void) {
    recomp_options_init();
    g_options.pending = g_options.applied;
}
static uint32_t changed_options(void) {
    uint32_t changes = 0u;
    if (g_options.pending.object_distance != g_options.applied.object_distance) changes |= RECOMP_OPTIONS_CHANGE_OBJECT_DISTANCE;
    if (g_options.pending.fps_cap != g_options.applied.fps_cap) changes |= RECOMP_OPTIONS_CHANGE_FPS;
    if (g_options.pending.aspect != g_options.applied.aspect) changes |= RECOMP_OPTIONS_CHANGE_ASPECT;
    if (g_options.pending.resolution != g_options.applied.resolution) changes |= RECOMP_OPTIONS_CHANGE_RESOLUTION;
    if (g_options.pending.anisotropic != g_options.applied.anisotropic) changes |= RECOMP_OPTIONS_CHANGE_AF;
    if (g_options.pending.display != g_options.applied.display) changes |= RECOMP_OPTIONS_CHANGE_DISPLAY;
    if (g_options.pending.haze != g_options.applied.haze) changes |= RECOMP_OPTIONS_CHANGE_HAZE;
    if (g_options.pending.wake_distance != g_options.applied.wake_distance) changes |= RECOMP_OPTIONS_CHANGE_WAKE;
    if (g_options.pending.npc_draw_distance != g_options.applied.npc_draw_distance) changes |= RECOMP_OPTIONS_CHANGE_NPC_DISTANCE;
    if (g_options.pending.npc_lod != g_options.applied.npc_lod) changes |= RECOMP_OPTIONS_CHANGE_NPC_LOD;
    if (g_options.pending.fixed_xbox_prompts != g_options.applied.fixed_xbox_prompts) changes |= RECOMP_OPTIONS_CHANGE_FIXED_XBOX;
    if (g_options.pending.og_bugs != g_options.applied.og_bugs) changes |= RECOMP_OPTIONS_CHANGE_OG_BUGS;
    if (g_options.pending.ps2_upgrades != g_options.applied.ps2_upgrades) changes |= RECOMP_OPTIONS_CHANGE_PS2_UPGRADES;
    return changes;
}
uint32_t recomp_options_apply(void) {
    uint32_t changes;
    recomp_options_init();
    changes = changed_options();
    if (!changes) return 0u;
    g_options.applied = g_options.pending;
    write_all_settings();
    if (g_options.callback) g_options.callback(changes);
    return changes;
}
static int cycle(int value, int maximum, int direction) {
    value += direction < 0 ? -1 : 1;
    if (value < 0) value = maximum; else if (value > maximum) value = 0;
    return value;
}
int recomp_options_adjust(uint32_t hash, int direction) {
    recomp_options_init();
    switch (hash) {
    case RECOMP_OPTIONS_OBJECT_DISTANCE_HASH: return 1; /* Disabled; consume without changing. */
    case RECOMP_OPTIONS_FPS_HASH: g_options.pending.fps_cap = cycle(g_options.pending.fps_cap, 4, direction); return 1;
    case RECOMP_OPTIONS_ASPECT_HASH: g_options.pending.aspect = cycle(g_options.pending.aspect, 3, direction); return 1;
    case RECOMP_OPTIONS_RESOLUTION_HASH: g_options.pending.resolution = cycle(g_options.pending.resolution, 5, direction); return 1;
    case RECOMP_OPTIONS_AF_HASH: g_options.pending.anisotropic ^= 1; return 1;
    case RECOMP_OPTIONS_DISPLAY_HASH: g_options.pending.display = cycle(g_options.pending.display, 2, direction); return 1;
    case RECOMP_OPTIONS_HAZE_HASH: g_options.pending.haze = cycle(g_options.pending.haze, 2, direction); return 1;
    case RECOMP_OPTIONS_WAKE_HASH: g_options.pending.wake_distance = cycle(g_options.pending.wake_distance, 3, direction); return 1;
    case RECOMP_OPTIONS_NPC_DISTANCE_HASH: return 1; /* Retired; keep the native distance. */
    case RECOMP_OPTIONS_NPC_LOD_HASH: g_options.pending.npc_lod ^= 1; return 1;
    case RECOMP_OPTIONS_FIXED_XBOX_HASH: g_options.pending.fixed_xbox_prompts ^= 1; return 1;
    case RECOMP_OPTIONS_OG_BUGS_HASH: g_options.pending.og_bugs ^= 1; return 1;
    case RECOMP_OPTIONS_PS2_UPGRADES_HASH: g_options.pending.ps2_upgrades ^= 1; return 1;
    default: return 0;
    }
}
const char *recomp_options_label(uint32_t hash) {
    static char label[96];
    static const char *haze_modes[] = { "OFF", "AUTHENTIC", "UNFILTERED" };
    static const char *aspects[] = { "ORIGINAL (4:3)", "16:9", "16:10", "21:9" };
    static const char *distances[] = { "ORIGINAL", "125%", "150%", "200%" };
    static const char *resolutions[] = { "NATIVE (640 X 480)", "1280 X 720", "1600 X 900", "1920 X 1080", "2560 X 1440", "3840 X 2160" };
    static const char *displays[] = { "WINDOWED", "BORDERLESS", "FULLSCREEN" };
    static const char *wake_distances[] = { "ORIGINAL", "150%", "200%", "300%" };
    recomp_options_init();
    if(hash>=RECOMP_ACK_ROW && hash<RECOMP_ACK_ROW+RECOMP_ACK_ROWS){
        static const char *lines[RECOMP_ACK_ROWS]={
            "Mercenaries Recompiled by Trenton Turner",
            "(AKA KraftMacAndChee)",
            "This project would not have been possible without:",
            "-Xemu", "-Xboxrecomp", "-Pandemic Studios", "-Cxbx", "-xdvdfs",
            "-Phantom Zero", "-Wyther", "-Daniel Mobley", "-The Menace.pro modding community",
            "Thank you to everyone listed here. Mercenaries",
            "Recompiled would not exist in its current state",
            "without you!", "BACK"};
        return lines[hash-RECOMP_ACK_ROW];
    }
    switch (hash) {
    case RECOMP_ACK_MENU_HASH:return "ACKNOWLEDGEMENTS";
    case RECOMP_QUIT_HASH:return "QUIT TO DESKTOP";
    case RECOMP_OPTIONS_OBJECT_DISTANCE_HASH: return "OBJECT DRAW DISTANCE: ORIGINAL (DISABLED)";
    case RECOMP_OPTIONS_MENU_HASH: return "RECOMP OPTIONS";
    case RECOMP_OPTIONS_FPS_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "FPS CAP: %s", g_fps_cap_labels[g_options.pending.fps_cap]); break;
    case RECOMP_OPTIONS_ASPECT_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "ASPECT RATIO: %s", aspects[g_options.pending.aspect]); break;
    case RECOMP_OPTIONS_RESOLUTION_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "RESOLUTION SCALE: %s", resolutions[g_options.pending.resolution]); break;
    case RECOMP_OPTIONS_AF_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "ANISOTROPIC FILTERING: %s", g_options.pending.anisotropic ? "16X" : "OFF"); break;
    case RECOMP_OPTIONS_DISPLAY_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "DISPLAY: %s", displays[g_options.pending.display]); break;
    case RECOMP_OPTIONS_HAZE_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "HAZE: %s", haze_modes[g_options.pending.haze]); break;
    case RECOMP_OPTIONS_WAKE_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "NPC WAKE DISTANCE: %s", wake_distances[g_options.pending.wake_distance]); break;
    case RECOMP_OPTIONS_NPC_DISTANCE_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "NPC DRAW DISTANCE: %s", distances[g_options.pending.npc_draw_distance]); break;
    case RECOMP_OPTIONS_NPC_LOD_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "NPC LOD: %s", g_options.pending.npc_lod ? "ALWAYS HIGH" : "ORIGINAL"); break;
    case RECOMP_OPTIONS_FIXED_XBOX_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "FIXED XBOX PROMPTS: %s", g_options.pending.fixed_xbox_prompts ? "ON" : "OFF"); break;
    case RECOMP_OPTIONS_PS2_UPGRADES_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "PS2 UPGRADES: %s", g_options.pending.ps2_upgrades ? "ON" : "OFF"); break;
    case RECOMP_OPTIONS_OG_BUGS_HASH: _snprintf_s(label, sizeof(label), _TRUNCATE, "OG BUGS: %s", g_options.pending.og_bugs ? "ON" : "OFF"); break;
    case RECOMP_OPTIONS_APPLY_HASH: return changed_options() ? "APPLY" : "APPLY (NO CHANGES)";
    default: return NULL;
    }
    return label;
}
uint32_t recomp_options_localization_hash(uint32_t hash) {
    if(hash==RECOMP_ACK_MENU_HASH || hash==RECOMP_QUIT_HASH ||
       (hash>=RECOMP_ACK_ROW && hash<RECOMP_ACK_ROW+RECOMP_ACK_ROWS))return 0x941EE5E8u;
    if(hash>=RECOMP_CONTROLS_TITLE && hash<RECOMP_CONTROLS_ROW+RECOMP_CONTROLS_ROWS)return 0x941EE5E8u;
    switch (hash) {
    case RECOMP_OPTIONS_OBJECT_DISTANCE_HASH:
    case RECOMP_OPTIONS_MENU_HASH:
    case RECOMP_OPTIONS_FPS_HASH:
    case RECOMP_OPTIONS_ASPECT_HASH:
    case RECOMP_OPTIONS_RESOLUTION_HASH:
    case RECOMP_OPTIONS_AF_HASH:
    case RECOMP_OPTIONS_DISPLAY_HASH:
    case RECOMP_OPTIONS_HAZE_HASH:
    case RECOMP_OPTIONS_WAKE_HASH:
    case RECOMP_OPTIONS_NPC_DISTANCE_HASH:
    case RECOMP_OPTIONS_NPC_LOD_HASH:
    case RECOMP_OPTIONS_FIXED_XBOX_HASH:
    case RECOMP_OPTIONS_OG_BUGS_HASH:
    case RECOMP_OPTIONS_PS2_UPGRADES_HASH:
    case RECOMP_OPTIONS_APPLY_HASH:
        return 0x941EE5E8u; /* retail OPTIONS: valid text/style metadata */
    default:
        return hash;
    }
}
int recomp_options_fps_cap(void) { recomp_options_init(); return g_fps_caps[g_options.applied.fps_cap]; }
int recomp_options_aspect_ratio(void) { recomp_options_init(); return g_options.applied.aspect; }
int recomp_options_draw_distance(void) { return 0; }
int recomp_options_resolution(void) { recomp_options_init(); return g_options.applied.resolution; }
int recomp_options_anisotropic_16x(void) { recomp_options_init(); return g_options.applied.anisotropic; }
int recomp_options_authentic_haze(void) { recomp_options_init(); return g_options.applied.haze; }
int recomp_options_npc_wake_distance(void) { recomp_options_init(); return g_options.applied.wake_distance; }
int recomp_options_npc_draw_distance(void) { recomp_options_init(); return g_options.applied.npc_draw_distance; }
int recomp_options_object_draw_distance(void) { recomp_options_init(); return g_options.applied.object_distance; }
int recomp_options_npc_lod(void) { recomp_options_init(); return g_options.applied.npc_lod; }
int recomp_options_fixed_xbox_prompts(void) { recomp_options_init(); return g_options.applied.fixed_xbox_prompts; }
int recomp_options_og_bugs(void) { recomp_options_init(); return g_options.applied.og_bugs; }
recomp_display_mode recomp_options_display_mode(void) { recomp_options_init(); return (recomp_display_mode)g_options.applied.display; }
void recomp_options_resolution_size(uint32_t *width, uint32_t *height) {
    static const uint32_t sizes[][2] = { {640u,480u}, {1280u,720u}, {1600u,900u}, {1920u,1080u}, {2560u,1440u}, {3840u,2160u} };
    recomp_options_init(); if (width) *width = sizes[g_options.applied.resolution][0]; if (height) *height = sizes[g_options.applied.resolution][1];
}
void recomp_options_internal_resolution_size(uint32_t *width, uint32_t *height) {
    static const uint32_t heights[] = { 480u, 720u, 900u, 1080u, 1440u, 2160u };
    uint32_t h, w;
    recomp_options_init();
    h = heights[g_options.applied.resolution];
    /* Xbox render targets are 4:3 even when the title advertises anamorphic
     * widescreen. Scale both axes uniformly; presentation aspect and the
     * camera hook handle widescreen. */
    w = (uint32_t)(((uint64_t)h * 4u + 1u) / 3u);
    if (width) *width = w;
    if (height) *height = h;
}
void recomp_options_presentation_aspect(uint32_t *width, uint32_t *height) {
    static const uint32_t ratios[][2] = { {4u,3u}, {16u,9u}, {16u,10u}, {21u,9u} };
    recomp_options_init(); if (width) *width = ratios[g_options.applied.aspect][0]; if (height) *height = ratios[g_options.applied.aspect][1];
}
float recomp_options_scale_draw_distance(float distance) {
    /* Keep the generated hook ABI, but always preserve the authored distance. */
    return distance;
}
float recomp_options_scale_camera_visibility(float visibility) {
    /* RedCamera's retail screen-real-estate metric is based on horizontal
     * frustum width. Hor+ widescreen deliberately widens that frustum while
     * keeping vertical composition unchanged, which would otherwise lower
     * every authored LOD/AI visibility value and retire wheels/actors early.
     * Restore the 4:3-equivalent metric; this depends on aspect, never on
     * internal render resolution. */
    static const float horizontal_expansion[] = {
        1.0f, 4.0f / 3.0f, 6.0f / 5.0f, 7.0f / 4.0f
    };
    float corrected;
    recomp_options_init();
    if (visibility <= 0.0f) return visibility;
    corrected = visibility * horizontal_expansion[g_options.applied.aspect];
    return corrected < 1.0f ? corrected : 1.0f;
}
static float npc_wake_distance_multiplier(void) {
    static const float multipliers[] = { 1.0f, 1.5f, 2.0f, 3.0f };
    recomp_options_init();
    return multipliers[g_options.applied.wake_distance];
}
static float npc_draw_distance_multiplier(void) {
    static const float multipliers[] = { 1.0f, 1.25f, 1.5f, 2.0f };
    recomp_options_init();
    return multipliers[g_options.applied.npc_draw_distance];
}
float recomp_options_scale_ai_visibility_threshold(float threshold) {
    /* Camera visibility is inversely proportional to distance, so dividing
     * the authored projected-size cutoff by N wakes an actor at N times the
     * retail distance. This is deliberately independent of render scale. */
    return threshold / npc_wake_distance_multiplier();
}
float recomp_options_scale_ai_behind_distance_squared(float distance_squared) {
    const float multiplier = npc_wake_distance_multiplier();
    return distance_squared * multiplier * multiplier;
}
float recomp_options_scale_npc_draw_distance(float distance,
                                              int lod_adjustment_type) {
    /* RedModel::LODH_L12FADE_SPAWNSLIDE_HUMAN is 5.  Leave vehicles,
     * buildings, effects, and all retail defaults untouched. */
    if (lod_adjustment_type != 5) return distance;
    return distance * npc_draw_distance_multiplier();
}
float recomp_options_scale_ambient_civ_distance(float distance) {
    /* Ambient civilians have a separate spawn/destroy disc in
     * RsTrafficManager. Scale it with their human-model cull so the option
     * does not merely leave empty space beyond the retail radius. */
    return distance * npc_draw_distance_multiplier();
}
int recomp_options_force_high_npc_lod(int lod_adjustment_type) {
    recomp_options_init();
    return lod_adjustment_type == 5 && g_options.applied.npc_lod != 0;
}
uint32_t recomp_options_high_npc_lod_mask(uint32_t mask) {
    /* Human heuristic retains mesh bits 1 and 2; bits 4/8 represent its
     * absent far meshes and must stay absent throughout the spawn fade. */
    return mask == 2u ? 1u : mask;
}
/* Scenery only: do not extend soldiers, traffic, vehicles or mission logic. */
int recomp_options_begin_world_load(void) {
    recomp_options_init();
    int previous = g_object_distance_for_world;
    g_object_distance_for_world = 0; /* Do not revive a stale extended range. */
    fprintf(stderr, "[RECOMP-OPTIONS] world load object distance: %d -> %d\n",
            previous, g_object_distance_for_world);
    return previous != g_object_distance_for_world;
}
float recomp_options_object_distance_multiplier(void) {
    /* Keep objects, activation grid and camera inside the retail terrain range.
     * Deliberately independent of saved/pending values while disabled. */
    return 1.0f;
}
float recomp_options_scale_object_distance(float distance, uint32_t type) {
    switch (type) {
    case 0xD93E11F8u: /* prop */
    case 0x1AC5539Bu: /* constrained */
    case 0xBBCAE592u: /* plant */
    case 0x49451132u: /* fixture */
    case 0x6D8B34D5u: /* tree */
    case 0xD290C23Bu: /* static */
    case 0x97AA0656u: /* constrainedlarge */
    case 0x175E9A40u: /* crate */
    case 0x509EDBBCu: /* carprop */
    case 0xF1DDE0F2u: /* propruin */
    case 0x9625EE4Du: /* largeprop */
    case 0x06D55D44u: /* treelarge */
    case 0xC0FC1F76u: /* staticlarge */
    case 0x660A425Du: /* fixturelarge */
    case 0x38DF0375u: /* building */
        return distance * recomp_options_object_distance_multiplier();
    default: return distance;
    }
}
static float selected_camera_aspect(float guest_aspect) {
    /* Preserve retail 4:3 object proportions after presentation scaling.
     * Camera aspect is height/width. The old 0.530 widescreen calibration
     * compressed world geometry horizontally by about 8% versus 4:3. */
    static const float aspects[] = {
        0.0f, 0.764f * (4.0f / 3.0f) / (16.0f / 9.0f),
        0.764f * (4.0f / 3.0f) / (16.0f / 10.0f),
        0.764f * (4.0f / 3.0f) / (21.0f / 9.0f)
    };
    recomp_options_init(); return g_options.applied.aspect == 0 ? guest_aspect : aspects[g_options.applied.aspect];
}
float recomp_options_perspective_fov(float horizontal_fov, float guest_aspect) {
    const float target_aspect = selected_camera_aspect(guest_aspect);
    if (target_aspect <= 0.0f || guest_aspect <= 0.0f) return horizontal_fov;
    return 2.0f * atanf(tanf(horizontal_fov * 0.5f) * guest_aspect / target_aspect);
}
float recomp_options_perspective_aspect(float guest_aspect) {
    return selected_camera_aspect(guest_aspect);
}

/* The retail satellite HUD uses a 512-unit horizontal canvas. Undo only
 * widescreen presentation's extra horizontal expansion around its center.
 * This is deliberately not a global HUD or camera transform. */
static float satellite_center_horizontal_scale(void) {
    uint32_t width, height;
    recomp_options_presentation_aspect(&width, &height);
    return (4.0f * (float)height) / (3.0f * (float)width);
}
float recomp_options_satellite_center_x(float x) {
    const float scale = satellite_center_horizontal_scale();
    return scale == 1.0f ? x : 256.0f + (x - 256.0f) * scale;
}
float recomp_options_satellite_center_width(float width) {
    return width * satellite_center_horizontal_scale();
}

/* Screen position and authored shape are independent under widescreen. */
float recomp_options_ui_x(float x, float anchor) {
    const float scale = satellite_center_horizontal_scale();
    return scale == 1.0f ? x : anchor + (x - anchor) * scale;
}

int recomp_options_ps2_upgrades(void) { recomp_options_init(); return g_options.applied.ps2_upgrades; }

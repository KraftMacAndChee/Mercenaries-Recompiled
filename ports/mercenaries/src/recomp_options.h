#ifndef MERCENARIES_RECOMP_OPTIONS_H
#define MERCENARIES_RECOMP_OPTIONS_H
#include <stdint.h>
#define RECOMP_ACK_MENU_HASH 0x52434143u
#define RECOMP_QUIT_HASH 0x52435154u
#define RECOMP_ACK_ROW 0x52435000u
#define RECOMP_ACK_ROWS 16u
int recomp_acknowledgements_active(void);
#define RECOMP_OPTIONS_MENU_HASH       0x52434F50u
#define RECOMP_OPTIONS_FPS_HASH        0x52430001u
#define RECOMP_OPTIONS_ASPECT_HASH     0x52430002u
#define RECOMP_OPTIONS_DISTANCE_HASH   0x52430003u
#define RECOMP_OPTIONS_RESOLUTION_HASH 0x52430004u
#define RECOMP_OPTIONS_AF_HASH         0x52430005u
#define RECOMP_OPTIONS_DISPLAY_HASH    0x52430006u
#define RECOMP_OPTIONS_APPLY_HASH      0x52430007u
#define RECOMP_OPTIONS_OBJECT_DISTANCE_HASH 0x5243000Du
#define RECOMP_OPTIONS_HAZE_HASH       0x52430008u
#define RECOMP_OPTIONS_WAKE_HASH       0x52430009u
#define RECOMP_OPTIONS_NPC_DISTANCE_HASH 0x5243000Au
#define RECOMP_OPTIONS_NPC_LOD_HASH      0x5243000Bu
#define RECOMP_OPTIONS_FIXED_XBOX_HASH   0x5243000Cu
#define RECOMP_OPTIONS_OG_BUGS_HASH      0x5243000Eu
#define RECOMP_OPTIONS_PS2_UPGRADES_HASH 0x5243000Fu
enum recomp_options_change {
    RECOMP_OPTIONS_CHANGE_FPS        = 1u << 0,
    RECOMP_OPTIONS_CHANGE_ASPECT     = 1u << 1,
    RECOMP_OPTIONS_CHANGE_DISTANCE   = 1u << 2,
    RECOMP_OPTIONS_CHANGE_RESOLUTION = 1u << 3,
    RECOMP_OPTIONS_CHANGE_AF         = 1u << 4,
    RECOMP_OPTIONS_CHANGE_DISPLAY    = 1u << 5,
    RECOMP_OPTIONS_CHANGE_HAZE       = 1u << 6,
    RECOMP_OPTIONS_CHANGE_WAKE       = 1u << 7,
    RECOMP_OPTIONS_CHANGE_NPC_DISTANCE = 1u << 8,
    RECOMP_OPTIONS_CHANGE_NPC_LOD      = 1u << 9,
    RECOMP_OPTIONS_CHANGE_FIXED_XBOX   = 1u << 10,
    RECOMP_OPTIONS_CHANGE_OBJECT_DISTANCE = 1u << 11,
    RECOMP_OPTIONS_CHANGE_OG_BUGS = 1u << 12,
    RECOMP_OPTIONS_CHANGE_PS2_UPGRADES = 1u << 13
};
typedef void (*recomp_options_apply_callback)(uint32_t changes);
typedef enum recomp_display_mode {
    RECOMP_DISPLAY_WINDOWED = 0,
    RECOMP_DISPLAY_BORDERLESS,
    RECOMP_DISPLAY_FULLSCREEN
} recomp_display_mode;
int recomp_options_ps2_upgrades(void);
void recomp_options_init(void);
void recomp_options_set_apply_callback(recomp_options_apply_callback callback);
void recomp_options_begin_edit(void);
void recomp_options_cancel_edit(void);
uint32_t recomp_options_apply(void);
void recomp_options_menu_transition(uint32_t owner, uint32_t next);
void recomp_options_prepare_menu_paint(uint32_t menu, uint32_t brush);
void recomp_options_replace_label(uint32_t hash, uint32_t buffer);
const char *recomp_options_label(uint32_t hash);
uint32_t recomp_options_localization_hash(uint32_t hash);
int recomp_options_adjust(uint32_t hash, int direction);
int recomp_options_fps_cap(void); /* 30, 60, 90, 120, or 0 (uncapped). */
int recomp_options_aspect_ratio(void);
int recomp_options_draw_distance(void);
int recomp_options_resolution(void);
int recomp_options_anisotropic_16x(void);
int recomp_options_authentic_haze(void); /* 0 off, 1 native filter, 2 unfiltered */
int recomp_options_object_draw_distance(void);
int recomp_options_npc_wake_distance(void);
int recomp_options_npc_draw_distance(void);
int recomp_options_npc_lod(void);
int recomp_options_fixed_xbox_prompts(void);
int recomp_options_og_bugs(void); /* On preserves the original bugs covered by this option. */
recomp_display_mode recomp_options_display_mode(void);
void recomp_options_resolution_size(uint32_t *width, uint32_t *height);
void recomp_options_internal_resolution_size(uint32_t *width, uint32_t *height);
void recomp_options_presentation_aspect(uint32_t *width, uint32_t *height);
float recomp_options_scale_draw_distance(float distance);
float recomp_options_scale_camera_visibility(float visibility);
float recomp_options_scale_ai_visibility_threshold(float threshold);
float recomp_options_scale_ai_behind_distance_squared(float distance_squared);
float recomp_options_scale_npc_draw_distance(float distance,
                                              int lod_adjustment_type);
float recomp_options_scale_ambient_civ_distance(float distance);
int recomp_options_force_high_npc_lod(int lod_adjustment_type);
uint32_t recomp_options_high_npc_lod_mask(uint32_t mask);
float recomp_options_scale_object_distance(float distance, uint32_t type);
int recomp_options_begin_world_load(void);
float recomp_options_object_distance_multiplier(void);
float recomp_options_perspective_fov(float horizontal_fov,
                                     float guest_aspect);
float recomp_options_perspective_aspect(float guest_aspect);
float recomp_options_satellite_center_x(float x);
float recomp_options_satellite_center_width(float width);
void recomp_options_refresh_retail_camera_projection(void);
#endif

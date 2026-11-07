#ifndef MERCENARIES_PLUGIN_HOOKS_H
#define MERCENARIES_PLUGIN_HOOKS_H
/* Plugin Interface 2 hook points. Names, arguments and handled/result
 * meanings are part of the plugin ABI: document every change in
 * sdk/README.md and never repurpose an existing name. */
#include <stdint.h>
#include "../sdk/mercenaries_plugin.h"
#ifdef __cplusplus
extern "C" {
#endif
enum merc_plugin_hook_id {
    MERC_HOOK_INPUT_VEHICLE_LOOK,      /* "input.vehicle_look" */
    MERC_HOOK_TURRET_CAMERA_VIEW,      /* "turret_camera.view" */
    MERC_HOOK_CHASE_CAMERA_SNAP,       /* "chase_camera.snap" */
    MERC_HOOK_CHASE_CAMERA_YAW,        /* "chase_camera.yaw" */
    MERC_HOOK_TURRET_UPDATE,           /* "turret.update" */
    MERC_HOOK_TURRET_PLAYER_AIM,       /* "turret.player_aim" */
    MERC_HOOK_TURRET_BALLISTIC_ARC,    /* "turret.ballistic_arc" */
    MERC_HOOK_WEAPON_PLAYER_FIRE,      /* "weapon.player_fire" */
    MERC_HOOK_HUD_CROSSHAIR,           /* "hud.crosshair" */
    MERC_HOOK_HUD_DUMB_TARGET_TURRET,  /* "hud.dumb_target_turret" */
    MERC_HOOK_HUD_DUMB_TARGET_POINT,   /* "hud.dumb_target_point" */
    MERC_HOOK_HUD_DUMB_TARGET_BRACKET, /* "hud.dumb_target_bracket" */
    MERC_HOOK_COUNT
};
/* Cheap check before building an event: any plugin subscribed? */
int merc_plugins_hook_active(unsigned id);
/* Offer the event to subscribers in load order. Returns event->handled.
 * Guest thread only; a no-op when nothing is subscribed. */
int merc_plugins_hook(unsigned id, MercPluginHookEvent *event);

static __inline MercPluginHookEvent merc_hook_event(uint32_t count, uint32_t a0, uint32_t a1,
                                                    uint32_t a2, uint32_t a3, uint32_t a4)
{
    MercPluginHookEvent e = {0};
    e.struct_size = (uint32_t)sizeof(e);
    e.api_version = MERCENARIES_PLUGIN_API_VERSION_2;
    e.arg_count = count;
    e.args[0] = a0; e.args[1] = a1; e.args[2] = a2; e.args[3] = a3; e.args[4] = a4;
    return e;
}

/* Guest calls for plugins, implemented next to the guest CPU context
 * helpers in recomp_manual.c. */
int recomp_plugin_guest_call_begin(uint32_t scratch_bytes, uint32_t *scratch_address);
int recomp_plugin_guest_call(uint32_t function, uint32_t this_ptr, const uint32_t *args,
                             uint32_t arg_count, uint32_t *eax);
void recomp_plugin_guest_call_end(void);
#ifdef __cplusplus
}
#endif
#endif

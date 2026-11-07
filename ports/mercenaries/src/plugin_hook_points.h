/* Plugin Interface 2 hook points (included by recomp_manual.c after
 * decoupled_turret_guest.h). Generated code calls recomp_hook_* at the sites
 * patched by Patch-Generated.py. Each offers the event to plugins first; if no
 * plugin handles it, the built-in Decoupled Turrets/Helicopters options decide, and with them
 * off every value is the retail one. Arguments are documented in
 * sdk/README.md. */
#ifndef MERCENARIES_PLUGIN_HOOK_POINTS_H
#define MERCENARIES_PLUGIN_HOOK_POINTS_H
#include "plugin_hooks.h"

static uint32_t g_hook_camera_turret;
static float g_hook_pivot[3];

static uint32_t hook_bits(float value) { return recomp_float_bits(value); }
static float hook_f32(uint32_t bits) { float f; memcpy(&f, &bits, sizeof(f)); return f; }

/* RsCameraStateTurret::Update fetched the barrel node: the camera's pivot. */
void recomp_hook_turret_camera_pivot(uint32_t turret, uint32_t matrix)
{
    g_hook_camera_turret = turret;
    if (matrix >= 0x00010000u && matrix <= 0x04000000u - 0x40u)
        for (int i = 0; i < 3; ++i) g_hook_pivot[i] = guest_f32(matrix + 0x30u + 4u * i);
    recomp_decoupled_turret_camera_matrix(turret, matrix, 0u);
}

/* "turret_camera.view": finished turret camera matrix, before it is stored. */
void recomp_hook_turret_camera_view(uint32_t matrix)
{
    if (merc_plugins_hook_active(MERC_HOOK_TURRET_CAMERA_VIEW)) {
        MercPluginHookEvent e = merc_hook_event(5u, matrix, g_hook_camera_turret, hook_bits(g_hook_pivot[0]),
                                                hook_bits(g_hook_pivot[1]), hook_bits(g_hook_pivot[2]));
        if (merc_plugins_hook(MERC_HOOK_TURRET_CAMERA_VIEW, &e)) return;
    }
    recomp_decoupled_turret_camera_final(matrix);
}

/* "chase_camera.snap": helicopter chase camera about to snap behind. */
int recomp_hook_chase_camera_snap(uint32_t state, uint32_t vehicle)
{
    if (merc_plugins_hook_active(MERC_HOOK_CHASE_CAMERA_SNAP)) {
        MercPluginHookEvent e = merc_hook_event(2u, state, vehicle, 0u, 0u, 0u);
        if (merc_plugins_hook(MERC_HOOK_CHASE_CAMERA_SNAP, &e)) return 1;
    }
    return recomp_decoupled_heli_camera(state, vehicle);
}

/* "chase_camera.yaw": final chase-camera yaw, after retail auto-reset. */
void recomp_hook_chase_camera_yaw(uint32_t state, uint32_t vehicle)
{
    if (state < 0x00010000u || state > 0x04000000u - 0x160u) return;
    if (merc_plugins_hook_active(MERC_HOOK_CHASE_CAMERA_YAW)) {
        MercPluginHookEvent e = merc_hook_event(3u, state, vehicle, guest_u32(state + 0x10Cu), 0u, 0u);
        if (merc_plugins_hook(MERC_HOOK_CHASE_CAMERA_YAW, &e)) {
            float yaw = hook_f32(e.result);
            if (isfinite(yaw)) {
                *(volatile float *)guest_ptr(state + 0x10Cu) = yaw;
                *(volatile uint8_t *)guest_ptr(state + 0x15Cu) = 0u; /* cancel pending soft reset */
            }
            return;
        }
    }
    recomp_decoupled_heli_final_yaw(state);
}

/* "turret.update": before RsActorTurret::Update consumes its settings. */
void recomp_hook_turret_update(uint32_t turret, float dt)
{
    if (merc_plugins_hook_active(MERC_HOOK_TURRET_UPDATE)) {
        MercPluginHookEvent e = merc_hook_event(2u, turret, hook_bits(dt), 0u, 0u, 0u);
        if (merc_plugins_hook(MERC_HOOK_TURRET_UPDATE, &e)) return;
    }
    recomp_decoupled_turret_drive(turret, dt);
}

/* "turret.player_aim": RsAiPlayer is aiming this turret for the player. */
void recomp_hook_turret_player_aim(uint32_t turret)
{
    if (merc_plugins_hook_active(MERC_HOOK_TURRET_PLAYER_AIM)) {
        MercPluginHookEvent e = merc_hook_event(1u, turret, 0u, 0u, 0u, 0u);
        merc_plugins_hook(MERC_HOOK_TURRET_PLAYER_AIM, &e);
    }
    decoupled_heli_note_turret(turret);
}

/* "turret.ballistic_arc": handled = aim straight, without the ballistic arc. */
int recomp_hook_turret_ballistic_arc_skip(uint32_t turret)
{
    if (merc_plugins_hook_active(MERC_HOOK_TURRET_BALLISTIC_ARC)) {
        MercPluginHookEvent e = merc_hook_event(1u, turret, 0u, 0u, 0u, 0u);
        if (merc_plugins_hook(MERC_HOOK_TURRET_BALLISTIC_ARC, &e)) return 1;
    }
    return recomp_decoupled_turret_skip_drop();
}

/* "weapon.player_fire": handled = fire along the barrel instead of at the
 * crosshair pick. site 0 = vehicle weapon, 1 = gunner seat. */
int recomp_hook_player_fire_along_barrel(uint32_t site)
{
    if (merc_plugins_hook_active(MERC_HOOK_WEAPON_PLAYER_FIRE)) {
        MercPluginHookEvent e = merc_hook_event(1u, site, 0u, 0u, 0u, 0u);
        if (merc_plugins_hook(MERC_HOOK_WEAPON_PLAYER_FIRE, &e)) return 1;
    }
    return recomp_decoupled_vehicle_live();
}

/* "hud.crosshair": after the main crosshair chose its mode; handled = use
 * the dumb-target reticle (static bracket + dynamic marker) this frame. */
int recomp_hook_crosshair_dumb_target(uint32_t crosshair, uint32_t mode)
{
    if (merc_plugins_hook_active(MERC_HOOK_HUD_CROSSHAIR)) {
        MercPluginHookEvent e = merc_hook_event(2u, crosshair, mode, 0u, 0u, 0u);
        if (merc_plugins_hook(MERC_HOOK_HUD_CROSSHAIR, &e)) return 1;
    }
    return recomp_decoupled_turret_live() && (mode == 6u || mode == 1u);
}

/* "hud.dumb_target_turret": the turret the dumb-target marker follows. */
uint32_t recomp_hook_dumb_target_turret(uint32_t turret)
{
    if (recomp_decoupled_turret_live()) turret = decoupled_turret_camera_turret();
    if (merc_plugins_hook_active(MERC_HOOK_HUD_DUMB_TARGET_TURRET)) {
        MercPluginHookEvent e = merc_hook_event(1u, turret, 0u, 0u, 0u, 0u);
        if (merc_plugins_hook(MERC_HOOK_HUD_DUMB_TARGET_TURRET, &e)) turret = e.result;
    }
    recomp_decoupled_turret_hud_turret(turret);
    return turret;
}

/* "hud.dumb_target_point": dynamic marker target in HUD pixels. Retail
 * replaces the barrel projection with the fixed aim point. Axis 1 (Y) is
 * offered before axis 0 (X) each frame. */
float recomp_hook_dumb_target_point(uint32_t axis, float hud_size, float retail)
{
    float value = recomp_decoupled_vehicle_live() ? recomp_decoupled_turret_gun_hud(axis, hud_size) : retail;
    if (merc_plugins_hook_active(MERC_HOOK_HUD_DUMB_TARGET_POINT)) {
        MercPluginHookEvent e = merc_hook_event(4u, axis, hook_bits(hud_size), hook_bits(retail), hook_bits(value), 0u);
        if (merc_plugins_hook(MERC_HOOK_HUD_DUMB_TARGET_POINT, &e)) {
            float v = hook_f32(e.result);
            if (isfinite(v)) value = v;
        }
    }
    return value;
}

/* "hud.dumb_target_bracket": static bracket position. Retail makes it track
 * the dynamic marker. Writable args[1]/args[2] = x/y in HUD pixels. */
void recomp_hook_dumb_target_bracket(uint32_t crosshair)
{
    if (crosshair < 0x00010000u || crosshair > 0x04000000u - 0x60u) return;
    int show = recomp_decoupled_vehicle_live();
    float x = show ? recomp_decoupled_turret_aim_hud(0u) : guest_f32(crosshair + 0x4Cu);
    float y = show ? recomp_decoupled_turret_aim_hud(1u) : guest_f32(crosshair + 0x50u);
    if (merc_plugins_hook_active(MERC_HOOK_HUD_DUMB_TARGET_BRACKET)) {
        MercPluginHookEvent e = merc_hook_event(3u, crosshair, hook_bits(x), hook_bits(y), 0u, 0u);
        if (merc_plugins_hook(MERC_HOOK_HUD_DUMB_TARGET_BRACKET, &e)) {
            float px = hook_f32(e.args[1]), py = hook_f32(e.args[2]);
            if (isfinite(px) && isfinite(py)) { x = px; y = py; show = 1; }
        }
    }
    if (!show) return;
    *(volatile float *)guest_ptr(crosshair + 0x4Cu) = x;
    *(volatile float *)guest_ptr(crosshair + 0x50u) = y;
    *(volatile float *)guest_ptr(crosshair + 0x54u) = 50.f;        /* bracket size */
    *(volatile float *)guest_ptr(crosshair + 0x58u) = 50.f;
    *(volatile uint32_t *)guest_ptr(crosshair + 0x5Cu) = guest_u32(crosshair + 0x40u); /* marker colour */
    *(volatile uint8_t *)guest_ptr(crosshair + 0x48u) = 1u;       /* draw the bracket */
}
#endif

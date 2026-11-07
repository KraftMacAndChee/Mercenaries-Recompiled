/* Decoupled turret aiming: guest-side integration (included by recomp_manual.c
 * after the guest CPU context helpers).
 *
 * Retail facts this relies on (RetroStrike source, retail XBE 1.0):
 *  - RsCameraStateTurret::Update (0x000A1000) builds the camera from the
 *    turret's barrel node/tip matrices: GetBarrelNodeMatrix (0x0003C500,
 *    sret) and GetBarrelTipMatrix (0x0003C420, sret + flag). Rotating those
 *    returned matrices about the barrel node points the camera anywhere while
 *    keeping the authored offsets, collision and horizon roll.
 *  - RsActorTurret::Update (0x0003CDF0) turns _fYawSetting (+0x248) and
 *    _fPitchSetting (+0x22C), each -1..1, into motion at _fYawSpeed (+0x238)
 *    and _fPitchSpeed (+0x21C) rad/s, clamped to the traverse limits. Pitch
 *    and yaw members share one layout: Speed, Max, Min, Current, Setting.
 *  - RsActorTurret::GetYawPitchDiffs (0x0003C830, thiscall, vector by value,
 *    float&, float&; ret 20) is what the retail AI uses to aim turrets. For
 *    ammo with GravityScale it aims a ballistic arc instead; the decoupled
 *    drive skips that so shell drop stays the player's to judge.
 *  - RsCamera::PickPixel (0x00097F00) returns the world point under a screen
 *    position from the live camera.
 *  - PblMatrix rows: X, Y, Z axes then translation; barrels fire along Y.
 *    RedCamera (RsCamera+0x1C) stores its matrix at +0x10 and its frustum
 *    (near, far, width, height) at +0x90, zoom at +0xB0; it looks along -Z. */
#ifndef MERCENARIES_DECOUPLED_TURRET_GUEST_H
#define MERCENARIES_DECOUPLED_TURRET_GUEST_H
#include "decoupled_turret.h"

/* Retail on-foot and turret aim point: RsAiPlayer.h dfAIMDEV. */
#define DT_AIM_X 0.5f
#define DT_AIM_Y (1.f/3.f)

#define DT_RS_CAMERA 0x004140E8u
#define DT_PICK_PIXEL 0x00097F00u
#define DT_BARREL_TIP 0x0003C420u
#define DT_YAW_PITCH_DIFFS 0x0003C830u

typedef struct { float v[3]; } dt_vec;

static dt_vec dt_read_vec(uint32_t a) { dt_vec r; for (int i = 0; i < 3; ++i) r.v[i] = guest_f32(a + 4u * i); return r; }
static void dt_write_vec(uint32_t a, dt_vec r) { for (int i = 0; i < 3; ++i) *(volatile float *)guest_ptr(a + 4u * i) = r.v[i]; }
static float dt_dot(dt_vec a, dt_vec b) { return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2]; }
static dt_vec dt_sub(dt_vec a, dt_vec b) { dt_vec r = {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2]}}; return r; }
static dt_vec dt_add(dt_vec a, dt_vec b) { dt_vec r = {{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2]}}; return r; }
static dt_vec dt_scale(dt_vec a, float s) { dt_vec r = {{a.v[0] * s, a.v[1] * s, a.v[2] * s}}; return r; }
static dt_vec dt_cross(dt_vec a, dt_vec b)
{
    dt_vec r = {{a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2], a.v[0] * b.v[1] - a.v[1] * b.v[0]}};
    return r;
}
static int dt_normalize(dt_vec *a)
{
    float l = sqrtf(dt_dot(*a, *a));
    if (!isfinite(l) || l < 1e-6f) return 0;
    *a = dt_scale(*a, 1.f / l);
    return 1;
}
static int dt_valid_address(uint32_t a, uint32_t size) { return a >= 0x00010000u && a <= 0x04000000u - size; }

/* Orthonormal frame around a forward direction, world up = +Y. */
typedef struct { dt_vec r, u, f; } dt_frame;
static int dt_make_frame(dt_vec f, dt_frame *out)
{
    dt_vec up = {{0.f, 1.f, 0.f}};
    if (!dt_normalize(&f)) return 0;
    dt_vec r = dt_cross(up, f);
    if (!dt_normalize(&r)) return 0;
    out->r = r; out->u = dt_cross(f, r); out->f = f;
    return 1;
}
/* Re-express a vector given in the barrel frame in the camera frame. */
static dt_vec dt_rotate(const dt_frame *from, const dt_frame *to, dt_vec a)
{
    float x = dt_dot(a, from->r), y = dt_dot(a, from->u), z = dt_dot(a, from->f);
    return dt_add(dt_add(dt_scale(to->r, x), dt_scale(to->u, y)), dt_scale(to->f, z));
}

/* Call a translated thiscall function with a 0x200-byte guest scratch area.
 * args[0] is the first parameter. Returns the scratch base or 0. */
static uint32_t dt_guest_call(uint32_t function, uint32_t self, const uint32_t *args, unsigned count,
                              recomp_saved_guest_cpu_context *saved)
{
    recomp_func_t fn = recomp_lookup(function);
    if (!fn || g_xbox_mem_offset == 0 || g_esp < 0x00012000u) return 0;
    recomp_save_guest_cpu_context(saved);
    g_esp = (g_esp - 0x200u) & ~0xFu;
    uint32_t scratch = g_esp;
    for (unsigned i = count; i-- > 0;) {
        uint32_t value = args[i];
        if (value == 0xFFFFFFFFu) value = scratch;          /* sret / out slot 0 */
        else if (value == 0xFFFFFFFEu) value = scratch + 0x40u; /* out slot 1 */
        else if (value == 0xFFFFFFFDu) value = scratch + 0x44u; /* out slot 2 */
        recomp_guest_push_u32(value);
    }
    recomp_guest_push_u32(0u); /* translated call return slot */
    g_ecx = self;
    fn();
    return scratch;
}

static int dt_barrel_tip(uint32_t turret, dt_vec *position, dt_vec *forward)
{
    recomp_saved_guest_cpu_context saved;
    const uint32_t args[2] = {0xFFFFFFFFu, 0u};
    uint32_t m = dt_guest_call(DT_BARREL_TIP, turret, args, 2, &saved);
    if (!m) return 0;
    *forward = dt_read_vec(m + 0x10u);
    *position = dt_read_vec(m + 0x30u);
    recomp_restore_guest_cpu_context(&saved);
    return dt_normalize(forward);
}

static int dt_pick_aim(dt_vec *point)
{
    recomp_saved_guest_cpu_context saved;
    const uint32_t args[7] = {0xFFFFFFFFu, recomp_float_bits(DT_AIM_X),
                              recomp_float_bits(DT_AIM_Y), 1u, 0u, 0u, 0x43C80000u /* 400 */};
    uint32_t r = dt_guest_call(DT_PICK_PIXEL, DT_RS_CAMERA, args, 7, &saved);
    if (!r) return 0;
    *point = dt_read_vec(r);
    recomp_restore_guest_cpu_context(&saved);
    return isfinite(point->v[0]) && isfinite(point->v[1]) && isfinite(point->v[2]);
}

/* Set only while the decoupled drive calls GetYawPitchDiffs; AI turrets keep
 * their ballistic aiming. */
static int g_dt_skip_drop;
int recomp_decoupled_turret_skip_drop(void) { return g_dt_skip_drop; }

static int dt_yaw_pitch_diffs(uint32_t turret, dt_vec target, float *yaw, float *pitch)
{
    recomp_saved_guest_cpu_context saved;
    const uint32_t args[5] = {recomp_float_bits(target.v[0]), recomp_float_bits(target.v[1]),
                              recomp_float_bits(target.v[2]), 0xFFFFFFFEu, 0xFFFFFFFDu};
    /* Seed the outputs: retail returns early without writing them when the
     * target is closer than the barrel tip. */
    uint32_t base = (g_esp - 0x200u) & ~0xFu;
    if (!dt_valid_address(base, 0x48u)) return 0;
    *(volatile float *)guest_ptr(base + 0x40u) = 0.f;
    *(volatile float *)guest_ptr(base + 0x44u) = 0.f;
    g_dt_skip_drop = 1;
    uint32_t r = dt_guest_call(DT_YAW_PITCH_DIFFS, turret, args, 5, &saved);
    g_dt_skip_drop = 0;
    if (!r) return 0;
    *yaw = guest_f32(r + 0x40u);
    *pitch = guest_f32(r + 0x44u);
    recomp_restore_guest_cpu_context(&saved);
    return isfinite(*yaw) && isfinite(*pitch);
}

static uint32_t dt_red_camera(void)
{
    uint32_t red = guest_u32(DT_RS_CAMERA + 0x1Cu);
    return dt_valid_address(red, 0xC0u) && guest_u32(red) == 0x00300DC4u ? red : 0u;
}

/* ---- camera: re-aim the finished turret camera about the barrel pivot ----
 *
 * The retail camera is built and collided as usual; only the final matrix is
 * turned so it looks exactly along the free-camera direction. The view must
 * not be derived from the barrel's current orientation: the turret chases the
 * view, so any coupling becomes a loop that keeps turning without input. */
static dt_vec g_dt_pivot;
static int g_dt_pivot_valid;

/* First barrel-node fetch of RsCameraStateTurret::Update: remember the pivot. */
void recomp_decoupled_turret_camera_matrix(uint32_t turret, uint32_t matrix, uint32_t kind)
{
    if (kind != 0u || !dt_valid_address(matrix, 0x40u) || !dt_valid_address(turret, 0x280u)) return;
    decoupled_turret_note_camera(turret);
    g_dt_pivot = dt_read_vec(matrix + 0x30u);
    g_dt_pivot_valid = 1;
}

/* Final camera matrix, just before RsCameraStateTurret stores it. PblMatrix
 * rows: X = screen right, Y = screen up, Z = -look, then position. */
void recomp_decoupled_turret_camera_final(uint32_t matrix)
{
    if (!decoupled_turret_enabled() || !g_dt_pivot_valid || !dt_valid_address(matrix, 0x40u)) return;
    g_dt_pivot_valid = 0;
    dt_vec look = dt_scale(dt_read_vec(matrix + 0x20u), -1.f);
    float desired[3];
    if (!decoupled_turret_direction(desired)) {
        decoupled_turret_seed(look.v); /* start exactly where retail looks */
        return;
    }
    dt_vec right = dt_read_vec(matrix);
    decoupled_turret_apply_input(right.v);
    if (!recomp_decoupled_turret_live() || !decoupled_turret_direction(desired)) return;
    dt_vec view = {{desired[0], desired[1], desired[2]}};
    dt_frame from, to;
    if (!dt_make_frame(look, &from) || !dt_make_frame(view, &to)) return;
    for (uint32_t row = 0; row < 3u; ++row)
        dt_write_vec(matrix + row * 0x10u, dt_rotate(&from, &to, dt_read_vec(matrix + row * 0x10u)));
    dt_vec position = dt_read_vec(matrix + 0x30u);
    dt_write_vec(matrix + 0x30u, dt_add(g_dt_pivot, dt_rotate(&from, &to, dt_sub(position, g_dt_pivot))));
}

/* ---- helicopter pilot: free chase-camera yaw ----
 * RsCameraStateMario64::_UpdateCharacterTypeBehaviour (0x0009D840) snaps the
 * helicopter camera behind the vehicle every frame (DoSoftReset with FLT_MAX)
 * before building it from _fYaw (+0x10C). While live, skip that snap and
 * supply the free yaw. The vehicle's stored matrix is at +0xB0; retail's
 * "behind" yaw is atan2 of its Z row. */
static uint32_t g_dt_heli_vehicle;

int recomp_decoupled_heli_camera(uint32_t state, uint32_t vehicle)
{
    if (!decoupled_heli_enabled() || !dt_valid_address(state, 0x160u) || !dt_valid_address(vehicle, 0xF0u)) return 0;
    uint32_t red = dt_red_camera();
    if (!red) return 0;
    dt_vec back = dt_read_vec(vehicle + 0xD0u);
    float heading = atan2f(back.v[0], back.v[2]);
    /* Which way a rightward turn moves yaw=atan2(x,z) for this camera:
     * the sign of cross(look, screen right).y. */
    dt_vec look = dt_scale(dt_read_vec(red + 0x30u), -1.f), right = dt_read_vec(red + 0x10u);
    float right_turn = look.v[2] * right.v[0] - look.v[0] * right.v[2] >= 0.f ? 1.f : -1.f;
    float yaw;
    g_dt_heli_vehicle = vehicle;
    if (!decoupled_heli_camera(vehicle, heading, right_turn, &yaw)) return 0;
    *(volatile float *)guest_ptr(state + 0x10Cu) = yaw;
    return 1;
}

/* Just before the view is built from _fYaw: retail UpdateAutoReset and
 * DoSoftReset (+0x15C _bDoSoftReset) pull the camera behind the vehicle
 * whenever _fDeltaYaw is zero, which is always true while the mouse drives
 * the free yaw. Apply the free yaw last and drop that pending reset. */
void recomp_decoupled_heli_final_yaw(uint32_t state)
{
    float yaw;
    if (!dt_valid_address(state, 0x160u) || !decoupled_heli_free_yaw(&yaw)) return;
    *(volatile float *)guest_ptr(state + 0x10Cu) = yaw;
    *(volatile uint8_t *)guest_ptr(state + 0x15Cu) = 0u;
}

/* ---- turret: slew toward the camera aim point ---- */
static float g_dt_aim_distance = 100.f;

void recomp_decoupled_turret_drive(uint32_t turret, float dt)
{
    const int turret_mode = turret != 0u && turret == decoupled_turret_camera_turret() && recomp_decoupled_turret_live();
    const int heli_mode = !turret_mode && recomp_decoupled_heli_live() && decoupled_heli_turret(turret);
    if (!turret_mode && !heli_mode) return;
    if (!dt_valid_address(turret, 0x280u) || !isfinite(dt) || dt <= 0.f || dt > .25f) return;
    dt_vec aim, tip, forward;
    float yaw = 0.f, pitch = 0.f;
    if (!dt_pick_aim(&aim)) return;
    /* A pick on our own vehicle (camera behind the turret) is not a target:
     * aim far along the same ray instead. */
    uint32_t red = dt_red_camera();
    if (red) {
        dt_vec eye = dt_read_vec(red + 0x40u), ray = dt_sub(aim, eye);
        float reach = sqrtf(dt_dot(ray, ray));
        dt_vec body = g_dt_pivot;
        if (heli_mode && dt_valid_address(g_dt_heli_vehicle, 0xF0u)) body = dt_read_vec(g_dt_heli_vehicle + 0xE0u);
        float pivot = body.v[0] || body.v[1] || body.v[2] ?
            sqrtf(dt_dot(dt_sub(body, eye), dt_sub(body, eye))) : 0.f;
        if (isfinite(reach) && reach < pivot + 4.f && dt_normalize(&ray))
            aim = dt_add(eye, dt_scale(ray, 400.f));
    }
    if (!dt_yaw_pitch_diffs(turret, aim, &yaw, &pitch)) return;
    if (dt_barrel_tip(turret, &tip, &forward)) {
        float d = sqrtf(dt_dot(dt_sub(aim, tip), dt_sub(aim, tip)));
        if (isfinite(d) && d > 1.f) g_dt_aim_distance = d;
    }
    const float yaw_speed = guest_f32(turret + 0x238u), pitch_speed = guest_f32(turret + 0x21Cu);
    /* Settings are fractions of full traverse speed: reach the aim point this
     * update when possible, never faster than the turret's authored speed. */
    float yaw_setting = yaw_speed > 1e-4f ? yaw / (yaw_speed * dt) : 0.f;
    float pitch_setting = pitch_speed > 1e-4f ? pitch / (pitch_speed * dt) : 0.f;
    yaw_setting = yaw_setting < -1.f ? -1.f : yaw_setting > 1.f ? 1.f : yaw_setting;
    pitch_setting = pitch_setting < -1.f ? -1.f : pitch_setting > 1.f ? 1.f : pitch_setting;
    *(volatile float *)guest_ptr(turret + 0x248u) = yaw_setting;
    *(volatile float *)guest_ptr(turret + 0x22Cu) = pitch_setting;
}

/* ---- HUD: gun marker where the barrel will hit, bracket at the aim point ---- */
static float g_dt_gun_screen[2] = {DT_AIM_X, DT_AIM_Y};
static float g_dt_hud_size[2] = {640.f, 480.f};
static uint32_t g_dt_hud_turret;

/* The turret RsHudMainCrossHair::_UpdateDumbTarget is tracking this frame. */
void recomp_decoupled_turret_hud_turret(uint32_t turret) { g_dt_hud_turret = turret; }

static void dt_update_gun_screen(void)
{
    uint32_t turret = g_dt_hud_turret, red = dt_red_camera();
    dt_vec tip, forward;
    if (!red || !dt_valid_address(turret, 0x280u) || !dt_barrel_tip(turret, &tip, &forward)) return;
    dt_vec point = dt_add(tip, dt_scale(forward, g_dt_aim_distance));
    dt_vec x = dt_read_vec(red + 0x10u), y = dt_read_vec(red + 0x20u), z = dt_read_vec(red + 0x30u);
    dt_vec v = dt_sub(point, dt_read_vec(red + 0x40u));
    float near_plane = guest_f32(red + 0x90u), zoom = guest_f32(red + 0xB0u);
    float width = guest_f32(red + 0x98u) / zoom, height = guest_f32(red + 0x9Cu) / zoom;
    float lx = dt_dot(v, x), ly = dt_dot(v, y), depth = -dt_dot(v, z);
    if (!(near_plane > 0.f && width > 0.f && height > 0.f) || !isfinite(width) || !isfinite(height)) return;
    if (depth < .05f) depth = .05f; /* behind the camera: pin toward the edge */
    float fx = .5f + lx * near_plane / depth / width;
    float fy = .5f - ly * near_plane / depth / height;
    if (!isfinite(fx) || !isfinite(fy)) return;
    g_dt_gun_screen[0] = fx < .02f ? .02f : fx > .98f ? .98f : fx;
    g_dt_gun_screen[1] = fy < .02f ? .02f : fy > .98f ? .98f : fy;
}

/* axis 1 is called first with the HUD height, axis 0 then with the width. */
float recomp_decoupled_turret_gun_hud(uint32_t axis, float hud_size)
{
    if (axis > 1u) return 0.f;
    g_dt_hud_size[axis] = hud_size;
    if (axis == 1u) dt_update_gun_screen();
    return hud_size * g_dt_gun_screen[axis];
}

float recomp_decoupled_turret_aim_hud(uint32_t axis)
{
    return axis ? g_dt_hud_size[1] * DT_AIM_Y : g_dt_hud_size[0] * DT_AIM_X;
}
#endif

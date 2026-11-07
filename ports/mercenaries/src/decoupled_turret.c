#include "decoupled_turret.h"
#include <windows.h>
#include <math.h>

static int g_turret_enabled, g_heli_enabled;
static uint32_t g_camera_turret;
static ULONGLONG g_camera_turret_ms, g_turret_mouse_ms, g_turret_pad_ms;
static int g_turret_dir_valid;
static float g_turret_yaw, g_turret_pitch;       /* world: yaw=atan2(x,z), pitch up */
static float g_pending_yaw, g_pending_pitch;
#define TURRET_PITCH_LIMIT 1.40f                 /* ~80 degrees */

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void decoupled_turret_load(const char *path)
{
    g_turret_enabled = GetPrivateProfileIntA("Controls", "DecoupledTurrets", 0, path) == 1;
    g_heli_enabled = GetPrivateProfileIntA("Controls", "DecoupledHelicopters", 0, path) == 1;
    g_turret_dir_valid = 0;
}

void decoupled_turret_save(const char *path)
{
    WritePrivateProfileStringA("Controls", "DecoupledTurrets", g_turret_enabled ? "1" : "0", path);
    WritePrivateProfileStringA("Controls", "DecoupledHelicopters", g_heli_enabled ? "1" : "0", path);
}

int decoupled_turret_enabled(void) { return g_turret_enabled; }
void decoupled_turret_set_enabled(int enabled) { g_turret_enabled = enabled != 0; g_turret_dir_valid = 0; }

void decoupled_turret_mouse(float yaw_angle, float pitch_angle)
{
    if (!isfinite(yaw_angle) || !isfinite(pitch_angle)) return;
    g_turret_mouse_ms = GetTickCount64();
    g_pending_yaw += yaw_angle;
    g_pending_pitch += pitch_angle;
}

void decoupled_turret_controller_input(void)
{
    g_turret_pad_ms = GetTickCount64();
    g_pending_yaw = g_pending_pitch = 0.f;
    g_turret_dir_valid = 0; /* re-seed from the retail view when the mouse resumes */
}

void decoupled_turret_note_camera(uint32_t turret)
{
    ULONGLONG now = GetTickCount64();
    /* A different turret, or a gap (vehicle exit, cutscene, menu), starts the
     * free camera where the retail view looks. */
    if (turret != g_camera_turret || now - g_camera_turret_ms > 500u) g_turret_dir_valid = 0;
    g_camera_turret = turret;
    g_camera_turret_ms = now;
}

uint32_t decoupled_turret_camera_turret(void) { return g_camera_turret; }

int recomp_decoupled_turret_live(void)
{
    ULONGLONG now = GetTickCount64();
    return g_turret_enabled && g_camera_turret &&
           now - g_camera_turret_ms < 250u &&
           now - g_turret_mouse_ms < 250u &&
           now - g_turret_pad_ms > 500u;
}

int decoupled_turret_direction(float forward[3])
{
    if (!g_turret_dir_valid) return 0;
    float c = cosf(g_turret_pitch);
    forward[0] = sinf(g_turret_yaw) * c;
    forward[1] = sinf(g_turret_pitch);
    forward[2] = cosf(g_turret_yaw) * c;
    return 1;
}

void decoupled_turret_seed(const float forward[3])
{
    float horizontal = sqrtf(forward[0] * forward[0] + forward[2] * forward[2]);
    if (!isfinite(horizontal) || (horizontal < 1e-4f && fabsf(forward[1]) < 1e-4f)) return;
    g_turret_yaw = atan2f(forward[0], forward[2]);
    g_turret_pitch = clampf(atan2f(forward[1], horizontal), -TURRET_PITCH_LIMIT, TURRET_PITCH_LIMIT);
    g_pending_yaw = g_pending_pitch = 0.f;
    g_turret_dir_valid = 1;
}

/* ---- Helicopter pilot ---- */
static uint32_t g_heli_vehicle;
static ULONGLONG g_heli_camera_ms, g_heli_mouse_ms, g_heli_pad_ms;
static int g_heli_valid;
static float g_heli_yaw, g_heli_pending, g_heli_heading, g_heli_right_turn = 1.f;
#define HELI_TURRETS 4
static uint32_t g_heli_turret[HELI_TURRETS];
static ULONGLONG g_heli_turret_ms[HELI_TURRETS];

static float wrap_pi(float a)
{
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}

void decoupled_heli_mouse(float yaw_angle)
{
    if (!isfinite(yaw_angle)) return;
    g_heli_mouse_ms = GetTickCount64();
    g_heli_pending += yaw_angle;
}

void decoupled_heli_controller_input(void)
{
    g_heli_pad_ms = GetTickCount64();
    g_heli_pending = 0.f;
    g_heli_valid = 0;
}

int decoupled_heli_enabled(void) { return g_heli_enabled; }
void decoupled_heli_set_enabled(int enabled) { g_heli_enabled = enabled != 0; g_heli_valid = 0; }

int recomp_decoupled_heli_live(void)
{
    ULONGLONG now = GetTickCount64();
    return g_heli_enabled && g_heli_vehicle && g_heli_valid &&
           now - g_heli_camera_ms < 250u &&
           now - g_heli_mouse_ms < 250u &&
           now - g_heli_pad_ms > 500u;
}

int decoupled_heli_camera(uint32_t vehicle, float heading_yaw, float right_turn, float *yaw)
{
    ULONGLONG now = GetTickCount64();
    if (!g_heli_enabled || !isfinite(heading_yaw)) return 0;
    if (vehicle != g_heli_vehicle || now - g_heli_camera_ms > 500u) g_heli_valid = 0;
    g_heli_vehicle = vehicle;
    g_heli_camera_ms = now;
    g_heli_heading = heading_yaw;
    g_heli_right_turn = right_turn < 0.f ? -1.f : 1.f;
    if (!g_heli_valid) {
        /* Start exactly where retail puts the camera: behind the helicopter. */
        g_heli_yaw = heading_yaw;
        g_heli_pending = 0.f;
        g_heli_valid = 1;
        return 0;
    }
    g_heli_yaw = wrap_pi(g_heli_yaw + g_heli_right_turn * g_heli_pending);
    g_heli_pending = 0.f;
    if (!recomp_decoupled_heli_live()) return 0;
    *yaw = g_heli_yaw;
    return 1;
}

int decoupled_heli_free_yaw(float *yaw)
{
    if (!recomp_decoupled_heli_live()) return 0;
    *yaw = g_heli_yaw;
    return 1;
}

float decoupled_heli_steer(void)
{
    if (!recomp_decoupled_heli_live()) return 0.f;
    /* Retail RsAiPlayer::TurnTo for helicopters: fTurn = yaw error * pi,
     * clamped. A positive turn axis turns right; right_turn maps that onto
     * this camera's yaw sense. */
    float turn = wrap_pi(g_heli_yaw - g_heli_heading) * 3.14159265f * g_heli_right_turn;
    return clampf(turn, -1.f, 1.f);
}

void decoupled_heli_note_turret(uint32_t turret)
{
    ULONGLONG now = GetTickCount64();
    int slot = 0;
    for (int i = 0; i < HELI_TURRETS; ++i) {
        if (g_heli_turret[i] == turret) { slot = i; break; }
        if (g_heli_turret_ms[i] < g_heli_turret_ms[slot]) slot = i;
    }
    g_heli_turret[slot] = turret;
    g_heli_turret_ms[slot] = now;
}

int decoupled_heli_turret(uint32_t turret)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < HELI_TURRETS; ++i)
        if (turret && g_heli_turret[i] == turret && now - g_heli_turret_ms[i] < 250u) return 1;
    return 0;
}

int recomp_decoupled_vehicle_live(void) { return recomp_decoupled_turret_live() || recomp_decoupled_heli_live(); }

void decoupled_turret_apply_input(const float camera_right[3])
{
    if (!g_turret_dir_valid) { g_pending_yaw = g_pending_pitch = 0.f; return; }
    /* World handedness is not assumed: turn toward the camera's screen-right
     * axis for positive (rightward) mouse yaw. d(forward)/d(yaw) is
     * (cos yaw, 0, -sin yaw). */
    float sign = 1.f;
    if (camera_right && isfinite(camera_right[0]) && isfinite(camera_right[2])) {
        float d = cosf(g_turret_yaw) * camera_right[0] - sinf(g_turret_yaw) * camera_right[2];
        if (d < 0.f) sign = -1.f;
    }
    g_turret_yaw += sign * g_pending_yaw;
    while (g_turret_yaw > 3.14159265f) g_turret_yaw -= 6.28318531f;
    while (g_turret_yaw < -3.14159265f) g_turret_yaw += 6.28318531f;
    g_turret_pitch = clampf(g_turret_pitch + g_pending_pitch, -TURRET_PITCH_LIMIT, TURRET_PITCH_LIMIT);
    g_pending_yaw = g_pending_pitch = 0.f;
}

/* A host-owned camera pose. Retail camera states continue updating so disabling
 * this override restores the live player/vehicle camera without stale pointers. */
#include "free_cam.h"
#include "recomp_controls.h"
#include "recomp/recomp_types.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static volatile LONG requested;
static volatile LONG time_mode; /* 0 normal, 1 frozen, 2 twenty percent */
static LONG resume_mode;
static uint32_t tick_remainder;
static LONG clock_generation;
static volatile LONG generation;
static LONG pose_generation;
static int initialized;
static uint32_t state_owner;
static XBOX_GAMEPAD input;
static float position[3], yaw, pitch, roll;
static float test_pose[5];
static int test_pose_pending;
int recomp_freecam_enabled(void)
{
    return InterlockedCompareExchange(&requested, 0, 0) != 0;
}
void recomp_freecam_set(int enabled)
{
    /* UI only requests a mode; guest memory is touched on the camera thread. */
    if (!enabled) {
        InterlockedExchange(&time_mode, 0);
        resume_mode = 0;
    }
    if (InterlockedExchange(&requested, enabled != 0) != (enabled != 0))
        InterlockedIncrement(&generation);
}
float recomp_freecam_time_scale(void)
{
    if (!recomp_freecam_enabled())
        return 1.f;
    LONG mode = InterlockedCompareExchange(&time_mode, 0, 0);
    return mode == 1 ? 0.f : mode == 2 ? .2f : 1.f;
}
void recomp_freecam_time_key(unsigned key)
{
    if (!recomp_freecam_enabled())
        return;
    LONG mode = InterlockedCompareExchange(&time_mode, 0, 0);
    if (key == '1') {
        if (mode == 1)
            mode = resume_mode;
        else {
            resume_mode = mode;
            mode = 1;
        }
    } else if (key == '2') {
        mode = mode == 2 ? 0 : 2;
        resume_mode = 0;
    }
    InterlockedExchange(&time_mode, mode);
}
uint32_t recomp_freecam_game_ticks(uint32_t ticks)
{
    /* Leave the authored time-base rate intact (pause/cinematics own it).
     * Only scale incoming game ticks; the no-pause clock receives all ticks. */
    if (MEM32(0x413F6Cu) != 0x4249D707u)
        recomp_freecam_set(0);
    LONG gen = InterlockedCompareExchange(&generation, 0, 0);
    if (clock_generation != gen) {
        tick_remainder = 0;
        clock_generation = gen;
    }
    float rate = recomp_freecam_time_scale();
    if (rate == 1.f) {
        tick_remainder = 0;
        return ticks;
    }
    if (rate == 0.f)
        return 0;
    uint64_t scaled = (uint64_t)ticks + tick_remainder;
    tick_remainder = (uint32_t)(scaled % 5u);
    return (uint32_t)(scaled / 5u);
}
void recomp_freecam_context(uint32_t main_state)
{
    /* Isolated developer regression only: same mode request as the checkbox. */
    static const char *test_path;
    static int checked;
    static unsigned sequence;
    static ULONGLONG last_poll;
    if (!checked) {
        checked = 1;
        if (getenv("MERCENARIES_TEST_ISOLATE_INPUT"))
            test_path = getenv("MERCENARIES_TEST_FREECAM_FILE");
    }
    if (test_path && GetTickCount64() - last_poll >= 100) {
        last_poll = GetTickCount64();
        FILE *f = fopen(test_path, "r");
        if (f) {
            unsigned next, enabled;
            float pose[5];
            int fields = fscanf(f, "%u %u %f %f %f %f %f", &next, &enabled, &pose[0], &pose[1],
                                &pose[2], &pose[3], &pose[4]);
            if (fields >= 2 && next > sequence && enabled <= 1) {
                if (fields == 7 && enabled && isfinite(pose[0]) && isfinite(pose[1]) &&
                    isfinite(pose[2]) && isfinite(pose[3]) && isfinite(pose[4])) {
                    memcpy(test_pose, pose, sizeof(pose));
                    test_pose_pending = 1;
                }
                sequence = next;
                recomp_freecam_set(enabled);
                fprintf(stderr, "[FREECAM-TEST] seq=%u enabled=%u\n", next, enabled);
            }
            fclose(f);
        }
    }
    if (main_state != 0x4249D707u)
        recomp_freecam_set(0);
}
void recomp_freecam_input(XBOX_INPUT_STATE *state, int gameplay, int blocked)
{
    memset(&input, 0, sizeof(input));
    if (!recomp_freecam_enabled() || !gameplay)
        return;
    if (!blocked)
        input = state->Gamepad;
    /* Keep Pause available. All movement/actions belong to the camera. */
    WORD pause = state->Gamepad.wButtons & 0x10;
    memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    state->Gamepad.wButtons = pause;
}
static float stick(SHORT value)
{
    float v = (float)value / 32767.f, a = fabsf(v);
    if (a <= .24f)
        return 0.f;
    return copysignf(fminf(1.f, (a - .24f) / .76f), v);
}
void recomp_freecam_camera(uint32_t camera, uint32_t matrix, float dt)
{
    if (!recomp_freecam_enabled()) {
        initialized = 0;
        return;
    }
    if (camera < 0x10000u || camera > 0x03ffff00u || matrix < 0x10000u || matrix > 0x03ffffc0u)
        return;
    uint32_t owner = MEM32(camera + 0x24u);
    LONG gen = InterlockedCompareExchange(&generation, 0, 0);
    if (initialized && pose_generation == gen && owner != state_owner) {
        /* A cutscene/death/scope transition owns its camera. Do not override it. */
        recomp_freecam_set(0);
        initialized = 0;
        memset(&input, 0, sizeof(input));
        return;
    }
    if (!initialized || pose_generation != gen) {
        for (unsigned i = 0; i < 3; i++)
            position[i] = MEMF(matrix + 0x30u + 4 * i);
        float by = MEMF(matrix + 0x24u);
        if (!isfinite(by))
            return;
        pitch = asinf(fmaxf(-1.f, fminf(1.f, by)));
        yaw = atan2f(MEMF(matrix + 0x20u), MEMF(matrix + 0x28u));
        roll = 0.f;
        state_owner = owner;
        pose_generation = gen;
        initialized = 1;
    }
    if (test_pose_pending) {
        memcpy(position, test_pose, 3 * sizeof(float));
        yaw = test_pose[3];
        pitch = test_pose[4];
        roll = 0.f;
        test_pose_pending = 0;
    }
    /* Camera movement uses the unpaused clock, including during a full freeze. */
    dt = MEMF(0x413F98u);
    if (!isfinite(dt) || dt < 0)
        dt = 0;
    if (dt > .1f)
        dt = .1f;
    float mx = 0, my = 0;
    recomp_controls_freecam_look(&mx, &my);
    yaw -= stick(input.sThumbRX) * 2.f * dt + mx;
    pitch -= stick(input.sThumbRY) * 2.f * dt;
    pitch += my;
    pitch = fmaxf(-1.553343f, fminf(1.553343f, pitch));
    yaw = remainderf(yaw, 6.283185307f);
    /* Q/E bank around the viewing axis at 60 degrees per real-time second. */
    roll = remainderf(roll + recomp_controls_freecam_roll() * 1.047197551f * dt, 6.283185307f);
    float sy = sinf(yaw), cy = cosf(yaw), sp = sinf(pitch), cp = cosf(pitch);
    float right[3] = {cy, 0, -sy}, up[3] = {-sy * sp, cp, -cy * sp},
          back[3] = {sy * cp, sp, cy * cp};
    float sr = sinf(roll), cr = cosf(roll);
    for (unsigned i = 0; i < 3; i++) {
        float r = right[i], u = up[i];
        right[i] = r * cr + u * sr;
        up[i] = u * cr - r * sr;
    }
    float x = stick(input.sThumbLX), z = stick(input.sThumbLY);
    float y = (input.bAnalogButtons[0] / 255.f) - ((input.wButtons & 0x40) ? 1.f : 0.f);
    float length = sqrtf(x * x + y * y + z * z);
    if (length > 1) {
        x /= length;
        y /= length;
        z /= length;
    }
    for (unsigned i = 0; i < 3; i++) {
        position[i] += 20.f * dt * (right[i] * x - back[i] * z + (i == 1 ? y : 0));
        MEMF(matrix + 4 * i) = right[i];
        MEMF(matrix + 0x10 + 4 * i) = up[i];
        MEMF(matrix + 0x20 + 4 * i) = back[i];
        MEMF(matrix + 0x30 + 4 * i) = position[i];
    }
    MEMF(matrix + 0xC) = MEMF(matrix + 0x1C) = MEMF(matrix + 0x2C) = 0;
    MEMF(matrix + 0x3C) = 1;
}

/* Streaming is a second observer, not a replacement for the mercenary's camera.
 * A pending/new owner has no usable pose until the camera update publishes it. */
int recomp_freecam_focus(float position_out[3], float direction_out[3])
{
    if (!recomp_freecam_enabled() || !initialized ||
        pose_generation != InterlockedCompareExchange(&generation, 0, 0))
        return 0;
    float cp = cosf(pitch);
    for (unsigned i = 0; i < 3; i++) {
        if (!isfinite(position[i]))
            return 0;
        position_out[i] = position[i];
    }
    direction_out[0] = -sinf(yaw) * cp;
    direction_out[1] = -sinf(pitch);
    direction_out[2] = -cosf(yaw) * cp;
    return 1;
}

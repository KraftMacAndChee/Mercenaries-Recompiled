/* Preserve the retail 30 Hz turret-roll recurrence without its variable-step
 * feedback bias. The retail previous GetPitch() is a projected world angle,
 * not the Z rotation applied by SetRotateZ(). Fractional updates must interpolate
 * the applied rotation, or a stationary tilted tank rolls when dt changes. */
#ifndef MERCENARIES_TURRET_CAMERA_ROLL_H
#define MERCENARIES_TURRET_CAMERA_ROLL_H
#include <math.h>
static float mercenaries_turret_roll(float target, float projected_old,
                                    float old_x_y, float old_y_y, float dt)
{
    const float reference_dt = 1.0f / 30.0f;
    const float reference_factor = 12.5f * reference_dt;
    float legacy_factor = fminf(1.0f, fmaxf(0.0f, 12.5f * dt));
    float legacy = (float)((double)projected_old +
        ((double)target - projected_old) * legacy_factor);
    if (!isfinite(target) || !isfinite(projected_old) || !isfinite(dt) ||
        !isfinite(old_x_y) || !isfinite(old_y_y) || dt < 0.0f || dt > 0.25f)
        return legacy;
    /* Keep the original arithmetic at its reference cadence, including rounding. */
    if (fabsf(dt - reference_dt) < 1.0e-8f) return legacy;
    double projection = hypot((double)old_x_y, (double)old_y_y);
    if (projection < 1.0e-5 || projection > 1.0001 || old_y_y < 0.0f) return legacy;
    if (projection > 1.0) projection = 1.0;
    double angle = atan2((double)old_x_y, (double)old_y_y);
    double old_pitch = projected_old;
    double steps = (double)dt / reference_dt;
    /* At most seven whole reference updates plus a fractional update. */
    while (steps >= 1.0) {
        angle = old_pitch + ((double)target - old_pitch) * reference_factor;
        old_pitch = asin(fmax(-1.0, fmin(1.0, sin(angle) * projection)));
        steps -= 1.0;
    }
    double next = old_pitch + ((double)target - old_pitch) * reference_factor;
    return (float)(angle + steps * (next - angle));
}
#endif

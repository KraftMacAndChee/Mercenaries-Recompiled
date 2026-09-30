#ifndef MERCENARIES_BOID_WINGS_H
#define MERCENARIES_BOID_WINGS_H
#include <math.h>
#include <stdint.h>
/* Independent bird-only cadence. Retail seagull phase .25 is the raised-wing
 * pose, verified with fixed-phase renders. Never hold an arbitrary frame. */
#define BOID_GLIDE_PHASE 0.25f
typedef struct {
    float flap_left, glide_left, previous_phase;
    uint32_t random;
} boid_wings;
static float boid_wing_sample(boid_wings *w)
{
    w->random = 1664525u * w->random + 1013904223u;
    return (float)(w->random >> 8) * (1.0f / 16777216.0f);
}
static void boid_wings_init(boid_wings *w, unsigned bird, float phase)
{
    w->random = 0x57494E47u + bird * 7919u;
    w->flap_left = 2.0f + 4.0f * boid_wing_sample(w);
    w->glide_left = 0;
    w->previous_phase = phase;
}
/* Return 1 only while intentionally holding the raised-wing pose. The caller
 * writes the exact phase on entry and restores the original playback rate on
 * exit; animation evaluation itself remains active throughout. */
static int boid_wings_advance(boid_wings *w, float seconds, float phase)
{
    if (!isfinite(seconds) || seconds <= 0 || !isfinite(phase) || phase < 0 || phase > 1)
        return w->glide_left > 0;
    seconds = fminf(seconds, 0.25f);
    if (w->glide_left > 0) {
        w->glide_left = fmaxf(0, w->glide_left - seconds);
        if (w->glide_left == 0)
            w->flap_left = 3.0f + 3.0f * boid_wing_sample(w);
    } else {
        w->flap_left = fmaxf(0, w->flap_left - seconds);
        int raised = (w->previous_phase < BOID_GLIDE_PHASE || phase < w->previous_phase) &&
                     phase >= BOID_GLIDE_PHASE;
        if (w->flap_left == 0 && raised)
            w->glide_left = 4.0f + 4.0f * boid_wing_sample(w);
    }
    w->previous_phase = w->glide_left > 0 ? BOID_GLIDE_PHASE : phase;
    return w->glide_left > 0;
}
#endif

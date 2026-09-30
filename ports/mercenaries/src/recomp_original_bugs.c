#include "recomp_original_bugs.h"
#include "recomp_options.h"

int recomp_original_bug_fix_enabled(recomp_original_bug_fix fix)
{
    switch (fix) {
    case RECOMP_FIX_NW_MAFIA2_MUSIC:
    case RECOMP_FIX_DATELINE_TYPING_SOUND:
    case RECOMP_FIX_SKY_CLOUD_TRANSITION:
        return !recomp_options_og_bugs();
    default:
        return 0;
    }
}

uint32_t recomp_original_bug_cue_alias(uint32_t name)
{
    /* Resume the Mafia exploration cue heard before NW_mafia2's missing
     * mafia.Main_loop request. music.Main_loop is the main-menu theme.
     * Resolve before handle comparison and lookup to reuse a playing cue. */
    if (name == 0xFD50E6A3u &&
        recomp_original_bug_fix_enabled(RECOMP_FIX_NW_MAFIA2_MUSIC))
        return 0xC69A677Bu;
    return name;
}

uint32_t recomp_original_bug_dateline_chars(uint32_t previous)
{
    /* Retail SetText restarts the timer but leaves the sound counter from the
     * previous title. Equal/shorter titles then never request text_blips.
     * Reset only when a new dateline starts, not during its animation. */
    return recomp_original_bug_fix_enabled(RECOMP_FIX_DATELINE_TYPING_SOUND)
        ? 0u : previous;
}

/* SetSkyTexture folds the weather blend around 0.5. Flat clouds need the
 * original outgoing weight, before that fold, when one atmosphere has no
 * cloud texture. Retail otherwise drops the entire layer on region entry. */
static float sky_outgoing_weight;
static float sky_cloud_opacity = 1.0f;

void recomp_original_bug_sky_blend(float outgoing_weight)
{
    sky_outgoing_weight = outgoing_weight;
}

void recomp_original_bug_cloud_params(volatile uint32_t *incoming, volatile uint32_t *outgoing)
{
    const uint32_t clouds = 0x43404EB9u; /* sky_clouds */
    static int have_transition;
    static uint32_t last_incoming;
    static float last_weight, start_opacity;
    if (!recomp_original_bug_fix_enabled(RECOMP_FIX_SKY_CLOUD_TRANSITION) ||
        !(sky_outgoing_weight >= 0.0f && sky_outgoing_weight <= 1.0f) ||
        (*incoming != 0u && *incoming != clouds) ||
        (*outgoing != 0u && *outgoing != clouds)) {
        sky_cloud_opacity = 1.0f;
        have_transition = 0;
        return;
    }

    if (!have_transition)
        start_opacity = *outgoing ? 1.0f : 0.0f;
    else if (*incoming != last_incoming || sky_outgoing_weight > last_weight + 0.00001f)
        /* Reversing across a region boundary starts from the visible coverage,
         * not the binary texture name chosen by Atmosphere::Interpolate. */
        start_opacity = sky_cloud_opacity;
    last_incoming = *incoming;
    last_weight = sky_outgoing_weight;
    have_transition = 1;
    sky_cloud_opacity = (*incoming ? 1.0f - last_weight : 0.0f) + start_opacity * last_weight;

    /* Both retail flat-cloud pixel programs sample the surviving texture.
     * Coverage fades through vertex alpha; RGB and depth/fog stay authored. */
    if (*incoming || *outgoing || sky_cloud_opacity > 0.0f)
        *incoming = *outgoing = clouds;
}

void recomp_original_bug_cloud_colors(volatile float *rgba)
{
    if (!recomp_original_bug_fix_enabled(RECOMP_FIX_SKY_CLOUD_TRANSITION) ||
        sky_cloud_opacity == 1.0f)
        return;
    for (unsigned i = 0; i < 9; ++i)
        rgba[i * 4 + 3] *= sky_cloud_opacity;
}

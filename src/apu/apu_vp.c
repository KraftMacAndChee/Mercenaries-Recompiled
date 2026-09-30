/*
 * MCPX APU Voice Processor - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#include "fpconv.h"

extern volatile int g_audio_muted;

/* Opt-in, low-volume diagnostics for DirectSound stream starvation. */
static int g_trace_apu_starvation = -1;
static int64_t g_trace_apu_starvation_period_us;
static uint64_t g_trace_apu_starvation_calls[MCPX_HW_MAX_VOICES];
static uint64_t g_trace_apu_starvation_padded[MCPX_HW_MAX_VOICES];
static uint32_t g_trace_apu_starvation_current[MCPX_HW_MAX_VOICES];
static uint32_t g_trace_apu_starvation_max[MCPX_HW_MAX_VOICES];

/* Opt-in stream-segment trace. This records guest-visible SSL programming and
 * completion instead of inferring stream health from the final host mix. */
static int g_trace_apu_ssl = -1;
static uint32_t g_trace_apu_ssl_events;
static uint32_t g_trace_apu_ssl_last_offset[MCPX_HW_MAX_VOICES];
static uint32_t g_trace_apu_ssl_last_length[MCPX_HW_MAX_VOICES];
static int8_t g_trace_apu_ssl_last_index[MCPX_HW_MAX_VOICES];
static int8_t g_trace_apu_ssl_last_seg[MCPX_HW_MAX_VOICES];
static bool g_trace_apu_ssl_last_valid[MCPX_HW_MAX_VOICES];

/* Opt-in trace for guest writes to the per-voice sample-rate/pitch target. */
static int g_trace_apu_pitch = -1;
static uint32_t g_trace_apu_pitch_events;
static int16_t g_trace_apu_pitch_last[MCPX_HW_MAX_VOICES];
static bool g_trace_apu_pitch_last_valid[MCPX_HW_MAX_VOICES];

static bool trace_apu_pitch_enabled(void)
{
    if (g_trace_apu_pitch < 0) {
        g_trace_apu_pitch = getenv("MERCENARIES_TRACE_APU_PITCH") != NULL;
    }
    return g_trace_apu_pitch != 0;
}

static void trace_apu_pitch_write(MCPXAPUState *d, uint32_t voice,
                                  uint32_t argument)
{
    if (!trace_apu_pitch_enabled() ||
        voice >= MCPX_HW_MAX_VOICES ||
        g_trace_apu_pitch_events >= 4096u) {
        return;
    }
    const int16_t pitch = (int16_t)(argument >> 16);
    if (g_trace_apu_pitch_last_valid[voice] &&
        g_trace_apu_pitch_last[voice] == pitch) {
        return;
    }
    g_trace_apu_pitch_last_valid[voice] = true;
    g_trace_apu_pitch_last[voice] = pitch;
    const float rate = 1.0f / powf(2.0f, pitch / 4096.0f);
    fprintf(stderr,
            "[APU-PITCH] voice=%u argument=%08X pitch=%d rate=%.7g ep=%d\n",
            voice, argument, pitch, rate,
            d ? d->ep_frame_div : 0);
    ++g_trace_apu_pitch_events;
}

static bool trace_apu_ssl_enabled(void)
{
    if (g_trace_apu_ssl < 0) {
        g_trace_apu_ssl = getenv("MERCENARIES_TRACE_APU_SSL") != NULL;
    }
    return g_trace_apu_ssl != 0;
}

static void trace_apu_ssl_event(const char *event, uint32_t voice,
                                int ssl_index, int ssl_seg,
                                uint32_t base, uint32_t count,
                                uint32_t offset, uint32_t length)
{
    if (!trace_apu_ssl_enabled() || g_trace_apu_ssl_events >= 8192u) {
        return;
    }
    fprintf(stderr,
            "[APU-SSL] event=%s voice=%u ssl=%d seg=%d base=%u count=%u "
            "offset=%08X length=%08X ep=%d\n",
            event, voice, ssl_index, ssl_seg, base, count, offset, length,
            g_state ? g_state->ep_frame_div : 0);
    ++g_trace_apu_ssl_events;
}

static void trace_apu_ssl_consume_if_changed(MCPXAPUState *d, uint32_t voice,
                                             int ssl_index, int ssl_seg,
                                             uint32_t offset, uint32_t length)
{
    if (!trace_apu_ssl_enabled() || voice >= MCPX_HW_MAX_VOICES) {
        return;
    }
    if (g_trace_apu_ssl_last_valid[voice] &&
        g_trace_apu_ssl_last_index[voice] == ssl_index &&
        g_trace_apu_ssl_last_seg[voice] == ssl_seg &&
        g_trace_apu_ssl_last_offset[voice] == offset &&
        g_trace_apu_ssl_last_length[voice] == length) {
        return;
    }
    g_trace_apu_ssl_last_valid[voice] = true;
    g_trace_apu_ssl_last_index[voice] = (int8_t)ssl_index;
    g_trace_apu_ssl_last_seg[voice] = (int8_t)ssl_seg;
    g_trace_apu_ssl_last_offset[voice] = offset;
    g_trace_apu_ssl_last_length[voice] = length;
    trace_apu_ssl_event("consume", voice, ssl_index, ssl_seg,
                        d->vp.ssl[voice].base[ssl_index],
                        d->vp.ssl[voice].count[ssl_index], offset, length);
}

static void trace_apu_starvation(MCPXAPUState *d, uint16_t v, int padded)
{
    if (g_trace_apu_starvation < 0) {
        g_trace_apu_starvation =
            getenv("MERCENARIES_TRACE_APU_STARVATION") != NULL;
    }
    if (!g_trace_apu_starvation) {
        return;
    }

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (g_trace_apu_starvation_period_us == 0) {
        g_trace_apu_starvation_period_us = now_us;
    }

    if (padded > 0) {
        ++g_trace_apu_starvation_calls[v];
        g_trace_apu_starvation_padded[v] += (uint64_t)padded;
        ++g_trace_apu_starvation_current[v];
        if (g_trace_apu_starvation_current[v] >
            g_trace_apu_starvation_max[v]) {
            g_trace_apu_starvation_max[v] =
                g_trace_apu_starvation_current[v];
        }
    } else {
        g_trace_apu_starvation_current[v] = 0;
    }

    if (now_us - g_trace_apu_starvation_period_us < 1000000) {
        return;
    }

    for (uint32_t handle = 0; handle < MCPX_HW_MAX_VOICES; ++handle) {
        if (g_trace_apu_starvation_calls[handle] == 0) {
            continue;
        }
        uint32_t ssl_index = d->vp.ssl[handle].ssl_index;
        fprintf(stderr,
                "[APU-STARVE] voice=%u calls=%llu padded=%llu "
                "max_consecutive=%u ssl=%u seg=%u counts=%u/%u\n",
                handle,
                (unsigned long long)g_trace_apu_starvation_calls[handle],
                (unsigned long long)g_trace_apu_starvation_padded[handle],
                g_trace_apu_starvation_max[handle], ssl_index,
                d->vp.ssl[handle].ssl_seg,
                d->vp.ssl[handle].count[0], d->vp.ssl[handle].count[1]);
    }
    memset(g_trace_apu_starvation_calls, 0,
           sizeof(g_trace_apu_starvation_calls));
    memset(g_trace_apu_starvation_padded, 0,
           sizeof(g_trace_apu_starvation_padded));
    memset(g_trace_apu_starvation_max, 0,
           sizeof(g_trace_apu_starvation_max));
    g_trace_apu_starvation_period_us = now_us;
}

/* #define DEBUG_MCPX */

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do { } while (0)
#endif

/* ============================================================
 * Voice list register table
 * ============================================================ */

static const struct {
    hwaddr top, current, next;
} voice_list_regs[] = {
    { NV_PAPU_TVL2D, NV_PAPU_CVL2D, NV_PAPU_NVL2D }, /* 2D */
    { NV_PAPU_TVL3D, NV_PAPU_CVL3D, NV_PAPU_NVL3D }, /* 3D */
    { NV_PAPU_TVLMP, NV_PAPU_CVLMP, NV_PAPU_NVLMP }, /* MP */
};

/* ============================================================
 * Notify status helper
 * ============================================================ */

static void set_notify_status(MCPXAPUState *d, uint32_t v, int notifier,
                              int status)
{
    hwaddr notify_offset = d->regs[NV_PAPU_FENADDR];
    uint8_t previous_status;
    uint8_t previous_valid;
    notify_offset += 16 * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                           v * MCPX_HW_NOTIFIER_COUNT + notifier);
    notify_offset += 15;

    previous_status = ldub_phys(address_space_memory, notify_offset);
    previous_valid = ldub_phys(address_space_memory, notify_offset - 1);
    stb_phys(address_space_memory, notify_offset, (uint8_t)status);
    stb_phys(address_space_memory, notify_offset - 1, 1);

    if (notifier == MCPX_HW_NOTIFIER_SSLA_DONE ||
        notifier == MCPX_HW_NOTIFIER_SSLB_DONE) {
        const int ssl_index = notifier - MCPX_HW_NOTIFIER_SSLA_DONE;
        if (previous_status != (uint8_t)status || previous_valid != 1u) {
            trace_apu_ssl_event("notify", v, ssl_index,
                                d->vp.ssl[v].ssl_seg,
                                d->vp.ssl[v].base[ssl_index],
                                d->vp.ssl[v].count[ssl_index],
                                (uint32_t)notify_offset, (uint32_t)status);
        }
    }

    qatomic_or(&d->regs[NV_PAPU_ISTS],
               NV_PAPU_ISTS_FEVINTSTS | NV_PAPU_ISTS_FENINTSTS);
    d->set_irq = true;
}

/* ============================================================
 * Filter helpers
 * ============================================================ */

static void voice_reset_filters(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    memset(&d->vp.filters[v].svf, 0, sizeof(d->vp.filters[v].svf));
    hrtf_filter_clear_history(&d->vp.filters[v].hrtf);
    if (d->vp.filters[v].resampler) {
        src_reset(d->vp.filters[v].resampler);
    }
}

static bool voice_should_mute(uint16_t v)
{
    bool m = (g_dbg_voice_monitor >= 0) && (v != g_dbg_voice_monitor);
    return m || mcpx_apu_debug_is_muted(v);
}

/* ============================================================
 * Utility functions
 * ============================================================ */

static float clampf(float v, float mn, float mx)
{
    if (v < mn) return mn;
    if (v > mx) return mx;
    return v;
}

static float attenuate(uint16_t vol)
{
    vol &= 0xFFF;
    return (vol == 0xFFF) ? 0.0f : powf(10.0f, vol / (64.0f * -20.0f));
}

/* ============================================================
 * Voice register accessors (read/write voice struct in RAM)
 * ============================================================ */

static uint32_t voice_get_mask(MCPXAPUState *d, uint16_t voice_handle,
                               hwaddr offset, uint32_t mask)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    return (ldl_le_phys(address_space_memory, voice + offset) & mask) >>
           ctz32(mask);
}

void mcpx_apu_debug_dump_stream_voices(void)
{
    MCPXAPUState *d = g_state;
    if (!d) {
        fprintf(stderr, "[APU-STREAM-DUMP] unavailable\n");
        return;
    }

    qemu_mutex_lock(&d->lock);
    fprintf(stderr,
            "[APU-STREAM-DUMP] frame=%d ep=%d samples=%llu "
            "next_us=%lld now_us=%lld pause=%d idle=%d muted=%d "
            "SECTL=%08X FECTL=%08X "
            "ISTS=%08X IEN=%08X VPV=%08X VPSSL=%08X FEN=%08X "
            "lists=%04X/%04X/%04X\n",
            d->frame_count, d->ep_frame_div,
            (unsigned long long)d->ep_frame_div * NUM_SAMPLES_PER_FRAME,
            (long long)d->next_frame_time_us,
            (long long)qemu_clock_get_us(QEMU_CLOCK_REALTIME),
            d->pause_requested, d->is_idle, g_audio_muted,
            d->regs[NV_PAPU_SECTL], d->regs[NV_PAPU_FECTL],
            d->regs[NV_PAPU_ISTS], d->regs[NV_PAPU_IEN],
            d->regs[NV_PAPU_VPVADDR], d->regs[NV_PAPU_VPSSLADDR],
            d->regs[NV_PAPU_FENADDR], d->regs[NV_PAPU_TVL2D] & 0xffff,
            d->regs[NV_PAPU_TVL3D] & 0xffff,
            d->regs[NV_PAPU_TVLMP] & 0xffff);

    for (uint32_t v = 0; v < MCPX_HW_MAX_VOICES; ++v) {
        const uint32_t fmt = voice_get_mask(d, (uint16_t)v,
                                             NV_PAVS_VOICE_CFG_FMT,
                                             0xffffffffu);
        const uint32_t state = voice_get_mask(d, (uint16_t)v,
                                               NV_PAVS_VOICE_PAR_STATE,
                                               0xffffffffu);
        const uint32_t vbin = voice_get_mask(d, (uint16_t)v,
                                              NV_PAVS_VOICE_CFG_VBIN,
                                              0xffffffffu);
        const uint32_t vola = voice_get_mask(d, (uint16_t)v,
                                              NV_PAVS_VOICE_TAR_VOLA,
                                              0xffffffffu);
        const uint32_t volb = voice_get_mask(d, (uint16_t)v,
                                              NV_PAVS_VOICE_TAR_VOLB,
                                              0xffffffffu);
        const uint32_t volc = voice_get_mask(d, (uint16_t)v,
                                              NV_PAVS_VOICE_TAR_VOLC,
                                              0xffffffffu);
        const uint32_t hrtf = voice_get_mask(d, (uint16_t)v,
                                              NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                              NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (!(fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) &&
            !(state & NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE))
            continue;

        const MCPXAPUVPSSLData *ssl = &d->vp.ssl[v];
        uint32_t seg_offset = 0, seg_length = 0;
        if (ssl->ssl_index >= 0 && ssl->ssl_index < MCPX_HW_SSLS_PER_VOICE &&
            ssl->ssl_seg >= 0 &&
            ssl->ssl_seg < ssl->count[ssl->ssl_index]) {
            const uint32_t page = ssl->base[ssl->ssl_index] + ssl->ssl_seg;
            const hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8u;
            seg_offset = ldl_le_phys(address_space_memory, addr);
            seg_length = ldl_le_phys(address_space_memory, addr + 4u);
        }
        const hwaddr notify = d->regs[NV_PAPU_FENADDR] +
            16u * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                   v * MCPX_HW_NOTIFIER_COUNT);
        fprintf(stderr,
                "  voice=%u fmt=%08X state=%08X cbo=%06X ebo=%06X "
                "ssl=%d:%d A=%u+%u B=%u+%u seg=%08X/%08X "
                "vbin=%08X hrtf=%03X vol=%08X/%08X/%08X "
                "notify=%02X/%02X/%02X/%02X\n",
                v, fmt, state,
                voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                               NV_PAVS_VOICE_PAR_OFFSET_CBO),
                voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                               NV_PAVS_VOICE_PAR_NEXT_EBO),
                ssl->ssl_index, ssl->ssl_seg,
                ssl->base[0], ssl->count[0], ssl->base[1], ssl->count[1],
                seg_offset, seg_length, vbin, hrtf, vola, volb, volc,
                ldub_phys(address_space_memory, notify + 15u),
                ldub_phys(address_space_memory, notify + 31u),
                ldub_phys(address_space_memory, notify + 47u),
                ldub_phys(address_space_memory, notify + 63u));
    }
    fflush(stderr);
    qemu_mutex_unlock(&d->lock);
}
static void voice_set_mask(MCPXAPUState *d, uint16_t voice_handle,
                           hwaddr offset, uint32_t mask, uint32_t val)
{
    hwaddr voice = d->regs[NV_PAPU_VPVADDR] + voice_handle * NV_PAVS_SIZE;
    uint32_t v = ldl_le_phys(address_space_memory, voice + offset) & ~mask;
    stl_le_phys(address_space_memory, voice + offset,
                v | ((val << ctz32(mask)) & mask));
}

/* ============================================================
 * Voice off / lock
 * ============================================================ */

static void trace_voice_envelope(MCPXAPUState *d, uint16_t v,
                                 const char *event)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_APU_ENVELOPE") != NULL;
    if (!enabled)
        return;
    fprintf(stderr,
            "[APU-ENVELOPE] event=%s us=%lld voice=%u "
            "state=%08X count=%08X release=%u\n",
            event, (long long)qemu_clock_get_us(QEMU_CLOCK_REALTIME), v,
            voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, 0xFFFFFFFFu),
            voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, 0xFFFFFFFFu),
            voice_get_mask(d, v, NV_PAVS_VOICE_TAR_LFO_ENV,
                           NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE));
}

static void voice_off(MCPXAPUState *d, uint16_t v)
{
    trace_voice_envelope(d, v, "off");
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);

    bool stream = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    int notifier = MCPX_HW_NOTIFIER_SSLA_DONE;
    if (stream) {
        assert(v < MCPX_HW_MAX_VOICES);
        assert(d->vp.ssl[v].ssl_index <= 1);
        notifier += d->vp.ssl[v].ssl_index;
    }
    set_notify_status(d, v, notifier, NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
}

static void voice_lock(MCPXAPUState *d, uint16_t v, bool lock)
{
    assert(v < MCPX_HW_MAX_VOICES);
    qemu_mutex_lock(&d->lock);

    uint64_t mask = 1ULL << (v % 64);
    if (lock) {
        d->vp.voice_locked[v / 64] |= mask;
    } else {
        d->vp.voice_locked[v / 64] &= ~mask;
    }

    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);
}

static bool is_voice_locked(MCPXAPUState *d, uint16_t v)
{
    assert(v < MCPX_HW_MAX_VOICES);
    uint64_t mask = 1ULL << (v % 64);
    return (qatomic_read(&d->vp.voice_locked[v / 64]) & mask) != 0;
}

/* ============================================================
 * HRIR coefficient setter
 * ============================================================ */

static void set_hrir_coeff_tar(MCPXAPUState *d, int channel, int coeff_idx,
                               int8_t value)
{
    int entry = d->vp.hrtf.current_entry;
    d->vp.hrtf.entries[entry].hrir[channel][coeff_idx] = int8_to_float(value);
}

static bool trace_hrtf_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_APU_HRTF") != NULL;
    return enabled != 0;
}

static void trace_hrtf_entry(const MCPXAPUState *d)
{
    static int64_t last_trace_us;
    const int entry = d->vp.hrtf.current_entry;
    double energy[2] = { 0.0, 0.0 };
    double l1[2] = { 0.0, 0.0 };
    double sum[2] = { 0.0, 0.0 };
    int64_t now_us;

    if (!trace_hrtf_enabled())
        return;
    /* HRIR targets are double-buffered and may be rewritten for every 3D
     * source on every game frame.  Synchronous stderr writes here stall the
     * title thread and can create the very audio hitch we are diagnosing.
     * Keep this table-level sample global and sparse; the active-voice trace
     * below carries the per-source evidence at audio-frame time. */
    now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (last_trace_us && now_us - last_trace_us < 1000000)
        return;
    last_trace_us = now_us;
    for (int channel = 0; channel < 2; ++channel) {
        for (int tap = 0; tap < HRTF_NUM_TAPS; ++tap) {
            const double value = d->vp.hrtf.entries[entry].hrir[channel][tap];
            energy[channel] += value * value;
            l1[channel] += fabs(value);
            sum[channel] += value;
        }
    }
    fprintf(stderr,
            "[APU-HRTF-TABLE] entry=%d itd=%.6f "
            "left_rms=%.8f left_l1=%.8f left_sum=%.8f "
            "right_rms=%.8f right_l1=%.8f right_sum=%.8f\n",
            entry, d->vp.hrtf.entries[entry].itd,
            sqrt(energy[0] / HRTF_NUM_TAPS), l1[0], sum[0],
            sqrt(energy[1] / HRTF_NUM_TAPS), l1[1], sum[1]);
}

static void trace_hrtf_voice(const MCPXAPUState *d, uint16_t voice,
                             uint16_t handle,
                             const float samples[][2], double input_energy,
                             const int bin[8], const uint16_t vol[8])
{
    static int64_t last_trace_us[MCPX_HW_MAX_3D_VOICES];
    double output_energy[2] = { 0.0, 0.0 };
    double coeff_l1[2] = { 0.0, 0.0 };
    int64_t now_us;

    if (!trace_hrtf_enabled() || input_energy < 1.0e-10)
        return;
    now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (last_trace_us[voice] && now_us - last_trace_us[voice] < 1000000)
        return;
    last_trace_us[voice] = now_us;

    for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; ++sample) {
        output_energy[0] += (double)samples[sample][0] * samples[sample][0];
        output_energy[1] += (double)samples[sample][1] * samples[sample][1];
    }
    for (int channel = 0; channel < 2; ++channel) {
        for (int tap = 0; tap < HRTF_NUM_TAPS; ++tap)
            coeff_l1[channel] +=
                fabs(d->vp.filters[voice].hrtf.ch[channel].hrir_coeff_tar[tap]);
    }
    fprintf(stderr,
            "[APU-HRTF-VOICE] voice=%u handle=%u input=%.8f "
            "left=%.8f right=%.8f coeff_l1=%.5f/%.5f itd=%.3f "
            "routes=%d:%03X,%d:%03X,%d:%03X,%d:%03X\n",
            voice, handle,
            sqrt(input_energy / (NUM_SAMPLES_PER_FRAME * 2.0)),
            sqrt(output_energy[0] / NUM_SAMPLES_PER_FRAME),
            sqrt(output_energy[1] / NUM_SAMPLES_PER_FRAME),
            coeff_l1[0], coeff_l1[1],
            d->vp.filters[voice].hrtf.itd_tar,
            bin[0], vol[0], bin[1], vol[1],
            bin[2], vol[2], bin[3], vol[3]);
}

/* ============================================================
 * Front-End method dispatch
 * ============================================================ */

static void fe_method(MCPXAPUState *d, uint32_t method, uint32_t argument)
{
    unsigned int slot;

    d->regs[NV_PAPU_FEDECMETH] = method;
    d->regs[NV_PAPU_FEDECPARAM] = argument;
    unsigned int selected_handle, list;

    switch (method) {
    case NV1BA0_PIO_VOICE_LOCK:
        voice_lock(d, (uint16_t)d->regs[NV_PAPU_FECV], argument & 1);
        break;

    case NV1BA0_PIO_SET_ANTECEDENT_VOICE:
        d->regs[NV_PAPU_FEAV] = argument;
        break;

    case NV1BA0_PIO_VOICE_ON: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);
        const uint32_t previous_link = voice_get_mask(d,
            (uint16_t)selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK,
            NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        const uint32_t previous_active = voice_get_mask(d,
            (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
            NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);

        list = GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_LST);
        if (list != NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_INHERIT) {
            unsigned int top_reg = voice_list_regs[list - 1].top;
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           d->regs[top_reg]);
            d->regs[top_reg] = selected_handle;
        } else {
            unsigned int antecedent_voice =
                GET_MASK(d->regs[NV_PAPU_FEAV], NV_PAPU_FEAV_VALUE);
            assert(antecedent_voice != 0xFFFF);

            uint32_t next_handle = voice_get_mask(
                d, (uint16_t)antecedent_voice, NV_PAVS_VOICE_TAR_PITCH_LINK,
                NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
            voice_set_mask(d, (uint16_t)selected_handle,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           next_handle);
            voice_set_mask(d, (uint16_t)antecedent_voice,
                           NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE,
                           selected_handle);
        }

        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
        d->vp.ssl[selected_handle].ssl_seg = 0;
        d->vp.ssl[selected_handle].ssl_index = 0;

        unsigned int ea_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVA);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR, ea_start);
        if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, delay_time * 16);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, 0);
        } else if (ea_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EACOUNT, hold_time * 16);
        }

        unsigned int ef_start = GET_MASK(argument, NV1BA0_PIO_VOICE_ON_ENVF);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR, ef_start);
        if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY) {
            uint16_t delay_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENV0_EA_DELAYTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, delay_time * 16);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK) {
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, 0);
        } else if (ef_start == NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD) {
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
                    NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                           NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, hold_time * 16);
        }

        voice_reset_filters(d, (uint16_t)selected_handle);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);

        if (getenv("MERCENARIES_TRACE_APU_VOICE_ON") != NULL) {
            const uint32_t fmt = voice_get_mask(
                d, (uint16_t)selected_handle, NV_PAVS_VOICE_CFG_FMT,
                0xFFFFFFFFu);
            const uint32_t pitch_link = voice_get_mask(
                d, (uint16_t)selected_handle, NV_PAVS_VOICE_TAR_PITCH_LINK,
                0xFFFFFFFFu);
            const int16_t pitch = (int16_t)(pitch_link >> 16);
            const uint32_t env0 = voice_get_mask(
                d, (uint16_t)selected_handle, NV_PAVS_VOICE_CFG_ENV0,
                0xFFFFFFFFu);
            const int8_t pitch_scale = (int8_t)(env0 >> 24);
            const float base_rate = 1.0f / powf(2.0f, pitch / 4096.0f);
            fprintf(stderr,
                    "[APU-VOICE-ON] handle=%u list=%u argument=%08X "
                    "antecedent=%08X previous-link=%04X previous-active=%u "
                    "fmt=%08X stream=%u stereo=%u sample=%u container=%u "
                    "spb=%u pitch=%d pitch-scale=%d base-rate=%.7g "
                    "ba=%06X lbo=%06X ebo=%06X ssl=%u+%u/%u+%u "
                    "vbin=%08X vol=%08X/%08X/%08X\n",
                    selected_handle, list, argument, d->regs[NV_PAPU_FEAV],
                    previous_link, previous_active, fmt,
                    (fmt & NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0u,
                    (fmt & NV_PAVS_VOICE_CFG_FMT_STEREO) != 0u,
                    GET_MASK(fmt, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE),
                    GET_MASK(fmt, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE),
                    1u + GET_MASK(fmt,
                                  NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK),
                    pitch, pitch_scale, base_rate,
                    voice_get_mask(d, (uint16_t)selected_handle,
                                   NV_PAVS_VOICE_CUR_PSL_START,
                                   NV_PAVS_VOICE_CUR_PSL_START_BA),
                    voice_get_mask(d, (uint16_t)selected_handle,
                                   NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                   NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO),
                    voice_get_mask(d, (uint16_t)selected_handle,
                                   NV_PAVS_VOICE_PAR_NEXT,
                                   NV_PAVS_VOICE_PAR_NEXT_EBO),
                    d->vp.ssl[selected_handle].base[0],
                    d->vp.ssl[selected_handle].count[0],
                    d->vp.ssl[selected_handle].base[1],
                    d->vp.ssl[selected_handle].count[1],
                    voice_get_mask(d, (uint16_t)selected_handle,
                                   NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFFu),
                    voice_get_mask(d, (uint16_t)selected_handle,
                                   NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFFu),
                    voice_get_mask(d, (uint16_t)selected_handle,
                                   NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFFu),
                    voice_get_mask(d, (uint16_t)selected_handle,
                                   NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFFu));
            fflush(stderr);
        }

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_RELEASE: {
        selected_handle = argument & NV1BA0_PIO_VOICE_ON_HANDLE;
        trace_voice_envelope(d, (uint16_t)selected_handle, "release");

        bool locked = is_voice_locked(d, (uint16_t)selected_handle);
        if (!locked) voice_lock(d, (uint16_t)selected_handle, true);

        uint16_t rr;
        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EACOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EACUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        rr = (uint16_t)voice_get_mask(d, (uint16_t)selected_handle,
            NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_CUR_ECNT,
                       NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, rr * 16);
        voice_set_mask(d, (uint16_t)selected_handle, NV_PAVS_VOICE_PAR_STATE,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR,
                       NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);

        if (!locked) voice_lock(d, (uint16_t)selected_handle, false);
        break;
    }

    case NV1BA0_PIO_VOICE_OFF:
        voice_off(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_OFF_HANDLE));
        break;

    case NV1BA0_PIO_VOICE_PAUSE:
        voice_set_mask(d, (uint16_t)(argument & NV1BA0_PIO_VOICE_PAUSE_HANDLE),
                       NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_PAUSED,
                       (argument & NV1BA0_PIO_VOICE_PAUSE_ACTION) != 0);
        break;

    case NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY:
        d->vp.hrtf.current_entry =
            GET_MASK(argument, NV1BA0_PIO_SET_CURRENT_HRTF_ENTRY_HANDLE);
        break;

    case NV1BA0_PIO_SET_CURRENT_VOICE:
        d->regs[NV_PAPU_FECV] = argument;
        break;

    case NV1BA0_PIO_SET_VOICE_CFG_VBIN:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_VBIN, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_FMT:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_FMT, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV0:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV0, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENV1:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENV1, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_ENVF:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_ENVF, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_MISC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CFG_MISC, 0xFFFFFFFF, argument);
        break;

    case NV1BA0_PIO_SET_VOICE_TAR_HRTF: {
        int handle = GET_MASK(argument, NV1BA0_PIO_SET_VOICE_TAR_HRTF_HANDLE);
        int current_voice = d->regs[NV_PAPU_FECV];
        int previous_handle = voice_get_mask(
            d, (uint16_t)current_voice, NV_PAVS_VOICE_CFG_HRTF_TARGET,
            NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET,
                       NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE, handle);
        if (current_voice < MCPX_HW_MAX_3D_VOICES &&
            handle != HRTF_NULL_HANDLE) {
            assert(handle < HRTF_ENTRY_COUNT);
            hrtf_filter_set_target_params(&d->vp.filters[current_voice].hrtf,
                                          d->vp.hrtf.entries[handle].hrir,
                                          d->vp.hrtf.entries[handle].itd);
        }
        if (trace_hrtf_enabled() && previous_handle != handle &&
            (previous_handle == HRTF_NULL_HANDLE ||
             handle == HRTF_NULL_HANDLE)) {
            fprintf(stderr,
                    "[APU-HRTF-TARGET] voice=%d previous=%d target=%d\n",
                    current_voice, previous_handle, handle);
        }
        break;
    }

    case NV1BA0_PIO_SET_VOICE_TAR_VOLA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_VOLC:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_LFO_ENV:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_LFO_ENV, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCA:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCA, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_FCB:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_TAR_FCB, 0xFFFFFFFF, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_TAR_PITCH: {
        const uint32_t current_voice = d->regs[NV_PAPU_FECV];
        voice_set_mask(d, (uint16_t)current_voice,
                       NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH,
                       (argument & NV1BA0_PIO_SET_VOICE_TAR_PITCH_STEP) >> 16);
        trace_apu_pitch_write(d, current_voice, argument);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_BASE:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSL_START,
                       NV_PAVS_VOICE_CUR_PSL_START_BA, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                       NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_BUF_CBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_OFFSET,
                       NV_PAVS_VOICE_PAR_OFFSET_CBO, argument);
        break;
    case NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO:
        voice_set_mask(d, (uint16_t)d->regs[NV_PAPU_FECV],
                       NV_PAVS_VOICE_PAR_NEXT,
                       NV_PAVS_VOICE_PAR_NEXT_EBO, argument);
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE:
        d->vp.inbuf_sge_handle = argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.inbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE:
        d->vp.outbuf_sge_handle =
            argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_HANDLE;
        break;

    case NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET: {
        hwaddr sge_address =
            d->regs[NV_PAPU_VPSGEADDR] + d->vp.outbuf_sge_handle * 8;
        stl_le_phys(address_space_memory, sge_address,
                    argument & NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET_PARAMETER);
        break;
    }

    case NV1BA0_PIO_SET_VOICE_SSL_A: {
        int ssl = 0;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        trace_apu_ssl_event("set-list", (uint32_t)current_voice, ssl,
                            d->vp.ssl[current_voice].ssl_seg,
                            d->vp.ssl[current_voice].base[ssl],
                            d->vp.ssl[current_voice].count[ssl], 0u, argument);
        break;
    }
    case NV1BA0_PIO_SET_VOICE_SSL_B: {
        int ssl = 1;
        int current_voice = d->regs[NV_PAPU_FECV];
        assert(current_voice < MCPX_HW_MAX_VOICES);
        d->vp.ssl[current_voice].base[ssl] =
            GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_BASE);
        d->vp.ssl[current_voice].count[ssl] =
            (uint8_t)GET_MASK(argument, NV1BA0_PIO_SET_VOICE_SSL_A_COUNT);
        trace_apu_ssl_event("set-list", (uint32_t)current_voice, ssl,
                            d->vp.ssl[current_voice].ssl_seg,
                            d->vp.ssl[current_voice].base[ssl],
                            d->vp.ssl[current_voice].count[ssl], 0u, argument);
        break;
    }

    case NV1BA0_PIO_SET_CURRENT_SSL: {
        assert((argument & 0x3f) == 0);
        assert(argument < (MCPX_HW_MAX_SSL_PRDS * NV_PSGE_SIZE));
        d->vp.ssl_base_page = argument;
        break;
    }

    case NV1BA0_PIO_SET_HRTF_SUBMIXES:
        d->vp.hrtf_submix[0] = (uint8_t)((argument >> 0) & 0x1f);
        d->vp.hrtf_submix[1] = (uint8_t)((argument >> 8) & 0x1f);
        d->vp.hrtf_submix[2] = (uint8_t)((argument >> 16) & 0x1f);
        d->vp.hrtf_submix[3] = (uint8_t)((argument >> 24) & 0x1f);
        if (getenv("MERCENARIES_TRACE_APU_HEADROOM") != NULL) {
            fprintf(stderr,
                    "[APU-HEADROOM] type=hrtf-submix bins=%u/%u/%u/%u "
                    "raw=%08X\n",
                    d->vp.hrtf_submix[0], d->vp.hrtf_submix[1],
                    d->vp.hrtf_submix[2], d->vp.hrtf_submix[3], argument);
            fflush(stderr);
        }
        break;

    case NV1BA0_PIO_SET_HRTF_HEADROOM:
        d->vp.hrtf_headroom = (uint8_t)(argument & NV1BA0_PIO_SET_HRTF_HEADROOM_AMOUNT);
        if (getenv("MERCENARIES_TRACE_APU_HEADROOM") != NULL) {
            fprintf(stderr,
                    "[APU-HEADROOM] type=hrtf amount=%u raw=%08X\n",
                    d->vp.hrtf_headroom, argument);
            fflush(stderr);
        }
        break;

    case SE2FE_IDLE_VOICE:
        if (d->regs[NV_PAPU_FETFORCE1] & NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FETRAPREASON;
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FETRAPREASON_REQUESTED;
            d->set_irq = true;
        }
        break;

    default:
        /* Handle range-based cases that can't use case ranges in MSVC */
        if (method >= NV1BA0_PIO_SET_HRIR && method < NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            slot = (method - NV1BA0_PIO_SET_HRIR) / 4;
            int8_t left0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT0);
            int8_t right0 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT0);
            int8_t left1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_LEFT1);
            int8_t right1 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_RIGHT1);
            int coeff_idx = slot * 2;
            set_hrir_coeff_tar(d, 0, coeff_idx, left0);
            set_hrir_coeff_tar(d, 1, coeff_idx, right0);
            set_hrir_coeff_tar(d, 0, coeff_idx + 1, left1);
            set_hrir_coeff_tar(d, 1, coeff_idx + 1, right1);
        } else if (method == NV1BA0_PIO_SET_HRIR_X) {
            assert(d->vp.hrtf.current_entry < HRTF_ENTRY_COUNT);
            int8_t left30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_LEFT30);
            int8_t right30 = (int8_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_RIGHT30);
            int16_t itd = (int16_t)GET_MASK(argument, NV1BA0_PIO_SET_HRIR_X_ITD);
            set_hrir_coeff_tar(d, 0, 30, left30);
            set_hrir_coeff_tar(d, 1, 30, right30);
            d->vp.hrtf.entries[d->vp.hrtf.current_entry].itd = s6p9_to_float(itd);
            trace_hrtf_entry(d);
        } else if (method >= NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET &&
                   method < NV1BA0_PIO_SET_SSL_SEGMENT_LENGTH + 8 * 64) {
            assert((method & 0x3) == 0);
            hwaddr addr = d->regs[NV_PAPU_VPSSLADDR]
                          + (d->vp.ssl_base_page * 8)
                          + (method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET);
            stl_le_phys(address_space_memory, addr, argument);
            trace_apu_ssl_event((method & 4u) ? "set-length" : "set-offset",
                                d->regs[NV_PAPU_FECV], -1,
                                (int)((method - NV1BA0_PIO_SET_SSL_SEGMENT_OFFSET) / 8u),
                                d->vp.ssl_base_page, 0u, (uint32_t)addr,
                                (uint32_t)argument);
        } else if (method >= NV1BA0_PIO_SET_SUBMIX_HEADROOM &&
                   method <= NV1BA0_PIO_SET_SUBMIX_HEADROOM + 4 * (NUM_MIXBINS - 1)) {
            assert((method & 3) == 0);
            slot = (method - NV1BA0_PIO_SET_SUBMIX_HEADROOM) / 4;
            d->vp.submix_headroom[slot] =
                (uint8_t)(argument & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
            if (getenv("MERCENARIES_TRACE_APU_HEADROOM") != NULL) {
                fprintf(stderr,
                        "[APU-HEADROOM] type=submix bin=%u amount=%u "
                        "raw=%08X\n",
                        slot, d->vp.submix_headroom[slot], argument);
                fflush(stderr);
            }
        } else if ((method >= NV1BA0_PIO_SET_OUTBUF_BA &&
                    method < NV1BA0_PIO_SET_OUTBUF_BA + 32) ||
                   (method >= NV1BA0_PIO_SET_OUTBUF_LEN &&
                    method < NV1BA0_PIO_SET_OUTBUF_LEN + 32)) {
            /* Outbuf base/length - ignore for now */
        } else {
            /* Unknown method - silently ignore */
            DPRINTF("Unknown FE method: 0x%08X arg=0x%08X\n", method, argument);
        }
        break;
    }
}

/* ============================================================
 * VP MMIO read/write (exposed to apu_core.c)
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;

    switch (addr) {
    case NV1BA0_PIO_FREE:
        return 0x80; /* Always pretend queue is empty */
    default:
        break;
    }
    return 0;
}

void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val,
                        unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    (void)size;

    /* Dispatch known methods through fe_method */
    fe_method(d, (uint32_t)addr, (uint32_t)val);
}

/* ============================================================
 * SGE data pointer resolution
 * ============================================================ */

static hwaddr get_data_ptr(hwaddr sge_base, unsigned int max_sge, uint32_t addr)
{
    unsigned int entry = addr / TARGET_PAGE_SIZE;
    assert(entry <= max_sge);
    uint32_t prd_address =
        ldl_le_phys(address_space_memory, sge_base + entry * 4 * 2);
    return prd_address + addr % TARGET_PAGE_SIZE;
}

/* ============================================================
 * Envelope processing
 * ============================================================ */

static float voice_step_envelope(MCPXAPUState *d, uint16_t v, uint32_t reg_0,
                           uint32_t reg_a, uint32_t rr_reg, uint32_t rr_mask,
                           uint32_t lvl_reg, uint32_t lvl_mask,
                           uint32_t count_mask, uint32_t cur_mask)
{
    uint8_t cur = (uint8_t)voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask);
    switch (cur) {
    case NV_PAVS_VOICE_PAR_STATE_EFCUR_OFF:
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        return 1.0f;

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DELAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0x00);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 0.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_ATTACK: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t attack_rate =
            (uint16_t)voice_get_mask(d, v, reg_0, NV_PAVS_VOICE_CFG_ENV0_EA_ATTACKRATE);
        float value;
        if (attack_rate == 0) {
            value = 255.0f;
        } else {
            if (count <= (uint32_t)(attack_rate * 16)) {
                value = (count * 0xFF) / (float)(attack_rate * 16);
            } else {
                value = 255.0f;
            }
        }
        voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        if (count == (uint32_t)(attack_rate * 16)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t hold_time =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_HOLDTIME);
            count = hold_time * 16;
        } else {
            count++;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_HOLD: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        voice_set_mask(d, v, lvl_reg, lvl_mask, 0xFF);
        if (count == 0) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
            uint16_t decay_rate =
                (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
            count = decay_rate * 16;
        } else {
            count--;
        }
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        return 1.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_DECAY: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t decay_rate =
            (uint16_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_DECAYRATE);
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        float value;
        if (decay_rate == 0) {
            value = 0.0f;
        } else {
            value = 255.0f * powf(0.99988799f, (decay_rate * 16 - count) *
                                                   4096.0f / decay_rate);
        }
        if (value <= (sustain_level + 0.2f) || (value > 255.0f)) {
            cur++;
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, cur);
        } else {
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
            voice_set_mask(d, v, lvl_reg, lvl_mask, (uint32_t)value);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_SUSTAIN: {
        uint8_t sustain_level =
            (uint8_t)voice_get_mask(d, v, reg_a, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL);
        voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, 0x00);
        voice_set_mask(d, v, lvl_reg, lvl_mask, sustain_level);
        return sustain_level / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE: {
        uint16_t count =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask);
        uint16_t release_rate = (uint16_t)voice_get_mask(d, v, rr_reg, rr_mask);
        if (release_rate == 0) count = 0;
        float value = 0;
        if (count == 0) {
            voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, cur_mask, ++cur);
        } else {
            float pos = clampf(1 - count / (release_rate * 16.0f), 0, 1);
            uint8_t lvl = (uint8_t)voice_get_mask(d, v, lvl_reg, lvl_mask);
            value = powf((float)M_E, -6.91f * pos) * lvl;
            count--;
            voice_set_mask(d, v, NV_PAVS_VOICE_CUR_ECNT, count_mask, count);
        }
        return value / 255.0f;
    }

    case NV_PAVS_VOICE_PAR_STATE_EFCUR_FORCE_RELEASE:
        if (count_mask == NV_PAVS_VOICE_CUR_ECNT_EACOUNT) {
            voice_off(d, v);
        }
        return 0.0f;

    default:
        fprintf(stderr, "[APU] Unknown envelope state 0x%x\n", cur);
        return 0.0f;
    }
}

/* ============================================================
 * Sample fetching from voice buffers
 * ============================================================ */

static int voice_get_samples(MCPXAPUState *d, uint32_t v, float samples[][2],
                       int num_samples_requested)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    unsigned int sample_size = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE);
    unsigned int container_sizes[4] = { 1, 2, 0, 4 };
    unsigned int container_size_index = voice_get_mask(
        d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE);
    unsigned int container_size = container_sizes[container_size_index];
    bool stream = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_DATA_TYPE) != 0;
    bool paused = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;
    bool loop = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                               NV_PAVS_VOICE_CFG_FMT_LOOP) != 0;
    uint32_t ebo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_NEXT,
                                  NV_PAVS_VOICE_PAR_NEXT_EBO);
    uint32_t cbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    uint32_t lbo = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSH_SAMPLE,
                                  NV_PAVS_VOICE_CUR_PSH_SAMPLE_LBO);
    uint32_t ba = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CUR_PSL_START,
                                 NV_PAVS_VOICE_CUR_PSL_START_BA);
    unsigned int samples_per_block =
        1 + voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_SAMPLES_PER_BLOCK);
    bool persist = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                                  NV_PAVS_VOICE_CFG_FMT_PERSIST) != 0;

    int ssl_index = 0, ssl_seg = 0, page = 0, count = 0;
    int seg_len = 0, seg_cs = 0, seg_spb = 0, seg_s = 0;
    hwaddr segment_offset = 0;
    uint32_t segment_length = 0;
    size_t block_size;

    int adpcm_block_index = -1;
    uint32_t adpcm_block[36 * 2 / 4];
    int16_t adpcm_decoded[65 * 2];

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                   NV_PAVS_VOICE_PAR_STATE_NEW_VOICE, 0);

    if (paused) return -1;

    if (stream) {
        if (!persist) {
            int eacur = voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_STATE,
                                       NV_PAVS_VOICE_PAR_STATE_EACUR);
            if (eacur < NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE) {
                voice_off(d, (uint16_t)v);
                return -1;
            }
        }

        assert(!loop);
        ssl_index = d->vp.ssl[v].ssl_index;
        ssl_seg = d->vp.ssl[v].ssl_seg;
        page = d->vp.ssl[v].base[ssl_index] + ssl_seg;
        count = d->vp.ssl[v].count[ssl_index];

        if (count == 0) {
            voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                           NV_PAVS_VOICE_PAR_OFFSET_CBO, 0);
            d->vp.ssl[v].ssl_seg = 0;
            if (!persist) {
                d->vp.ssl[v].ssl_index = 0;
                voice_off(d, (uint16_t)v);
            } else {
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE +
                                  d->vp.ssl[v].ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
            return -1;
        }

        hwaddr addr = d->regs[NV_PAPU_VPSSLADDR] + page * 8;
        segment_offset = ldl_le_phys(address_space_memory, addr);
        segment_length = ldl_le_phys(address_space_memory, addr + 4);
        trace_apu_ssl_consume_if_changed(d, v, ssl_index, ssl_seg,
                                         (uint32_t)segment_offset,
                                         segment_length);
        assert(segment_offset != 0);
        assert(segment_length != 0);
        seg_len = (segment_length >> 0) & 0xffff;
        seg_cs = (segment_length >> 16) & 3;
        seg_spb = (segment_length >> 18) & 0x1f;
        seg_s = (segment_length >> 23) & 1;
        container_size_index = seg_cs;
        if (seg_cs == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM) {
            sample_size = NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24;
        }
        assert(seg_len > 0);
        ebo = seg_len - 1;
    }

    bool adpcm =
        (container_size_index == NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM);

    if (adpcm) {
        block_size = 36;
    } else {
        block_size = container_size;
    }
    block_size *= samples_per_block;

    int sample_count = 0;
    for (; (sample_count < num_samples_requested) && (cbo <= ebo);
         sample_count++, cbo++) {
        if (adpcm) {
            unsigned int block_index = cbo / ADPCM_SAMPLES_PER_BLOCK;
            unsigned int block_position = cbo % ADPCM_SAMPLES_PER_BLOCK;
            if (adpcm_block_index != (int)block_index) {
                uint32_t linear_addr = block_index * (uint32_t)block_size;
                if (stream) {
                    hwaddr addr = segment_offset + linear_addr;
                    memcpy(adpcm_block, &d->ram_ptr[addr & 0x03FFFFFF],
                           block_size);
                } else {
                    linear_addr += ba;
                    for (unsigned int word_index = 0;
                         word_index < (9 * samples_per_block); word_index++) {
                        hwaddr addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR],
                                                   0xFFFFFFFF, linear_addr);
                        adpcm_block[word_index] =
                            ldl_le_phys(address_space_memory, addr);
                        linear_addr += 4;
                    }
                }
                adpcm_decode_block(adpcm_decoded, (uint8_t *)adpcm_block,
                                   block_size, channels);
                adpcm_block_index = block_index;
            }

            samples[sample_count][0] =
                int16_to_float(adpcm_decoded[block_position * channels]);
            if (stereo) {
                samples[sample_count][1] = int16_to_float(
                    adpcm_decoded[block_position * channels + 1]);
            }
        } else {
            hwaddr addr;
            if (stream) {
                addr = segment_offset + cbo * block_size;
            } else {
                uint32_t linear_addr = ba + cbo * (uint32_t)block_size;
                addr = get_data_ptr(d->regs[NV_PAPU_VPSGEADDR], 0xFFFFFFFF,
                                    linear_addr);
            }

            for (unsigned int channel = 0; channel < channels; channel++) {
                uint32_t ival;
                float fval;
                switch (sample_size) {
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_U8:
                    ival = ldub_phys(address_space_memory, addr);
                    fval = uint8_to_float((uint8_t)(ival & 0xff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16:
                    ival = lduw_le_phys(address_space_memory, addr);
                    fval = int16_to_float((int16_t)(ival & 0xffff));
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S24:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int24_to_float(ival);
                    break;
                case NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S32:
                    ival = ldl_le_phys(address_space_memory, addr);
                    fval = int32_to_float(ival);
                    break;
                default:
                    fval = 0.0f;
                    break;
                }
                samples[sample_count][channel] = fval;
                addr += container_size;
            }
        }

        if (!stereo) {
            samples[sample_count][1] = samples[sample_count][0];
        }
    }

    if (cbo >= ebo) {
        if (stream) {
            d->vp.ssl[v].ssl_seg += 1;
            cbo = 0;
            if (d->vp.ssl[v].ssl_seg < d->vp.ssl[v].count[ssl_index]) {
                /* Move to next segment */
            } else {
                int next_index = (ssl_index + 1) % 2;
                d->vp.ssl[v].ssl_index = next_index;
                d->vp.ssl[v].ssl_seg = 0;
                trace_apu_ssl_event("advance-list", v, ssl_index, ssl_seg,
                                    d->vp.ssl[v].base[ssl_index],
                                    d->vp.ssl[v].count[ssl_index],
                                    (uint32_t)segment_offset, segment_length);
                set_notify_status(d, v, MCPX_HW_NOTIFIER_SSLA_DONE + ssl_index,
                                  NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS);
            }
        } else {
            if (loop) {
                cbo = lbo;
            } else {
                cbo = ebo;
                voice_off(d, (uint16_t)v);
            }
        }
    }

    voice_set_mask(d, (uint16_t)v, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_CBO, cbo);
    return sample_count;
}

/* ============================================================
 * Voice resampling
 *
 * The callback supplies source samples to the stateful linear resampler
 * in apu_shim.h. Fractional position is retained across reads.
 * ============================================================ */

static long voice_resample_callback(void *cb_data, float **data)
{
    MCPXAPUVoiceFilter *filter = (MCPXAPUVoiceFilter *)cb_data;
    const uint16_t v = filter->voice;
    MCPXAPUState *d = g_state;
    int sample_count = 0;

    assert(d != NULL);
    assert(v < MCPX_HW_MAX_VOICES);
    assert(filter == &d->vp.filters[v]);

    while (sample_count < NUM_SAMPLES_PER_FRAME) {
        int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                    NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        int count;
        if (!active)
            break;
        count = voice_get_samples(
            d, v, (float (*)[2])&filter->resample_buf[2 * sample_count],
            NUM_SAMPLES_PER_FRAME - sample_count);
        /* A zero-length read can be a successful stream-segment advance.
         * Match Xemu and retry the now-current segment; only a negative
         * result is an actual resampler/source failure. */
        if (count < 0)
            break;
        sample_count += count;
    }

    trace_apu_starvation(d, v, NUM_SAMPLES_PER_FRAME - sample_count);
    if (sample_count < NUM_SAMPLES_PER_FRAME) {
        memset(&filter->resample_buf[2 * sample_count], 0,
               2u * (NUM_SAMPLES_PER_FRAME - sample_count) *
                   sizeof(float));
        sample_count = NUM_SAMPLES_PER_FRAME;
    }
    *data = filter->resample_buf;
    return sample_count;
}

static int voice_resample(MCPXAPUState *d, uint16_t v, float samples[][2],
                          int requested_num, float rate)
{
    MCPXAPUVoiceFilter *filter;

    assert(v < MCPX_HW_MAX_VOICES);
    filter = &d->vp.filters[v];
    if (filter->resampler == NULL) {
        int error;
        filter->voice = v;
        filter->resampler = src_callback_new(
            voice_resample_callback, SRC_SINC_FASTEST, 2, &error, filter);
        if (filter->resampler == NULL) {
            fprintf(stderr, "[APU] resampler init failed: %s\n",
                    src_strerror(error));
            return -1;
        }
    }
    return src_callback_read(filter->resampler, rate, requested_num,
                             (float *)samples);
}

/* ============================================================
 * Voice processing (main per-voice function)
 * ============================================================ */

static int peek_ahead_multipass_bin(MCPXAPUState *d, uint16_t v)
{
    bool first = true;
    bool visited[MCPX_HW_MAX_VOICES] = { false };
    const uint16_t start_voice = v;

    while (v != 0xFFFF) {
        /* This is monitor lookahead, not the hardware voice-list executor.
         * A retired/relinked guest entry must not trap an audio worker in
         * a cycle while the CPU waits for the frame's APU lock. Run518
         * captured handle83 linking to itself here. No unique downstream
         * multipass destination exists for an invalid/cyclic chain; keep
         * normal mixing and report it instead of guessing a destination or
         * modifying guest links. The producer/lifetime issue remains traced. */
        if (v >= MCPX_HW_MAX_VOICES || visited[v]) {
            static volatile LONG reports;
            if (InterlockedIncrement(&reports) <= 16)
                fprintf(stderr,
                        "[APU-MP-CHAIN] start=%u invalid-or-repeated=%u ep=%d\n",
                        start_voice, v, d->ep_frame_div);
            return -1;
        }
        visited[v] = true;
        bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
        if (multipass) {
            if (first)
                break;
            return (int)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                       NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        }
        v = (uint16_t)voice_get_mask(
            d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
            NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);
        first = false;
    }
    return -1;
}

static inline float voice_route_headroom(const MCPXAPUState *d,
                                         uint16_t voice,
                                         int route,
                                         int mixbin)
{
    /* The first four routes of a 3D voice are the HRTF outputs. The MCPX
     * applies SET_HRTF_HEADROOM to those routes even though SET_HRTF_SUBMIXES
     * redirects them into ordinary mixbin numbers. Routes 4-7 and every 2D
     * voice use the selected mixbin's headroom. Keep the VP-monitor fallback
     * on the same rule as the hardware mix path. */
    const uint8_t amount =
        voice < MCPX_HW_MAX_3D_VOICES && route < 4 ?
            d->vp.hrtf_headroom : d->vp.submix_headroom[mixbin];
    return (float)(1u << amount);
}

static inline unsigned int voice_route_channel(bool hrtf_applied,
                                               unsigned int source_channels,
                                               int route)
{
    /* Xbox 3D mixbin order is FL, BL, FR, BR. SET_HRTF_SUBMIXES therefore
     * commonly contains 6, 8, 7, 9: the first two routes consume the left
     * HRTF ear and the next two consume the right ear. Alternating ears with
     * route % 2 incorrectly turns the front/back gains into ear selection;
     * at an extreme listener pitch that can discard an entire mono source.
     * Routes 4-7 retain the source voice's ordinary channel mapping. */
    if (hrtf_applied && route < 4)
        return (unsigned int)route / 2u;
    return (unsigned int)route % source_channels;
}

static int g_trace_voice_energy = -1;
static int64_t g_trace_voice_energy_start_us;
static double g_trace_voice_energy_pre[MCPX_HW_MAX_VOICES];
static double g_trace_voice_energy_post[MCPX_HW_MAX_VOICES];
static double g_trace_voice_energy_gain[MCPX_HW_MAX_VOICES];
static uint64_t g_trace_voice_energy_samples[MCPX_HW_MAX_VOICES];
static SRWLOCK g_trace_voice_energy_lock = SRWLOCK_INIT;

static void trace_voice_energy_record(uint16_t voice, const float samples[][2],
                                     double pre_energy, float monitor_gain)
{
    double post_energy = 0.0;
    if (!g_trace_voice_energy)
        return;
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
        post_energy += (double)samples[i][0] * samples[i][0];
        post_energy += (double)samples[i][1] * samples[i][1];
    }
    /* Workers may finish simultaneously. Only accumulate here: resetting the
     * shared counters or printing from a worker races other contributions.
     * The lock also handles a repeated handle in a diagnostic malformed list. */
    AcquireSRWLockExclusive(&g_trace_voice_energy_lock);
    g_trace_voice_energy_pre[voice] += pre_energy;
    g_trace_voice_energy_post[voice] += post_energy;
    g_trace_voice_energy_gain[voice] += monitor_gain;
    g_trace_voice_energy_samples[voice] += NUM_SAMPLES_PER_FRAME * 2u;
    ReleaseSRWLockExclusive(&g_trace_voice_energy_lock);
}

/* Called by the frame thread only, after every voice worker has completed. */
static void trace_voice_energy_flush(void)
{
    if (!g_trace_voice_energy)
        return;
    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (g_trace_voice_energy_start_us == 0)
        g_trace_voice_energy_start_us = now_us;
    if (now_us - g_trace_voice_energy_start_us < 1000000)
        return;
    for (uint32_t i = 0; i < MCPX_HW_MAX_VOICES; ++i) {
        if (g_trace_voice_energy_samples[i] == 0)
            continue;
        fprintf(stderr,
                "[APU-VOICE-ENERGY] voice=%u samples=%llu pre=%.8f "
                "post=%.8f avg_gain=%.8f\n",
                i, (unsigned long long)g_trace_voice_energy_samples[i],
                sqrt(g_trace_voice_energy_pre[i] /
                     g_trace_voice_energy_samples[i]),
                sqrt(g_trace_voice_energy_post[i] /
                     g_trace_voice_energy_samples[i]),
                g_trace_voice_energy_gain[i] /
                    (g_trace_voice_energy_samples[i] /
                     (NUM_SAMPLES_PER_FRAME * 2u)));
    }
    memset(g_trace_voice_energy_pre, 0, sizeof(g_trace_voice_energy_pre));
    memset(g_trace_voice_energy_post, 0, sizeof(g_trace_voice_energy_post));
    memset(g_trace_voice_energy_gain, 0, sizeof(g_trace_voice_energy_gain));
    memset(g_trace_voice_energy_samples, 0, sizeof(g_trace_voice_energy_samples));
    g_trace_voice_energy_start_us = now_us;
    fflush(stderr);
}

static void voice_process(MCPXAPUState *d,
                          float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                          float sample_buf[NUM_SAMPLES_PER_FRAME][2],
                          uint16_t v, int voice_list)
{
    assert(v < MCPX_HW_MAX_VOICES);
    bool stereo = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                 NV_PAVS_VOICE_CFG_FMT_STEREO) != 0;
    unsigned int channels = stereo ? 2 : 1;
    bool paused = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                 NV_PAVS_VOICE_PAR_STATE_PAUSED) != 0;

    struct McpxApuDebugVoice *dbg = &g_dbg.vp.v[v];
    dbg->active = true;
    dbg->stereo = stereo;
    dbg->paused = paused;

    if (paused) return;

    /* Step filter envelope */
    float ef_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV1, NV_PAVS_VOICE_CFG_ENVF,
        NV_PAVS_VOICE_CFG_MISC, NV_PAVS_VOICE_CFG_MISC_EF_RELEASERATE,
        NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EFLVL,
        NV_PAVS_VOICE_CUR_ECNT_EFCOUNT, NV_PAVS_VOICE_PAR_STATE_EFCUR);
    if (ef_value < 0.0f) ef_value = 0.0f;
    if (ef_value > 1.0f) ef_value = 1.0f;

    int16_t p = (int16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                                         NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH);
    int8_t ps = (int8_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_ENV0,
                                        NV_PAVS_VOICE_CFG_ENV0_EF_PITCHSCALE);
    float rate = 1.0f / powf(2.0f, (p + ps * 32 * ef_value) / 4096.0f);
    dbg->rate = rate;

    /* Step amplitude envelope */
    float ea_value = voice_step_envelope(
        d, v, NV_PAVS_VOICE_CFG_ENV0, NV_PAVS_VOICE_CFG_ENVA,
        NV_PAVS_VOICE_TAR_LFO_ENV, NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE,
        NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_EALVL,
        NV_PAVS_VOICE_CUR_ECNT_EACOUNT, NV_PAVS_VOICE_PAR_STATE_EACUR);
    if (ea_value < 0.0f) ea_value = 0.0f;
    if (ea_value > 1.0f) ea_value = 1.0f;

    float samples[NUM_SAMPLES_PER_FRAME][2];
    memset(samples, 0, sizeof(samples));

    bool multipass = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS) != 0;
    dbg->multipass = multipass;

    if (multipass) {
        /* Read from multipass bin */
        int mp_bin = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                    NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        dbg->multipass_bin = (uint8_t)mp_bin;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            samples[i][0] = mixbins[mp_bin][i];
            samples[i][1] = mixbins[mp_bin][i];
        }
        bool clear_mix = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT,
                                        NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX) != 0;
        if (clear_mix) {
            memset(&mixbins[mp_bin][0], 0, sizeof(mixbins[0]));
        }
    } else {
        for (int sample_count = 0; sample_count < NUM_SAMPLES_PER_FRAME;) {
            int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                        NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
            if (!active) return;
            int count = voice_resample(d, v, &samples[sample_count],
                                       NUM_SAMPLES_PER_FRAME - sample_count, rate);
            if (count < 0) break;
            sample_count += count;
        }
    }

    int active = voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    if (!active) return;

    double trace_pre_energy = 0.0;
    if (g_trace_voice_energy) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
            trace_pre_energy += (double)samples[i][0] * samples[i][0];
            trace_pre_energy += (double)samples[i][1] * samples[i][1];
        }
    }

    /* Get volume bins */
    int bin[8];
    bin[0] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
    bin[1] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
    bin[2] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
    bin[3] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    bin[4] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    }

    uint16_t vol[8];
    vol[0] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0);
    vol[1] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME1);
    vol[2] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME2);
    vol[3] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME3);
    vol[4] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME4);
    vol[5] = (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME5);
    vol[6] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8) << 8);
    vol[6] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4) << 4);
    vol[6] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0);
    vol[7] = (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLC, NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8) << 8);
    vol[7] |= (uint16_t)(voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLB, NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4) << 4);
    vol[7] |= (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0);

    for (int i = 0; i < 8; i++) {
        dbg->bin[i] = (uint8_t)bin[i];
        dbg->vol[i] = vol[i];
    }

    if (voice_should_mute(v)) return;

    /* Low-pass filter */
    int fmode = voice_get_mask(d, v, NV_PAVS_VOICE_CFG_MISC,
                               NV_PAVS_VOICE_CFG_MISC_FMODE);
    bool lpf = false;
    if (v < MCPX_HW_MAX_3D_VOICES) {
        lpf = (fmode == 1);
    } else {
        lpf = stereo ? (fmode == 1) : (fmode & 1) != 0;
    }
    if (lpf) {
        for (int ch = 0; ch < 2; ch++) {
            int16_t fc = (int16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC0);
            float fc_f = clampf(powf(2, fc / 4096.0f), 0.003906f, 1.0f);
            uint16_t q = (uint16_t)voice_get_mask(
                d, v, NV_PAVS_VOICE_TAR_FCA + (ch % channels) * 4,
                NV_PAVS_VOICE_TAR_FCA_FC1);
            float q_f = clampf(q / (1.0f * 0x8000), 0.079407f, 1.0f);
            sv_filter *filter = &d->vp.filters[v].svf[ch];
            setup_svf(filter, fc_f, q_f, F_LP);
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                samples[i][ch] = run_svf(filter, samples[i][ch]);
                samples[i][ch] = fminf(fmaxf(samples[i][ch], -1.0f), 1.0f);
            }
        }
    }

    /* HRTF processing for 3D voices */
    bool hrtf_applied = false;
    if (v < MCPX_HW_MAX_3D_VOICES && d->vp.hrtf_enabled) {
        uint16_t hrtf_handle =
            (uint16_t)voice_get_mask(d, v, NV_PAVS_VOICE_CFG_HRTF_TARGET,
                                     NV_PAVS_VOICE_CFG_HRTF_TARGET_HANDLE);
        if (hrtf_handle != HRTF_NULL_HANDLE) {
            double hrtf_input_energy = 0.0;
            if (trace_hrtf_enabled()) {
                for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; ++sample) {
                    hrtf_input_energy +=
                        (double)samples[sample][0] * samples[sample][0];
                    hrtf_input_energy +=
                        (double)samples[sample][1] * samples[sample][1];
                }
            }
            hrtf_filter_process_mono_source(&d->vp.filters[v].hrtf, samples);
            trace_hrtf_voice(d, v, hrtf_handle, samples,
                             hrtf_input_energy, bin, vol);
            hrtf_applied = true;
        }
    }

    /* Mix into bins */
    for (int b = 0; b < 8; b++) {
        float g = ea_value;
        const float hr = voice_route_headroom(d, v, b, bin[b]);
        g *= attenuate(vol[b]) / hr;
        const unsigned int route_channel =
            voice_route_channel(hrtf_applied, channels, b);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            mixbins[bin[b]][i] += g * samples[i][route_channel];
        }
    }

    /* VP monitor mix */
    float trace_monitor_gain = 0.0f;
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int mp_bin = -1;
        if (voice_list == NV1BA0_PIO_SET_ANTECEDENT_VOICE_LIST_MP_TOP - 1)
            mp_bin = peek_ahead_multipass_bin(d, v);
        float g = 0.0f;
        for (int b = 0; b < 8; b++) {
            if (bin[b] == mp_bin)
                continue;
            /* Match Xemu's multipass recombination path: once a route has
             * been redirected into a destination mixbin, its recovered
             * monitor contribution uses that submix bin's headroom. HRTF
             * headroom applies in the primary 3D-route mix above, not again
             * here; applying it twice can attenuate fallback voices by 128x. */
            const float hr =
                (float)(1u << d->vp.submix_headroom[bin[b]]);
            float bg = attenuate(vol[b]) / hr;
            if (bg > g) g = bg;
        }
        g *= ea_value;
        trace_monitor_gain = g;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            sample_buf[i][0] += g * samples[i][0];
            sample_buf[i][1] += g * samples[i][1];
        }
    }
    trace_voice_energy_record(v, samples, trace_pre_energy,
                             trace_monitor_gain);

    (void)voice_list;
}

/* ============================================================
 * Parallel voice dispatch
 *
 * This follows Xemu's dependency-aware scheduler. Multipass voices which
 * consume one another's mixbin output remain on the same worker; independent
 * voices can run concurrently. The port originally retained these structures
 * but processed every HRTF/resampler serially on the APU thread, which becomes
 * a major frame-time cost once gameplay has many simultaneous voices.
 * ============================================================ */

static void get_voice_bin_src_dst(MCPXAPUState *d, int v,
                                  uint32_t *src, uint32_t *dst,
                                  uint32_t *clr)
{
    uint32_t src_v = 0;
    uint32_t dst_v = 0;
    uint32_t clr_v = 0;
    int bin[8];

    if (voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                       NV_PAVS_VOICE_CFG_FMT_MULTIPASS)) {
        int mp_bin = (int)voice_get_mask(
            d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
            NV_PAVS_VOICE_CFG_FMT_MULTIPASS_BIN);
        src_v |= 1u << mp_bin;
        if (voice_get_mask(d, (uint16_t)v, NV_PAVS_VOICE_CFG_FMT,
                           NV_PAVS_VOICE_CFG_FMT_CLEAR_MIX))
            clr_v |= 1u << mp_bin;
    }

    if (v < MCPX_HW_MAX_3D_VOICES) {
        bin[0] = d->vp.hrtf_submix[0];
        bin[1] = d->vp.hrtf_submix[1];
        bin[2] = d->vp.hrtf_submix[2];
        bin[3] = d->vp.hrtf_submix[3];
    } else {
        bin[0] = (int)voice_get_mask(d, (uint16_t)v,
            NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN);
        bin[1] = (int)voice_get_mask(d, (uint16_t)v,
            NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V1BIN);
        bin[2] = (int)voice_get_mask(d, (uint16_t)v,
            NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V2BIN);
        bin[3] = (int)voice_get_mask(d, (uint16_t)v,
            NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V3BIN);
    }
    bin[4] = (int)voice_get_mask(d, (uint16_t)v,
        NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V4BIN);
    bin[5] = (int)voice_get_mask(d, (uint16_t)v,
        NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V5BIN);
    bin[6] = (int)voice_get_mask(d, (uint16_t)v,
        NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V6BIN);
    bin[7] = (int)voice_get_mask(d, (uint16_t)v,
        NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_V7BIN);
    for (int i = 0; i < 8; ++i)
        dst_v |= 1u << bin[i];

    if (src) *src = src_v;
    if (dst) *dst = dst_v;
    if (clr) *clr = clr_v;
}

static void *voice_worker_thread(void *arg)
{
    MCPXAPUState *d = (MCPXAPUState *)arg;
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    VoiceWorker *self;
    int worker_id;

    rcu_register_thread();
    qemu_mutex_lock(&vwd->lock);
    worker_id = ctz64(vwd->workers_pending);
    self = &vwd->workers[worker_id];
    self->queue_len = 0;

    do {
        if (self->queue_len) {
            qemu_mutex_unlock(&vwd->lock);
            memset(self->mixbins, 0, sizeof(self->mixbins));
            memset(self->sample_buf, 0, sizeof(self->sample_buf));
            for (int i = 0; i < self->queue_len; ++i) {
                voice_process(d, self->mixbins, self->sample_buf,
                              (uint16_t)self->queue[i].voice,
                              self->queue[i].list);
            }
            qemu_mutex_lock(&vwd->lock);

            for (int b = 0; b < NUM_MIXBINS; ++b)
                for (int s = 0; s < NUM_SAMPLES_PER_FRAME; ++s)
                    vwd->mixbins[b][s] += self->mixbins[b][s];
            if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
                for (int s = 0; s < NUM_SAMPLES_PER_FRAME; ++s) {
                    d->vp.sample_buf[s][0] += self->sample_buf[s][0];
                    d->vp.sample_buf[s][1] += self->sample_buf[s][1];
                }
            }
            self->queue_len = 0;
        }

        vwd->workers_pending &= ~(1ull << worker_id);
        if (!vwd->workers_pending)
            qemu_cond_signal(&vwd->work_finished);
        qemu_cond_wait(&vwd->work_pending, &vwd->lock);
    } while (!vwd->workers_should_exit);

    qemu_mutex_unlock(&vwd->lock);
    rcu_unregister_thread();
    return NULL;
}

static void voice_work_enqueue(MCPXAPUState *d, int v, int list)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    assert(vwd->queue_len < (int)ARRAY_SIZE(vwd->queue));
    vwd->queue[vwd->queue_len].voice = v;
    vwd->queue[vwd->queue_len].list = list;
    ++vwd->queue_len;
}

static void voice_work_schedule(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    int next_worker = 0;
    bool group = false;
    uint32_t dirty = 0;

    for (int i = 0; i < vwd->queue_len; ++i) {
        uint32_t src, dst, clr;
        VoiceWorker *worker;
        get_voice_bin_src_dst(d, vwd->queue[i].voice, &src, &dst, &clr);
        if ((dst & MULTIPASS_BIN_MASK) & ~dirty)
            group = true;
        worker = &vwd->workers[next_worker];
        worker->queue[worker->queue_len++] = vwd->queue[i];
        vwd->workers_pending |= 1ull << next_worker;
        dirty = (dirty & ~clr) | dst;
        if (clr & MULTIPASS_BIN_MASK)
            group = false;
        if (!group)
            next_worker = (next_worker + 1) % vwd->num_workers;
        (void)src;
    }
}

static bool any_queued_voice_locked(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    for (int i = 0; i < vwd->queue_len; ++i)
        if (is_voice_locked(d, (uint16_t)vwd->queue[i].voice))
            return true;
    return false;
}

static void voice_work_dispatch(MCPXAPUState *d,
                                float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;

    while (vwd->queue_len && any_queued_voice_locked(d)) {
        if (qatomic_read(&d->pause_requested)) {
            vwd->queue_len = 0;
            return;
        }
        qemu_cond_timedwait(&d->cond, &d->lock, 1);
    }

    qemu_mutex_lock(&vwd->lock);
    if (vwd->queue_len) {
        memset(vwd->mixbins, 0, sizeof(vwd->mixbins));
        voice_work_schedule(d);
        qemu_cond_broadcast(&vwd->work_pending);
        while (vwd->workers_pending)
            qemu_cond_wait(&vwd->work_finished, &vwd->lock);
        vwd->queue_len = 0;
        for (int b = 0; b < NUM_MIXBINS; ++b)
            for (int s = 0; s < NUM_SAMPLES_PER_FRAME; ++s)
                mixbins[b][s] += vwd->mixbins[b][s];
    }
    qemu_mutex_unlock(&vwd->lock);
}

/* ============================================================
 * VP frame - process all voice lists
 *
 * Uses dependency-aware worker dispatch when workers are available;
 * otherwise processes voices on the frame thread.
 * ============================================================ */

void mcpx_apu_vp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    static int trace_voice_perf = -1;
    static int trace_mixbins = -1;
    static int64_t trace_period_start_us;
    static int64_t trace_mixbin_period_start_us;
    static uint64_t trace_frames;
    static uint64_t trace_traversed;
    static uint64_t trace_active;
    static uint64_t trace_voice_us;
    static uint64_t trace_handle_calls[MCPX_HW_MAX_VOICES];
    static uint64_t trace_handle_us[MCPX_HW_MAX_VOICES];
    static double trace_mixbin_energy[NUM_MIXBINS];
    static float trace_mixbin_peak[NUM_MIXBINS];
    static double trace_monitor_energy[2];
    static float trace_monitor_peak[2];
    static uint64_t trace_mixbin_frames;
    int frame_traversed = 0;
    int frame_active = 0;
    int64_t frame_voice_us = 0;
    const bool threaded = d->vp.voice_work_dispatch.num_workers > 0;

    if (trace_voice_perf < 0) {
        trace_voice_perf = getenv("MERCENARIES_TRACE_APU_VOICES") != NULL;
    }
    if (trace_mixbins < 0) {
        trace_mixbins = getenv("MERCENARIES_TRACE_APU_MIXBINS") != NULL;
    }
    /* Publish this once before worker dispatch; workers never mutate it. */
    if (g_trace_voice_energy < 0) {
        g_trace_voice_energy =
            getenv("MERCENARIES_TRACE_APU_VOICE_ENERGY") != NULL;
    }
    memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));

    for (int list = 0; list < 3; list++) {
        InterlockedExchange(&d->debug_worker_list, list);
        hwaddr top, current, next;
        top = voice_list_regs[list].top;
        current = voice_list_regs[list].current;
        next = voice_list_regs[list].next;

        d->regs[current] = d->regs[top];

        for (int i = 0; d->regs[current] != 0xFFFF; i++) {
            InterlockedExchange(&d->debug_worker_iteration, i);
            if (i >= MCPX_HW_MAX_VOICES) {
                DPRINTF("Voice list contains invalid entry!\n");
                break;
            }

            uint16_t v = (uint16_t)d->regs[current];
            InterlockedExchange(&d->debug_worker_voice, v);
            InterlockedExchange(&d->debug_worker_stage, 20);
            ++frame_traversed;
            d->regs[next] = voice_get_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                               NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE);

            if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE,
                                NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) {
                fe_method(d, SE2FE_IDLE_VOICE, v);
            } else {
                /* Preserve a diagnostic serial fallback, but use Xemu's
                 * dependency-aware worker scheduler by default. */
                int64_t voice_start_us = trace_voice_perf
                    ? qemu_clock_get_us(QEMU_CLOCK_REALTIME) : 0;
                ++frame_active;
                if (threaded)
                    voice_work_enqueue(d, v, list);
                else
                    voice_process(d, mixbins, d->vp.sample_buf, v, list);
                InterlockedExchange(&d->debug_worker_stage, 21);
                if (trace_voice_perf && !threaded) {
                    uint64_t elapsed_voice_us = (uint64_t)(
                        qemu_clock_get_us(QEMU_CLOCK_REALTIME) - voice_start_us);
                    frame_voice_us += (int64_t)elapsed_voice_us;
                    ++trace_handle_calls[v];
                    trace_handle_us[v] += elapsed_voice_us;
                }
            }
            d->regs[current] = d->regs[next];
        }
    }

    if (threaded) {
        int64_t voice_start_us = trace_voice_perf
            ? qemu_clock_get_us(QEMU_CLOCK_REALTIME) : 0;
        voice_work_dispatch(d, mixbins);
        if (trace_voice_perf)
            frame_voice_us += qemu_clock_get_us(QEMU_CLOCK_REALTIME) -
                              voice_start_us;
    }

    trace_voice_energy_flush();

    if (trace_voice_perf) {
        int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (trace_period_start_us == 0) {
            trace_period_start_us = now_us;
        }
        ++trace_frames;
        trace_traversed += (uint64_t)frame_traversed;
        trace_active += (uint64_t)frame_active;
        trace_voice_us += (uint64_t)frame_voice_us;
        if (now_us - trace_period_start_us >= 1000000) {
            fprintf(stderr,
                    "[APU-VOICES] frames=%llu traversed=%llu active=%llu "
                    "active_per_frame=%.2f voice_us=%llu us_per_voice=%.2f\n",
                    (unsigned long long)trace_frames,
                    (unsigned long long)trace_traversed,
                    (unsigned long long)trace_active,
                    trace_frames ? (double)trace_active / trace_frames : 0.0,
                    (unsigned long long)trace_voice_us,
                    trace_active ? (double)trace_voice_us / trace_active : 0.0);
            for (uint32_t handle = 0; handle < MCPX_HW_MAX_VOICES; ++handle) {
                if (trace_handle_calls[handle] == 0)
                    continue;
                fprintf(stderr,
                        "  [APU-VOICE] handle=%u calls=%llu us=%llu avg_us=%.2f\n",
                        handle,
                        (unsigned long long)trace_handle_calls[handle],
                        (unsigned long long)trace_handle_us[handle],
                        (double)trace_handle_us[handle] / trace_handle_calls[handle]);
                trace_handle_calls[handle] = 0;
                trace_handle_us[handle] = 0;
            }
            trace_period_start_us = now_us;
            trace_frames = 0;
            trace_traversed = 0;
            trace_active = 0;
            trace_voice_us = 0;
        }
    }

    if (trace_mixbins) {
        int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (trace_mixbin_period_start_us == 0)
            trace_mixbin_period_start_us = now_us;
        ++trace_mixbin_frames;
        for (int bin = 0; bin < NUM_MIXBINS; ++bin) {
            for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; ++sample) {
                float value = mixbins[bin][sample];
                float magnitude = fabsf(value);
                trace_mixbin_energy[bin] += (double)value * value;
                if (magnitude > trace_mixbin_peak[bin])
                    trace_mixbin_peak[bin] = magnitude;
            }
        }
        for (int channel = 0; channel < 2; ++channel) {
            for (int sample = 0; sample < NUM_SAMPLES_PER_FRAME; ++sample) {
                float value = d->vp.sample_buf[sample][channel];
                float magnitude = fabsf(value);
                trace_monitor_energy[channel] += (double)value * value;
                if (magnitude > trace_monitor_peak[channel])
                    trace_monitor_peak[channel] = magnitude;
            }
        }
        if (now_us - trace_mixbin_period_start_us >= 1000000) {
            const double sample_count =
                (double)trace_mixbin_frames * NUM_SAMPLES_PER_FRAME;
            fprintf(stderr,
                    "[APU-MIXBINS] frames=%llu monitor_l=%.6f/%.6f "
                    "monitor_r=%.6f/%.6f",
                    (unsigned long long)trace_mixbin_frames,
                    sqrt(trace_monitor_energy[0] / sample_count),
                    trace_monitor_peak[0],
                    sqrt(trace_monitor_energy[1] / sample_count),
                    trace_monitor_peak[1]);
            for (int bin = 0; bin < NUM_MIXBINS; ++bin) {
                const double rms = sqrt(trace_mixbin_energy[bin] / sample_count);
                if (rms > 0.000001 || trace_mixbin_peak[bin] > 0.000001f) {
                    fprintf(stderr, " bin%d=%.6f/%.6f", bin, rms,
                            trace_mixbin_peak[bin]);
                }
            }
            fprintf(stderr, "\n");
            fflush(stderr);
            memset(trace_mixbin_energy, 0, sizeof(trace_mixbin_energy));
            memset(trace_mixbin_peak, 0, sizeof(trace_mixbin_peak));
            memset(trace_monitor_energy, 0, sizeof(trace_monitor_energy));
            memset(trace_monitor_peak, 0, sizeof(trace_monitor_peak));
            trace_mixbin_frames = 0;
            trace_mixbin_period_start_us = now_us;
        }
    }

    InterlockedExchange(&d->debug_worker_voice, -1);
    InterlockedExchange(&d->debug_worker_list, -1);
    InterlockedExchange(&d->debug_worker_iteration, -1);

    /* VP monitor output */
    if (d->monitor.point == MCPX_APU_DEBUG_MON_VP) {
        int16_t isamp[NUM_SAMPLES_PER_FRAME * 2];
        src_float_to_short_array((float *)d->vp.sample_buf, isamp,
                                 NUM_SAMPLES_PER_FRAME * 2);
        int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            d->monitor.frame_buf[off + i][0] += isamp[2 * i];
            d->monitor.frame_buf[off + i][1] += isamp[2 * i + 1];
        }
        memset(d->vp.sample_buf, 0, sizeof(d->vp.sample_buf));
        memset(mixbins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    }
}

void mcpx_apu_debug_dump_progress(void)
{
    MCPXAPUState *d = g_state;
    if (!d) {
        fprintf(stderr, "[APU-PROGRESS] unavailable\n");
        return;
    }
    fprintf(stderr,
            "[APU-PROGRESS] stage=%ld heartbeat=%ld ep=%d voice=%ld list=%ld "
            "iteration=%ld SECTL=%08X FECTL=%08X\n",
            InterlockedCompareExchange(&d->debug_worker_stage, 0, 0),
            InterlockedCompareExchange(&d->debug_worker_heartbeat, 0, 0),
            d->ep_frame_div,
            InterlockedCompareExchange(&d->debug_worker_voice, 0, 0),
            InterlockedCompareExchange(&d->debug_worker_list, 0, 0),
            InterlockedCompareExchange(&d->debug_worker_iteration, 0, 0),
            d->regs[NV_PAPU_SECTL], d->regs[NV_PAPU_FECTL]);
    fflush(stderr);
}

/* ============================================================
 * VP Init / Finalize / Reset
 * ============================================================ */

static void mcpx_apu_vp_init_diagnostic_options(MCPXAPUState *d)
{
    /* Process options are immutable during a run. Cache before starting the
     * workers: a CRT environment scan per voice per 32-sample block causes
     * needless work and cross-worker environment-lock contention. */
    const bool enable_hrtf =
        getenv("MERCENARIES_ENABLE_APU_HRTF") != NULL;
    const bool disable_hrtf =
        getenv("MERCENARIES_TEST_DISABLE_HRTF") != NULL;
    d->vp.hrtf_enabled =
        (g_config.audio.hrtf || enable_hrtf) && !disable_hrtf;
}

void mcpx_apu_vp_init(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    SYSTEM_INFO info;
    int requested;

    mcpx_apu_vp_init_diagnostic_options(d);
    if (getenv("MERCENARIES_DISABLE_APU_WORKERS") != NULL) {
        vwd->num_workers = 0;
        fprintf(stderr, "[APU] VP voice workers disabled\n");
        return;
    }

    GetSystemInfo(&info);
    requested = g_config.audio.vp.num_workers > 0 ?
        g_config.audio.vp.num_workers : (int)info.dwNumberOfProcessors;
    /* Eight workers are enough to cover the title's practical active-voice
     * count without making synchronization dominate each 32-sample frame. */
    vwd->num_workers = MAX(1, MIN(requested, 8));
    vwd->workers = (VoiceWorker *)g_malloc0_n(
        vwd->num_workers, sizeof(VoiceWorker));
    if (!vwd->workers) {
        vwd->num_workers = 0;
        fprintf(stderr, "[APU] VP worker allocation failed; using serial mixer\n");
        return;
    }
    vwd->workers_should_exit = false;
    vwd->workers_pending = 0;
    vwd->queue_len = 0;
    qemu_mutex_init(&vwd->lock);
    qemu_mutex_lock(&vwd->lock);
    qemu_cond_init(&vwd->work_pending);
    qemu_cond_init(&vwd->work_finished);
    for (int i = 0; i < vwd->num_workers; ++i) {
        vwd->workers_pending |= 1ull << i;
        qemu_thread_create(&vwd->workers[i].thread, "mcpx.voice_worker",
                           voice_worker_thread, d, QEMU_THREAD_JOINABLE);
    }
    while (vwd->workers_pending)
        qemu_cond_wait(&vwd->work_finished, &vwd->lock);
    qemu_mutex_unlock(&vwd->lock);
    fprintf(stderr, "[APU] VP voice workers initialized: %d\n",
            vwd->num_workers);
}

void mcpx_apu_vp_finalize(MCPXAPUState *d)
{
    VoiceWorkDispatch *vwd = &d->vp.voice_work_dispatch;
    if (vwd->num_workers <= 0 || !vwd->workers)
        return;
    qemu_mutex_lock(&vwd->lock);
    vwd->workers_should_exit = true;
    qemu_cond_broadcast(&vwd->work_pending);
    qemu_mutex_unlock(&vwd->lock);
    for (int i = 0; i < vwd->num_workers; ++i)
        qemu_thread_join(&vwd->workers[i].thread);
    free(vwd->workers);
    vwd->workers = NULL;
    vwd->num_workers = 0;
}

void mcpx_apu_vp_reset(MCPXAPUState *d)
{
    d->vp.ssl_base_page = 0;
    d->vp.hrtf_headroom = 0;
    memset(d->vp.ssl, 0, sizeof(d->vp.ssl));
    memset(d->vp.hrtf_submix, 0, sizeof(d->vp.hrtf_submix));
    memset(d->vp.submix_headroom, 0, sizeof(d->vp.submix_headroom));
    memset(d->vp.voice_locked, 0, sizeof(d->vp.voice_locked));
    for (int v = 0; v < MCPX_HW_MAX_VOICES; v++) {
        hrtf_filter_init(&d->vp.filters[v].hrtf);
    }
}

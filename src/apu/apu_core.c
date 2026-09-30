/*
 * MCPX APU Core - Standalone extraction from xemu
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
#include "apu.h"
#include "apu_xaudio2.h"
#include "fpconv.h"

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
static int g_trace_apu_perf = -1;
static int g_trace_apu_pcm = -1;
static int g_trace_apu_irq = -1;
static uint32_t g_trace_apu_irq_events = 0;
static int64_t g_apu_perf_vp_us = 0;
static int64_t g_apu_perf_dsp_us = 0;
static int64_t g_apu_perf_monitor_us = 0;
static LONG g_apu_xgscnt_reads = 0;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

extern void xbox_kernel_raise_hardware_interrupt(uint32_t bus_level);
extern void xbox_kernel_lower_hardware_interrupt(uint32_t bus_level);

/* ============================================================
 * APU interrupt latching and guest-kernel delivery
 * ============================================================ */

static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    const uint32_t ien = qatomic_read(&d->regs[NV_PAPU_IEN]);
    const uint32_t ists = qatomic_read(&d->regs[NV_PAPU_ISTS]);
    const bool asserted =
        (ien & NV_PAPU_ISTS_GINTSTS) &&
        ((ists & ~NV_PAPU_ISTS_GINTSTS) & ien);
    if (g_trace_apu_irq < 0) {
        g_trace_apu_irq = getenv("MERCENARIES_TRACE_APU_IRQ") != NULL;
    }
    if (g_trace_apu_irq && g_trace_apu_irq_events++ < 512u) {
        fprintf(stderr,
                "[APU-IRQ] IEN=%08X ISTS=%08X FECTL=%08X assert=%u ep=%d\n",
                ien, ists, qatomic_read(&d->regs[NV_PAPU_FECTL]),
                asserted ? 1u : 0u, d->ep_frame_div);
    }
    if (asserted) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        xbox_kernel_raise_hardware_interrupt(5u);
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        xbox_kernel_lower_hardware_interrupt(5u);
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        /* XGSCNT is the APU sample counter, not a wall-clock timer. XMV uses
         * it as the master clock when an audio stream is enabled. */
        r = (uint64_t)d->ep_frame_div * NUM_SAMPLES_PER_FRAME;
        if (g_trace_apu_perf < 0)
            g_trace_apu_perf = getenv("MERCENARIES_TRACE_APU_PERF") != NULL;
        if (g_trace_apu_perf) {
            LONG reads = InterlockedIncrement(&g_apu_xgscnt_reads);
            if (reads <= 32 || (reads & 0xffff) == 0)
                fprintf(stderr, "[APU-XGSCNT] reads=%ld ep=%d value=%llu\n",
                        reads, d->ep_frame_div, (unsigned long long)r);
        }
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        if (g_trace_apu_perf < 0)
            g_trace_apu_perf = getenv("MERCENARIES_TRACE_APU_PERF") != NULL;
        if (g_trace_apu_perf)
            fprintf(stderr, "[APU-REG] addr=%05X old=%08X new=%08X ep=%d\n",
                    (unsigned int)addr, d->regs[addr], (uint32_t)val,
                    d->ep_frame_div);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* waveOut output buffer ring */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 256   /* one 8 x 32-sample MCPX output frame */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    /* The VP/DSP pipeline has filled all eight 32-sample slices by now.
     * Mix software voices into that same 256-sample frame without clearing
     * the hardware output first. */
    if (g_audio_muted) {
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    } else {
        if (g_test_tone.active) {
            for (int i = 0; i < MIXER_FRAME_SAMPLES; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) *
                                      g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }
        mixer_render(d->monitor.frame_buf, MIXER_FRAME_SAMPLES);
    }

    if (g_trace_apu_pcm < 0)
        g_trace_apu_pcm = getenv("MERCENARIES_TRACE_APU_PCM") != NULL;
    if (g_trace_apu_pcm) {
        static int64_t last_trace_us;
        static uint64_t absolute_sum;
        static uint64_t sample_count;
        static uint64_t nonzero_count;
        static int peak_left;
        static int peak_right;
        static uint32_t frame_count;
        int64_t now_us;
        for (int i = 0; i < MIXER_FRAME_SAMPLES; ++i) {
            int left = d->monitor.frame_buf[i][0];
            int right = d->monitor.frame_buf[i][1];
            int abs_left = left < 0 ? -left : left;
            int abs_right = right < 0 ? -right : right;
            if (abs_left > peak_left) peak_left = abs_left;
            if (abs_right > peak_right) peak_right = abs_right;
            absolute_sum += (uint64_t)abs_left + (uint64_t)abs_right;
            nonzero_count += left != 0;
            nonzero_count += right != 0;
            sample_count += 2u;
        }
        ++frame_count;
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        if (last_trace_us == 0) last_trace_us = now_us;
        if (now_us - last_trace_us >= 1000000) {
            fprintf(stderr,
                    "[APU-PCM] frames=%u samples=%llu nonzero=%llu "
                    "peak=%d/%d mean_abs=%.2f\n",
                    frame_count, (unsigned long long)sample_count,
                    (unsigned long long)nonzero_count, peak_left, peak_right,
                    sample_count ? (double)absolute_sum / sample_count : 0.0);
            absolute_sum = 0;
            sample_count = 0;
            nonzero_count = 0;
            peak_left = 0;
            peak_right = 0;
            frame_count = 0;
            last_trace_us = now_us;
        }
    }

    /* Submit exactly one completed MCPX output frame. */
    if (xa2_is_active()) {
        int samples = xa2_get_buffer_size();
        if (samples > MIXER_FRAME_SAMPLES)
            samples = MIXER_FRAME_SAMPLES;
        xa2_submit_samples((const int16_t *)d->monitor.frame_buf, samples);
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        return;
    }

    if (!g_waveout.initialized) {
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        return;
    }

    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];

    /* Wait if this buffer is still playing (with timeout). */
    int wait_loops = 0;
    while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
        qemu_mutex_unlock(&d->lock);
        Sleep(1);
        qemu_mutex_lock(&d->lock);
        if (++wait_loops > 50) break;
    }

    memcpy(g_waveout.bufs[idx], d->monitor.frame_buf,
           sizeof(d->monitor.frame_buf));
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));

    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

static void throttle(MCPXAPUState *d)
{
    int queued_buffers = -1;
    int queue_low_watermark = 1;

    if (d->ep_frame_div % 8) {
        return;
    }

    /* Match Xemu's queue-watermark pacing. A fixed timer cannot recover
     * after a decoder or renderer stall empties the host queue: it keeps
     * producing only one 256-sample frame every 5.33 ms. When XAudio2 is
     * starved, run frames back-to-back until more than one is queued; when
     * its ring is full, wait instead of producing and dropping a frame. */
    if (xa2_is_active()) {
        const int high = xa2_get_queue_capacity();
        queue_low_watermark = high / 2;
        queued_buffers = xa2_get_queued_buffers();
        while (!d->pause_requested && high > 0 &&
               queued_buffers >= high) {
            qemu_cond_timedwait(&d->cond, &d->lock, 1);
            queued_buffers = xa2_get_queued_buffers();
        }
    }

    if (queued_buffers >= 0 &&
        queued_buffers <= queue_low_watermark)
        return;

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > EP_FRAME_US) {
        d->next_frame_time_us = now_us;
    }

    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
        if (remaining_ms > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, (int)remaining_ms);
        } else {
            break;
        }
    }
    d->next_frame_time_us += EP_FRAME_US;

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    InterlockedExchange(&d->debug_worker_stage, 10);
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        if (g_trace_apu_perf < 0) {
            g_trace_apu_perf = getenv("MERCENARIES_TRACE_APU_PERF") != NULL;
        }
        if (g_trace_apu_perf) {
            fprintf(stderr,
                    "[APU-PERF] ep=%d samples=%llu elapsed_ms=%lld "
                    "subframes=%d rate=%.1f vp_us=%lld dsp_us=%lld "
                    "monitor_us=%lld pause=%d idle=%d muted=%d\n",
                    d->ep_frame_div,
                    (unsigned long long)d->ep_frame_div *
                        NUM_SAMPLES_PER_FRAME,
                    (long long)elapsed_ms, d->frame_count,
                    d->frame_count * 1000.0 / elapsed_ms,
                    (long long)g_apu_perf_vp_us,
                    (long long)g_apu_perf_dsp_us,
                    (long long)g_apu_perf_monitor_us,
                    d->pause_requested, d->is_idle, g_audio_muted);
        }
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
        g_apu_perf_vp_us = 0;
        g_apu_perf_dsp_us = 0;
        g_apu_perf_monitor_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    int64_t perf_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    InterlockedExchange(&d->debug_worker_stage, 11);
    mcpx_apu_vp_frame(d, mixbins);
    int64_t perf_vp_end_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    InterlockedExchange(&d->debug_worker_stage, 12);
    mcpx_apu_dsp_frame(d, mixbins);
    int64_t perf_dsp_end_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    InterlockedExchange(&d->debug_worker_stage, 13);
    mcpx_apu_monitor_frame(d);
    int64_t perf_monitor_end_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    g_apu_perf_vp_us += perf_vp_end_us - perf_start_us;
    g_apu_perf_dsp_us += perf_dsp_end_us - perf_vp_end_us;
    g_apu_perf_monitor_us += perf_monitor_end_us - perf_dsp_end_us;

    d->ep_frame_div++;
    InterlockedIncrement(&d->debug_worker_heartbeat);
    InterlockedExchange(&d->debug_worker_stage, 14);

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        /* In this static runtime, APU producers set set_irq while holding the
         * device lock and translated execution consumes the resulting latched
         * request at function-entry safepoints. Unlike Xemu, there is no BQL
         * that lets this worker synchronously enter guest interrupt handling.
         * Do not manufacture a new edge on every inactive 5 ms poll: doing so
         * races retail initialization and can keep the sequencer trapped.
         * Service only genuine device events (method traps and notifications). */
        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }
        /* The software mixer/test tone can run with the sequencer disabled.
         * Hardware-only output must not manufacture samples while trapped. */
        throttle(d);

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices, selected DSP path, monitor, host output. */
            se_frame(d);
        } else if (!g_test_tone.active && !g_mixer_active_count) {
            /* Xemu waits for guest trap/idle service without advancing the
             * sequencer. Calling monitor_frame here used to insert silent
             * 32-sample slots into otherwise valid dialogue, advance XGSCNT,
             * and submit partial hardware frames. Keep the pending frame and
             * its sample clock intact; the timed wait releases the APU lock
             * so the retail ISR can retire idle voices and clear the trap.
             * FECTL/SECTL writes signal this condition. */
            qemu_cond_timedwait(&d->cond, &d->lock, 5);
        } else {
            /* Host-only sources (including movie audio) still need delivery
             * while the Xbox sequencer is off, halted or trapped. */
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
            if (g_trace_apu_perf > 0) {
                static int64_t last_idle_trace_us;
                int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
                if (now_us - last_idle_trace_us >= 1000000) {
                    fprintf(stderr,
                            "[APU-PERF-IDLE] ep=%d samples=%llu SECTL=%08X "
                            "FECTL=%08X pause=%d idle=%d\n",
                            d->ep_frame_div,
                            (unsigned long long)d->ep_frame_div *
                                NUM_SAMPLES_PER_FRAME,
                            d->regs[NV_PAPU_SECTL], d->regs[NV_PAPU_FECTL],
                            d->pause_requested, d->is_idle);
                    last_idle_trace_us = now_us;
                }
            }
        }
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    mcpx_apu_dsp_reset_all(d);

    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

#if defined(_WIN32)
    /* Xemu requests the minimum multimedia timer period on Windows. Without
     * it, 5.333 ms APU batches wake roughly 1-2 ms late and XGSCNT runs
     * about 20 percent slow, starving XMV's audio-mastered movie clock. */
    timeBeginPeriod(1);
#endif

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Initialize GP/EP state; execution depends on the DSP options */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Initialize monitor output (XAudio2 with waveOut fallback) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    if (getenv("MERCENARIES_TEST_APU_HIGH_PRIORITY") != NULL) {
        BOOL priority_set = SetThreadPriority(d->apu_thread.thread,
                                              THREAD_PRIORITY_HIGHEST);
        fprintf(stderr, "[APU-PRIORITY] highest=%d error=%lu\n",
                priority_set != FALSE,
                priority_set ? 0ul : (unsigned long)GetLastError());
    }
    mcpx_apu_wait_for_idle(d);
    /* A QEMU device is resumed by the VM run-state machinery. This
     * standalone runtime has no such callback, so leave the worker running
     * after its startup handshake or hardware DirectSound voices never
     * advance and their packet notifiers can never complete. */
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);
    mcpx_apu_dsp_finalize(d);

    free(d);
    g_state = NULL;
#if defined(_WIN32)
    timeEndPeriod(1);
#endif
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Xemu exposes GP/EP DSP X/Y/P memories as aligned 32-bit MMIO.  Route all
 * accesses through the active DSP backend so P writes invalidate translated
 * blocks and GP mix-buffer accesses observe the X:0x1400 alias. */
static uint32_t mcpx_apu_gp_read32(MCPXAPUState *d, uint32_t addr)
{
    DSPState *dsp = d->gp.dsp;
    if (!dsp) return 0;
    if (addr < 0x4000u)
        return mcpx_apu_dsp_read_memory(dsp, 'X', addr / 4u);
    if (addr >= NV_PAPU_GPMIXBUF && addr < NV_PAPU_GPMIXBUF + 0x1000u)
        return mcpx_apu_dsp_read_memory(
            dsp, 'X', GP_DSP_MIXBUF_BASE +
            (addr - NV_PAPU_GPMIXBUF) / 4u);
    if (addr >= NV_PAPU_GPYMEM && addr < NV_PAPU_GPYMEM + 0x2000u)
        return mcpx_apu_dsp_read_memory(
            dsp, 'Y', (addr - NV_PAPU_GPYMEM) / 4u);
    if (addr >= NV_PAPU_GPPMEM && addr < NV_PAPU_GPPMEM + 0x4000u)
        return mcpx_apu_dsp_read_memory(
            dsp, 'P', (addr - NV_PAPU_GPPMEM) / 4u);
    return d->gp.regs[addr];
}

static void mcpx_apu_gp_write32(MCPXAPUState *d, uint32_t addr,
                                uint32_t value)
{
    DSPState *dsp = d->gp.dsp;
    uint32_t old_value;
    if (!dsp) return;
    value &= 0x00FFFFFFu;
    if (addr < 0x4000u)
        mcpx_apu_dsp_write_memory(dsp, 'X', addr / 4u, value);
    else if (addr >= NV_PAPU_GPMIXBUF && addr < NV_PAPU_GPMIXBUF + 0x1000u)
        mcpx_apu_dsp_write_memory(
            dsp, 'X', GP_DSP_MIXBUF_BASE +
            (addr - NV_PAPU_GPMIXBUF) / 4u, value);
    else if (addr >= NV_PAPU_GPYMEM && addr < NV_PAPU_GPYMEM + 0x2000u)
        mcpx_apu_dsp_write_memory(
            dsp, 'Y', (addr - NV_PAPU_GPYMEM) / 4u, value);
    else if (addr >= NV_PAPU_GPPMEM && addr < NV_PAPU_GPPMEM + 0x4000u)
        mcpx_apu_dsp_write_memory(
            dsp, 'P', (addr - NV_PAPU_GPPMEM) / 4u, value);
    else {
        old_value = d->gp.regs[addr];
        if (addr == NV_PAPU_GPRST)
            mcpx_apu_dsp_reset_write(dsp, old_value, value);
        d->gp.regs[addr] = value;
    }
}

static uint32_t mcpx_apu_ep_read32(MCPXAPUState *d, uint32_t addr)
{
    DSPState *dsp = d->ep.dsp;
    if (!dsp) return 0;
    if (addr < 0x3000u)
        return mcpx_apu_dsp_read_memory(dsp, 'X', addr / 4u);
    if (addr >= NV_PAPU_EPYMEM && addr < NV_PAPU_EPYMEM + 0x400u)
        return mcpx_apu_dsp_read_memory(
            dsp, 'Y', (addr - NV_PAPU_EPYMEM) / 4u);
    if (addr >= NV_PAPU_EPPMEM && addr < NV_PAPU_EPPMEM + 0x4000u)
        return mcpx_apu_dsp_read_memory(
            dsp, 'P', (addr - NV_PAPU_EPPMEM) / 4u);
    return d->ep.regs[addr];
}

static void mcpx_apu_ep_write32(MCPXAPUState *d, uint32_t addr,
                                uint32_t value)
{
    DSPState *dsp = d->ep.dsp;
    uint32_t old_value;
    if (!dsp) return;
    value &= 0x00FFFFFFu;
    if (addr < 0x3000u)
        mcpx_apu_dsp_write_memory(dsp, 'X', addr / 4u, value);
    else if (addr >= NV_PAPU_EPYMEM && addr < NV_PAPU_EPYMEM + 0x400u)
        mcpx_apu_dsp_write_memory(
            dsp, 'Y', (addr - NV_PAPU_EPYMEM) / 4u, value);
    else if (addr >= NV_PAPU_EPPMEM && addr < NV_PAPU_EPPMEM + 0x4000u)
        mcpx_apu_dsp_write_memory(
            dsp, 'P', (addr - NV_PAPU_EPPMEM) / 4u, value);
    else {
        old_value = d->ep.regs[addr];
        if (addr == NV_PAPU_EPRST) {
            mcpx_apu_dsp_reset_write(dsp, old_value, value);
            d->ep_frame_div = 0;
        }
        d->ep.regs[addr] = value;
    }
}

static uint64_t mcpx_apu_dsp_region_read(MCPXAPUState *d, bool gp,
                                         uint32_t addr, unsigned int size)
{
    uint64_t value = 0;
    unsigned int i;
    for (i = 0; i < size; ++i) {
        uint32_t byte_addr = addr + i;
        uint32_t word = gp ? mcpx_apu_gp_read32(d, byte_addr & ~3u)
                           : mcpx_apu_ep_read32(d, byte_addr & ~3u);
        value |= (uint64_t)((word >> ((byte_addr & 3u) * 8u)) & 0xFFu)
                 << (i * 8u);
    }
    return value;
}

static void mcpx_apu_dsp_region_write(MCPXAPUState *d, bool gp,
                                      uint32_t addr, uint64_t value,
                                      unsigned int size)
{
    unsigned int i;
    for (i = 0; i < size; ++i) {
        uint32_t byte_addr = addr + i;
        uint32_t aligned = byte_addr & ~3u;
        uint32_t shift = (byte_addr & 3u) * 8u;
        uint32_t word = gp ? mcpx_apu_gp_read32(d, aligned)
                           : mcpx_apu_ep_read32(d, aligned);
        word = (word & ~(0xFFu << shift)) |
               (((uint32_t)(value >> (i * 8u)) & 0xFFu) << shift);
        if (gp) mcpx_apu_gp_write32(d, aligned, word);
        else mcpx_apu_ep_write32(d, aligned, word);
    }
}

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    }
    else if (addr >= 0x30000 && addr < 0x40000) {
        if (is_write)
            mcpx_apu_dsp_region_write(d, true, (uint32_t)addr - 0x30000u,
                                      val, size);
    } else if (addr >= 0x50000 && addr < 0x60000) {
        if (is_write)
            mcpx_apu_dsp_region_write(d, false, (uint32_t)addr - 0x50000u,
                                      val, size);
    }
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr >= 0x30000 && addr < 0x40000) {
        return mcpx_apu_dsp_region_read(d, true, (uint32_t)addr - 0x30000u,
                                        size);
    } else if (addr >= 0x50000 && addr < 0x60000) {
        return mcpx_apu_dsp_region_read(d, false, (uint32_t)addr - 0x50000u,
                                        size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU test tone - monitor sine generator
 *
 * Writes a 440 Hz sine wave to monitor frame_buf, bypassing VP voices.
 * The selected host backend outputs the tone.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Enable SECTL and resume the APU thread for test-tone output */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers into monitor output
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    g_mixer_voices[slot].active = 0;
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || v->pcm_bytes == 0) return;
    v->looping = looping;
    v->play_offset = 0;
    v->active = 1;
    InterlockedIncrement((volatile LONG *)&g_mixer_active_count);

    /* Wake up APU thread if it was paused */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * play_offset is stored as a 16.16 fixed-point source frame position. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint32_t inc = (uint32_t)(((uint64_t)voice->sample_rate << 16) / 48000);
        uint32_t pos = voice->play_offset; /* 16.16 fixed-point */
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos = 0;
                    src_frame = 0;
                } else {
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = 0;
            } else if (voice->active) {
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
}

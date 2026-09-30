/**
 * D3D8→D3D11 Compatibility Device Implementation
 *
 * Implements the Xbox D3D8 IDirect3DDevice8 interface using D3D11.
 * The game's translated RenderWare code calls D3D8 methods through
 * COM vtables; this layer translates those calls to D3D11 equivalents.
 *
 * Architecture:
 * - D3D11 device and swap chain created during initialization
 * - Render state tracking: D3D8 states mapped to D3D11 state objects
 * - Texture/buffer management: D3D8 resource handles wrap D3D11 resources
 * - Fixed-function pipeline: emulated via D3D11 shaders (the Xbox D3D8
 *   pipeline is configurable but not fully programmable)
 *
 * Build: Requires Windows SDK with d3d11.h and dxgi.h
 */

#include "d3d8_internal.h"
#include <dxgi1_6.h>
#include "kernel/preview_log.h"
#include "d3d8_scaled_lines.h"
#include "d3d8_triangle_depth.h"
#include "d3d8_compiler.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Device diagnostics are launch options, not live renderer state. Only literal
 * keys are used here; retain absent values as well as present/empty strings so
 * a disabled capture does not rescan the CRT environment on every draw. Live
 * screenshot requests still use g_debug_armed/forced_capture_path below. Like
 * the D3D11 immediate context, this cache is owned by the rendering thread. */
static const char *d3d8_cached_getenv(const char *name)
{
    typedef struct D3D8EnvCacheEntry {
        const char *name;
        const char *value;
    } D3D8EnvCacheEntry;
    static D3D8EnvCacheEntry cache[64];
    uintptr_t key = (uintptr_t)name;
    size_t index = (size_t)((key >> 4u) ^ (key >> 12u)) & 63u;
    size_t probe;

    for (probe = 0u; probe < 64u; ++probe) {
        D3D8EnvCacheEntry *entry = &cache[(index + probe) & 63u];
        if (entry->name == name)
            return entry->value;
        if (entry->name == NULL) {
            entry->name = name;
            entry->value = getenv(name);
            return entry->value;
        }
    }
    return getenv(name);
}

/* ================================================================
 * Internal device state
 * ================================================================ */

/* Maximum tracked render states, texture stages, and transforms */
#define MAX_RENDER_STATES    256
#define MAX_TEXTURE_STAGES   4
#define MAX_TSS_STATES       32
#define MAX_TRANSFORMS       512
#define MAX_LIGHTS           8

typedef struct D3D8DeviceState {
    /* D3D11 objects */
    ID3D11Device            *d3d11_device;
    ID3D11DeviceContext     *d3d11_context;
    IDXGISwapChain          *swap_chain;
    BOOL                    flip_model;
    UINT                    swap_chain_flags;

    /* Default render targets */
    ID3D11RenderTargetView  *default_rtv;
    ID3D11DepthStencilView  *default_dsv;
    ID3D11Texture2D         *default_depth;
    ID3D11Texture2D         *cpu_frame_texture;
    ID3D11ShaderResourceView *cpu_frame_srv;
    ID3D11RenderTargetView  *current_rtv;
    ID3D11DepthStencilView  *current_dsv;
    UINT                    current_target_width;
    UINT                    current_target_height;
    UINT                    current_target_physical_width;
    UINT                    current_target_physical_height;
    ID3D11VertexShader      *resolve_vs;
    ID3D11PixelShader       *resolve_ps;
    ID3D11SamplerState      *resolve_sampler;
    ID3D11PixelShader       *packed_color_depth_ps;
    ID3D11DepthStencilState *packed_color_depth_state;
    BOOL                    packed_color_depth_checked;
    ID3D11PixelShader       *packed_color_depth_fallback_ps;
    ID3D11DepthStencilState *packed_color_depth_fallback_state;
    ID3D11PixelShader       *packed_color_stencil_bit_ps[8];
    ID3D11DepthStencilState *packed_color_stencil_bit_state[8];
    BOOL                    packed_color_depth_fallback_checked;
    ID3D11PixelShader       *depth_alias_ps;
    ID3D11PixelShader       *depth_alias_uniform_ps;
    ID3D11DepthStencilState *depth_alias_uniform_state;
    ID3D11PixelShader       *depth_alias_stencil_bit_ps[8];
    ID3D11DepthStencilState *depth_alias_stencil_bit_state[8];
    ID3D11Buffer            *depth_alias_cb;
    ID3D11Texture2D         *depth_alias_source_texture;
    ID3D11ShaderResourceView *depth_alias_source_depth_srv;
    ID3D11ShaderResourceView *depth_alias_source_stencil_srv;
    UINT                    depth_alias_source_width;
    UINT                    depth_alias_source_height;
    ID3D11BlendState        *host_overlay_blend;
    ID3D11PixelShader       *pvideo_ps;
    ID3D11Buffer            *pvideo_cb;
    ID3D11Texture2D         *pvideo_texture;
    ID3D11ShaderResourceView *pvideo_srv;
    UINT                    pvideo_width;
    UINT                    pvideo_height;
    ID3D11PixelShader       *masked_clear_ps;
    ID3D11Buffer            *masked_clear_cb;
    ID3D11BlendState        *masked_clear_blend[16];

    /* Window */
    HWND                    hwnd;
    UINT                    width;
    UINT                    height;
    D3DFORMAT               backbuffer_format;

    /* State tracking */
    DWORD                   render_states[MAX_RENDER_STATES];
    DWORD                   tss[MAX_TEXTURE_STAGES][MAX_TSS_STATES];
    D3DMATRIX               transforms[MAX_TRANSFORMS];
    D3DVIEWPORT8            viewport;
    D3DMATERIAL8            material;
    D3DLIGHT8               lights[MAX_LIGHTS];
    BOOL                    light_enable[MAX_LIGHTS];
    D3DGAMMARAMP            gamma_ramp;
    BOOL                    gamma_ramp_valid;

    /* Current shader/FVF */
    DWORD                   vertex_shader;
    DWORD                   pixel_shader;

    /* Scene state */
    BOOL                    in_scene;

    /* Reference count */
    LONG                    ref_count;
} D3D8DeviceState;

/* Global device instance (Xbox has a single D3D device) */
static D3D8DeviceState g_device_state;

#include "d3d8_frame_timing.h"

static BOOL g_pending_scanout_present;
static uint64_t g_frame_submissions, g_frame_interval_serial;
static uint32_t g_frame_intervals_us[512], g_frame_interval_count, g_frame_interval_next;
static LARGE_INTEGER g_frame_frequency, g_frame_previous;
static uint32_t g_debug_submission_source;
void d3d8_DebugSetSubmissionSource(uint32_t source) { g_debug_submission_source = source; }
uint32_t d3d8_DebugGetSubmissionSource(void) { return g_debug_submission_source; }
static void (*g_debug_submission_callback)(uint64_t, int64_t, int64_t);
void d3d8_DebugSetSubmissionCallback(void (*callback)(uint64_t, int64_t, int64_t))
{
    g_debug_submission_callback = callback;
}
static void record_frame_submission(void)
{
    LARGE_INTEGER now;
    ++g_frame_submissions;
    if (!g_frame_frequency.QuadPart) QueryPerformanceFrequency(&g_frame_frequency);
    if (g_frame_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&now)) return;
    if (g_frame_previous.QuadPart && now.QuadPart >= g_frame_previous.QuadPart) {
        double us = (double)(now.QuadPart - g_frame_previous.QuadPart) *
            1000000.0 / (double)g_frame_frequency.QuadPart;
        g_frame_intervals_us[g_frame_interval_next] =
            us > 4294967295.0 ? UINT32_MAX : (uint32_t)us;
        ++g_frame_interval_serial;
        g_frame_interval_next = (g_frame_interval_next + 1u) & 511u;
        if (g_frame_interval_count < 512u) ++g_frame_interval_count;
    }
    g_frame_previous = now;
    if (g_debug_submission_callback)
        g_debug_submission_callback(g_frame_submissions, now.QuadPart, g_frame_frequency.QuadPart);
}
static int compare_frame_intervals(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
void d3d8_GetFrameTimingSnapshot(D3D8FrameTimingSnapshot *out)
{
    uint32_t values[512], count = g_frame_interval_count;
    uint64_t sum = 0;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->submitted_frames = g_frame_submissions; out->samples = count;
    if (!count) return;
    memcpy(values, g_frame_intervals_us, count * sizeof(values[0]));
    qsort(values, count, sizeof(values[0]), compare_frame_intervals);
    for (uint32_t i = 0; i < count; ++i) sum += values[i];
    out->mean_ms = (double)sum / (double)count / 1000.0;
    out->p50_ms = values[(count * 50u + 99u) / 100u - 1u] / 1000.0;
    out->p95_ms = values[(count * 95u + 99u) / 100u - 1u] / 1000.0;
    out->p99_ms = values[(count * 99u + 99u) / 100u - 1u] / 1000.0;
    out->max_ms = values[count - 1u] / 1000.0;
}

uint32_t d3d8_GetFrameIntervalHistory(uint64_t after_serial, uint32_t *out,
                                    uint32_t capacity, uint64_t *serial)
{
    uint64_t available = after_serial <= g_frame_interval_serial ?
        g_frame_interval_serial - after_serial : 0;
    uint32_t count, first;
    if (serial) *serial = g_frame_interval_serial;
    if (!out || !capacity) return 0;
    if (available > g_frame_interval_count) available = g_frame_interval_count;
    if (available > capacity) available = capacity;
    count = (uint32_t)available;
    first = (g_frame_interval_next + 512u - count) & 511u;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = g_frame_intervals_us[(first + i) & 511u];
    return count;
}

static void (*g_host_overlay_callback)(void);
void d3d8_SetHostOverlayCallback(void (*callback)(void)){g_host_overlay_callback=callback;}
/* Preserve Present's result; querying removal reason is failure-only. */
static HRESULT preview_present(IDXGISwapChain *chain, UINT sync, UINT flags)
{
    if (g_device_state.flip_model && sync == 0u &&
        (g_device_state.swap_chain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
        BOOL fullscreen = FALSE;
        if (SUCCEEDED(IDXGISwapChain_GetFullscreenState(chain, &fullscreen, NULL)) && !fullscreen)
            flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    if(g_host_overlay_callback && !(flags & DXGI_PRESENT_TEST))g_host_overlay_callback();
    HRESULT result = IDXGISwapChain_Present(chain, sync, flags);
    /* Flip presentation unbinds back buffer zero. Restore the tracked guest
     * output target as well: guest rendering can still be on an offscreen RT. */
    if (g_device_state.flip_model && SUCCEEDED(result) && g_device_state.current_rtv)
        ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1u,
            &g_device_state.current_rtv, g_device_state.current_dsv);
    /* Direct FLIP_STALL presentation consumes any deferred host submission too.
     * Leaving the flag armed would later Present a discarded/stale backbuffer. */
    g_pending_scanout_present = FALSE;
    if (result == S_OK) record_frame_submission();
    if (FAILED(result))
        xbox_preview_log_event("graphics-error", "Present HRESULT=%08lX removal_reason=%08lX",
            result, g_device_state.d3d11_device ?
                ID3D11Device_GetDeviceRemovedReason(g_device_state.d3d11_device) : S_OK);
    return result;
}


static IDirect3DDevice8 g_device;
static BOOL g_device_initialized = FALSE;
static UINT g_presentation_aspect_width = 4u;
static UINT g_presentation_aspect_height = 3u;
static UINT g_frame_cap_fps = 30u;
static LONGLONG g_next_frame_slot;

void d3d8_SetPresentationAspect(UINT width, UINT height)
{
    if (width != 0u && height != 0u) {
        g_presentation_aspect_width = width;
        g_presentation_aspect_height = height;
    }
}

void d3d8_SetFrameCap(UINT fps)
{
    if (fps != 0u && fps != 30u && fps != 60u && fps != 90u && fps != 120u)
        fps = 30u;
    if (g_frame_cap_fps != fps) {
        g_frame_cap_fps = fps;
        g_next_frame_slot = 0; /* Do not inherit a deadline from the old cap. */
    }
}

/* Current resource bindings */
static IDirect3DVertexBuffer8 *g_cur_vb = NULL;
static UINT                    g_cur_vb_stride = 0;
static IDirect3DIndexBuffer8  *g_cur_ib = NULL;
static UINT                    g_cur_ib_base_vertex = 0;
static IDirect3DBaseTexture8  *g_cur_textures[4] = { NULL };

/* Forward declarations */
static const IDirect3DDevice8Vtbl g_device_vtbl;
static void up_ring_shutdown(void);

/* ================================================================
 * Public frame pump (called from recompiled game code)
 * ================================================================ */
static void d3d8_pump_host_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr, "[HOST-WINDOW] WM_QUIT site=frame-pump code=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static BOOL g_debug_capture_at_flip;
static BOOL g_debug_force_capture;
static unsigned int g_present_frame_count;
static char g_debug_armed_capture_path[MAX_PATH];
static char g_debug_forced_capture_path[MAX_PATH];
static void (*g_debug_present_callback)(void);
static void d3d8_debug_backbuffer_stats(void);
static void d3d8_debug_capture_display_refresh(ULONGLONG now_ms);
static void d3d8_debug_detect_present_flash(ULONGLONG now_ms);

void d3d8_DebugSetPresentCallback(void (*callback)(void))
{
    g_debug_present_callback = callback;
}

void d3d8_DebugArmFlipCapture(const char *path)
{
    if (!path || !path[0] || g_debug_armed_capture_path[0])
        return;
    strncpy_s(g_debug_armed_capture_path,
              sizeof(g_debug_armed_capture_path), path, _TRUNCATE);
    fprintf(stderr, "D3D8: armed event-relative flip capture: %s\n",
            g_debug_armed_capture_path);
}

/* Last pacing wait overshoot; uses the existing QPC samples, never another wait. */
static LONGLONG g_frame_pacing_late_ticks;

void d3d8_WaitForGuestFrameSlot(void)
{
    static LARGE_INTEGER frequency;
    static int initialized;
    LARGE_INTEGER now;
    LONGLONG interval;

    g_frame_pacing_late_ticks = 0;
    if (g_frame_cap_fps == 0u ||
        d3d8_cached_getenv("MERCENARIES_DISABLE_FLIP_PACING") != NULL) {
        g_next_frame_slot = 0;
        return;
    }
    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        initialized = 1;
    }
    if (frequency.QuadPart <= 0)
        return;

    QueryPerformanceCounter(&now);
    /* Pace each completed guest frame, whether it reaches scanout directly at
     * FLIP_STALL or after a deferred AA resolve.  This controls distinct
     * simulated/rendered frames rather than host repeats. The independent Xbox
     * vblank service stays at 60 Hz; it does not define this user-selected cap. */
    interval = frequency.QuadPart / g_frame_cap_fps;
    if (interval < 1) interval = 1;
    if (g_next_frame_slot == 0) {
        g_next_frame_slot = now.QuadPart + interval;
    } else if (now.QuadPart >= g_next_frame_slot) {
        /* A slow frame has already missed its slot. Rebase the next deadline
         * and return immediately: sleeping another complete interval here
         * turns a GPU-limited 16 fps workload into roughly 12 fps and causes
         * a recurring hitch. This does not uncap fast frames; their deadline
         * remains in the future and they still wait below. */
        g_next_frame_slot = now.QuadPart + interval;
        return;
    }

    while (now.QuadPart < g_next_frame_slot) {
        const LONGLONG remaining = g_next_frame_slot - now.QuadPart;
        const DWORD remaining_ms =
            (DWORD)((remaining * 1000) / frequency.QuadPart);
        if (remaining_ms > 1)
            Sleep(remaining_ms - 1);
        else
            SwitchToThread();
        QueryPerformanceCounter(&now);
    }
    g_frame_pacing_late_ticks = now.QuadPart - g_next_frame_slot;
    g_next_frame_slot += interval;
}

void d3d8_PresentFrame(void)
{
    static int trace_timing = -1;
    static LONGLONG trace_period_ticks, previous_start_ticks, previous_end_ticks;
    static double trace_ticks_per_ms;
    static UINT trace_calls;
    static uint64_t trace_previous_submissions, trace_other_submissions;
    static double trace_max_duration_ms, trace_max_gap_ms;
    static double trace_max_prepare_ms, trace_max_wait_ms, trace_max_present_ms;
    ULONGLONG start_ms;
    LARGE_INTEGER trace_start = {0}, trace_prepared = {0};
    LARGE_INTEGER trace_waited = {0}, trace_end = {0};
    double trace_gap_ms = 0.0, trace_outside_ms = 0.0;
    uint64_t trace_submissions_before = 0u, trace_submissions_between = 0u;

    if (trace_timing < 0) {
        LARGE_INTEGER frequency;
        trace_timing = d3d8_cached_getenv("MERCENARIES_TRACE_PRESENT_TIMING") != NULL;
        if (trace_timing) {
            trace_timing = QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0;
            if (trace_timing)
                trace_ticks_per_ms = (double)frequency.QuadPart / 1000.0;
        }
    }
    start_ms = GetTickCount64();
    if (trace_timing) {
        /* TickCount quantization can turn a ~33 ms gap into 47 ms. Use QPC
         * for attribution; keep the existing millisecond capture clock. */
        QueryPerformanceCounter(&trace_start);
        trace_submissions_before = g_frame_submissions;
        trace_submissions_between = trace_submissions_before - trace_previous_submissions;
        if (previous_start_ticks != 0)
            trace_gap_ms = (trace_start.QuadPart - previous_start_ticks) / trace_ticks_per_ms;
        if (previous_end_ticks != 0)
            trace_outside_ms = (trace_start.QuadPart - previous_end_ticks) / trace_ticks_per_ms;
        if (trace_gap_ms > trace_max_gap_ms)
            trace_max_gap_ms = trace_gap_ms;
        previous_start_ticks = trace_start.QuadPart;
    }
    d3d8_pump_host_messages();
    ++g_present_frame_count;
    if (g_debug_present_callback)
        g_debug_present_callback();

    if (g_debug_force_capture || g_debug_armed_capture_path[0] ||
        d3d8_cached_getenv("MERCENARIES_CAPTURE_FLIP_PATH") != NULL) {
        g_debug_capture_at_flip = TRUE;
        d3d8_debug_backbuffer_stats();
        g_debug_capture_at_flip = FALSE;
    }

    /* Sample the actual new frame before Present. Capturing from the periodic
     * vblank service when it has no new scanout observes a discarded DXGI
     * back buffer rather than the image currently visible on screen. */
    d3d8_debug_capture_display_refresh(start_ms);

    /* Direct FLIP_STALL completion uses the shared guest-frame clock.  A
     * deferred AA resolve uses the same clock from the scanout service.  Keep
     * DXGI itself immediate: SyncInterval=1 adds a second compositor throttle
     * and made 30 fps XMV frames freeze and then catch up to their audio. */
    if (trace_timing)
        QueryPerformanceCounter(&trace_prepared);
    d3d8_WaitForGuestFrameSlot();
    if (trace_timing)
        QueryPerformanceCounter(&trace_waited);
    if (g_device_state.swap_chain)
        preview_present(g_device_state.swap_chain, 0, 0);
    if (trace_timing) {
        double prepare_ms, wait_ms, present_ms, duration_ms;
        uint32_t submitted_interval_us;
        QueryPerformanceCounter(&trace_end);
        prepare_ms = (trace_prepared.QuadPart - trace_start.QuadPart) / trace_ticks_per_ms;
        wait_ms = (trace_waited.QuadPart - trace_prepared.QuadPart) / trace_ticks_per_ms;
        present_ms = (trace_end.QuadPart - trace_waited.QuadPart) / trace_ticks_per_ms;
        duration_ms = (trace_end.QuadPart - trace_start.QuadPart) / trace_ticks_per_ms;
        previous_end_ticks = trace_end.QuadPart;
        submitted_interval_us = g_frame_submissions > trace_submissions_before && g_frame_interval_count ?
            g_frame_intervals_us[(g_frame_interval_next + 511u) & 511u] : 0u;
        trace_other_submissions += trace_submissions_between;
        if (duration_ms > trace_max_duration_ms) trace_max_duration_ms = duration_ms;
        if (prepare_ms > trace_max_prepare_ms) trace_max_prepare_ms = prepare_ms;
        if (wait_ms > trace_max_wait_ms) trace_max_wait_ms = wait_ms;
        if (present_ms > trace_max_present_ms) trace_max_present_ms = present_ms;
        /* Deferred scanout may submit between direct calls. Only actual
         * submission intervals or expensive work identify a slow event;
         * a long direct-call gap alone is not a missed frame. Include minor
         * hitches (>18 ms at 60 Hz), and distinguish scheduler overshoot from
         * intentional limiter waiting without changing the pacing policy. */
        if (prepare_ms + present_ms >= 8.0 ||
            submitted_interval_us > (g_frame_cap_fps ? 1080000u / g_frame_cap_fps : 18000u))
            xbox_preview_log_sample("present-slow",
                "frame=%u direct_gap_ms=%.3f outside_ms=%.3f prepare_ms=%.3f "
                "wait_ms=%.3f present_ms=%.3f total_ms=%.3f other_submissions=%llu "
                "submitted_interval_us=%u pacing_late_ms=%.3f",
                g_present_frame_count, trace_gap_ms, trace_outside_ms,
                prepare_ms, wait_ms, present_ms, duration_ms,
                (unsigned long long)trace_submissions_between,
                submitted_interval_us,
                g_frame_pacing_late_ticks / trace_ticks_per_ms);
        trace_previous_submissions = g_frame_submissions;
        ++trace_calls;
        if (trace_period_ticks == 0)
            trace_period_ticks = trace_start.QuadPart;
        if ((trace_end.QuadPart - trace_period_ticks) / trace_ticks_per_ms >= 1000.0) {
            fprintf(stderr,
                    "[D3D-PRESENT-TIMING] calls=%u max_duration_ms=%.3f "
                    "max_start_gap_ms=%.3f max_prepare_ms=%.3f "
                    "max_wait_ms=%.3f max_present_ms=%.3f alternate_submissions=%llu\n",
                    trace_calls, trace_max_duration_ms, trace_max_gap_ms,
                    trace_max_prepare_ms, trace_max_wait_ms, trace_max_present_ms,
                    (unsigned long long)trace_other_submissions);
            fflush(stderr);
            trace_period_ticks = trace_end.QuadPart;
            trace_calls = 0u;
            trace_other_submissions = 0u;
            trace_max_duration_ms = trace_max_gap_ms = 0.0;
            trace_max_prepare_ms = trace_max_wait_ms = trace_max_present_ms = 0.0;
        }
    }
}

void d3d8_DebugCaptureFrameNow(void)
{
    fprintf(stderr, "D3D8: exact draw capture at present %u\n",
            g_present_frame_count + 1u);
    g_debug_force_capture = TRUE;
    d3d8_PresentFrame();
    g_debug_force_capture = FALSE;
}

void d3d8_DebugCaptureFrameToPath(const char *path)
{
    if (!path || !path[0]) {
        d3d8_DebugCaptureFrameNow();
        return;
    }
    strncpy_s(g_debug_forced_capture_path,
              sizeof(g_debug_forced_capture_path), path, _TRUNCATE);
    fprintf(stderr, "D3D8: exact non-presenting capture at present %u\n",
            g_present_frame_count + 1u);
    g_debug_force_capture = TRUE;
    d3d8_debug_backbuffer_stats();
    g_debug_force_capture = FALSE;
    g_debug_forced_capture_path[0] = '\0';
}

static int g_debug_display_capture_initialized;
static char g_debug_display_capture_prefix[1024];
static unsigned g_debug_display_capture_interval_ms;
static unsigned g_debug_display_capture_limit;
static unsigned g_debug_display_capture_count;
static ULONGLONG g_debug_display_capture_started_ms;
static ULONGLONG g_debug_display_capture_next_ms;
static char g_debug_queued_capture_path[MAX_PATH];

void d3d8_DebugQueueFrameCapture(const char *path)
{
    if (!path || !path[0] || g_debug_queued_capture_path[0]) return;
    strncpy_s(g_debug_queued_capture_path, sizeof(g_debug_queued_capture_path),
              path, _TRUNCATE);
}

static void d3d8_debug_capture_queued_frame(void)
{
    /* Input polling can occur after Present, when a DISCARD swap chain's
     * back buffer no longer contains the displayed frame. Consume requests
     * only at the next completed scanout, before either presentation path. */
    if (!g_debug_queued_capture_path[0]) return;
    d3d8_DebugCaptureFrameToPath(g_debug_queued_capture_path);
    g_debug_queued_capture_path[0] = '\0';
}

void d3d8_DebugStartDisplayCapture(const char *prefix,
                                   UINT interval_ms, UINT limit)
{
    if (!prefix || !prefix[0] || limit == 0u)
        return;
    if (interval_ms > 60000u)
        interval_ms = 60000u;
    if (limit > 256u)
        limit = 256u;
    strncpy_s(g_debug_display_capture_prefix,
              sizeof(g_debug_display_capture_prefix), prefix, _TRUNCATE);
    g_debug_display_capture_initialized = 1;
    g_debug_display_capture_interval_ms = interval_ms;
    g_debug_display_capture_limit = limit;
    g_debug_display_capture_count = 0u;
    g_debug_display_capture_started_ms = GetTickCount64();
    g_debug_display_capture_next_ms = g_debug_display_capture_started_ms;
}

static void d3d8_debug_capture_display_refresh(ULONGLONG now_ms)
{
    char path[1024];
    int length;

    d3d8_debug_capture_queued_frame();
    d3d8_debug_detect_present_flash(now_ms);

    if (!g_debug_display_capture_initialized) {
        const char *prefix;
        const char *text;
        g_debug_display_capture_initialized = 1;
        prefix = getenv("MERCENARIES_CAPTURE_DISPLAY_PREFIX");
        if (!prefix || !*prefix) return;
        strncpy_s(g_debug_display_capture_prefix,
                  sizeof(g_debug_display_capture_prefix), prefix, _TRUNCATE);
        text = getenv("MERCENARIES_CAPTURE_DISPLAY_INTERVAL_MS");
        g_debug_display_capture_interval_ms =
            text ? (unsigned)strtoul(text, NULL, 10) : 2000u;
        text = getenv("MERCENARIES_CAPTURE_DISPLAY_COUNT");
        g_debug_display_capture_limit =
            text ? (unsigned)strtoul(text, NULL, 10) : 32u;
        if (g_debug_display_capture_interval_ms < 250u)
            g_debug_display_capture_interval_ms = 250u;
        if (g_debug_display_capture_interval_ms > 60000u)
            g_debug_display_capture_interval_ms = 60000u;
        if (g_debug_display_capture_limit > 256u)
            g_debug_display_capture_limit = 256u;
        g_debug_display_capture_started_ms = now_ms;
        g_debug_display_capture_next_ms =
            now_ms + g_debug_display_capture_interval_ms;
    }
    if (!g_debug_display_capture_prefix[0] ||
        g_debug_display_capture_count >= g_debug_display_capture_limit ||
        now_ms < g_debug_display_capture_next_ms)
        return;
    ++g_debug_display_capture_count;
    g_debug_display_capture_next_ms =
        now_ms + g_debug_display_capture_interval_ms;
    length = snprintf(path, sizeof(path), "%s-%03u-%llu.bmp",
                      g_debug_display_capture_prefix,
                      g_debug_display_capture_count,
                      (unsigned long long)(now_ms -
                                           g_debug_display_capture_started_ms));
    if (length > 0 && (size_t)length < sizeof(path))
        d3d8_DebugCaptureFrameToPath(path);
}

BOOL d3d8_ServiceDisplayRefresh(BOOL present_pending_scanout)
{
    static LARGE_INTEGER refresh_frequency;
    static LONGLONG next_refresh;
    LARGE_INTEGER now;
    LONGLONG interval;
    ULONGLONG now_ms;

    d3d8_pump_host_messages();
    if (present_pending_scanout)
        g_pending_scanout_present = TRUE;
    if (refresh_frequency.QuadPart == 0)
        QueryPerformanceFrequency(&refresh_frequency);
    if (refresh_frequency.QuadPart <= 0)
        return FALSE;
    QueryPerformanceCounter(&now);
    interval = refresh_frequency.QuadPart / 60;
    if (interval <= 0)
        interval = 1;
    if (next_refresh == 0)
        next_refresh = now.QuadPart;
    if (now.QuadPart < next_refresh)
        return FALSE;
    if (now.QuadPart - next_refresh >= interval)
        next_refresh = now.QuadPart + interval;
    else
        next_refresh += interval;
    if (!g_device_state.swap_chain)
        return FALSE;
    /* A host vblank tick is not itself a new guest scanout. Re-presenting an
     * unchanged DXGI_SWAP_EFFECT_DISCARD chain rotates to undefined/stale
     * buffers; during XMV playback that replaced every decoded movie frame
     * with the last front-end frame. Preserve the 60 Hz Xbox vblank clock,
     * but submit to DXGI only after the deferred NV2A resolve copied a new
     * scanout into the host back buffer. Direct FLIP_STALL frames already use
     * d3d8_PresentFrame(). */
    if (g_pending_scanout_present) {
        d3d8_DebugSetSubmissionSource(5u);
        now_ms = GetTickCount64();
        d3d8_debug_capture_display_refresh(now_ms);
        preview_present(g_device_state.swap_chain, 0, 0);
        g_pending_scanout_present = FALSE;
    }
    return TRUE;
}

void d3d8_UploadFrameX8R8G8B8(const void *pixels, UINT source_pitch,
                               UINT width, UINT height)
{
    static BYTE *converted;
    static size_t converted_capacity;
    static UINT upload_samples;
    D3D11_TEXTURE2D_DESC desc;
    size_t required;
    UINT y;
    UINT nonblack_pixels = 0;
    HRESULT hr;

    if (!pixels || !g_device_state.swap_chain ||
        !g_device_state.d3d11_device || !g_device_state.d3d11_context ||
        !width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
        height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || source_pitch < width * 4u) {
        return;
    }

    required = (size_t)width * height * 4u;
    if (required > converted_capacity) {
        BYTE *new_buffer = (BYTE *)realloc(converted, required);
        if (!new_buffer) return;
        converted = new_buffer;
        converted_capacity = required;
    }

    for (y = 0; y < height; ++y) {
        const BYTE *src = (const BYTE *)pixels + (size_t)y * source_pitch;
        BYTE *dst = converted + (size_t)y * width * 4u;
        UINT x;
        for (x = 0; x < width; ++x) {
            if ((src[x * 4u] | src[x * 4u + 1u] |
                 src[x * 4u + 2u]) != 0u)
                ++nonblack_pixels;
            dst[x * 4u + 0u] = src[x * 4u + 2u];
            dst[x * 4u + 1u] = src[x * 4u + 1u];
            dst[x * 4u + 2u] = src[x * 4u + 0u];
            dst[x * 4u + 3u] = 0xFFu;
        }
    }

    if (nonblack_pixels == 0u) return;

    /* UpdateSubresource with a null box consumes the destination's full
     * extent. Upload into a source-sized texture, never the larger swap-chain
     * buffer; presentation then handles scaling and aspect ratio normally. */
    memset(&desc, 0, sizeof(desc));
    if (g_device_state.cpu_frame_texture) {
        ID3D11Texture2D_GetDesc(g_device_state.cpu_frame_texture, &desc);
        if (desc.Width != width || desc.Height != height) {
            ID3D11ShaderResourceView_Release(g_device_state.cpu_frame_srv);
            ID3D11Texture2D_Release(g_device_state.cpu_frame_texture);
            g_device_state.cpu_frame_srv = NULL;
            g_device_state.cpu_frame_texture = NULL;
        }
    }
    if (!g_device_state.cpu_frame_texture) {
        memset(&desc, 0, sizeof(desc));
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
            &desc, NULL, &g_device_state.cpu_frame_texture);
        if (FAILED(hr)) return;
        hr = ID3D11Device_CreateShaderResourceView(g_device_state.d3d11_device,
            (ID3D11Resource *)g_device_state.cpu_frame_texture, NULL,
            &g_device_state.cpu_frame_srv);
        if (FAILED(hr)) {
            ID3D11Texture2D_Release(g_device_state.cpu_frame_texture);
            g_device_state.cpu_frame_texture = NULL;
            return;
        }
    }
    ID3D11DeviceContext_UpdateSubresource(g_device_state.d3d11_context,
        (ID3D11Resource *)g_device_state.cpu_frame_texture, 0, NULL,
        converted, width * 4u, 0);
    d3d8_CopyTextureToBackbuffer(g_device_state.cpu_frame_texture,
        g_device_state.cpu_frame_srv, width, height);

    if (upload_samples < 5u) {
        fprintf(stderr,
                "D3D8: Uploaded Xbox framebuffer %ux%u pitch=%u pixels=%u\n",
                width, height, source_pitch, nonblack_pixels);
        ++upload_samples;
    }
}

static void d3d8_write_backbuffer_bmp(const char *path,
                                      const D3D11_TEXTURE2D_DESC *desc,
                                      const D3D11_MAPPED_SUBRESOURCE *mapped,
                                      BOOL alpha_view)
{
    FILE *fp;
    uint8_t header[54] = {0};
    uint8_t *row;
    const uint32_t row_bytes = (desc->Width * 3u + 3u) & ~3u;
    const uint32_t image_bytes = row_bytes * desc->Height;
    const uint32_t file_bytes = 54u + image_bytes;
    const uint32_t pixel_offset = 54u;
    const uint32_t dib_size = 40u;
    const uint16_t planes = 1u, bits_per_pixel = 24u;
    uint32_t x, y;

    if (!path || !*path || !mapped->pData)
        return;
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "D3D8: could not write startup capture: %s\n", path);
        return;
    }
    header[0] = 'B'; header[1] = 'M';
    memcpy(header + 2, &file_bytes, sizeof(file_bytes));
    memcpy(header + 10, &pixel_offset, sizeof(pixel_offset));
    memcpy(header + 14, &dib_size, sizeof(dib_size));
    memcpy(header + 18, &desc->Width, sizeof(desc->Width));
    memcpy(header + 22, &desc->Height, sizeof(desc->Height));
    memcpy(header + 26, &planes, sizeof(planes));
    memcpy(header + 28, &bits_per_pixel, sizeof(bits_per_pixel));
    memcpy(header + 34, &image_bytes, sizeof(image_bytes));
    fwrite(header, 1, sizeof(header), fp);

    row = (uint8_t *)calloc(1, row_bytes);
    if (row) {
        for (y = 0; y < desc->Height; ++y) {
            const uint8_t *src = (const uint8_t *)mapped->pData +
                (size_t)(desc->Height - 1u - y) * mapped->RowPitch;
            memset(row, 0, row_bytes);
            for (x = 0; x < desc->Width; ++x) {
                /* Swap-chain format is R8G8B8A8; BMP stores B,G,R. */
                row[x * 3u + 0u] = src[x * 4u + (alpha_view ? 3u : 2u)];
                row[x * 3u + 1u] = src[x * 4u + (alpha_view ? 3u : 1u)];
                row[x * 3u + 2u] = src[x * 4u + (alpha_view ? 3u : 0u)];
            }
            fwrite(row, 1, row_bytes, fp);
        }
        free(row);
    }
    fclose(fp);
    fprintf(stderr, "D3D8: wrote startup GPU capture: %s\n", path);
}

#define PRESENT_FLASH_SAMPLE_WIDTH 64u
#define PRESENT_FLASH_SAMPLE_HEIGHT 36u
#define PRESENT_FLASH_SAMPLE_COUNT \
    (PRESENT_FLASH_SAMPLE_WIDTH * PRESENT_FLASH_SAMPLE_HEIGHT)

/* Opt-in presentation diagnostic for the large, transient colour washes seen
 * during Mercenaries gameplay. It samples the completed host back buffer
 * rather than guessing which guest effect produced the image. A flash is
 * reported only when a signed brightness change covers most of the screen,
 * which rejects ordinary camera motion while catching the measured 98% red
 * and grey transitions in the reference capture. Normal builds never map the
 * back buffer because this diagnostic is disabled unless explicitly set. */
static void d3d8_debug_detect_present_flash(ULONGLONG now_ms)
{
    static int initialized;
    static int enabled;
    static int previous_valid;
    static uint8_t previous[PRESENT_FLASH_SAMPLE_COUNT][3];
    static unsigned capture_count;
    static unsigned capture_limit;
    static ULONGLONG started_ms;
    static const char *capture_prefix;
    static const char *gate_path;
    ID3D11Texture2D *back_buffer = NULL, *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    int64_t signed_sum = 0;
    uint64_t absolute_sum = 0;
    uint64_t channel_sum[3] = { 0u, 0u, 0u };
    uint32_t positive = 0u, negative = 0u;
    uint32_t sx, sy, sample = 0u;
    HRESULT hr;

    if (!initialized) {
        const char *limit_text;
        initialized = 1;
        enabled = d3d8_cached_getenv(
            "MERCENARIES_TRACE_PRESENT_FLASH") != NULL;
        if (!enabled)
            return;
        capture_prefix = d3d8_cached_getenv(
            "MERCENARIES_CAPTURE_PRESENT_FLASH_PREFIX");
        gate_path = d3d8_cached_getenv(
            "MERCENARIES_TRACE_PRESENT_FLASH_GATE_FILE");
        limit_text = d3d8_cached_getenv(
            "MERCENARIES_CAPTURE_PRESENT_FLASH_COUNT");
        capture_limit = limit_text ?
            (unsigned)strtoul(limit_text, NULL, 10) : 32u;
        if (capture_limit > 64u)
            capture_limit = 64u;
        started_ms = now_ms;
    }
    if (!enabled || !g_device_state.swap_chain ||
        !g_device_state.d3d11_device || !g_device_state.d3d11_context)
        return;
    if (gate_path && gate_path[0] &&
        GetFileAttributesA(gate_path) == INVALID_FILE_ATTRIBUTES) {
        previous_valid = 0;
        return;
    }

    hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                  &IID_ID3D11Texture2D,
                                  (void **)&back_buffer);
    if (FAILED(hr) || !back_buffer)
        return;
    ID3D11Texture2D_GetDesc(back_buffer, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0u;
    hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device, &desc,
                                      NULL, &staging);
    if (SUCCEEDED(hr) && staging) {
        ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
            (ID3D11Resource *)staging, (ID3D11Resource *)back_buffer);
        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
            (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            for (sy = 0u; sy < PRESENT_FLASH_SAMPLE_HEIGHT; ++sy) {
                const uint32_t y =
                    ((sy * 2u + 1u) * desc.Height) /
                    (PRESENT_FLASH_SAMPLE_HEIGHT * 2u);
                const uint8_t *row = (const uint8_t *)mapped.pData +
                    (size_t)y * mapped.RowPitch;
                for (sx = 0u; sx < PRESENT_FLASH_SAMPLE_WIDTH; ++sx) {
                    const uint32_t x =
                        ((sx * 2u + 1u) * desc.Width) /
                        (PRESENT_FLASH_SAMPLE_WIDTH * 2u);
                    const uint8_t *pixel = row + (size_t)x * 4u;
                    int delta = 0;
                    uint32_t channel;
                    for (channel = 0u; channel < 3u; ++channel) {
                        channel_sum[channel] += pixel[channel];
                        if (previous_valid)
                            delta += (int)pixel[channel] -
                                     (int)previous[sample][channel];
                        previous[sample][channel] = pixel[channel];
                    }
                    if (previous_valid) {
                        delta /= 3;
                        signed_sum += delta;
                        absolute_sum += (uint64_t)abs(delta);
                        if (delta >= 8)
                            ++positive;
                        else if (delta <= -8)
                            ++negative;
                    }
                    ++sample;
                }
            }
            if (previous_valid) {
                const uint32_t coherent =
                    positive > negative ? positive : negative;
                const int64_t mean_delta =
                    signed_sum / (int64_t)PRESENT_FLASH_SAMPLE_COUNT;
                const uint64_t mean_absolute =
                    absolute_sum / PRESENT_FLASH_SAMPLE_COUNT;
                if (coherent * 100u >= PRESENT_FLASH_SAMPLE_COUNT * 72u &&
                    llabs(mean_delta) >= 10 && mean_absolute >= 12u) {
                    char path[1024] = { 0 };
                    int length = 0;
                    if (capture_prefix && capture_prefix[0] &&
                        capture_count < capture_limit) {
                        length = snprintf(path, sizeof(path),
                            "%s-%03u-%llu.bmp", capture_prefix,
                            capture_count + 1u,
                            (unsigned long long)(now_ms - started_ms));
                        if (length > 0 && (size_t)length < sizeof(path))
                            d3d8_write_backbuffer_bmp(
                                path, &desc, &mapped, FALSE);
                    }
                    ++capture_count;
                    fprintf(stderr,
                        "[PRESENT-FLASH] tick=%llu elapsed=%llu n=%u "
                        "direction=%s coherent=%u/%u mean_delta=%lld "
                        "mean_abs=%llu mean_rgb=%.2f/%.2f/%.2f capture=%s\n",
                        (unsigned long long)now_ms,
                        (unsigned long long)(now_ms - started_ms),
                        capture_count, mean_delta >= 0 ? "up" : "down",
                        coherent, PRESENT_FLASH_SAMPLE_COUNT,
                        (long long)mean_delta,
                        (unsigned long long)mean_absolute,
                        (double)channel_sum[0] / PRESENT_FLASH_SAMPLE_COUNT,
                        (double)channel_sum[1] / PRESENT_FLASH_SAMPLE_COUNT,
                        (double)channel_sum[2] / PRESENT_FLASH_SAMPLE_COUNT,
                        path[0] ? path : "none");
                    fflush(stderr);
                }
            }
            previous_valid = 1;
            ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                      (ID3D11Resource *)staging, 0);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11Texture2D_Release(back_buffer);
}

BOOL d3d8_DebugCaptureTextureToPath(ID3D11Texture2D *texture,
                                    const char *path)
{
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!texture || !path || !*path || !g_device_state.d3d11_device ||
        !g_device_state.d3d11_context)
        return FALSE;

    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device, &desc,
                                      NULL, &staging);
    if (FAILED(hr) || !staging)
        return FALSE;

    ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
        (ID3D11Resource *)staging, (ID3D11Resource *)texture);
    memset(&mapped, 0, sizeof(mapped));
    hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
        (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (SUCCEEDED(hr)) {
        uint8_t channel_min[4] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu };
        uint8_t channel_max[4] = { 0u, 0u, 0u, 0u };
        uint64_t channel_sum[4] = { 0u, 0u, 0u, 0u };
        uint32_t x, y, channel;
        const uint64_t pixel_count =
            (uint64_t)desc.Width * (uint64_t)desc.Height;

        for (y = 0u; y < desc.Height; ++y) {
            const uint8_t *row = (const uint8_t *)mapped.pData +
                (size_t)y * mapped.RowPitch;
            for (x = 0u; x < desc.Width; ++x) {
                const uint8_t *pixel = row + x * 4u;
                for (channel = 0u; channel < 4u; ++channel) {
                    const uint8_t value = pixel[channel];
                    if (value < channel_min[channel])
                        channel_min[channel] = value;
                    if (value > channel_max[channel])
                        channel_max[channel] = value;
                    channel_sum[channel] += value;
                }
            }
        }
        fprintf(stderr,
                "D3D8: capture RGBA stats %s size=%ux%u "
                "R=%u..%u/%.3f G=%u..%u/%.3f "
                "B=%u..%u/%.3f A=%u..%u/%.3f\n",
                path, desc.Width, desc.Height,
                channel_min[0], channel_max[0], pixel_count ?
                    (double)channel_sum[0] / (double)pixel_count : 0.0,
                channel_min[1], channel_max[1], pixel_count ?
                    (double)channel_sum[1] / (double)pixel_count : 0.0,
                channel_min[2], channel_max[2], pixel_count ?
                    (double)channel_sum[2] / (double)pixel_count : 0.0,
                channel_min[3], channel_max[3], pixel_count ?
                    (double)channel_sum[3] / (double)pixel_count : 0.0);
        d3d8_write_backbuffer_bmp(path, &desc, &mapped, FALSE);
        if (getenv("MERCENARIES_CAPTURE_TEXTURE_ALPHA") != NULL) {
            char alpha_path[1024];
            int length = snprintf(alpha_path, sizeof(alpha_path),
                                  "%s.alpha.bmp", path);
            if (length > 0 && (size_t)length < sizeof(alpha_path))
                d3d8_write_backbuffer_bmp(alpha_path, &desc, &mapped, TRUE);
        }
        ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                  (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
    return SUCCEEDED(hr);
}
static void d3d8_debug_backbuffer_stats(void)
{
    static int captured;
    static unsigned int capture_attempts;
    const char *capture_index_env;
    const char *capture_count_env;
    const char *capture_stride_env;
    const char *flip_capture_path;
    const char *capture_path;
    const char *write_path;
    unsigned int capture_index;
    unsigned int capture_count;
    unsigned int capture_stride;
    unsigned int capture_ordinal;
    char series_path[MAX_PATH];
    ID3D11Texture2D *back_buffer = NULL, *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint32_t nonblack = 0, nonwhite = 0, sample_count = 0;
    uint32_t x, y;
    HRESULT hr;

    flip_capture_path = g_debug_armed_capture_path[0] ?
        g_debug_armed_capture_path : d3d8_cached_getenv("MERCENARIES_CAPTURE_FLIP_PATH");
    if (flip_capture_path && !g_debug_capture_at_flip &&
        !g_debug_force_capture)
        return;
    capture_path = g_debug_force_capture ?
        (g_debug_forced_capture_path[0] ? g_debug_forced_capture_path :
         d3d8_cached_getenv("MERCENARIES_CAPTURE_SKY_PATH")) :
        (g_debug_capture_at_flip && flip_capture_path ?
            flip_capture_path : d3d8_cached_getenv("MERCENARIES_CAPTURE_SPLASH_PATH"));
    if ((!g_debug_force_capture && captured) ||
        (d3d8_cached_getenv("MERCENARIES_TRACE_SPLASH") == NULL &&
         capture_path == NULL) ||
        !g_device_state.swap_chain || !g_device_state.d3d11_device ||
        !g_device_state.d3d11_context)
        return;
    capture_index_env = g_debug_capture_at_flip ?
        d3d8_cached_getenv("MERCENARIES_CAPTURE_FLIP_INDEX") :
        d3d8_cached_getenv("MERCENARIES_CAPTURE_SPLASH_INDEX");
    capture_index = capture_index_env ?
        (unsigned int)strtoul(capture_index_env, NULL, 10) : 1u;
    if (capture_index == 0u)
        capture_index = 1u;
    capture_count_env = d3d8_cached_getenv("MERCENARIES_CAPTURE_FLIP_COUNT");
    capture_count = capture_count_env ?
        (unsigned int)strtoul(capture_count_env, NULL, 10) : 1u;
    if (capture_count == 0u)
        capture_count = 1u;
    ++capture_attempts;
    if (g_debug_force_capture) {
        /* An explicit capture-to-path request is synchronous and owns its
         * filename. It must not be suppressed by the unrelated one-shot flip
         * capture counter after an earlier diagnostic image was written. */
        capture_ordinal = 0u;
    } else {
        if (capture_attempts < capture_index)
            return;
        capture_stride_env = d3d8_cached_getenv("MERCENARIES_CAPTURE_FLIP_STRIDE");
        capture_stride = capture_stride_env ?
            (unsigned int)strtoul(capture_stride_env, NULL, 10) : 1u;
        if (capture_stride == 0u)
            capture_stride = 1u;
        if ((capture_attempts - capture_index) % capture_stride != 0u)
            return;
        capture_ordinal = (capture_attempts - capture_index) / capture_stride;
        if (capture_ordinal >= capture_count)
            return;
        if (capture_ordinal + 1u >= capture_count)
            captured = 1;
    }
    write_path = capture_path;
    if (capture_count > 1u && !g_debug_force_capture) {
        _snprintf_s(series_path, sizeof(series_path), _TRUNCATE,
                    "%s-%04u.bmp", capture_path, capture_attempts);
        write_path = series_path;
    }
    hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                  &IID_ID3D11Texture2D,
                                  (void **)&back_buffer);
    if (FAILED(hr) || !back_buffer)
        return;
    ID3D11Texture2D_GetDesc(back_buffer, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device, &desc,
                                      NULL, &staging);
    if (SUCCEEDED(hr) && staging) {
        ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
            (ID3D11Resource *)staging, (ID3D11Resource *)back_buffer);
        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
            (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr)) {
            for (y = 0; y < desc.Height; y += 8u) {
                const uint8_t *row = (const uint8_t *)mapped.pData +
                                     (size_t)y * mapped.RowPitch;
                for (x = 0; x < desc.Width; x += 8u) {
                    const uint8_t *px = row + x * 4u;
                    if ((px[0] | px[1] | px[2]) != 0u) ++nonblack;
                    if (px[0] != 0xFFu || px[1] != 0xFFu ||
                        px[2] != 0xFFu) ++nonwhite;
                    ++sample_count;
                }
            }
            fprintf(stderr,
                    "D3D8: post-draw backbuffer %ux%u samples=%u "
                    "nonblack=%u nonwhite=%u center=%02X%02X%02X%02X\n",
                    desc.Width, desc.Height, sample_count, nonblack, nonwhite,
                    ((uint8_t *)mapped.pData)[(desc.Height / 2u) *
                        mapped.RowPitch + (desc.Width / 2u) * 4u + 0u],
                    ((uint8_t *)mapped.pData)[(desc.Height / 2u) *
                        mapped.RowPitch + (desc.Width / 2u) * 4u + 1u],
                    ((uint8_t *)mapped.pData)[(desc.Height / 2u) *
                        mapped.RowPitch + (desc.Width / 2u) * 4u + 2u],
                    ((uint8_t *)mapped.pData)[(desc.Height / 2u) *
                        mapped.RowPitch + (desc.Width / 2u) * 4u + 3u]);
            d3d8_write_backbuffer_bmp(
                write_path, &desc, &mapped, FALSE);
            ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
                                      (ID3D11Resource *)staging, 0);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11Texture2D_Release(back_buffer);
}
/* ================================================================
 * Internal accessors (used by d3d8_resources/shaders/states)
 * ================================================================ */

IDirect3DDevice8    *d3d8_GetDevice(void) { return &g_device; }
ID3D11Device        *d3d8_GetD3D11Device(void) { return g_device_state.d3d11_device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return g_device_state.d3d11_context; }
IDXGISwapChain      *d3d8_GetSwapChain(void) { return g_device_state.swap_chain; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return g_device_state.default_rtv; }
HWND                 d3d8_GetHWND(void) { return g_device_state.hwnd; }
UINT                 d3d8_GetBackbufferWidth(void) { return g_device_state.width; }
UINT                 d3d8_GetBackbufferHeight(void) { return g_device_state.height; }
UINT                 d3d8_GetRenderTargetWidth(void) { return g_device_state.current_target_width; }
UINT                 d3d8_GetRenderTargetHeight(void) { return g_device_state.current_target_height; }

void d3d8_BindRenderTargets(ID3D11RenderTargetView *rtv,
                            ID3D11DepthStencilView *dsv,
                            UINT width, UINT height)
{
    ID3D11ShaderResourceView *null_srvs[4] = { NULL, NULL, NULL, NULL };
    D3D11_VIEWPORT viewport;

    if (!g_device_state.d3d11_context)
        return;
    if (!rtv) {
        rtv = g_device_state.default_rtv;
        dsv = g_device_state.default_dsv;
        width = g_device_state.width;
        height = g_device_state.height;
    }

    /* A guest surface commonly changes from a texture source to a render
     * target. D3D11 requires every SRV alias to be unbound first. */
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 4, null_srvs);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
                                           1, &rtv, dsv);
    g_device_state.current_rtv = rtv;
    g_device_state.current_dsv = dsv;
    g_device_state.current_target_width = width;
    g_device_state.current_target_height = height;
    g_device_state.current_target_physical_width = width;
    g_device_state.current_target_physical_height = height;

    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)width;
    viewport.Height = (FLOAT)height;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context,
                                       1, &viewport);
}

void d3d8_SetLogicalRenderTargetSize(UINT width, UINT height)
{
    if (width) g_device_state.current_target_width = width;
    if (height) g_device_state.current_target_height = height;
}

void d3d8_BindExternalTexture(UINT stage, ID3D11ShaderResourceView *srv)
{
    if (!g_device_state.d3d11_context || stage >= 4u)
        return;
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             stage, 1, &srv);
    if (srv && g_device_state.tss[stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
        g_device_state.tss[stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
}

static BOOL d3d8_ensure_resolve_pipeline(void)
{
    static const char vs_source[] =
        "struct VSOut { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };"
        "VSOut main(uint id : SV_VertexID) {"
        " float2 uv=float2((id << 1) & 2, id & 2); VSOut o; o.uv=uv;"
        " o.position=float4(uv.x*2.0-1.0, 1.0-uv.y*2.0, 0.0, 1.0); return o; }";
    static const char ps_source[] =
        "Texture2D source_texture : register(t0);"
        "SamplerState source_sampler : register(s0);"
        "float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0)"
        " : SV_Target { float4 c=source_texture.Sample(source_sampler, uv);"
        " return float4(c.rgb,1.0); }";
    ID3DBlob *vs_blob = NULL, *ps_blob = NULL, *errors = NULL;
    D3D11_SAMPLER_DESC sampler_desc;
    HRESULT hr;

    if (g_device_state.resolve_vs && g_device_state.resolve_ps &&
        g_device_state.resolve_sampler) return TRUE;
    fprintf(stderr, "D3D8: Compiling fixed resolve pipeline\n");
    hr = d3d8_compile_shader(vs_source, sizeof(vs_source) - 1u, "nv2a_resolve_vs",
                    NULL, NULL, "main", "vs_5_0", 0, 0, &vs_blob, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: resolve VS compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        goto fail;
    }
    if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
    hr = ID3D11Device_CreateVertexShader(g_device_state.d3d11_device,
        ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob),
        NULL, &g_device_state.resolve_vs);
    if (FAILED(hr)) goto fail;
    hr = d3d8_compile_shader(ps_source, sizeof(ps_source) - 1u, "nv2a_resolve_ps",
                    NULL, NULL, "main", "ps_5_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: resolve PS compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        goto fail;
    }
    if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
    hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
        ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob),
        NULL, &g_device_state.resolve_ps);
    if (FAILED(hr)) goto fail;
    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = ID3D11Device_CreateSamplerState(g_device_state.d3d11_device,
        &sampler_desc, &g_device_state.resolve_sampler);
    if (FAILED(hr)) goto fail;
    if (vs_blob) ID3D10Blob_Release(vs_blob);
    if (ps_blob) ID3D10Blob_Release(ps_blob);
    return TRUE;
fail:
    if (errors) ID3D10Blob_Release(errors);
    if (vs_blob) ID3D10Blob_Release(vs_blob);
    if (ps_blob) ID3D10Blob_Release(ps_blob);
    if (g_device_state.resolve_vs) { ID3D11VertexShader_Release(g_device_state.resolve_vs); g_device_state.resolve_vs = NULL; }
    if (g_device_state.resolve_ps) { ID3D11PixelShader_Release(g_device_state.resolve_ps); g_device_state.resolve_ps = NULL; }
    if (g_device_state.resolve_sampler) { ID3D11SamplerState_Release(g_device_state.resolve_sampler); g_device_state.resolve_sampler = NULL; }
    return FALSE;
}

static BOOL d3d8_ensure_packed_color_depth_pipeline(void)
{
    static const char ps_source[] =
        "Texture2D<float4> source_texture : register(t0);"
        "struct PSOut { float depth : SV_Depth; uint stencil : SV_StencilRef; };"
        "PSOut main(float4 position : SV_POSITION) {"
        " uint4 rgba=(uint4)round(saturate(source_texture.Load("
        " int3(int2(position.xy),0)))*255.0);"
        " uint depth_bits=rgba.g|(rgba.r<<8)|(rgba.a<<16); PSOut o;"
        " o.depth=(float)depth_bits/16777215.0; o.stencil=rgba.b; return o; }";
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2;
    D3D11_DEPTH_STENCIL_DESC depth_desc;
    ID3DBlob *ps_blob = NULL, *errors = NULL;
    HRESULT hr;

    if (g_device_state.packed_color_depth_checked)
        return g_device_state.packed_color_depth_ps != NULL &&
               g_device_state.packed_color_depth_state != NULL;
    g_device_state.packed_color_depth_checked = TRUE;
    memset(&options2, 0, sizeof(options2));
    hr = ID3D11Device_CheckFeatureSupport(g_device_state.d3d11_device,
        D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2));
    if (FAILED(hr) || !options2.PSSpecifiedStencilRefSupported) {
        fprintf(stderr, "D3D8: GPU packed-color/depth alias conversion "
                        "unsupported; retaining CPU fallback\n");
        return FALSE;
    }
    if (!d3d8_ensure_resolve_pipeline())
        return FALSE;
    hr = d3d8_compile_shader(ps_source, sizeof(ps_source) - 1u,
                    "nv2a_packed_color_depth_ps", NULL, NULL, "main",
                    "ps_5_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: packed-color/depth PS compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) :
                         "unknown");
        goto fail;
    }
    if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
    hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
        ID3D10Blob_GetBufferPointer(ps_blob),
        ID3D10Blob_GetBufferSize(ps_blob), NULL,
        &g_device_state.packed_color_depth_ps);
    if (FAILED(hr))
        goto fail;
    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth_desc.StencilEnable = TRUE;
    depth_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    depth_desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    depth_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depth_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    depth_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    depth_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    depth_desc.BackFace = depth_desc.FrontFace;
    hr = ID3D11Device_CreateDepthStencilState(g_device_state.d3d11_device,
        &depth_desc, &g_device_state.packed_color_depth_state);
    if (FAILED(hr))
        goto fail;
    ID3D10Blob_Release(ps_blob);
    fprintf(stderr, "D3D8: GPU packed-color/depth alias conversion enabled\n");
    return TRUE;

fail:
    if (errors) ID3D10Blob_Release(errors);
    if (ps_blob) ID3D10Blob_Release(ps_blob);
    if (g_device_state.packed_color_depth_ps) {
        ID3D11PixelShader_Release(g_device_state.packed_color_depth_ps);
        g_device_state.packed_color_depth_ps = NULL;
    }
    if (g_device_state.packed_color_depth_state) {
        ID3D11DepthStencilState_Release(g_device_state.packed_color_depth_state);
        g_device_state.packed_color_depth_state = NULL;
    }
    return FALSE;
}
static BOOL d3d8_ensure_packed_color_depth_fallback_pipeline(void)
{
    static const char depth_source[] =
        "Texture2D<float4> source_texture : register(t0);"
        "float main(float4 position : SV_POSITION) : SV_Depth {"
        " uint4 rgba=(uint4)round(saturate(source_texture.Load("
        " int3(int2(position.xy),0)))*255.0);"
        " uint depth_bits=rgba.g|(rgba.r<<8)|(rgba.a<<16);"
        " return (float)depth_bits/16777215.0; }";
    static const char stencil_source[] =
        "Texture2D<float4> source_texture : register(t0);"
        "void main(float4 position : SV_POSITION) {"
        " uint4 rgba=(uint4)round(saturate(source_texture.Load("
        " int3(int2(position.xy),0)))*255.0);"
        " if ((rgba.b & STENCIL_BIT)==0) discard; }";
    D3D11_DEPTH_STENCIL_DESC depth_desc;
    ID3DBlob *ps_blob = NULL, *errors = NULL;
    UINT bit;
    HRESULT hr;

    if (g_device_state.packed_color_depth_fallback_checked)
        return g_device_state.packed_color_depth_fallback_ps != NULL &&
               g_device_state.packed_color_depth_fallback_state != NULL &&
               g_device_state.packed_color_stencil_bit_ps[7] != NULL &&
               g_device_state.packed_color_stencil_bit_state[7] != NULL;
    g_device_state.packed_color_depth_fallback_checked = TRUE;
    if (!d3d8_ensure_resolve_pipeline())
        goto fail;
    hr = d3d8_compile_shader(depth_source, sizeof(depth_source) - 1u,
                    "nv2a_packed_color_depth_fallback_ps", NULL, NULL,
                    "main", "ps_5_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: packed-color depth fallback PS compile "
                        "failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) :
                         "unknown");
        goto fail;
    }
    if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
    hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
        ID3D10Blob_GetBufferPointer(ps_blob),
        ID3D10Blob_GetBufferSize(ps_blob), NULL,
        &g_device_state.packed_color_depth_fallback_ps);
    ID3D10Blob_Release(ps_blob);
    ps_blob = NULL;
    if (FAILED(hr))
        goto fail;

    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth_desc.StencilEnable = FALSE;
    hr = ID3D11Device_CreateDepthStencilState(g_device_state.d3d11_device,
        &depth_desc, &g_device_state.packed_color_depth_fallback_state);
    if (FAILED(hr))
        goto fail;

    for (bit = 0u; bit < 8u; ++bit) {
        D3D_SHADER_MACRO macros[2];
        char bit_text[8];
        char shader_name[64];

        snprintf(bit_text, sizeof(bit_text), "%u", 1u << bit);
        macros[0].Name = "STENCIL_BIT";
        macros[0].Definition = bit_text;
        macros[1].Name = NULL;
        macros[1].Definition = NULL;
        snprintf(shader_name, sizeof(shader_name),
                 "nv2a_packed_color_stencil_bit_%u_ps", bit);
        hr = d3d8_compile_shader(stencil_source, sizeof(stencil_source) - 1u,
                        shader_name, macros, NULL, "main", "ps_5_0", 0, 0,
                        &ps_blob, &errors);
        if (FAILED(hr)) {
            fprintf(stderr, "D3D8: packed-color stencil-bit PS compile "
                            "failed: %s\n",
                    errors ? (char *)ID3D10Blob_GetBufferPointer(errors) :
                             "unknown");
            goto fail;
        }
        if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
        hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
            ID3D10Blob_GetBufferPointer(ps_blob),
            ID3D10Blob_GetBufferSize(ps_blob), NULL,
            &g_device_state.packed_color_stencil_bit_ps[bit]);
        ID3D10Blob_Release(ps_blob);
        ps_blob = NULL;
        if (FAILED(hr))
            goto fail;

        memset(&depth_desc, 0, sizeof(depth_desc));
        depth_desc.DepthEnable = FALSE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth_desc.StencilEnable = TRUE;
        depth_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        depth_desc.StencilWriteMask = (UINT8)(1u << bit);
        depth_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        depth_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        depth_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        depth_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        depth_desc.BackFace = depth_desc.FrontFace;
        hr = ID3D11Device_CreateDepthStencilState(
            g_device_state.d3d11_device, &depth_desc,
            &g_device_state.packed_color_stencil_bit_state[bit]);
        if (FAILED(hr))
            goto fail;
    }
    fprintf(stderr, "D3D8: exact GPU packed-color/depth stencil-bit "
                    "fallback enabled\n");
    return TRUE;

fail:
    if (errors) ID3D10Blob_Release(errors);
    if (ps_blob) ID3D10Blob_Release(ps_blob);
    for (bit = 0u; bit < 8u; ++bit) {
        if (g_device_state.packed_color_stencil_bit_state[bit]) {
            ID3D11DepthStencilState_Release(
                g_device_state.packed_color_stencil_bit_state[bit]);
            g_device_state.packed_color_stencil_bit_state[bit] = NULL;
        }
        if (g_device_state.packed_color_stencil_bit_ps[bit]) {
            ID3D11PixelShader_Release(
                g_device_state.packed_color_stencil_bit_ps[bit]);
            g_device_state.packed_color_stencil_bit_ps[bit] = NULL;
        }
    }
    if (g_device_state.packed_color_depth_fallback_state) {
        ID3D11DepthStencilState_Release(
            g_device_state.packed_color_depth_fallback_state);
        g_device_state.packed_color_depth_fallback_state = NULL;
    }
    if (g_device_state.packed_color_depth_fallback_ps) {
        ID3D11PixelShader_Release(
            g_device_state.packed_color_depth_fallback_ps);
        g_device_state.packed_color_depth_fallback_ps = NULL;
    }
    return FALSE;
}
static BOOL d3d8_ensure_depth_alias_pipeline(void)
{
    static const char ps_source[] =
        "cbuffer AliasConstants : register(b0) { uint4 alias_dims; };"
        "Texture2D<float> source_depth : register(t0);"
        "Texture2D<uint4> source_stencil : register(t1);"
        "struct PSOut { float4 color : SV_Target; float depth : SV_Depth;"
        " uint stencil : SV_StencilRef; };"
        "PSOut main(float4 position : SV_POSITION) {"
        " uint2 dst=(uint2)position.xy; uint2 src;"
        " src.x=min((dst.x*alias_dims.x+alias_dims.z/2)/alias_dims.z,"
        " alias_dims.x-1);"
        " src.y=min((dst.y*alias_dims.y+alias_dims.w/2)/alias_dims.w,"
        " alias_dims.y-1);"
        " float z=source_depth.Load(int3(src,0));"
        " uint zb=(uint)round(saturate(z)*16777215.0);"
        " uint s=source_stencil.Load(int3(src,0)).g&255; PSOut o;"
        /* Xbox A8R8G8B8 interprets the little-endian Z24S8 word as
         * R=depth middle, G=depth low, B=stencil, A=depth high.  Guest color
         * render targets use canonical RGBA host channels, so reproduce that
         * sampled value rather than copying the raw guest byte order into an
         * R8G8B8A8 texture. */
        " o.color=float4(s,zb&255,(zb>>8)&255,(zb>>16)&255)/255.0;"
        " o.depth=(float)zb/16777215.0; o.stencil=s; return o; }";
    D3D11_BUFFER_DESC buffer_desc;
    ID3DBlob *ps_blob = NULL, *errors = NULL;
    HRESULT hr;

    if (g_device_state.depth_alias_ps && g_device_state.depth_alias_cb)
        return TRUE;
    if (!d3d8_ensure_packed_color_depth_pipeline())
        return FALSE;
    hr = d3d8_compile_shader(ps_source, sizeof(ps_source) - 1u,
                    "nv2a_depth_alias_ps", NULL, NULL, "main",
                    "ps_5_0", 0, 0, &ps_blob, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: depth-alias PS compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) :
                         "unknown");
        goto fail;
    }
    if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
    hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
        ID3D10Blob_GetBufferPointer(ps_blob),
        ID3D10Blob_GetBufferSize(ps_blob), NULL,
        &g_device_state.depth_alias_ps);
    if (FAILED(hr))
        goto fail;
    if (!g_device_state.depth_alias_cb) {
        memset(&buffer_desc, 0, sizeof(buffer_desc));
        buffer_desc.ByteWidth = 16u;
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device,
            &buffer_desc, NULL, &g_device_state.depth_alias_cb);
        if (FAILED(hr))
            goto fail;
    }
    ID3D10Blob_Release(ps_blob);
    fprintf(stderr, "D3D8: GPU scaled depth-alias conversion enabled\n");
    return TRUE;

fail:
    if (errors) ID3D10Blob_Release(errors);
    if (ps_blob) ID3D10Blob_Release(ps_blob);
    if (g_device_state.depth_alias_ps) {
        ID3D11PixelShader_Release(g_device_state.depth_alias_ps);
        g_device_state.depth_alias_ps = NULL;
    }
    return FALSE;
}

static BOOL d3d8_ensure_depth_alias_uniform_pipeline(void)
{
    static const char ps_source[] =
        "cbuffer AliasConstants : register(b0) { uint4 alias_dims; };"
        "Texture2D<float> source_depth : register(t0);"
        "Texture2D<uint4> source_stencil : register(t1);"
        "struct PSOut { float4 color : SV_Target; float depth : SV_Depth; };"
        "PSOut main(float4 position : SV_POSITION) {"
        " uint2 dst=(uint2)position.xy;"
        " uint2 src=(dst*alias_dims.xy+alias_dims.zw/2)/alias_dims.zw;"
        " src=min(src,alias_dims.xy-1);"
        " float z=source_depth.Load(int3(src,0));"
        " uint zb=(uint)round(saturate(z)*16777215.0);"
        " uint s=source_stencil.Load(int3(src,0)).g&255; PSOut o;"
        " o.color=float4(s,zb&255,(zb>>8)&255,(zb>>16)&255)/255.0;"
        " o.depth=(float)zb/16777215.0; return o; }";
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_DEPTH_STENCIL_DESC depth_desc;
    ID3DBlob *ps_blob = NULL, *errors = NULL;
    HRESULT hr;

    if (g_device_state.depth_alias_uniform_ps &&
        g_device_state.depth_alias_uniform_state &&
        g_device_state.depth_alias_cb)
        return TRUE;
    if (!d3d8_ensure_resolve_pipeline())
        return FALSE;
    if (!g_device_state.depth_alias_uniform_ps) {
        hr = d3d8_compile_shader(ps_source, sizeof(ps_source) - 1u,
                        "nv2a_depth_alias_uniform_ps", NULL, NULL, "main",
                        "ps_5_0", 0, 0, &ps_blob, &errors);
        if (FAILED(hr)) {
            fprintf(stderr,
                    "D3D8: uniform-stencil depth-alias PS compile failed: %s\n",
                    errors ? (char *)ID3D10Blob_GetBufferPointer(errors) :
                             "unknown");
            goto fail;
        }
        if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
        hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
            ID3D10Blob_GetBufferPointer(ps_blob),
            ID3D10Blob_GetBufferSize(ps_blob), NULL,
            &g_device_state.depth_alias_uniform_ps);
        ID3D10Blob_Release(ps_blob);
        ps_blob = NULL;
        if (FAILED(hr))
            goto fail;
    }
    if (!g_device_state.depth_alias_uniform_state) {
        memset(&depth_desc, 0, sizeof(depth_desc));
        depth_desc.DepthEnable = TRUE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth_desc.StencilEnable = FALSE;
        hr = ID3D11Device_CreateDepthStencilState(
            g_device_state.d3d11_device, &depth_desc,
            &g_device_state.depth_alias_uniform_state);
        if (FAILED(hr))
            goto fail;
    }
    if (!g_device_state.depth_alias_cb) {
        memset(&buffer_desc, 0, sizeof(buffer_desc));
        buffer_desc.ByteWidth = 16u;
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device,
            &buffer_desc, NULL, &g_device_state.depth_alias_cb);
        if (FAILED(hr))
            goto fail;
    }
    fprintf(stderr,
            "D3D8: GPU uniform-stencil scaled depth-alias conversion enabled\n");
    return TRUE;

fail:
    if (errors) ID3D10Blob_Release(errors);
    if (ps_blob) ID3D10Blob_Release(ps_blob);
    if (g_device_state.depth_alias_uniform_state) {
        ID3D11DepthStencilState_Release(
            g_device_state.depth_alias_uniform_state);
        g_device_state.depth_alias_uniform_state = NULL;
    }
    if (g_device_state.depth_alias_uniform_ps) {
        ID3D11PixelShader_Release(g_device_state.depth_alias_uniform_ps);
        g_device_state.depth_alias_uniform_ps = NULL;
    }
    return FALSE;
}

static BOOL d3d8_ensure_depth_alias_stencil_bit_pipeline(void)
{
    static const char ps_source[] =
        "cbuffer AliasConstants : register(b0) { uint4 alias_dims; };"
        "Texture2D<uint4> source_stencil : register(t1);"
        "void main(float4 position : SV_POSITION) {"
        " uint2 dst=(uint2)position.xy;"
        " uint2 src=(dst*alias_dims.xy+alias_dims.zw/2)/alias_dims.zw;"
        " src=min(src,alias_dims.xy-1);"
        " uint s=source_stencil.Load(int3(src,0)).g&255;"
        " if ((s & STENCIL_BIT)==0) discard; }";
    D3D11_DEPTH_STENCIL_DESC depth_desc;
    static BOOL logged;
    UINT bit;

    if (!d3d8_ensure_depth_alias_uniform_pipeline())
        return FALSE;
    for (bit = 0u; bit < 8u; ++bit) {
        ID3DBlob *ps_blob = NULL, *errors = NULL;
        D3D_SHADER_MACRO macros[2];
        char bit_text[8];
        char shader_name[64];
        HRESULT hr;

        if (g_device_state.depth_alias_stencil_bit_ps[bit] &&
            g_device_state.depth_alias_stencil_bit_state[bit])
            continue;
        snprintf(bit_text, sizeof(bit_text), "%u", 1u << bit);
        macros[0].Name = "STENCIL_BIT";
        macros[0].Definition = bit_text;
        macros[1].Name = NULL;
        macros[1].Definition = NULL;
        snprintf(shader_name, sizeof(shader_name),
                 "nv2a_depth_alias_stencil_bit_%u_ps", bit);
        hr = d3d8_compile_shader(ps_source, sizeof(ps_source) - 1u, shader_name,
                        macros, NULL, "main", "ps_5_0", 0, 0,
                        &ps_blob, &errors);
        if (FAILED(hr)) {
            fprintf(stderr,
                    "D3D8: depth-alias stencil-bit PS compile failed: %s\n",
                    errors ? (char *)ID3D10Blob_GetBufferPointer(errors) :
                             "unknown");
            if (errors) ID3D10Blob_Release(errors);
            if (ps_blob) ID3D10Blob_Release(ps_blob);
            goto fail;
        }
        if (errors) ID3D10Blob_Release(errors);
        hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
            ID3D10Blob_GetBufferPointer(ps_blob),
            ID3D10Blob_GetBufferSize(ps_blob), NULL,
            &g_device_state.depth_alias_stencil_bit_ps[bit]);
        ID3D10Blob_Release(ps_blob);
        if (FAILED(hr))
            goto fail;

        memset(&depth_desc, 0, sizeof(depth_desc));
        depth_desc.DepthEnable = FALSE;
        depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth_desc.StencilEnable = TRUE;
        depth_desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        depth_desc.StencilWriteMask = (UINT8)(1u << bit);
        depth_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        depth_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        depth_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        depth_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        depth_desc.BackFace = depth_desc.FrontFace;
        hr = ID3D11Device_CreateDepthStencilState(
            g_device_state.d3d11_device, &depth_desc,
            &g_device_state.depth_alias_stencil_bit_state[bit]);
        if (FAILED(hr))
            goto fail;
    }
    if (!logged) {
        fprintf(stderr,
                "D3D8: GPU stencil-bit scaled depth-alias conversion enabled\n");
        logged = TRUE;
    }
    return TRUE;

fail:
    for (bit = 0u; bit < 8u; ++bit) {
        if (g_device_state.depth_alias_stencil_bit_state[bit]) {
            ID3D11DepthStencilState_Release(
                g_device_state.depth_alias_stencil_bit_state[bit]);
            g_device_state.depth_alias_stencil_bit_state[bit] = NULL;
        }
        if (g_device_state.depth_alias_stencil_bit_ps[bit]) {
            ID3D11PixelShader_Release(
                g_device_state.depth_alias_stencil_bit_ps[bit]);
            g_device_state.depth_alias_stencil_bit_ps[bit] = NULL;
        }
    }
    return FALSE;
}

static void d3d8_release_depth_alias_source_views(void)
{
    if (g_device_state.depth_alias_source_stencil_srv) {
        ID3D11ShaderResourceView_Release(
            g_device_state.depth_alias_source_stencil_srv);
        g_device_state.depth_alias_source_stencil_srv = NULL;
    }
    if (g_device_state.depth_alias_source_depth_srv) {
        ID3D11ShaderResourceView_Release(
            g_device_state.depth_alias_source_depth_srv);
        g_device_state.depth_alias_source_depth_srv = NULL;
    }
    if (g_device_state.depth_alias_source_texture) {
        ID3D11Texture2D_Release(g_device_state.depth_alias_source_texture);
        g_device_state.depth_alias_source_texture = NULL;
    }
    g_device_state.depth_alias_source_width = 0u;
    g_device_state.depth_alias_source_height = 0u;
}

static BOOL d3d8_ensure_depth_alias_source_views(UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC texture_desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc;
    HRESULT hr;

    if (g_device_state.depth_alias_source_texture &&
        g_device_state.depth_alias_source_width == width &&
        g_device_state.depth_alias_source_height == height)
        return TRUE;
    d3d8_release_depth_alias_source_views();
    memset(&texture_desc, 0, sizeof(texture_desc));
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1u;
    texture_desc.ArraySize = 1u;
    texture_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    texture_desc.SampleDesc.Count = 1u;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
        &texture_desc, NULL, &g_device_state.depth_alias_source_texture);
    if (FAILED(hr))
        goto fail;
    memset(&view_desc, 0, sizeof(view_desc));
    view_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipLevels = 1u;
    hr = ID3D11Device_CreateShaderResourceView(g_device_state.d3d11_device,
        (ID3D11Resource *)g_device_state.depth_alias_source_texture,
        &view_desc, &g_device_state.depth_alias_source_depth_srv);
    if (FAILED(hr))
        goto fail;
    view_desc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    hr = ID3D11Device_CreateShaderResourceView(g_device_state.d3d11_device,
        (ID3D11Resource *)g_device_state.depth_alias_source_texture,
        &view_desc, &g_device_state.depth_alias_source_stencil_srv);
    if (FAILED(hr))
        goto fail;
    g_device_state.depth_alias_source_width = width;
    g_device_state.depth_alias_source_height = height;
    return TRUE;

fail:
    d3d8_release_depth_alias_source_views();
    return FALSE;
}
static BOOL d3d8_ensure_masked_clear_pipeline(UINT write_mask)
{
    static const char ps_source[] =
        "cbuffer ClearConstants : register(b0) { float4 clear_color; };"
        "float4 main() : SV_Target { return clear_color; }";
    ID3DBlob *ps_blob = NULL, *errors = NULL;
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_BLEND_DESC blend_desc;
    HRESULT hr;

    write_mask &= 0x0Fu;
    if (!write_mask || !d3d8_ensure_resolve_pipeline())
        return FALSE;
    if (!g_device_state.masked_clear_ps) {
        fprintf(stderr, "D3D8: Compiling fixed masked-clear pipeline\n");
        hr = d3d8_compile_shader(ps_source, sizeof(ps_source) - 1u,
                        "nv2a_masked_clear_ps", NULL, NULL, "main",
                        "ps_5_0", 0, 0, &ps_blob, &errors);
        if (FAILED(hr)) {
            fprintf(stderr, "D3D8: masked-clear PS compile failed: %s\n",
                    errors ? (char *)ID3D10Blob_GetBufferPointer(errors) :
                             "unknown");
            if (errors) ID3D10Blob_Release(errors);
            return FALSE;
        }
        if (errors) ID3D10Blob_Release(errors);
        hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
            ID3D10Blob_GetBufferPointer(ps_blob),
            ID3D10Blob_GetBufferSize(ps_blob), NULL,
            &g_device_state.masked_clear_ps);
        ID3D10Blob_Release(ps_blob);
        if (FAILED(hr)) return FALSE;
    }
    if (!g_device_state.masked_clear_cb) {
        memset(&buffer_desc, 0, sizeof(buffer_desc));
        buffer_desc.ByteWidth = 16u;
        buffer_desc.Usage = D3D11_USAGE_DEFAULT;
        buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device,
            &buffer_desc, NULL, &g_device_state.masked_clear_cb);
        if (FAILED(hr)) return FALSE;
    }
    if (!g_device_state.masked_clear_blend[write_mask]) {
        memset(&blend_desc, 0, sizeof(blend_desc));
        blend_desc.RenderTarget[0].BlendEnable = FALSE;
        blend_desc.RenderTarget[0].RenderTargetWriteMask = (UINT8)write_mask;
        hr = ID3D11Device_CreateBlendState(g_device_state.d3d11_device,
            &blend_desc, &g_device_state.masked_clear_blend[write_mask]);
        if (FAILED(hr)) return FALSE;
    }
    return TRUE;
}

BOOL d3d8_ClearRenderTargetMasked(DWORD color, UINT write_mask,
                                  UINT left, UINT top,
                                  UINT right, UINT bottom)
{
    ID3D11VertexShader *old_vs = NULL;
    ID3D11PixelShader *old_ps = NULL;
    ID3D11InputLayout *old_layout = NULL;
    ID3D11RasterizerState *old_raster = NULL;
    ID3D11BlendState *old_blend = NULL;
    ID3D11DepthStencilState *old_depth = NULL;
    ID3D11Buffer *old_cb = NULL;
    D3D11_PRIMITIVE_TOPOLOGY old_topology;
    D3D11_VIEWPORT old_viewports[16];
    D3D11_VIEWPORT viewport;
    UINT old_viewport_count = 16u;
    FLOAT old_blend_factor[4];
    UINT old_sample_mask, old_stencil_ref;
    float clear_color[4];

    write_mask &= 0x0Fu;
    if (!g_device_state.current_rtv || left >= right || top >= bottom ||
        !d3d8_ensure_masked_clear_pipeline(write_mask))
        return FALSE;

    clear_color[0] = ((color >> 16) & 0xFFu) / 255.0f;
    clear_color[1] = ((color >> 8) & 0xFFu) / 255.0f;
    clear_color[2] = (color & 0xFFu) / 255.0f;
    clear_color[3] = ((color >> 24) & 0xFFu) / 255.0f;
    ID3D11DeviceContext_UpdateSubresource(g_device_state.d3d11_context,
        (ID3D11Resource *)g_device_state.masked_clear_cb, 0, NULL,
        clear_color, 0, 0);

    ID3D11DeviceContext_VSGetShader(g_device_state.d3d11_context,
                                    &old_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_device_state.d3d11_context,
                                    &old_ps, NULL, NULL);
    ID3D11DeviceContext_PSGetConstantBuffers(g_device_state.d3d11_context,
                                             0, 1, &old_cb);
    ID3D11DeviceContext_IAGetInputLayout(g_device_state.d3d11_context,
                                         &old_layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_device_state.d3d11_context,
                                               &old_topology);
    ID3D11DeviceContext_RSGetState(g_device_state.d3d11_context, &old_raster);
    ID3D11DeviceContext_RSGetViewports(g_device_state.d3d11_context,
                                       &old_viewport_count, old_viewports);
    ID3D11DeviceContext_OMGetBlendState(g_device_state.d3d11_context,
        &old_blend, old_blend_factor, &old_sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_device_state.d3d11_context,
                                               &old_depth, &old_stencil_ref);

    memset(&viewport, 0, sizeof(viewport));
    viewport.TopLeftX = (FLOAT)left;
    viewport.TopLeftY = (FLOAT)top;
    viewport.Width = (FLOAT)(right - left);
    viewport.Height = (FLOAT)(bottom - top);
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, NULL);
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context,
                                       1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context,
                                   g_device_state.resolve_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context,
                                   g_device_state.masked_clear_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_device_state.d3d11_context,
                                             0, 1,
                                             &g_device_state.masked_clear_cb);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
        g_device_state.masked_clear_blend[write_mask], NULL, ~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
                                               NULL, 0u);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, 3, 0);

    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context,
                                   old_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context,
                                   old_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_device_state.d3d11_context,
                                             0, 1, &old_cb);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context,
                                         old_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context,
                                               old_topology);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, old_raster);
    if (old_viewport_count)
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context,
                                           old_viewport_count, old_viewports);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
        old_blend, old_blend_factor, old_sample_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
                                               old_depth, old_stencil_ref);
    if (old_vs) ID3D11VertexShader_Release(old_vs);
    if (old_ps) ID3D11PixelShader_Release(old_ps);
    if (old_cb) ID3D11Buffer_Release(old_cb);
    if (old_layout) ID3D11InputLayout_Release(old_layout);
    if (old_raster) ID3D11RasterizerState_Release(old_raster);
    if (old_blend) ID3D11BlendState_Release(old_blend);
    if (old_depth) ID3D11DepthStencilState_Release(old_depth);
    return TRUE;
}
BOOL d3d8_CopyTextureToBackbuffer(ID3D11Texture2D *texture,
                                  ID3D11ShaderResourceView *srv,
                                  UINT width, UINT height)
{
    ID3D11Texture2D *back_buffer = NULL;
    ID3D11VertexShader *old_vs = NULL;
    ID3D11PixelShader *old_ps = NULL;
    ID3D11InputLayout *old_layout = NULL;
    ID3D11RasterizerState *old_raster = NULL;
    ID3D11SamplerState *old_sampler = NULL;
    ID3D11ShaderResourceView *old_srv = NULL;
    ID3D11BlendState *old_blend = NULL;
    ID3D11DepthStencilState *old_depth = NULL;
    ID3D11ShaderResourceView *null_srv = NULL;
    D3D11_PRIMITIVE_TOPOLOGY old_topology;
    D3D11_VIEWPORT viewport;
    FLOAT old_blend_factor[4];
    UINT old_sample_mask = ~0u;
    UINT old_stencil_ref = 0u;
    BOOL full_viewport;
    HRESULT hr;

    if (!texture || !srv || !g_device_state.swap_chain) return FALSE;
    hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                  &IID_ID3D11Texture2D, (void **)&back_buffer);
    if (FAILED(hr) || !back_buffer) return FALSE;
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 0, NULL, NULL);
    memset(&viewport, 0, sizeof(viewport));
    if ((uint64_t)g_device_state.width * g_presentation_aspect_height >
        (uint64_t)g_device_state.height * g_presentation_aspect_width) {
        viewport.Height = (FLOAT)g_device_state.height;
        viewport.Width = (FLOAT)((uint64_t)g_device_state.height *
                                 g_presentation_aspect_width /
                                 g_presentation_aspect_height);
        viewport.TopLeftX = ((FLOAT)g_device_state.width - viewport.Width) * 0.5f;
    } else {
        viewport.Width = (FLOAT)g_device_state.width;
        viewport.Height = (FLOAT)((uint64_t)g_device_state.width *
                                  g_presentation_aspect_height /
                                  g_presentation_aspect_width);
        viewport.TopLeftY = ((FLOAT)g_device_state.height - viewport.Height) * 0.5f;
    }
    viewport.MaxDepth = 1.0f;
    full_viewport = viewport.TopLeftX == 0.0f && viewport.TopLeftY == 0.0f &&
                    viewport.Width == (FLOAT)g_device_state.width &&
                    viewport.Height == (FLOAT)g_device_state.height;
    if (width == g_device_state.width && height == g_device_state.height &&
        full_viewport) {
        ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
            (ID3D11Resource *)back_buffer, (ID3D11Resource *)texture);
    } else {
        if (!d3d8_ensure_resolve_pipeline()) {
            ID3D11Texture2D_Release(back_buffer);
            ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
                1, &g_device_state.current_rtv, g_device_state.current_dsv);
            return FALSE;
        }
        ID3D11DeviceContext_VSGetShader(g_device_state.d3d11_context, &old_vs, NULL, NULL);
        ID3D11DeviceContext_PSGetShader(g_device_state.d3d11_context, &old_ps, NULL, NULL);
        ID3D11DeviceContext_IAGetInputLayout(g_device_state.d3d11_context, &old_layout);
        ID3D11DeviceContext_RSGetState(g_device_state.d3d11_context, &old_raster);
        ID3D11DeviceContext_IAGetPrimitiveTopology(g_device_state.d3d11_context, &old_topology);
        ID3D11DeviceContext_PSGetSamplers(g_device_state.d3d11_context, 0, 1, &old_sampler);
        ID3D11DeviceContext_PSGetShaderResources(g_device_state.d3d11_context,
                                                 0, 1, &old_srv);
        ID3D11DeviceContext_OMGetBlendState(g_device_state.d3d11_context,
                                            &old_blend, old_blend_factor,
                                            &old_sample_mask);
        ID3D11DeviceContext_OMGetDepthStencilState(g_device_state.d3d11_context,
                                                   &old_depth,
                                                   &old_stencil_ref);
        ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1,
                                               &g_device_state.default_rtv, NULL);
        {
            const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            ID3D11DeviceContext_ClearRenderTargetView(
                g_device_state.d3d11_context,
                g_device_state.default_rtv, black);
        }
        /* Resolve/present must not inherit the guest scissor state. */
        ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, NULL);
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &viewport);
        ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context,
                                                   D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context,
                                       g_device_state.resolve_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context,
                                       g_device_state.resolve_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(g_device_state.d3d11_context, 0, 1,
                                         &g_device_state.resolve_sampler);
        /* PCRTC scanout is an opaque RGB presentation operation. It must not
         * inherit the guest draw that happened to precede the flip (notably
         * the flare occlusion pass's source-alpha blend/depth state). */
        ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
                                            NULL, NULL, ~0u);
        ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
                                                   NULL, 0u);
        ID3D11DeviceContext_Draw(g_device_state.d3d11_context, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 1, &null_srv);
        ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context, old_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context, old_ps, NULL, 0);
        ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, old_layout);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, old_topology);
        ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, old_raster);
        ID3D11DeviceContext_PSSetSamplers(g_device_state.d3d11_context, 0, 1, &old_sampler);
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                                 0, 1, &old_srv);
        ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
                                            old_blend, old_blend_factor,
                                            old_sample_mask);
        ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
                                                   old_depth,
                                                   old_stencil_ref);
        if (old_vs) ID3D11VertexShader_Release(old_vs);
        if (old_ps) ID3D11PixelShader_Release(old_ps);
        if (old_layout) ID3D11InputLayout_Release(old_layout);
        if (old_raster) ID3D11RasterizerState_Release(old_raster);
        if (old_sampler) ID3D11SamplerState_Release(old_sampler);
        if (old_srv) ID3D11ShaderResourceView_Release(old_srv);
        if (old_blend) ID3D11BlendState_Release(old_blend);
        if (old_depth) ID3D11DepthStencilState_Release(old_depth);
    }
    ID3D11Texture2D_Release(back_buffer);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1,
        &g_device_state.current_rtv, g_device_state.current_dsv);
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)g_device_state.current_target_physical_width;
    viewport.Height = (FLOAT)g_device_state.current_target_physical_height;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &viewport);
    return TRUE;
}

typedef struct D3D8PvideoConstants {
    FLOAT source_uv[4];
    FLOAT background_pixels[4];
    FLOAT color_key[4];
    FLOAT background_size[4];
} D3D8PvideoConstants;

static BOOL d3d8_ensure_pvideo_pipeline(UINT width, UINT height)
{
    static const char ps_source[] =
        "Texture2D video_texture : register(t0);"
        "Texture2D background_texture : register(t1);"
        "SamplerState video_sampler : register(s0);"
        "cbuffer PvideoConstants : register(b0) {"
        " float4 source_uv; float4 background_pixels;"
        " float4 color_key; float4 background_size; };"
        "float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0)"
        " : SV_Target {"
        " if (color_key.w > 0.5) {"
        "  int2 p=int2(background_pixels.xy + uv * background_pixels.zw);"
        "  p=clamp(p, int2(0,0), int2(background_size.xy)-1);"
        "  uint3 actual=(uint3)round(saturate(background_texture.Load(int3(p,0)).rgb)*255.0);"
        "  uint3 wanted=(uint3)round(saturate(color_key.rgb)*255.0);"
        "  if (any(actual != wanted)) discard;"
        " }"
        " return video_texture.Sample(video_sampler, source_uv.xy + uv * source_uv.zw);"
        "}";
    D3D11_BUFFER_DESC cb_desc;
    D3D11_TEXTURE2D_DESC texture_desc;
    ID3DBlob *ps_blob = NULL, *errors = NULL;
    HRESULT hr;
    if (!width || !height || !d3d8_ensure_resolve_pipeline()) return FALSE;
    if (!g_device_state.pvideo_ps) {
        hr = d3d8_compile_shader(ps_source, sizeof(ps_source) - 1u, "nv2a_pvideo_ps",
                        NULL, NULL, "main", "ps_5_0", 0, 0,
                        &ps_blob, &errors);
        if (FAILED(hr)) {
            fprintf(stderr, "D3D8: PVIDEO PS compile failed: %s\n",
                    errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
            if (errors) ID3D10Blob_Release(errors);
            return FALSE;
        }
        if (errors) ID3D10Blob_Release(errors);
        hr = ID3D11Device_CreatePixelShader(g_device_state.d3d11_device,
            ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob),
            NULL, &g_device_state.pvideo_ps);
        ID3D10Blob_Release(ps_blob);
        if (FAILED(hr)) return FALSE;
    }
    if (!g_device_state.pvideo_cb) {
        memset(&cb_desc, 0, sizeof(cb_desc));
        cb_desc.ByteWidth = sizeof(D3D8PvideoConstants);
        cb_desc.Usage = D3D11_USAGE_DEFAULT;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(ID3D11Device_CreateBuffer(g_device_state.d3d11_device,
                &cb_desc, NULL, &g_device_state.pvideo_cb))) return FALSE;
    }
    if (g_device_state.pvideo_texture &&
        (g_device_state.pvideo_width != width || g_device_state.pvideo_height != height)) {
        ID3D11ShaderResourceView_Release(g_device_state.pvideo_srv);
        ID3D11Texture2D_Release(g_device_state.pvideo_texture);
        g_device_state.pvideo_srv = NULL;
        g_device_state.pvideo_texture = NULL;
        g_device_state.pvideo_width = g_device_state.pvideo_height = 0u;
    }
    if (!g_device_state.pvideo_texture) {
        memset(&texture_desc, 0, sizeof(texture_desc));
        texture_desc.Width = width; texture_desc.Height = height;
        texture_desc.MipLevels = texture_desc.ArraySize = 1u;
        texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture_desc.SampleDesc.Count = 1u;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_device_state.d3d11_device,
                &texture_desc, NULL, &g_device_state.pvideo_texture))) return FALSE;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_device_state.d3d11_device,
                (ID3D11Resource *)g_device_state.pvideo_texture, NULL,
                &g_device_state.pvideo_srv))) {
            ID3D11Texture2D_Release(g_device_state.pvideo_texture);
            g_device_state.pvideo_texture = NULL;
            return FALSE;
        }
        g_device_state.pvideo_width = width;
        g_device_state.pvideo_height = height;
    }
    return TRUE;
}

static BOOL d3d8_composite_overlay(
    const void *bgra_pixels, UINT source_width, UINT source_height,
    UINT source_pitch, FLOAT source_x, FLOAT source_y,
    FLOAT source_step_x, FLOAT source_step_y,
    UINT output_x, UINT output_y, UINT output_width, UINT output_height,
    UINT logical_display_width, UINT logical_display_height,
    ID3D11ShaderResourceView *background_srv,
    UINT background_width, UINT background_height,
    BOOL color_key_enabled, DWORD color_key, BOOL alpha_blend)
{
    D3D8PvideoConstants c;
    ID3D11VertexShader *old_vs = NULL;
    ID3D11PixelShader *old_ps = NULL;
    ID3D11Buffer *old_cb = NULL;
    ID3D11InputLayout *old_layout = NULL;
    ID3D11RasterizerState *old_raster = NULL;
    ID3D11SamplerState *old_sampler = NULL;
    ID3D11ShaderResourceView *old_srvs[2] = { NULL, NULL };
    ID3D11ShaderResourceView *new_srvs[2], *null_srvs[2] = { NULL, NULL };
    ID3D11BlendState *old_blend = NULL;
    ID3D11DepthStencilState *old_depth = NULL;
    D3D11_PRIMITIVE_TOPOLOGY old_topology;
    D3D11_VIEWPORT old_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT old_viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    FLOAT old_blend_factor[4]; UINT old_sample_mask, old_stencil_ref;
    D3D11_VIEWPORT content, overlay;
    if (!bgra_pixels || source_pitch < source_width * 4u ||
        !source_width || !source_height || !output_width || !output_height ||
        !logical_display_width || !logical_display_height ||
        (color_key_enabled && (!background_srv || !background_width || !background_height)) ||
        !g_device_state.swap_chain || !g_device_state.default_rtv ||
        !d3d8_ensure_pvideo_pipeline(source_width, source_height)) return FALSE;
    if (alpha_blend && !g_device_state.host_overlay_blend) {
        D3D11_BLEND_DESC blend = {0};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(ID3D11Device_CreateBlendState(g_device_state.d3d11_device,
                &blend, &g_device_state.host_overlay_blend))) return FALSE;
    }
    ID3D11DeviceContext_UpdateSubresource(g_device_state.d3d11_context,
        (ID3D11Resource *)g_device_state.pvideo_texture, 0, NULL,
        bgra_pixels, source_pitch, 0);
    memset(&c, 0, sizeof(c));
    c.source_uv[0] = source_x / source_width; c.source_uv[1] = source_y / source_height;
    c.source_uv[2] = output_width * source_step_x / source_width;
    c.source_uv[3] = output_height * source_step_y / source_height;
    c.background_pixels[0] = (FLOAT)output_x * background_width / logical_display_width;
    c.background_pixels[1] = (FLOAT)output_y * background_height / logical_display_height;
    c.background_pixels[2] = (FLOAT)output_width * background_width / logical_display_width;
    c.background_pixels[3] = (FLOAT)output_height * background_height / logical_display_height;
    c.color_key[0] = (FLOAT)((color_key >> 16) & 0xFFu) / 255.0f;
    c.color_key[1] = (FLOAT)((color_key >> 8) & 0xFFu) / 255.0f;
    c.color_key[2] = (FLOAT)(color_key & 0xFFu) / 255.0f;
    c.color_key[3] = color_key_enabled ? 1.0f : 0.0f;
    c.background_size[0] = (FLOAT)background_width;
    c.background_size[1] = (FLOAT)background_height;
    ID3D11DeviceContext_UpdateSubresource(g_device_state.d3d11_context,
        (ID3D11Resource *)g_device_state.pvideo_cb, 0, NULL, &c, 0, 0);
    memset(&content, 0, sizeof(content));
    if ((uint64_t)g_device_state.width * g_presentation_aspect_height >
        (uint64_t)g_device_state.height * g_presentation_aspect_width) {
        content.Height = (FLOAT)g_device_state.height;
        content.Width = (FLOAT)((uint64_t)g_device_state.height * g_presentation_aspect_width / g_presentation_aspect_height);
        content.TopLeftX = ((FLOAT)g_device_state.width - content.Width) * 0.5f;
    } else {
        content.Width = (FLOAT)g_device_state.width;
        content.Height = (FLOAT)((uint64_t)g_device_state.width * g_presentation_aspect_height / g_presentation_aspect_width);
        content.TopLeftY = ((FLOAT)g_device_state.height - content.Height) * 0.5f;
    }
    content.MaxDepth = 1.0f; memset(&overlay, 0, sizeof(overlay));
    overlay.TopLeftX = content.TopLeftX + content.Width * output_x / logical_display_width;
    overlay.TopLeftY = content.TopLeftY + content.Height * output_y / logical_display_height;
    overlay.Width = content.Width * output_width / logical_display_width;
    overlay.Height = content.Height * output_height / logical_display_height;
    overlay.MaxDepth = 1.0f;
    ID3D11DeviceContext_VSGetShader(g_device_state.d3d11_context, &old_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_device_state.d3d11_context, &old_ps, NULL, NULL);
    ID3D11DeviceContext_PSGetConstantBuffers(g_device_state.d3d11_context, 0, 1, &old_cb);
    ID3D11DeviceContext_IAGetInputLayout(g_device_state.d3d11_context, &old_layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_device_state.d3d11_context, &old_topology);
    ID3D11DeviceContext_RSGetState(g_device_state.d3d11_context, &old_raster);
    ID3D11DeviceContext_RSGetViewports(g_device_state.d3d11_context, &old_viewport_count, old_viewports);
    ID3D11DeviceContext_PSGetSamplers(g_device_state.d3d11_context, 0, 1, &old_sampler);
    ID3D11DeviceContext_PSGetShaderResources(g_device_state.d3d11_context, 0, 2, old_srvs);
    ID3D11DeviceContext_OMGetBlendState(g_device_state.d3d11_context, &old_blend, old_blend_factor, &old_sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_device_state.d3d11_context, &old_depth, &old_stencil_ref);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 2, null_srvs);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 0, NULL, NULL);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1, &g_device_state.default_rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
        alpha_blend ? g_device_state.host_overlay_blend : NULL, NULL, ~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context, NULL, 0u);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, NULL);
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &overlay);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context, g_device_state.resolve_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context, g_device_state.pvideo_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_device_state.d3d11_context, 0, 1, &g_device_state.pvideo_cb);
    new_srvs[0] = g_device_state.pvideo_srv; new_srvs[1] = background_srv;
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 2, new_srvs);
    ID3D11DeviceContext_PSSetSamplers(g_device_state.d3d11_context, 0, 1, &g_device_state.resolve_sampler);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 2, null_srvs);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1, &g_device_state.current_rtv, g_device_state.current_dsv);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context, old_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context, old_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_device_state.d3d11_context, 0, 1, &old_cb);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, old_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, old_topology);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, old_raster);
    if (old_viewport_count) ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, old_viewport_count, old_viewports);
    ID3D11DeviceContext_PSSetSamplers(g_device_state.d3d11_context, 0, 1, &old_sampler);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 2, old_srvs);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context, old_blend, old_blend_factor, old_sample_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context, old_depth, old_stencil_ref);
    if (old_vs) ID3D11VertexShader_Release(old_vs); if (old_ps) ID3D11PixelShader_Release(old_ps);
    if (old_cb) ID3D11Buffer_Release(old_cb); if (old_layout) ID3D11InputLayout_Release(old_layout);
    if (old_raster) ID3D11RasterizerState_Release(old_raster); if (old_sampler) ID3D11SamplerState_Release(old_sampler);
    if (old_srvs[0]) ID3D11ShaderResourceView_Release(old_srvs[0]); if (old_srvs[1]) ID3D11ShaderResourceView_Release(old_srvs[1]);
    if (old_blend) ID3D11BlendState_Release(old_blend); if (old_depth) ID3D11DepthStencilState_Release(old_depth);
    return TRUE;
}

BOOL d3d8_CompositeVideoOverlay(
    const void *bgra_pixels, UINT source_width, UINT source_height,
    UINT source_pitch, FLOAT source_x, FLOAT source_y,
    FLOAT source_step_x, FLOAT source_step_y,
    UINT output_x, UINT output_y, UINT output_width, UINT output_height,
    UINT logical_display_width, UINT logical_display_height,
    ID3D11ShaderResourceView *background_srv,
    UINT background_width, UINT background_height,
    BOOL color_key_enabled, DWORD color_key)
{
    return d3d8_composite_overlay(bgra_pixels, source_width, source_height, source_pitch,
        source_x, source_y, source_step_x, source_step_y, output_x, output_y,
        output_width, output_height, logical_display_width, logical_display_height,
        background_srv, background_width, background_height, color_key_enabled, color_key, FALSE);
}

/* Host diagnostics use premultiplied alpha; retail video remains opaque. */
BOOL d3d8_CompositeHostOverlay(const void *pixels, UINT width, UINT height,
    UINT x, UINT y, UINT logical_width, UINT logical_height)
{
    return d3d8_composite_overlay(pixels, width, height, width*4, 0, 0, 1, 1,
        x, y, width, height, logical_width, logical_height, NULL, 0, 0, FALSE, 0, TRUE);
}

BOOL d3d8_CopyTextureToRenderTarget(ID3D11ShaderResourceView *srv,
                                    ID3D11RenderTargetView *rtv,
                                    UINT width, UINT height)
{
    ID3D11VertexShader *old_vs = NULL;
    ID3D11PixelShader *old_ps = NULL;
    ID3D11InputLayout *old_layout = NULL;
    ID3D11RasterizerState *old_raster = NULL;
    ID3D11SamplerState *old_sampler = NULL;
    ID3D11BlendState *old_blend = NULL;
    ID3D11DepthStencilState *old_depth = NULL;
    ID3D11ShaderResourceView *null_srvs[4] = { NULL, NULL, NULL, NULL };
    D3D11_PRIMITIVE_TOPOLOGY old_topology;
    D3D11_VIEWPORT viewport;
    FLOAT old_blend_factor[4];
    UINT old_sample_mask, old_stencil_ref;

    if (!srv || !rtv || !width || !height || !d3d8_ensure_resolve_pipeline())
        return FALSE;
    ID3D11DeviceContext_VSGetShader(g_device_state.d3d11_context, &old_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_device_state.d3d11_context, &old_ps, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(g_device_state.d3d11_context, &old_layout);
    ID3D11DeviceContext_RSGetState(g_device_state.d3d11_context, &old_raster);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_device_state.d3d11_context, &old_topology);
    ID3D11DeviceContext_PSGetSamplers(g_device_state.d3d11_context, 0, 1, &old_sampler);
    ID3D11DeviceContext_OMGetBlendState(g_device_state.d3d11_context, &old_blend,
                                        old_blend_factor, &old_sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_device_state.d3d11_context,
                                               &old_depth, &old_stencil_ref);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 0, NULL, NULL);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 4, null_srvs);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context, NULL, NULL, ~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context, NULL, 0u);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, NULL);
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)width;
    viewport.Height = (FLOAT)height;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context,
                                               D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context,
                                   g_device_state.resolve_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context,
                                   g_device_state.resolve_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(g_device_state.d3d11_context, 0, 1,
                                     &g_device_state.resolve_sampler);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context, 0, 4, null_srvs);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context, old_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context, old_ps, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, old_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, old_topology);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, old_raster);
    ID3D11DeviceContext_PSSetSamplers(g_device_state.d3d11_context, 0, 1, &old_sampler);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context, old_blend,
                                        old_blend_factor, old_sample_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
                                               old_depth, old_stencil_ref);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1,
        &g_device_state.current_rtv, g_device_state.current_dsv);
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)g_device_state.current_target_physical_width;
    viewport.Height = (FLOAT)g_device_state.current_target_physical_height;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &viewport);
    if (old_vs) ID3D11VertexShader_Release(old_vs);
    if (old_ps) ID3D11PixelShader_Release(old_ps);
    if (old_layout) ID3D11InputLayout_Release(old_layout);
    if (old_raster) ID3D11RasterizerState_Release(old_raster);
    if (old_sampler) ID3D11SamplerState_Release(old_sampler);
    if (old_blend) ID3D11BlendState_Release(old_blend);
    if (old_depth) ID3D11DepthStencilState_Release(old_depth);
    return TRUE;
}
BOOL d3d8_CopyScaledDepthStencilAlias(ID3D11Texture2D *source,
                                      ID3D11ShaderResourceView *source_depth_srv,
                                      ID3D11ShaderResourceView *source_stencil_srv,
                                      UINT source_width, UINT source_height,
                                      ID3D11DepthStencilView *destination_dsv,
                                      ID3D11RenderTargetView *alias_color_rtv,
                                      UINT destination_width,
                                      UINT destination_height,
                                      BOOL uniform_stencil_known,
                                      UINT uniform_stencil,
                                      UINT possible_stencil_bits,
                                      BOOL destination_stencil_matches)
{
    ID3D11VertexShader *old_vs = NULL;
    ID3D11PixelShader *old_ps = NULL;
    ID3D11Buffer *old_cb = NULL;
    ID3D11InputLayout *old_layout = NULL;
    ID3D11RasterizerState *old_raster = NULL;
    ID3D11BlendState *old_blend = NULL;
    ID3D11DepthStencilState *old_depth = NULL;
    ID3D11ShaderResourceView *old_srvs[2] = { NULL, NULL };
    ID3D11ShaderResourceView *new_srvs[2];
    ID3D11ShaderResourceView *null_srvs[2] = { NULL, NULL };
    ID3D11RenderTargetView *old_rtv = NULL;
    ID3D11DepthStencilView *old_dsv = NULL;
    D3D11_PRIMITIVE_TOPOLOGY old_topology;
    D3D11_VIEWPORT old_viewports[16];
    D3D11_VIEWPORT viewport;
    UINT old_viewport_count = 16u;
    FLOAT old_blend_factor[4];
    UINT old_sample_mask, old_stencil_ref;
    UINT dimensions[4];
    BOOL use_uniform_stencil;
    BOOL use_shader_stencil = FALSE;
    BOOL use_stencil_bit_passes = FALSE;
    UINT stencil_bit_pass_count = 0u;
    const BOOL color_only = destination_dsv == NULL;

    if (!source || !source_width || !source_height ||
        !alias_color_rtv || !destination_width || !destination_height)
        return FALSE;
    /* A sampled Xbox zeta allocation needs only its packed-color view.  In
     * that case there is no destination DSV to update, and the uniform
     * pipeline is preferable because it preserves the packed stencil byte in
     * color without requiring SV_StencilRef support. */
    use_uniform_stencil = (color_only || uniform_stencil_known) &&
        d3d8_ensure_depth_alias_uniform_pipeline();
    if (!use_uniform_stencil) {
        use_shader_stencil = d3d8_ensure_depth_alias_pipeline();
        if (!use_shader_stencil) {
            if (!d3d8_ensure_depth_alias_stencil_bit_pipeline())
                return FALSE;
            use_stencil_bit_passes = TRUE;
        }
    }
    if ((!source_depth_srv || !source_stencil_srv) &&
        !d3d8_ensure_depth_alias_source_views(source_width, source_height))
        return FALSE;
    if (!source_depth_srv || !source_stencil_srv) {
        ID3D11DeviceContext_CopyResource(g_device_state.d3d11_context,
            (ID3D11Resource *)g_device_state.depth_alias_source_texture,
            (ID3D11Resource *)source);
        source_depth_srv = g_device_state.depth_alias_source_depth_srv;
        source_stencil_srv = g_device_state.depth_alias_source_stencil_srv;
    }
    dimensions[0] = source_width;
    dimensions[1] = source_height;
    dimensions[2] = destination_width;
    dimensions[3] = destination_height;
    ID3D11DeviceContext_UpdateSubresource(g_device_state.d3d11_context,
        (ID3D11Resource *)g_device_state.depth_alias_cb, 0u, NULL,
        dimensions, 0u, 0u);

    ID3D11DeviceContext_VSGetShader(g_device_state.d3d11_context,
                                    &old_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_device_state.d3d11_context,
                                    &old_ps, NULL, NULL);
    ID3D11DeviceContext_PSGetConstantBuffers(g_device_state.d3d11_context,
                                             0, 1, &old_cb);
    ID3D11DeviceContext_IAGetInputLayout(g_device_state.d3d11_context,
                                         &old_layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_device_state.d3d11_context,
                                               &old_topology);
    ID3D11DeviceContext_RSGetState(g_device_state.d3d11_context, &old_raster);
    ID3D11DeviceContext_RSGetViewports(g_device_state.d3d11_context,
                                       &old_viewport_count, old_viewports);
    ID3D11DeviceContext_PSGetShaderResources(g_device_state.d3d11_context,
                                             0, 2, old_srvs);
    ID3D11DeviceContext_OMGetRenderTargets(g_device_state.d3d11_context,
                                           1, &old_rtv, &old_dsv);
    ID3D11DeviceContext_OMGetBlendState(g_device_state.d3d11_context,
        &old_blend, old_blend_factor, &old_sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_device_state.d3d11_context,
                                               &old_depth, &old_stencil_ref);

    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 2, null_srvs);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
                                           0, NULL, NULL);
    if (!color_only && use_uniform_stencil && !destination_stencil_matches) {
        ID3D11DeviceContext_ClearDepthStencilView(
            g_device_state.d3d11_context, destination_dsv,
            D3D11_CLEAR_STENCIL, 1.0f, (UINT8)(uniform_stencil & 0xFFu));
    } else if (!color_only && use_stencil_bit_passes) {
        ID3D11DeviceContext_ClearDepthStencilView(
            g_device_state.d3d11_context, destination_dsv,
            D3D11_CLEAR_STENCIL, 1.0f, 0u);
    }
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
        1, &alias_color_rtv, destination_dsv);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
                                        NULL, NULL, ~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
        color_only ? NULL :
        use_shader_stencil ? g_device_state.packed_color_depth_state :
                             g_device_state.depth_alias_uniform_state,
        0u);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, NULL);
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)destination_width;
    viewport.Height = (FLOAT)destination_height;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context,
                                       1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context,
                                   g_device_state.resolve_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context,
        use_shader_stencil ? g_device_state.depth_alias_ps :
                             g_device_state.depth_alias_uniform_ps,
        NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_device_state.d3d11_context,
        0, 1, &g_device_state.depth_alias_cb);
    new_srvs[0] = source_depth_srv;
    new_srvs[1] = source_stencil_srv;
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 2, new_srvs);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, 3, 0);

    if (!color_only && use_stencil_bit_passes) {
        UINT bit;

        ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
                                               0, NULL, destination_dsv);
        for (bit = 0u; bit < 8u; ++bit) {
            if ((possible_stencil_bits & (1u << bit)) == 0u)
                continue;
            ++stencil_bit_pass_count;
            ID3D11DeviceContext_OMSetDepthStencilState(
                g_device_state.d3d11_context,
                g_device_state.depth_alias_stencil_bit_state[bit],
                1u << bit);
            ID3D11DeviceContext_PSSetShader(
                g_device_state.d3d11_context,
                g_device_state.depth_alias_stencil_bit_ps[bit], NULL, 0);
            ID3D11DeviceContext_Draw(g_device_state.d3d11_context, 3, 0);
        }
        if (d3d8_cached_getenv("MERCENARIES_TRACE_DEPTH_ALIAS_STENCIL") != NULL) {
            static UINT trace_count;
            if (trace_count++ < 64u) {
                fprintf(stderr,
                        "[D3D-DEPTH-ALIAS-BITS] possible=%02X passes=%u "
                        "size=%ux%u->%ux%u\n",
                        possible_stencil_bits & 0xFFu, stencil_bit_pass_count,
                        source_width, source_height,
                        destination_width, destination_height);
            }
        }
    }

    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 2, null_srvs);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
                                           1, &old_rtv, old_dsv);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context,
                                   old_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context,
                                   old_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_device_state.d3d11_context,
                                             0, 1, &old_cb);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context,
                                         old_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context,
                                               old_topology);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, old_raster);
    if (old_viewport_count)
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context,
                                           old_viewport_count, old_viewports);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 2, old_srvs);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
        old_blend, old_blend_factor, old_sample_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
                                               old_depth, old_stencil_ref);
    if (old_vs) ID3D11VertexShader_Release(old_vs);
    if (old_ps) ID3D11PixelShader_Release(old_ps);
    if (old_cb) ID3D11Buffer_Release(old_cb);
    if (old_layout) ID3D11InputLayout_Release(old_layout);
    if (old_raster) ID3D11RasterizerState_Release(old_raster);
    if (old_srvs[0]) ID3D11ShaderResourceView_Release(old_srvs[0]);
    if (old_srvs[1]) ID3D11ShaderResourceView_Release(old_srvs[1]);
    if (old_rtv) ID3D11RenderTargetView_Release(old_rtv);
    if (old_dsv) ID3D11DepthStencilView_Release(old_dsv);
    if (old_blend) ID3D11BlendState_Release(old_blend);
    if (old_depth) ID3D11DepthStencilState_Release(old_depth);
    return TRUE;
}
BOOL d3d8_CopyPackedColorToDepthStencil(ID3D11ShaderResourceView *srv,
                                        ID3D11DepthStencilView *dsv,
                                        UINT width, UINT height)
{
    ID3D11VertexShader *old_vs = NULL;
    ID3D11PixelShader *old_ps = NULL;
    ID3D11InputLayout *old_layout = NULL;
    ID3D11RasterizerState *old_raster = NULL;
    ID3D11BlendState *old_blend = NULL;
    ID3D11DepthStencilState *old_depth = NULL;
    ID3D11ShaderResourceView *old_srv = NULL;
    ID3D11RenderTargetView *old_rtv = NULL;
    ID3D11DepthStencilView *old_dsv = NULL;
    ID3D11ShaderResourceView *null_srv = NULL;
    D3D11_PRIMITIVE_TOPOLOGY old_topology;
    D3D11_VIEWPORT old_viewports[16];
    D3D11_VIEWPORT viewport;
    UINT old_viewport_count = 16u;
    FLOAT old_blend_factor[4];
    UINT old_sample_mask, old_stencil_ref;
    BOOL use_shader_stencil;
    BOOL use_stencil_bit_passes = FALSE;
    UINT bit;

    if (!srv || !dsv || !width || !height)
        return FALSE;
    use_shader_stencil = d3d8_ensure_packed_color_depth_pipeline();
    if (!use_shader_stencil) {
        if (!d3d8_ensure_packed_color_depth_fallback_pipeline())
            return FALSE;
        use_stencil_bit_passes = TRUE;
    }
    ID3D11DeviceContext_VSGetShader(g_device_state.d3d11_context,
                                    &old_vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(g_device_state.d3d11_context,
                                    &old_ps, NULL, NULL);
    ID3D11DeviceContext_IAGetInputLayout(g_device_state.d3d11_context,
                                         &old_layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(g_device_state.d3d11_context,
                                               &old_topology);
    ID3D11DeviceContext_RSGetState(g_device_state.d3d11_context, &old_raster);
    ID3D11DeviceContext_RSGetViewports(g_device_state.d3d11_context,
                                       &old_viewport_count, old_viewports);
    ID3D11DeviceContext_PSGetShaderResources(g_device_state.d3d11_context,
                                             0, 1, &old_srv);
    ID3D11DeviceContext_OMGetRenderTargets(g_device_state.d3d11_context,
                                           1, &old_rtv, &old_dsv);
    ID3D11DeviceContext_OMGetBlendState(g_device_state.d3d11_context,
        &old_blend, old_blend_factor, &old_sample_mask);
    ID3D11DeviceContext_OMGetDepthStencilState(g_device_state.d3d11_context,
                                               &old_depth, &old_stencil_ref);

    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 1, &null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
                                           0, NULL, NULL);
    if (use_stencil_bit_passes)
        ID3D11DeviceContext_ClearDepthStencilView(
            g_device_state.d3d11_context, dsv, D3D11_CLEAR_STENCIL,
            1.0f, 0u);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
                                           0, NULL, dsv);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
                                        NULL, NULL, ~0u);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
        use_shader_stencil ? g_device_state.packed_color_depth_state :
                             g_device_state.packed_color_depth_fallback_state,
        0u);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, NULL);
    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (FLOAT)width;
    viewport.Height = (FLOAT)height;
    viewport.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context,
                                       1, &viewport);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context,
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context,
                                   g_device_state.resolve_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context,
                                   use_shader_stencil ?
                                       g_device_state.packed_color_depth_ps :
                                       g_device_state.packed_color_depth_fallback_ps,
                                   NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 1, &srv);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, 3, 0);

    if (use_stencil_bit_passes) {
        for (bit = 0u; bit < 8u; ++bit) {
            ID3D11DeviceContext_OMSetDepthStencilState(
                g_device_state.d3d11_context,
                g_device_state.packed_color_stencil_bit_state[bit],
                1u << bit);
            ID3D11DeviceContext_PSSetShader(
                g_device_state.d3d11_context,
                g_device_state.packed_color_stencil_bit_ps[bit], NULL, 0);
            ID3D11DeviceContext_Draw(g_device_state.d3d11_context, 3, 0);
        }
    }

    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 1, &null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context,
                                           1, &old_rtv, old_dsv);
    ID3D11DeviceContext_VSSetShader(g_device_state.d3d11_context,
                                   old_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_device_state.d3d11_context,
                                   old_ps, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(g_device_state.d3d11_context,
                                         old_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context,
                                               old_topology);
    ID3D11DeviceContext_RSSetState(g_device_state.d3d11_context, old_raster);
    if (old_viewport_count)
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context,
                                           old_viewport_count, old_viewports);
    ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                                             0, 1, &old_srv);
    ID3D11DeviceContext_OMSetBlendState(g_device_state.d3d11_context,
        old_blend, old_blend_factor, old_sample_mask);
    ID3D11DeviceContext_OMSetDepthStencilState(g_device_state.d3d11_context,
                                               old_depth, old_stencil_ref);
    if (old_vs) ID3D11VertexShader_Release(old_vs);
    if (old_ps) ID3D11PixelShader_Release(old_ps);
    if (old_layout) ID3D11InputLayout_Release(old_layout);
    if (old_raster) ID3D11RasterizerState_Release(old_raster);
    if (old_srv) ID3D11ShaderResourceView_Release(old_srv);
    if (old_rtv) ID3D11RenderTargetView_Release(old_rtv);
    if (old_dsv) ID3D11DepthStencilView_Release(old_dsv);
    if (old_blend) ID3D11BlendState_Release(old_blend);
    if (old_depth) ID3D11DepthStencilState_Release(old_depth);
    return TRUE;
}
const DWORD         *d3d8_GetRenderStates(void) { return g_device_state.render_states; }
const DWORD         *d3d8_GetTSS(DWORD stage) { return (stage < MAX_TEXTURE_STAGES) ? g_device_state.tss[stage] : NULL; }
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) {
    return ((DWORD)type < MAX_TRANSFORMS) ? &g_device_state.transforms[(DWORD)type] : NULL;
}

const D3DLIGHT8     *d3d8_GetLight(DWORD index) {
    return (index < MAX_LIGHTS) ? &g_device_state.lights[index] : NULL;
}

BOOL                 d3d8_GetLightEnable(DWORD index) {
    return (index < MAX_LIGHTS) ? g_device_state.light_enable[index] : FALSE;
}

const D3DMATERIAL8  *d3d8_GetMaterial(void) {
    return &g_device_state.material;
}

UINT                 d3d8_GetNumLights(void) {
    return MAX_LIGHTS;
}

/* ================================================================
 * D3D11 initialization helpers
 * ================================================================ */

/* Native Windows benefits from direct compositor buffers. Retain the
 * proven compatibility path under Wine unless a diagnostic explicitly asks
 * to test flip; the rollback switch always takes precedence. */
static BOOL d3d8_flip_presentation_requested(void)
{
    if (d3d8_cached_getenv("MERCENARIES_DISABLE_FLIP_PRESENT") != NULL)
        return FALSE;
    if (d3d8_cached_getenv("MERCENARIES_TEST_FLIP_PRESENT") != NULL)
        return TRUE;
    return GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") == NULL;
}

static HRESULT d3d11_try_device_and_swap_chain(
    D3D8DeviceState *state,
    D3DPRESENT_PARAMETERS *pp,
    IDXGIAdapter *selected_adapter)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    UINT create_flags = 0;
    HRESULT hr;

#ifdef _DEBUG
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = pp->BackBufferCount ? pp->BackBufferCount : 1;
    scd.BufferDesc.Width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    scd.BufferDesc.Height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = pp->hDeviceWindow;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = pp->Windowed;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    /* Require modern DXGI's unthrottled flip capability; older systems keep
     * the existing blit chain. The guest's 30/60 Hz pacing remains separate. */
    if (d3d8_flip_presentation_requested()) {
        IDXGIFactory5 *factory = NULL;
        BOOL tearing = FALSE;
        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory5, (void **)&factory))) {
            if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(factory,
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing) {
                scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                scd.BufferCount = 2u;
                scd.Windowed = TRUE;
                /* Keep compositor-synchronized presentation by default. The
                 * old windowed blit path did not explicitly permit tearing. */
                if (d3d8_cached_getenv("MERCENARIES_TEST_FLIP_TEARING") != NULL)
                    scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
            }
            IDXGIFactory5_Release(factory);
        }
    }

    hr = D3D11CreateDeviceAndSwapChain(
        selected_adapter,
        selected_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        create_flags,
        NULL, 0,
        D3D11_SDK_VERSION,
        &scd,
        &state->swap_chain,
        &state->d3d11_device,
        &feature_level,
        &state->d3d11_context
    );

    /* Flip chains enter exclusive mode after creation, then resize before
     * their first Present. No back-buffer views exist at this point. */
    if (SUCCEEDED(hr) && scd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD && !pp->Windowed) {
        hr = IDXGISwapChain_SetFullscreenState(state->swap_chain, TRUE, NULL);
        if (SUCCEEDED(hr))
            hr = IDXGISwapChain_ResizeBuffers(state->swap_chain, 0u,
                scd.BufferDesc.Width, scd.BufferDesc.Height, scd.BufferDesc.Format, scd.Flags);
    }
    if (FAILED(hr) && scd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD) {
        if (state->swap_chain) { IDXGISwapChain_SetFullscreenState(state->swap_chain, FALSE, NULL); IDXGISwapChain_Release(state->swap_chain); state->swap_chain = NULL; }
        if (state->d3d11_context) { ID3D11DeviceContext_Release(state->d3d11_context); state->d3d11_context = NULL; }
        if (state->d3d11_device) { ID3D11Device_Release(state->d3d11_device); state->d3d11_device = NULL; }
        xbox_preview_log_event("graphics", "flip creation failed HRESULT=%08lX; retaining blit presentation", hr);
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        scd.BufferCount = pp->BackBufferCount ? pp->BackBufferCount : 1u;
        scd.Flags = 0u;
        scd.Windowed = pp->Windowed;
        hr = D3D11CreateDeviceAndSwapChain(selected_adapter,
            selected_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            NULL, create_flags, NULL, 0, D3D11_SDK_VERSION, &scd,
            &state->swap_chain, &state->d3d11_device, &feature_level, &state->d3d11_context);
    }
    if (FAILED(hr)) {
        /* A failed adapter attempt must leave no objects behind before the
         * next adapter is tried, including partial outputs from a driver. */
        if (state->swap_chain) { IDXGISwapChain_SetFullscreenState(state->swap_chain, FALSE, NULL); IDXGISwapChain_Release(state->swap_chain); state->swap_chain = NULL; }
        if (state->d3d11_context) { ID3D11DeviceContext_Release(state->d3d11_context); state->d3d11_context = NULL; }
        if (state->d3d11_device) { ID3D11Device_Release(state->d3d11_device); state->d3d11_device = NULL; }
        return hr;
    }

    {
        IDXGIDevice *dxgi = NULL; IDXGIAdapter *adapter = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(state->d3d11_device,&IID_IDXGIDevice,(void**)&dxgi))) {
            if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi,&adapter))) {
                DXGI_ADAPTER_DESC description;
                if (SUCCEEDED(IDXGIAdapter_GetDesc(adapter,&description)))
                    xbox_preview_log_event("graphics", "adapter=%ls vendor=%04X device=%04X vram_mb=%llu feature=%X output=%ux%u",
                        description.Description,description.VendorId,description.DeviceId,
                        (unsigned long long)(description.DedicatedVideoMemory/(1024*1024)),
                        feature_level,scd.BufferDesc.Width,scd.BufferDesc.Height);
                IDXGIAdapter_Release(adapter);
            }
            IDXGIDevice_Release(dxgi);
        }
    }
    /* Options own display changes. Disable DXGI's independent Alt+Enter path. */
    { IDXGIFactory *factory=NULL;
      if(SUCCEEDED(IDXGISwapChain_GetParent(state->swap_chain,&IID_IDXGIFactory,(void**)&factory))){
        IDXGIFactory_MakeWindowAssociation(factory,pp->hDeviceWindow,DXGI_MWA_NO_ALT_ENTER);
        IDXGIFactory_Release(factory);
      }
    }
    state->flip_model = scd.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD;
    state->swap_chain_flags = scd.Flags;
    xbox_preview_log_event("graphics", "presentation=%s buffers=%u flags=%08X",
        state->flip_model ? "flip-discard" : "blit-discard", scd.BufferCount, scd.Flags);
    state->hwnd = pp->hDeviceWindow;
    state->width = scd.BufferDesc.Width;
    state->height = scd.BufferDesc.Height;

    return S_OK;
}

/* DXGI ranks external/discrete GPUs ahead of integrated GPUs for this
 * preference. Do not guess from vendor IDs or dedicated-memory sizes: Intel
 * also makes discrete GPUs and integrated GPUs can report reserved VRAM.
 * Older DXGI implementations retain the default-adapter path. */
static HRESULT d3d11_create_device_and_swap_chain(
    D3D8DeviceState *state,
    D3DPRESENT_PARAMETERS *pp)
{
    IDXGIFactory6 *factory = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory6, (void **)&factory);
    if (SUCCEEDED(hr)) {
        for (UINT index = 0; ; ++index) {
            IDXGIAdapter1 *adapter = NULL;
            DXGI_ADAPTER_DESC1 description;
            hr = IDXGIFactory6_EnumAdapterByGpuPreference(factory, index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1,
                (void **)&adapter);
            if (FAILED(hr)) break;
            hr = IDXGIAdapter1_GetDesc1(adapter, &description);
            if (SUCCEEDED(hr) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                xbox_preview_log_event("graphics-adapter",
                    "trying high-performance rank=%u adapter=%ls vendor=%04X device=%04X",
                    index, description.Description, description.VendorId, description.DeviceId);
                /* Try both presentation paths on this GPU before moving to
                 * the next one; a flip-chain failure should not pick the iGPU. */
                hr = d3d11_try_device_and_swap_chain(state, pp, (IDXGIAdapter *)adapter);
                if (SUCCEEDED(hr)) {
                    IDXGIAdapter1_Release(adapter);
                    IDXGIFactory6_Release(factory);
                    return hr;
                }
                xbox_preview_log_event("graphics-adapter",
                    "rank=%u creation failed HRESULT=%08lX; trying another adapter", index, hr);
            }
            IDXGIAdapter1_Release(adapter);
        }
        IDXGIFactory6_Release(factory);
    }
    xbox_preview_log_event("graphics-adapter",
        "high-performance selection unavailable or exhausted; trying default adapter");
    hr = d3d11_try_device_and_swap_chain(state, pp, NULL);
    if (FAILED(hr)) {
        xbox_preview_log_event("graphics-error", "D3D11 device creation HRESULT=%08lX", hr);
        fprintf(stderr, "D3D8: Failed to create D3D11 device: 0x%08lX\n", hr);
    }
    return hr;
}

static HRESULT d3d11_create_render_targets(D3D8DeviceState *state)
{
    ID3D11Texture2D *back_buffer = NULL;
    D3D11_TEXTURE2D_DESC depth_desc;
    HRESULT hr;

    /* Create render target view from swap chain back buffer */
    hr = IDXGISwapChain_GetBuffer(state->swap_chain, 0,
                                   &IID_ID3D11Texture2D,
                                   (void **)&back_buffer);
    if (FAILED(hr)) return hr;

    hr = ID3D11Device_CreateRenderTargetView(state->d3d11_device,
                                              (ID3D11Resource *)back_buffer,
                                              NULL, &state->default_rtv);
    ID3D11Texture2D_Release(back_buffer);
    if (FAILED(hr)) return hr;

    /* Create depth stencil */
    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.Width = state->width;
    depth_desc.Height = state->height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &depth_desc,
                                       NULL, &state->default_depth);
    if (FAILED(hr)) return hr;

    hr = ID3D11Device_CreateDepthStencilView(state->d3d11_device,
                                              (ID3D11Resource *)state->default_depth,
                                              NULL, &state->default_dsv);
    if (FAILED(hr)) return hr;

    /* Bind default render targets */
    ID3D11DeviceContext_OMSetRenderTargets(state->d3d11_context, 1,
                                            &state->default_rtv,
                                            state->default_dsv);
    state->current_rtv = state->default_rtv;
    state->current_dsv = state->default_dsv;
    state->current_target_width = state->width;
    state->current_target_height = state->height;
    state->current_target_physical_width = state->width;
    state->current_target_physical_height = state->height;

    return S_OK;
}

BOOL d3d8_ResizePresentation(UINT width, UINT height, BOOL fullscreen)
{
    D3D8DeviceState *state = &g_device_state;
    HRESULT hr;

    if (!state->swap_chain || !state->d3d11_context || !width || !height)
        return FALSE;

    ID3D11DeviceContext_OMSetRenderTargets(state->d3d11_context, 0u, NULL,
                                           NULL);
    ID3D11DeviceContext_Flush(state->d3d11_context);
    state->current_rtv = NULL;
    state->current_dsv = NULL;
    if (state->default_dsv) {
        ID3D11DepthStencilView_Release(state->default_dsv);
        state->default_dsv = NULL;
    }
    if (state->default_depth) {
        ID3D11Texture2D_Release(state->default_depth);
        state->default_depth = NULL;
    }
    if (state->default_rtv) {
        ID3D11RenderTargetView_Release(state->default_rtv);
        state->default_rtv = NULL;
    }

    /* For flip chains a mode switch must precede ResizeBuffers. Preserve
     * the legacy order for the compatibility path. */
    if (state->flip_model) {
        hr = IDXGISwapChain_SetFullscreenState(state->swap_chain, fullscreen, NULL);
        if (FAILED(hr)) {
            d3d11_create_render_targets(state);
            d3d8_BindRenderTargets(NULL,NULL,0u,0u);
            return FALSE;
        }
    } else if (!fullscreen) {
        IDXGISwapChain_SetFullscreenState(state->swap_chain, FALSE, NULL);
    }
    hr = IDXGISwapChain_ResizeBuffers(state->swap_chain,
                                     state->flip_model ? 0u : 1u, width, height,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, state->swap_chain_flags);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: ResizeBuffers %ux%u failed: 0x%08lX\n",
                width, height, hr);
        d3d11_create_render_targets(state);
        d3d8_BindRenderTargets(NULL,NULL,0u,0u);
        return FALSE;
    }
    state->width = width;
    state->height = height;
    hr = d3d11_create_render_targets(state);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: render-target recreation failed: 0x%08lX\n",
                hr);
        return FALSE;
    }
    if (fullscreen && !state->flip_model) {
        hr = IDXGISwapChain_SetFullscreenState(state->swap_chain, TRUE, NULL);
        if (FAILED(hr)) {
            fprintf(stderr, "D3D8: fullscreen transition failed: 0x%08lX\n",
                    hr);
            return FALSE;
        }
    }
    d3d8_BindRenderTargets(NULL, NULL, 0u, 0u);
    return TRUE;
}

static void d3d8_init_default_states(D3D8DeviceState *state)
{
    /* Set Xbox D3D8 default render states */
    memset(state->render_states, 0, sizeof(state->render_states));
    state->render_states[D3DRS_ZENABLE]           = 1;
    state->render_states[D3DRS_FILLMODE]          = D3DFILL_SOLID;
    state->render_states[D3DRS_SHADEMODE]         = 2; /* D3DSHADE_GOURAUD */
    state->render_states[D3DRS_ZWRITEENABLE]      = TRUE;
    state->render_states[D3DRS_ALPHATESTENABLE]    = FALSE;
    state->render_states[D3DRS_SRCBLEND]          = D3DBLEND_ONE;
    state->render_states[D3DRS_DESTBLEND]         = D3DBLEND_ZERO;
    state->render_states[D3DRS_CULLMODE]          = D3DCULL_CCW;
    state->render_states[D3DRS_ZFUNC]             = D3DCMP_LESSEQUAL;
    state->render_states[D3DRS_ALPHAREF]          = 0;
    state->render_states[D3DRS_ALPHAFUNC]         = D3DCMP_ALWAYS;
    state->render_states[D3DRS_ALPHABLENDENABLE]   = FALSE;
    state->render_states[D3DRS_FOGENABLE]         = FALSE;
    state->render_states[D3DRS_STENCILENABLE]     = FALSE;
    state->render_states[D3DRS_COLORWRITEENABLE]  = 0x0F;

    /* Default viewport */
    state->viewport.X = 0;
    state->viewport.Y = 0;
    state->viewport.Width = state->width;
    state->viewport.Height = state->height;
    state->viewport.MinZ = 0.0f;
    state->viewport.MaxZ = 1.0f;

    /* Identity matrices */
    for (int i = 0; i < MAX_TRANSFORMS; i++) {
        memset(&state->transforms[i], 0, sizeof(D3DMATRIX));
        state->transforms[i]._11 = 1.0f;
        state->transforms[i]._22 = 1.0f;
        state->transforms[i]._33 = 1.0f;
        state->transforms[i]._44 = 1.0f;
    }

    state->vertex_shader = 0;
    state->pixel_shader = 0;
    state->in_scene = FALSE;
}

/* ================================================================
 * IDirect3DDevice8 method implementations
 * ================================================================ */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{
    (void)self;
    return InterlockedIncrement(&g_device_state.ref_count);
}

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    (void)self;
    LONG ref = InterlockedDecrement(&g_device_state.ref_count);
    if (ref <= 0) {
        /* Cleanup subsystems first */
        up_ring_shutdown();
        d3d8_scaled_lines_shutdown();
        d3d8_triangle_depth_shutdown();
        d3d8_vsh_shutdown();
        d3d8_combiners_shutdown();
        d3d8_states_shutdown();
        d3d8_shaders_shutdown();

        /* Cleanup D3D11 resources */
        D3D8DeviceState *s = &g_device_state;
        if (s->pvideo_srv) { ID3D11ShaderResourceView_Release(s->pvideo_srv); s->pvideo_srv = NULL; }
        if (s->pvideo_texture) { ID3D11Texture2D_Release(s->pvideo_texture); s->pvideo_texture = NULL; }
        if (s->pvideo_cb) { ID3D11Buffer_Release(s->pvideo_cb); s->pvideo_cb = NULL; }
        if (s->host_overlay_blend) { ID3D11BlendState_Release(s->host_overlay_blend); s->host_overlay_blend = NULL; }
        if (s->pvideo_ps) { ID3D11PixelShader_Release(s->pvideo_ps); s->pvideo_ps = NULL; }
        if (s->resolve_sampler) { ID3D11SamplerState_Release(s->resolve_sampler); s->resolve_sampler = NULL; }
        for (UINT i = 0; i < 16u; ++i) {
            if (s->masked_clear_blend[i]) {
                ID3D11BlendState_Release(s->masked_clear_blend[i]);
                s->masked_clear_blend[i] = NULL;
            }
        }
        if (s->masked_clear_cb) { ID3D11Buffer_Release(s->masked_clear_cb); s->masked_clear_cb = NULL; }
        if (s->masked_clear_ps) { ID3D11PixelShader_Release(s->masked_clear_ps); s->masked_clear_ps = NULL; }
        d3d8_release_depth_alias_source_views();
        for (UINT i = 0; i < 8u; ++i) {
            if (s->packed_color_stencil_bit_state[i]) { ID3D11DepthStencilState_Release(s->packed_color_stencil_bit_state[i]); s->packed_color_stencil_bit_state[i] = NULL; }
            if (s->packed_color_stencil_bit_ps[i]) { ID3D11PixelShader_Release(s->packed_color_stencil_bit_ps[i]); s->packed_color_stencil_bit_ps[i] = NULL; }
            if (s->depth_alias_stencil_bit_state[i]) { ID3D11DepthStencilState_Release(s->depth_alias_stencil_bit_state[i]); s->depth_alias_stencil_bit_state[i] = NULL; }
            if (s->depth_alias_stencil_bit_ps[i]) { ID3D11PixelShader_Release(s->depth_alias_stencil_bit_ps[i]); s->depth_alias_stencil_bit_ps[i] = NULL; }
        }
        if (s->packed_color_depth_fallback_state) { ID3D11DepthStencilState_Release(s->packed_color_depth_fallback_state); s->packed_color_depth_fallback_state = NULL; }
        if (s->packed_color_depth_fallback_ps) { ID3D11PixelShader_Release(s->packed_color_depth_fallback_ps); s->packed_color_depth_fallback_ps = NULL; }
        if (s->depth_alias_cb) { ID3D11Buffer_Release(s->depth_alias_cb); s->depth_alias_cb = NULL; }
        if (s->depth_alias_uniform_state) { ID3D11DepthStencilState_Release(s->depth_alias_uniform_state); s->depth_alias_uniform_state = NULL; }
        if (s->depth_alias_uniform_ps) { ID3D11PixelShader_Release(s->depth_alias_uniform_ps); s->depth_alias_uniform_ps = NULL; }
        if (s->depth_alias_ps) { ID3D11PixelShader_Release(s->depth_alias_ps); s->depth_alias_ps = NULL; }
        if (s->packed_color_depth_state) { ID3D11DepthStencilState_Release(s->packed_color_depth_state); s->packed_color_depth_state = NULL; }
        if (s->packed_color_depth_ps) { ID3D11PixelShader_Release(s->packed_color_depth_ps); s->packed_color_depth_ps = NULL; }
        if (s->resolve_ps) { ID3D11PixelShader_Release(s->resolve_ps); s->resolve_ps = NULL; }
        if (s->cpu_frame_srv) { ID3D11ShaderResourceView_Release(s->cpu_frame_srv); s->cpu_frame_srv = NULL; }
        if (s->cpu_frame_texture) { ID3D11Texture2D_Release(s->cpu_frame_texture); s->cpu_frame_texture = NULL; }
        if (s->resolve_vs) { ID3D11VertexShader_Release(s->resolve_vs); s->resolve_vs = NULL; }
        if (s->default_dsv) { ID3D11DepthStencilView_Release(s->default_dsv); s->default_dsv = NULL; }
        if (s->default_depth) { ID3D11Texture2D_Release(s->default_depth); s->default_depth = NULL; }
        if (s->default_rtv) { ID3D11RenderTargetView_Release(s->default_rtv); s->default_rtv = NULL; }
        if (s->swap_chain) { IDXGISwapChain_Release(s->swap_chain); s->swap_chain = NULL; }
        if (s->d3d11_context) { ID3D11DeviceContext_Release(s->d3d11_context); s->d3d11_context = NULL; }
        if (s->d3d11_device) { ID3D11Device_Release(s->d3d11_device); s->d3d11_device = NULL; }
        g_device_initialized = FALSE;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8)
{
    (void)self; (void)ppD3D8;
    /* TODO: return the factory */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *pCaps)
{
    (void)self; (void)pCaps;
    /* TODO: fill with Xbox NV2A capabilities */
    return S_OK;
}

static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *pMode)
{
    (void)self; (void)pMode;
    return S_OK;
}

static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *pParams)
{
    (void)self; (void)pParams;
    return S_OK;
}

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP)
{
    (void)self; (void)pPP;
    /* TODO: resize swap chain */
    return S_OK;
}

static DWORD g_d3d_begin_count = 0;
static DWORD g_d3d_end_count = 0;
static DWORD g_d3d_clear_count = 0;
static DWORD g_d3d_draw_count = 0;
static DWORD g_d3d_settransform_count = 0;
static DWORD g_d3d_setrs_count = 0;
static DWORD g_d3d_settexture_count = 0;

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty)
{
    static DWORD frame_count = 0;
    static DWORD last_tick = 0;
    (void)self; (void)src; (void)dst; (void)hWnd; (void)pDirty;

    frame_count++;
    DWORD now = GetTickCount();
    if (last_tick == 0) last_tick = now;
    if (now - last_tick >= 2000) {
        fprintf(stderr, "  [D3D] %.1fs: %u present (%.1f fps), %u begin, %u end, "
                "%u clear, %u draw, %u xform, %u rs, %u tex\n",
                (now - last_tick) / 1000.0, frame_count,
                frame_count * 1000.0 / (now - last_tick),
                g_d3d_begin_count, g_d3d_end_count,
                g_d3d_clear_count, g_d3d_draw_count,
                g_d3d_settransform_count, g_d3d_setrs_count,
                g_d3d_settexture_count);
        fflush(stderr);
        frame_count = 0;
        g_d3d_begin_count = g_d3d_end_count = 0;
        g_d3d_clear_count = g_d3d_draw_count = 0;
        g_d3d_settransform_count = g_d3d_setrs_count = 0;
        g_d3d_settexture_count = 0;
        last_tick = now;
    }

    /* Pump Windows messages: the game's internal main loop drives rendering,
     * so our external message pump never runs. Process messages here to keep
     * the window responsive and handle input. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr, "[HOST-WINDOW] WM_QUIT site=present code=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return preview_present(g_device_state.swap_chain, 1, 0);
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)iBackBuffer; (void)Type; (void)ppSurface;
    /* TODO: wrap back buffer as D3D8 surface */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = TRUE;
    g_d3d_begin_count++;
    return S_OK;
}

static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = FALSE;
    g_d3d_end_count++;
    return S_OK;
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    (void)self; (void)Count; (void)pRects; (void)Stencil;
    g_d3d_clear_count++;

    if (Flags & D3DCLEAR_TARGET) {
        float clear_color[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,  /* R */
            ((Color >>  8) & 0xFF) / 255.0f,  /* G */
            ((Color >>  0) & 0xFF) / 255.0f,  /* B */
            ((Color >> 24) & 0xFF) / 255.0f,  /* A */
        };
        if (g_device_state.current_rtv)
            ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context,
                                                       g_device_state.current_rtv,
                                                       clear_color);
    }

    if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        UINT clear_flags = 0;
        if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= D3D11_CLEAR_DEPTH;
        if (Flags & D3DCLEAR_STENCIL) clear_flags |= D3D11_CLEAR_STENCIL;

        if (g_device_state.current_dsv)
            ID3D11DeviceContext_ClearDepthStencilView(g_device_state.d3d11_context,
                                                        g_device_state.current_dsv,
                                                        clear_flags, Z, (UINT8)Stencil);
    }

    return S_OK;
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
    (void)self;
    g_d3d_settransform_count++;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        g_device_state.transforms[(DWORD)State] = *pMatrix;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
    (void)self;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        *pMatrix = g_device_state.transforms[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value)
{
    (void)self;
    g_d3d_setrs_count++;
    if ((DWORD)State < MAX_RENDER_STATES) {
        g_device_state.render_states[(DWORD)State] = Value;
    }
    /* Mark combiner state dirty if any PS register combiner state changed */
    if ((DWORD)State >= D3DRS_PSALPHAINPUTS0 && (DWORD)State <= D3DRS_PSINPUTTEXTURE) {
        d3d8_combiners_mark_dirty();
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue)
{
    (void)self;
    if ((DWORD)State < MAX_RENDER_STATES && pValue) {
        *pValue = g_device_state.render_states[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES) {
        g_device_state.tss[Stage][(DWORD)Type] = Value;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES && pValue) {
        *pValue = g_device_state.tss[Stage][(DWORD)Type];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
    (void)self;
    g_d3d_settexture_count++;
    if (Stage >= 4) return E_INVALIDARG;
    g_cur_textures[Stage] = pTexture;

    /* Bind SRV to pixel shader */
    if (pTexture) {
        D3D8Texture *tex = (D3D8Texture *)pTexture;
        if (tex->srv) {
            ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                Stage, 1, &tex->srv);
        }
        /* Mark texture stage as active */
        if (g_device_state.tss[Stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    } else {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
            Stage, 1, &null_srv);
        g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
    (void)self; (void)Stage; (void)ppTexture;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
    (void)self;
    if (StreamNumber != 0) return S_OK; /* Only stream 0 supported */
    g_cur_vb = pStreamData;
    g_cur_vb_stride = Stride;

    if (pStreamData) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)pStreamData;
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &Stride, &offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
    (void)self; (void)StreamNumber; (void)ppStreamData; (void)pStride;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
    (void)self;
    g_cur_ib = pIndexData;
    g_cur_ib_base_vertex = BaseVertexIndex;

    if (pIndexData) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)pIndexData;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32)
            ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
    (void)self; (void)ppIndexData; (void)pBaseVertexIndex;
    return E_NOTIMPL;
}

static D3D11_PRIMITIVE_TOPOLOGY map_primitive_type(D3DPRIMITIVETYPE pt, UINT count, UINT *out_count)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: *out_count = count + 2; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case D3DPT_TRIANGLEFAN:   *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_LINELIST:      *out_count = count * 2; return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     *out_count = count + 1; return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_POINTLIST:     *out_count = count;     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_QUADLIST:      *out_count = count * 6; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                  *out_count = 0;          return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* ================================================================
 * Triangle fan / quad list → triangle list conversion
 *
 * D3D11 doesn't support triangle fans or quad lists.
 * Convert vertex data in-place to triangle list.
 * Returns malloc'd buffer (caller must free) or NULL if no conversion needed.
 * ================================================================ */

static void *convert_fan_or_quad(D3DPRIMITIVETYPE pt, const void *src,
                                  UINT prim_count, UINT stride,
                                  UINT *out_vertex_count)
{
    BYTE *dst;
    const BYTE *s = (const BYTE *)src;
    UINT i;

    if (pt == D3DPT_TRIANGLEFAN) {
        /* Fan: vertex 0 is the hub, each triangle is (0, i+1, i+2) */
        UINT tri_verts = prim_count * 3;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            memcpy(dst + (i * 3 + 0) * stride, s, stride);                      /* v0 (hub) */
            memcpy(dst + (i * 3 + 1) * stride, s + (i + 1) * stride, stride);   /* v[i+1] */
            memcpy(dst + (i * 3 + 2) * stride, s + (i + 2) * stride, stride);   /* v[i+2] */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    if (pt == D3DPT_QUADLIST) {
        /* Quad list: each quad (v0,v1,v2,v3) → 2 triangles (v0,v1,v2), (v0,v2,v3) */
        UINT tri_verts = prim_count * 6;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            const BYTE *q = s + i * 4 * stride;
            memcpy(dst + (i * 6 + 0) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 1) * stride, q + 1 * stride, stride);  /* v1 */
            memcpy(dst + (i * 6 + 2) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 3) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 4) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 5) * stride, q + 3 * stride, stride);  /* v3 */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    return NULL; /* no conversion needed */
}

/* ================================================================
 * DrawPrimitiveUP ring buffer
 *
 * Instead of creating and destroying a D3D11 buffer on every
 * DrawPrimitiveUP call, use a persistent ring buffer.
 * ================================================================ */

#define UP_RING_BUFFER_SIZE (4 * 1024 * 1024)  /* 4MB ring buffer */

static ID3D11Buffer *g_up_ring_buffer = NULL;
static UINT          g_up_ring_offset = 0;

typedef struct D3D8DrawPerfTotals {
    UINT64 calls;
    UINT64 bytes;
    LONGLONG upload_ticks;
    LONGLONG vertex_ticks;
    LONGLONG pixel_ticks;
    LONGLONG state_ticks;
    LONGLONG issue_ticks;
    LONGLONG total_ticks;
    ULONGLONG interval_start_ms;
    LONGLONG qpc_frequency;
} D3D8DrawPerfTotals;

static void trace_draw_primitive_up_perf(UINT bytes,
    LONGLONG upload_ticks, LONGLONG vertex_ticks, LONGLONG pixel_ticks,
    LONGLONG state_ticks, LONGLONG issue_ticks, LONGLONG total_ticks)
{
    static int enabled = -1;
    static D3D8DrawPerfTotals totals;
    const ULONGLONG now = GetTickCount64();
    LARGE_INTEGER frequency;
    LONGLONG qpc_frequency;
    double scale;

    if (enabled < 0)
        enabled = d3d8_cached_getenv("MERCENARIES_TRACE_D3D_DRAW_PERF") != NULL;
    if (!enabled)
        return;
    if (totals.qpc_frequency == 0) {
        QueryPerformanceFrequency(&frequency);
        totals.qpc_frequency = frequency.QuadPart;
    }
    qpc_frequency = totals.qpc_frequency;
    scale = qpc_frequency > 0 ? 1000.0 / (double)qpc_frequency : 0.0;
    /* A one-second aggregate cannot attribute one bad frame. Retain the
     * costly individual draw through the bounded asynchronous event queue. */
    if ((double)total_ticks * scale >= 8.0) {
        xbox_preview_log_sample("draw-slow",
            "path=up bytes=%u total_ms=%.3f upload_ms=%.3f vertex_ms=%.3f "
            "pixel_ms=%.3f state_ms=%.3f issue_ms=%.3f",
            bytes, (double)total_ticks * scale, (double)upload_ticks * scale,
            (double)vertex_ticks * scale, (double)pixel_ticks * scale,
            (double)state_ticks * scale, (double)issue_ticks * scale);
    }
    if (totals.interval_start_ms == 0u)
        totals.interval_start_ms = now;
    ++totals.calls;
    totals.bytes += bytes;
    totals.upload_ticks += upload_ticks;
    totals.vertex_ticks += vertex_ticks;
    totals.pixel_ticks += pixel_ticks;
    totals.state_ticks += state_ticks;
    totals.issue_ticks += issue_ticks;
    totals.total_ticks += total_ticks;
    if (now - totals.interval_start_ms < 1000u)
        return;
    fprintf(stderr,
        "[D3D-DRAW-PERF] calls=%llu bytes=%llu upload_ms=%.3f "
        "vertex_ms=%.3f pixel_ms=%.3f state_ms=%.3f issue_ms=%.3f "
        "total_ms=%.3f interval_ms=%llu\n",
        (unsigned long long)totals.calls,
        (unsigned long long)totals.bytes,
        (double)totals.upload_ticks * scale,
        (double)totals.vertex_ticks * scale,
        (double)totals.pixel_ticks * scale,
        (double)totals.state_ticks * scale,
        (double)totals.issue_ticks * scale,
        (double)totals.total_ticks * scale,
        (unsigned long long)(now - totals.interval_start_ms));
    fflush(stderr);
    memset(&totals, 0, sizeof(totals));
    totals.qpc_frequency = qpc_frequency;
    totals.interval_start_ms = now;
}

static HRESULT up_ring_init(void)
{
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = UP_RING_BUFFER_SIZE;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, NULL, &g_up_ring_buffer);
}

static void up_ring_shutdown(void)
{
    if (g_up_ring_buffer) {
        ID3D11Buffer_Release(g_up_ring_buffer);
        g_up_ring_buffer = NULL;
    }
    g_up_ring_offset = 0;
}

/* Upload vertex data to ring buffer, returns offset. Returns (UINT)-1 on failure. */
static UINT up_ring_upload(const void *data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_MAP map_type;
    HRESULT hr;
    UINT offset;

    if (!g_up_ring_buffer) {
        if (FAILED(up_ring_init())) return (UINT)-1;
    }

    if (size > UP_RING_BUFFER_SIZE) return (UINT)-1;

    /* Wrap around if not enough space */
    if (g_up_ring_offset + size > UP_RING_BUFFER_SIZE) {
        g_up_ring_offset = 0;
        map_type = D3D11_MAP_WRITE_DISCARD;
    } else {
        map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
    }

    hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0, map_type, 0, &mapped);
    if (FAILED(hr)) return (UINT)-1;

    offset = g_up_ring_offset;
    memcpy((BYTE *)mapped.pData + offset, data, size);

    ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0);

    g_up_ring_offset = (offset + size + 15) & ~15;  /* 16-byte align */
    return offset;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Prepare pipeline: shaders, input layout, constant buffers, render states */
    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    const BOOL programmable_vsh = d3d8_vsh_prepare_draw(g_device_state.vertex_shader);
    if (!programmable_vsh)
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    else
        d3d8_shaders_prepare_pixel();
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    if (d3d8_cached_getenv("MERCENARIES_DEBUG_SOLID_PIXEL") != NULL)
        d3d8_shaders_bind_debug_pixel();
    d3d8_states_apply();

    D3D8TriangleDepthScope depth_scope = {0};
    if (programmable_vsh && d3d8_combiners_requires_triangle_depth())
        d3d8_triangle_depth_begin(g_device_state.d3d11_device,
            g_device_state.d3d11_context, (float)g_device_state.current_target_width,
            (float)g_device_state.current_target_height, &depth_scope);
    D3D8ScaledLineScope line_scope = {0};
    if (programmable_vsh && g_device_state.current_target_height &&
        (PrimitiveType == D3DPT_LINELIST || PrimitiveType == D3DPT_LINESTRIP))
        d3d8_scaled_lines_begin(g_device_state.d3d11_device,
            g_device_state.d3d11_context,
            (float)g_device_state.current_target_physical_height /
                (float)g_device_state.current_target_height, &line_scope);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, StartVertex);
    d3d8_scaled_lines_end(g_device_state.d3d11_context, &line_scope);
    d3d8_triangle_depth_end(g_device_state.d3d11_context, &depth_scope);
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    (void)self; (void)MinVertexIndex; (void)NumVertices;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT index_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    const BOOL programmable_vsh = d3d8_vsh_prepare_draw(g_device_state.vertex_shader);
    if (!programmable_vsh)
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    else
        d3d8_shaders_prepare_pixel();
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    if (d3d8_cached_getenv("MERCENARIES_DEBUG_SOLID_PIXEL") != NULL)
        d3d8_shaders_bind_debug_pixel();
    d3d8_states_apply();

    D3D8TriangleDepthScope depth_scope = {0};
    if (programmable_vsh && d3d8_combiners_requires_triangle_depth())
        d3d8_triangle_depth_begin(g_device_state.d3d11_device,
            g_device_state.d3d11_context, (float)g_device_state.current_target_width,
            (float)g_device_state.current_target_height, &depth_scope);
    D3D8ScaledLineScope line_scope = {0};
    if (programmable_vsh && g_device_state.current_target_height &&
        (PrimitiveType == D3DPT_LINELIST || PrimitiveType == D3DPT_LINESTRIP))
        d3d8_scaled_lines_begin(g_device_state.d3d11_device,
            g_device_state.d3d11_context,
            (float)g_device_state.current_target_physical_height /
                (float)g_device_state.current_target_height, &line_scope);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, StartIndex, (INT)g_cur_ib_base_vertex);
    d3d8_scaled_lines_end(g_device_state.d3d11_context, &line_scope);
    d3d8_triangle_depth_end(g_device_state.d3d11_context, &depth_scope);
    return S_OK;
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count, vb_size, ring_offset;
    const void *draw_data = pVertexData;
    void *converted = NULL;
    const int trace_perf =
        d3d8_cached_getenv("MERCENARIES_TRACE_D3D_DRAW_PERF") != NULL;
    LARGE_INTEGER perf_begin = { 0 }, perf_after_upload = { 0 };
    LARGE_INTEGER perf_after_vertex = { 0 }, perf_after_pixel = { 0 };
    LARGE_INTEGER perf_after_state = { 0 }, perf_after_issue = { 0 };

    if (!pVertexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Convert triangle fans and quad lists to triangle lists */
    if (PrimitiveType == D3DPT_TRIANGLEFAN || PrimitiveType == D3DPT_QUADLIST) {
        converted = convert_fan_or_quad(PrimitiveType, pVertexData,
                                         PrimitiveCount, VertexStreamZeroStride,
                                         &vertex_count);
        if (converted) draw_data = converted;
    }

    vb_size = vertex_count * VertexStreamZeroStride;

    /* Upload to ring buffer */
    if (trace_perf)
        QueryPerformanceCounter(&perf_begin);
    ring_offset = up_ring_upload(draw_data, vb_size);
    if (converted) free(converted);
    if (trace_perf)
        QueryPerformanceCounter(&perf_after_upload);

    if (ring_offset == (UINT)-1) return E_OUTOFMEMORY;

    /* Bind ring buffer at the right offset */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &g_up_ring_buffer, &VertexStreamZeroStride, &ring_offset);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    const BOOL programmable_vsh = d3d8_vsh_prepare_draw(g_device_state.vertex_shader);
    if (!programmable_vsh)
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    else
        d3d8_shaders_prepare_pixel();
    if (trace_perf)
        QueryPerformanceCounter(&perf_after_vertex);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    if (trace_perf)
        QueryPerformanceCounter(&perf_after_pixel);
    if (d3d8_cached_getenv("MERCENARIES_DEBUG_SOLID_PIXEL") != NULL)
        d3d8_shaders_bind_debug_pixel();
    d3d8_states_apply();
    if (trace_perf)
        QueryPerformanceCounter(&perf_after_state);

    D3D8TriangleDepthScope depth_scope = {0};
    if (programmable_vsh && d3d8_combiners_requires_triangle_depth())
        d3d8_triangle_depth_begin(g_device_state.d3d11_device,
            g_device_state.d3d11_context, (float)g_device_state.current_target_width,
            (float)g_device_state.current_target_height, &depth_scope);
    D3D8ScaledLineScope line_scope = {0};
    if (programmable_vsh && g_device_state.current_target_height &&
        (PrimitiveType == D3DPT_LINELIST || PrimitiveType == D3DPT_LINESTRIP))
        d3d8_scaled_lines_begin(g_device_state.d3d11_device,
            g_device_state.d3d11_context,
            (float)g_device_state.current_target_physical_height /
                (float)g_device_state.current_target_height, &line_scope);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, 0);
    d3d8_scaled_lines_end(g_device_state.d3d11_context, &line_scope);
    d3d8_triangle_depth_end(g_device_state.d3d11_context, &depth_scope);
    if (trace_perf) {
        QueryPerformanceCounter(&perf_after_issue);
        trace_draw_primitive_up_perf(vb_size,
            perf_after_upload.QuadPart - perf_begin.QuadPart,
            perf_after_vertex.QuadPart - perf_after_upload.QuadPart,
            perf_after_pixel.QuadPart - perf_after_vertex.QuadPart,
            perf_after_state.QuadPart - perf_after_pixel.QuadPart,
            perf_after_issue.QuadPart - perf_after_state.QuadPart,
            perf_after_issue.QuadPart - perf_begin.QuadPart);
    }
    d3d8_debug_backbuffer_stats();

    /* Restore previous VB binding if any */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        UINT restore_offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &restore_offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self; (void)MinVertexIndex;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *tmp_vb = NULL, *tmp_ib = NULL;
    UINT index_count, vb_size, ib_size, offset = 0;
    UINT idx_bytes;
    DXGI_FORMAT ib_fmt;
    HRESULT hr;

    if (!pVertexData || !pIndexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    idx_bytes = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
    ib_fmt = (IndexDataFormat == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    vb_size = NumVertices * VertexStreamZeroStride;
    ib_size = index_count * idx_bytes;

    /* Create temp vertex buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = vb_size;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = pVertexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_vb);
    if (FAILED(hr)) return hr;

    /* Create temp index buffer */
    bd.ByteWidth = ib_size;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = pIndexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_ib);
    if (FAILED(hr)) { ID3D11Buffer_Release(tmp_vb); return hr; }

    /* Bind, prepare, draw */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &tmp_vb, &VertexStreamZeroStride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
        tmp_ib, ib_fmt, 0);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    const BOOL programmable_vsh = d3d8_vsh_prepare_draw(g_device_state.vertex_shader);
    if (!programmable_vsh)
        d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    else
        d3d8_shaders_prepare_pixel();
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    if (d3d8_cached_getenv("MERCENARIES_DEBUG_SOLID_PIXEL") != NULL)
        d3d8_shaders_bind_debug_pixel();
    d3d8_states_apply();

    D3D8TriangleDepthScope depth_scope = {0};
    if (programmable_vsh && d3d8_combiners_requires_triangle_depth())
        d3d8_triangle_depth_begin(g_device_state.d3d11_device,
            g_device_state.d3d11_context, (float)g_device_state.current_target_width,
            (float)g_device_state.current_target_height, &depth_scope);
    D3D8ScaledLineScope line_scope = {0};
    if (programmable_vsh && g_device_state.current_target_height &&
        (PrimitiveType == D3DPT_LINELIST || PrimitiveType == D3DPT_LINESTRIP))
        d3d8_scaled_lines_begin(g_device_state.d3d11_device,
            g_device_state.d3d11_context,
            (float)g_device_state.current_target_physical_height /
                (float)g_device_state.current_target_height, &line_scope);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, 0, 0);
    d3d8_scaled_lines_end(g_device_state.d3d11_context, &line_scope);
    d3d8_triangle_depth_end(g_device_state.d3d11_context, &depth_scope);

    /* Cleanup temp buffers */
    ID3D11Buffer_Release(tmp_ib);
    ID3D11Buffer_Release(tmp_vb);

    /* Restore previous bindings */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &offset);
    }
    if (g_cur_ib) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)g_cur_ib;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateTextureImpl(Width, Height, Levels, Usage, Format, ppTexture);
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateVertexBufferImpl(Length, Usage, FVF, ppVertexBuffer);
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateIndexBufferImpl(Length, Usage, Format, ppIndexBuffer);
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)Lockable; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Width; (void)Height; (void)Format; (void)MultiSample; (void)ppSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface)
{
    (void)self; (void)pRenderTarget; (void)pZStencilSurface;
    /* TODO: resolve D3D8 surface to D3D11 RTV/DSV */
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget)
{
    (void)self; (void)ppRenderTarget;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface)
{
    (void)self; (void)ppZStencilSurface;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) {
        g_device_state.viewport = *pViewport;

        D3D11_VIEWPORT d3d11_vp;
        d3d11_vp.TopLeftX = (FLOAT)pViewport->X;
        d3d11_vp.TopLeftY = (FLOAT)pViewport->Y;
        d3d11_vp.Width    = (FLOAT)pViewport->Width;
        d3d11_vp.Height   = (FLOAT)pViewport->Height;
        d3d11_vp.MinDepth = pViewport->MinZ;
        d3d11_vp.MaxDepth = pViewport->MaxZ;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &d3d11_vp);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) *pViewport = g_device_state.viewport;
    return S_OK;
}

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) g_device_state.material = *pMaterial;
    return S_OK;
}

static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) *pMaterial = g_device_state.material;
    return S_OK;
}

static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) g_device_state.lights[Index] = *pLight;
    return S_OK;
}

static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) *pLight = g_device_state.lights[Index];
    return S_OK;
}

static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD Index, BOOL Enable)
{
    (void)self;
    if (Index < MAX_LIGHTS) g_device_state.light_enable[Index] = Enable;
    return S_OK;
}

static HRESULT __stdcall dev_CreateVertexShader(IDirect3DDevice8 *self, const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
    (void)self; (void)pDeclaration; (void)Usage;
    if (!pHandle) return E_INVALIDARG;
    if (!pFunction) return E_INVALIDARG;
    /* Count instructions: each is 4 DWORDs, last has bit 0 of word[3] set (END flag) */
    {
        int i, num_insns = 0;
        for (i = 0; i < 136; i++) {
            num_insns++;
            if (pFunction[i * 4 + 3] & 1) break;  /* END bit in last word */
        }
        return d3d8_vsh_create_shader(pFunction, num_insns, pHandle);
    }
}

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.vertex_shader = Handle;
    return S_OK;
}

static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.vertex_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self;
    d3d8_vsh_set_constant(Register, pConstantData, ConstantCount);
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.pixel_shader = Handle;
    d3d8_combiners_set_pixel_shader(Handle);
    return S_OK;
}

static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.pixel_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self; (void)Register; (void)pConstantData; (void)ConstantCount;
    return S_OK;
}

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp)
{
    static unsigned int trace_serial;
    unsigned int hash = 2166136261u;
    const unsigned char *bytes;
    size_t i;
    (void)self;
    if (!pRamp) return;
    g_device_state.gamma_ramp = *pRamp;
    g_device_state.gamma_ramp_valid = TRUE;
    if (!getenv("MERCENARIES_TRACE_GAMMA")) return;
    bytes = (const unsigned char *)pRamp;
    for (i = 0; i < sizeof(*pRamp); ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    fprintf(stderr,
        "D3D8: gamma[%u] flags=%08lX fnv=%08X "
        "r={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u} "
        "g={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u} "
        "b={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u}\n",
        ++trace_serial, (unsigned long)Flags, hash,
        pRamp->red[0], pRamp->red[16], pRamp->red[32], pRamp->red[64],
        pRamp->red[96], pRamp->red[128], pRamp->red[160], pRamp->red[192],
        pRamp->red[224], pRamp->red[255],
        pRamp->green[0], pRamp->green[16], pRamp->green[32], pRamp->green[64],
        pRamp->green[96], pRamp->green[128], pRamp->green[160], pRamp->green[192],
        pRamp->green[224], pRamp->green[255],
        pRamp->blue[0], pRamp->blue[16], pRamp->blue[32], pRamp->blue[64],
        pRamp->blue[96], pRamp->blue[128], pRamp->blue[160], pRamp->blue[192],
        pRamp->blue[224], pRamp->blue[255]);
    fflush(stderr);
}

static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp)
{
    (void)self;
    if (pRamp && g_device_state.gamma_ramp_valid)
        *pRamp = g_device_state.gamma_ramp;
}

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries)
{
    (void)self; (void)PaletteNumber; (void)pEntries;
    return S_OK;
}

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush)
{
    (void)self; (void)Count; (void)ppPush;
    /* TODO: Xbox push buffer emulation */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *pPush)
{
    (void)self; (void)pPush;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD Flags)
{
    (void)self; (void)Flags;

    /* Pump Windows messages (same as dev_Present) */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            fprintf(stderr, "[HOST-WINDOW] WM_QUIT site=swap code=%llu\n",
                    (unsigned long long)msg.wParam);
            fflush(stderr);
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return preview_present(g_device_state.swap_chain, 1, 0);
}

/* ================================================================
 * Vtable
 * ================================================================ */

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetDirect3D,
    dev_GetDeviceCaps,
    dev_GetDisplayMode,
    dev_GetCreationParameters,
    dev_Reset,
    dev_Present,
    dev_GetBackBuffer,
    dev_BeginScene,
    dev_EndScene,
    dev_Clear,
    dev_SetTransform,
    dev_GetTransform,
    dev_SetRenderState,
    dev_GetRenderState,
    dev_SetTextureStageState,
    dev_GetTextureStageState,
    dev_SetTexture,
    dev_GetTexture,
    dev_SetStreamSource,
    dev_GetStreamSource,
    dev_SetIndices,
    dev_GetIndices,
    dev_DrawPrimitive,
    dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture,
    dev_CreateVertexBuffer,
    dev_CreateIndexBuffer,
    dev_CreateRenderTarget,
    dev_CreateDepthStencilSurface,
    dev_SetRenderTarget,
    dev_GetRenderTarget,
    dev_GetDepthStencilSurface,
    dev_SetViewport,
    dev_GetViewport,
    dev_SetMaterial,
    dev_GetMaterial,
    dev_SetLight,
    dev_GetLight,
    dev_LightEnable,
    dev_SetVertexShader,
    dev_GetVertexShader,
    dev_SetVertexShaderConstant,
    dev_SetPixelShader,
    dev_GetPixelShader,
    dev_SetPixelShaderConstant,
    dev_SetGammaRamp,
    dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush,
    dev_EndPush,
    dev_Swap,
};

/* ================================================================
 * Public API
 * ================================================================ */

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_device_initialized ? &g_device : NULL;
}

/* ================================================================
 * IDirect3D8 factory implementation
 * ================================================================ */

static IDirect3D8 g_d3d8;
static LONG g_d3d8_ref = 0;

static HRESULT __stdcall d3d8_QueryInterface(IDirect3D8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall d3d8_AddRef(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_d3d8_ref);
}

static ULONG __stdcall d3d8_Release(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedDecrement(&g_d3d8_ref);
}

static HRESULT __stdcall d3d8_CreateDevice(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice)
{
    (void)self; (void)Adapter; (void)DeviceType; (void)BehaviorFlags;
    HRESULT hr;

    if (!pPP || !ppDevice) return E_INVALIDARG;

    memset(&g_device_state, 0, sizeof(g_device_state));
    g_device_state.ref_count = 1;

    if (!pPP->hDeviceWindow) pPP->hDeviceWindow = hFocusWindow;

    hr = d3d11_create_device_and_swap_chain(&g_device_state, pPP);
    if (FAILED(hr)) return hr;

    hr = d3d11_create_render_targets(&g_device_state);
    if (FAILED(hr)) return hr;

    d3d8_init_default_states(&g_device_state);

    /* Set initial viewport (D3D11 requires explicit viewport) */
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = (FLOAT)g_device_state.width;
        vp.Height   = (FLOAT)g_device_state.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
    }

    /* Initialize shader and state subsystems */
    hr = d3d8_shaders_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Shader init failed: 0x%08lX\n", hr);
        return hr;
    }

    /* The resolve/copy pipeline consists entirely of fixed host shaders.  Do
     * not defer their compilation until the first guest surface resolve: that
     * can happen several minutes into gameplay, after the game has created a
     * large number of worker objects and heaps.  Besides making the first
     * resolve hitch, late compilation exposed a reproducible crash inside
     * D3DCompiler when the North Korean roadblock streamed in.  Compile and
     * validate this invariant host pipeline while the D3D device is being
     * initialized, alongside the other fixed shaders. */
    if (!d3d8_ensure_resolve_pipeline()) {
        fprintf(stderr, "D3D8: Resolve pipeline init failed\n");
        return E_FAIL;
    }

    hr = d3d8_states_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: State init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_combiners_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Combiner init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to fixed-function pixel shaders */
    }

    hr = d3d8_vsh_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VSH init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to FVF vertex shaders */
    }

    g_device.lpVtbl = &g_device_vtbl;
    g_device_initialized = TRUE;

    *ppDevice = &g_device;
    fprintf(stderr, "D3D8: Device created (%ux%u)\n", g_device_state.width, g_device_state.height);
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d8_QueryInterface,
    d3d8_AddRef,
    d3d8_Release,
    d3d8_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}

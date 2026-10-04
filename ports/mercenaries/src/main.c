#include "kernel/mod_overlay.h"
#include "mod_compatibility.h"
#include "app_icon.h"
#include "recomp_controls.h"
#include "dev_menu.h"
#include "dev_overlay.h"
#include "free_cam.h"
#include "shader_warmup.h"
/**
 * Mercenaries - recompiled game entry point.
 *
 * Hosts the supported retail XBE using translated code, mapped guest memory,
 * kernel compatibility services, and the host graphics/audio/input backends.
 * WinMain owns initialization ordering and failure handling; guest-memory
 * translation and the register/stack ABI must be ready before guest entry.
 *
 * XBE Details:
 *   Title:       Mercenaries
 *   Title ID:    0x4C410015
 *   Base addr:   0x00010000
 *   Entry point: 0x0022964A
 *   Code size:   2,377,244 bytes (.text)
 *   Sections:    14
 *   Kernel imports: 111
 */

#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "bcrypt.lib")
#include "apu.h"
#include "apu_mmio_hook.h"

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "nv2a_mmio_hook.h"
#include "recomp_options.h"
#include "preview_log.h"
#include "d3d8_frame_timing.h"
#include "nv2a_pgraph_d3d11.h"

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_ebx, g_esi, g_edi;
extern uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;
extern volatile uint32_t g_recomp_current_func;
extern volatile uint32_t g_recomp_entry_trace_enabled;
extern volatile uint32_t g_recomp_irq_entry_safepoint_enabled;
extern volatile uint32_t g_recomp_particle_list_watch_enabled;
extern volatile uint32_t g_recomp_target_call_trace_enabled;
extern void d3d8_DebugStartDisplayCapture(const char *prefix,
                                          unsigned int interval_ms,
                                          unsigned int limit);
extern volatile uint32_t g_recomp_recent_funcs[64];
extern volatile uint32_t g_recomp_recent_func_idx;
extern volatile uint32_t g_recomp_recent_game_funcs[256];
extern volatile uint32_t g_recomp_recent_game_func_idx;
extern void recomp_lua_dump_poscall_ring_on_crash(void);
extern volatile uint32_t g_recomp_recent_asset_requests[512];
extern volatile uint32_t g_recomp_recent_asset_request_idx;
extern volatile uint32_t g_nv2a_pfifo_diag_submission;
extern volatile uint32_t g_nv2a_pfifo_diag_get;
extern volatile uint32_t g_nv2a_pfifo_diag_put;
extern volatile uint32_t g_nv2a_pfifo_diag_word;
extern volatile uint32_t g_nv2a_pfifo_diag_method;
extern volatile uint32_t g_nv2a_pfifo_diag_subchannel;
extern volatile uint32_t g_nv2a_pfifo_diag_stage;
extern volatile uint64_t g_nv2a_mmio_put_hits;
extern volatile uintptr_t g_nv2a_mmio_put_last_rip_before;
extern volatile uintptr_t g_nv2a_mmio_put_last_rip_after;
extern volatile uint32_t g_nv2a_mmio_put_last_value;

/* ── XBE Constants ─────────────────────────────────────────── */

#define MERCENARIES_ENTRY_POINT 0x0022964A
#define DEFAULT_GAME_DIR        "game_files\\mercenaries-retail"
#define DEFAULT_SAVE_DIR        "saves\\mercenaries"

static const char *g_game_dir = DEFAULT_GAME_DIR;
static const char *g_save_dir = DEFAULT_SAVE_DIR;
static char g_game_dir_storage[MAX_PATH];
static char g_save_dir_storage[MAX_PATH];
static const unsigned char g_required_retail_xbe_sha256[32] = {
    0xAA, 0x08, 0xEA, 0x21, 0xD9, 0x52, 0xAC, 0x35,
    0xF4, 0x9C, 0x02, 0xC7, 0xE2, 0xED, 0x08, 0xAA,
    0x25, 0xAD, 0x75, 0x35, 0xFB, 0xBB, 0xCC, 0xC9,
    0x56, 0x36, 0x77, 0x5F, 0x37, 0xBE, 0x99, 0xD7
};
static char g_xbe_path[MAX_PATH];
static HANDLE g_kernel_tick_thread;
static volatile LONG g_kernel_tick_stop;

static DWORD WINAPI kernel_tick_thread_main(LPVOID unused)
{
    (void)unused;
    while (InterlockedCompareExchange(&g_kernel_tick_stop, 0, 0) == 0) {
        xbox_kernel_bridge_update_tick_count();
        Sleep(1);
    }
    return 0;
}

static BOOL start_kernel_tick_thread(void)
{
    InterlockedExchange(&g_kernel_tick_stop, 0);
    g_kernel_tick_thread = CreateThread(NULL, 0, kernel_tick_thread_main,
                                        NULL, 0, NULL);
    return g_kernel_tick_thread != NULL;
}

static void stop_kernel_tick_thread(void)
{
    if (!g_kernel_tick_thread) return;
    InterlockedExchange(&g_kernel_tick_stop, 1);
    WaitForSingleObject(g_kernel_tick_thread, INFINITE);
    CloseHandle(g_kernel_tick_thread);
    g_kernel_tick_thread = NULL;
}

/* Sample existing counters/guest camera only. No heap walk, shader trace,
 * GPU query, screenshot, or disk access runs in the presentation callback. */
static void preview_snapshot(const char *reason)
{
    PgraphD3D11Stats stats;
    PgraphD3D11DiagnosticState gpu;
    uint32_t width=0,height=0,internal_width=0,internal_height=0;
    uint32_t state=0,camera=0;float x=0,y=0,z=0;
    float world_x=0,world_y=0,world_z=0;
    uint32_t awake=0,stream_used=0,stream_active=0,stream_pending=0;
    int world_ready=0;
    if(!xbox_preview_log_enabled()) return;
    pgraph_d3d11_get_stats(&stats);
    pgraph_d3d11_get_diagnostic_state(&gpu);
    xbox_preview_log_event("gpu-state",
        "last_draw color=%08X depth=%08X vsh=%08X comb=%08X primitive=%u ztest=%u zwrite=%u blend=%u alpha=%u textures=%08X,%08X,%08X,%08X",
        gpu.color,gpu.depth,gpu.vertex_shader,gpu.combiner,gpu.primitive,gpu.depth_test,gpu.depth_write,gpu.blend,gpu.alpha_test,
        gpu.texture[0],gpu.texture[1],gpu.texture[2],gpu.texture[3]);
    recomp_options_resolution_size(&width,&height);
    recomp_options_internal_resolution_size(&internal_width,&internal_height);
    if(g_xbox_mem_offset) {
        const uintptr_t base=(uintptr_t)g_xbox_mem_offset;
        state=*(const uint32_t*)(base+0x413F68u);
        camera=*(const uint32_t*)(base+0x414104u);
        /* RedWorld's last activation centre can differ from the rendered
         * camera (notably Free Cam). Keep both in the pre-incident history. */
        world_ready=camera>=0x10000u && camera<=0x3FFFFB0u;
        if(world_ready) {
            world_x=*(const float*)(base+0x641D70u);
            world_y=*(const float*)(base+0x641D74u);
            world_z=*(const float*)(base+0x641D78u);
            awake=*(const uint32_t*)(base+0x30EE74u);
            const uint32_t stream=*(const uint32_t*)(base+0x85BF18u);
            if(stream>=0x10000u && stream<=0x03FFFC38u) {
                stream_used=*(const uint32_t*)(base+stream+0x3B4u);
                stream_active=*(const uint32_t*)(base+stream+0x218u);
                stream_pending=*(const uint8_t*)(base+stream) ? 64u :
                    ((*(const uint32_t*)(base+stream+8u)-
                      *(const uint32_t*)(base+stream+4u)) & 63u);
            }
        }
        /* Existing 8x8 standing table, column 1 (player); no actor/heap walk. */
        xbox_preview_log_event("faction-standing",
            "to_player_by_faction=%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g event_active=%u event_free=%u event_high_water=%u",
            *(const float*)(base+0x323358u), *(const float*)(base+0x323378u),
            *(const float*)(base+0x323398u), *(const float*)(base+0x3233B8u),
            *(const float*)(base+0x3233D8u), *(const float*)(base+0x3233F8u),
            *(const float*)(base+0x323418u), *(const float*)(base+0x323438u),
            *(const uint32_t*)(base+0x30E7A0u), *(const uint32_t*)(base+0x30E7B0u),
            *(const uint32_t*)(base+0x365EB4u));
        if(camera>=0x10000u && camera<=0x3FFFFB0u) {
            const float *position=(const float*)(base+camera+0x40u);
            x=position[0];y=position[1];z=position[2];
        }
    }
    xbox_preview_log_event(reason,
        "state=%08X camera=%08X pos=%.3f,%.3f,%.3f pfifo=%u/%u method=%04X draws=%u verts=%u clears=%u ignored=%u",
        state,camera,x,y,z,g_nv2a_pfifo_diag_submission,g_nv2a_pfifo_diag_stage,
        g_nv2a_pfifo_diag_method,stats.draw_calls,stats.vertices_submitted,stats.clears,stats.methods_ignored);
    { void recomp_hq_preview_snapshot(void); recomp_hq_preview_snapshot(); }
    { void recomp_music_preview_snapshot(void); recomp_music_preview_snapshot(); }
    xbox_preview_log_event("world-loading",
        "ready=%d world_pos=%.3f,%.3f,%.3f freecam=%d awake=%u stream_used=%u stream_active=%u stream_pending=%u",
        world_ready,world_x,world_y,world_z,recomp_freecam_enabled(),awake,stream_used,stream_active,stream_pending);
    xbox_preview_log_event("options",
        "output=%ux%u internal=%ux%u fps_cap=%d vsync=%d aspect=%d af16=%d distance=%d haze=%d npc_draw=%d npc_lod=%d display=%d",
        width,height,internal_width,internal_height,recomp_options_fps_cap(),recomp_options_vsync(),recomp_options_aspect_ratio(),
        recomp_options_anisotropic_16x(),recomp_options_draw_distance(),recomp_options_authentic_haze(),
        recomp_options_npc_draw_distance(),recomp_options_npc_lod(),recomp_options_display_mode());
}
/* Automatic pre-incident history: export the existing fixed ring once a
 * second. F8 only bookmarks this timeline; it never starts collection. */
static void preview_recent_history(ULONGLONG now)
{
    static ULONGLONG previous;
    static uint64_t cursor;
    uint64_t serial;
    uint32_t values[96], count, emitted = 0;
    char list[576]; size_t used = 0;
    if (previous && now - previous < 1000u) return;
    previous = now;
    count = d3d8_GetFrameIntervalHistory(cursor, values, 96u, &serial);
    list[0] = 0;
    for (; emitted < count; ++emitted) {
        int n = snprintf(list + used, sizeof(list) - used, "%s%u",
                         emitted ? "," : "", values[emitted]);
        if (n < 0 || (size_t)n >= sizeof(list) - used) {
            list[used] = 0; break;
        }
        used += (size_t)n;
    }
    xbox_preview_log_event("frame-history",
        "interval_serial=%llu samples=%u omitted=%llu chronological_us=%s",
        (unsigned long long)serial, emitted,
        (unsigned long long)(serial >= cursor ? serial - cursor - emitted : 0), list);
    cursor = serial;
    preview_snapshot("scene");
}

static void preview_frame_sample(BOOL scanout,BOOL refreshed)
{
    static ULONGLONG begin,last,max_gap;
    static unsigned frames,refreshes,polls;
    static uint64_t previous_submissions;
    ULONGLONG now;
    if(!xbox_preview_log_enabled()) return;
    now=GetTickCount64();
    preview_recent_history(now);
    if(!begin) begin=now;
    if(last && now-last>max_gap) max_gap=now-last;
    last=now;++polls;frames+=scanout!=0;refreshes+=refreshed!=0;
    if(now-begin>=5000) {
        D3D8FrameTimingSnapshot timing;
        d3d8_GetFrameTimingSnapshot(&timing);
        xbox_preview_log_event("performance","interval_ms=%llu service_polls=%u service_scanouts=%u submitted_frames=%llu vblank_refreshes=%u max_service_gap_ms=%llu",
            now-begin,polls,frames,(unsigned long long)(timing.submitted_frames-previous_submissions),refreshes,max_gap);
        previous_submissions = timing.submitted_frames;
        xbox_preview_log_event("frame-times","last_frames=%u mean_ms=%.3f p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f",
            timing.samples,timing.mean_ms,timing.p50_ms,timing.p95_ms,timing.p99_ms,timing.max_ms);
        begin=now;frames=refreshes=polls=0;max_gap=0;
    }
}

static void mercenaries_host_service(void)
{
    static int trace_timing = -1;
    static int test_resolution_transitions = -1;
    static int test_presentation_resize_transitions;
    static unsigned int test_resolution_frames;
    static unsigned int test_resolution_delay_ms;
    static ULONGLONG test_resolution_start_ms;
    static LONGLONG frequency;
    static LONGLONG interval_start;
    static LONGLONG previous_start;
    static LONGLONG max_gap, max_scanout, max_refresh;
    static unsigned int service_calls, refresh_calls, completed_frames;
    LARGE_INTEGER start = {0}, after_scanout = {0}, finish = {0};
    BOOL completed_scanout, refreshed;

    if (trace_timing < 0) {
        trace_timing = getenv("MERCENARIES_TRACE_PRESENT_TIMING") != NULL;
        if (trace_timing) {
            LARGE_INTEGER counter_frequency;
            QueryPerformanceFrequency(&counter_frequency);
            frequency = counter_frequency.QuadPart;
            if (frequency <= 0)
                trace_timing = 0;
        }
    }
    if (trace_timing)
        QueryPerformanceCounter(&start);
    completed_scanout = nv2a_hook_service_scanout();
    if (completed_scanout)
        d3d8_WaitForGuestFrameSlot();
    if (trace_timing)
        QueryPerformanceCounter(&after_scanout);
    refreshed = d3d8_ServiceDisplayRefresh(completed_scanout);
    preview_frame_sample(completed_scanout, refreshed);
    if (test_resolution_transitions < 0) {
        const char *delay_text;
        test_resolution_transitions =
            getenv("MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS") != NULL;
        test_presentation_resize_transitions = getenv(
            "MERCENARIES_TEST_PRESENTATION_RESIZE_TRANSITIONS") != NULL;
        delay_text = getenv(
            "MERCENARIES_TEST_INTERNAL_RESOLUTION_TRANSITIONS_DELAY_MS");
        test_resolution_delay_ms = delay_text ?
            (unsigned int)strtoul(delay_text, NULL, 10) : 0u;
        test_resolution_start_ms = GetTickCount64();
    }
    if (test_resolution_transitions && completed_scanout &&
        GetTickCount64() - test_resolution_start_ms >=
            test_resolution_delay_ms) {
        if (test_resolution_frames == 0u) {
            const char *prefix =
                getenv("MERCENARIES_TEST_INTERNAL_RESOLUTION_CAPTURE_PREFIX");
            if (prefix && *prefix)
                d3d8_DebugStartDisplayCapture(prefix, 500u, 64u);
        }
        ++test_resolution_frames;
        if (test_resolution_frames == 10u) {
            fprintf(stderr,
                    "[TEST-RESOLUTION-TRANSITION] frame=10 size=2880x2160\n");
            pgraph_d3d11_set_internal_resolution(2880u, 2160u);
            if (test_presentation_resize_transitions)
                d3d8_ResizePresentation((UINT)GetSystemMetrics(SM_CXSCREEN),
                    (UINT)GetSystemMetrics(SM_CYSCREEN), FALSE);
        } else if (test_resolution_frames == 30u) {
            fprintf(stderr,
                    "[TEST-RESOLUTION-TRANSITION] frame=30 size=640x480\n");
            pgraph_d3d11_set_internal_resolution(640u, 480u);
            if (test_presentation_resize_transitions)
                d3d8_ResizePresentation((UINT)GetSystemMetrics(SM_CXSCREEN),
                    (UINT)GetSystemMetrics(SM_CYSCREEN), FALSE);
        }
    }
    if (refreshed && g_xbox_mem_offset != 0) {
        volatile uint32_t *vblank_count = (volatile uint32_t *)(
            (uintptr_t)g_xbox_mem_offset + 0x00793564u);
        const uint32_t count = ++*vblank_count;
        const uint32_t device = *(const volatile uint32_t *)(
            (uintptr_t)g_xbox_mem_offset + 0x00299378u);
        if (device >= 0x00010000u && device <= 0x03FFE214u) {
            volatile uint32_t *field_count = (volatile uint32_t *)(
                (uintptr_t)g_xbox_mem_offset + device + 0x1DE8u);
            if ((int32_t)(count - *field_count) > 0)
                *field_count = count;
        }
    }
    /* Gameplay primarily presents through this service callback, not through
     * d3d8_PresentFrame. Count refreshes separately from callback polls: these
     * are host refreshes, NOT proof of newly simulated/rendered game frames.
     * Opt-in timing leaves scanout, presentation and guest vblank behavior
     * unchanged, and distinguishes a stalled guest from slow GPU/display work. */
    if (trace_timing) {
        LONGLONG gap;
        QueryPerformanceCounter(&finish);
        gap = previous_start ? start.QuadPart - previous_start : 0;
        if (gap > max_gap) max_gap = gap;
        if (after_scanout.QuadPart - start.QuadPart > max_scanout)
            max_scanout = after_scanout.QuadPart - start.QuadPart;
        if (finish.QuadPart - after_scanout.QuadPart > max_refresh)
            max_refresh = finish.QuadPart - after_scanout.QuadPart;
        previous_start = start.QuadPart;
        if (service_calls == 0u)
            interval_start = start.QuadPart;
        ++service_calls;
        if (refreshed) ++refresh_calls;
        if (completed_scanout) ++completed_frames;
        if (finish.QuadPart - interval_start >= frequency) {
            const double milliseconds = 1000.0 / (double)frequency;
            fprintf(stderr,
                    "[HOST-SERVICE-TIMING] polls=%u refreshes=%u frames=%u "
                    "interval_ms=%.3f max_poll_gap_ms=%.3f "
                    "max_scanout_ms=%.3f max_refresh_ms=%.3f\n",
                    service_calls, refresh_calls, completed_frames,
                    (finish.QuadPart - interval_start) * milliseconds,
                    max_gap * milliseconds, max_scanout * milliseconds,
                    max_refresh * milliseconds);
            fflush(stderr);
            service_calls = refresh_calls = completed_frames = 0u;
            max_gap = max_scanout = max_refresh = 0;
        }
    }
}

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);
static BOOL find_default_game_dir(void);
static BOOL verify_retail_xbe(const void *data, size_t size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);
extern void recomp_func_trace_dump(void);
extern void pgraph_d3d11_init(void);
extern void pgraph_d3d11_shutdown(void);

static void trace_dump_counted_manager_summary(const char *label,
                                               uint32_t manager_va)
{
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t request_states[3] = {0};
    uint32_t read_states[3] = {0};
    uint32_t object_states[8] = {0};
    uint32_t null_requests = 0;
    uint32_t objects = 0;
    uint32_t node;

    if (base == 0u) return;
    node = *(const volatile uint32_t *)(base + manager_va + 0x24u);
    while (node >= 0x00010000u && node < 0x04000000u && objects < 4096u) {
        const uint32_t object =
            *(const volatile uint32_t *)(base + node + 8u);
        uint32_t request;
        uint32_t next;

        if (object == 0u) break;
        if (object < 0x00010000u || object >= 0x04000000u) break;
        if (*(const volatile uint8_t *)(base + object + 0x20u) < 8u)
            ++object_states[*(const volatile uint8_t *)(base + object + 0x20u)];
        if (strcmp(label, "MODEL") == 0 && objects < 12u) {
            fprintf(stderr,
                    "  [MODEL PENDING %u] object=%08X name=%08X request=%08X "
                    "loaded=%d desired=%d state=%u desired_state=%u list=%u "
                    "ticks=%d/%d/%d/%d\n",
                    objects, object,
                    *(const volatile uint32_t *)(base + object + 0x24u),
                    *(const volatile uint32_t *)(base + object + 0x10u),
                    *(const volatile int16_t *)(base + object + 0x14u),
                    *(const volatile int16_t *)(base + object + 0x16u),
                    *(const volatile uint8_t *)(base + object + 0x20u),
                    *(const volatile uint8_t *)(base + object + 0x21u),
                    *(const volatile uint8_t *)(base + object + 0x22u),
                    *(const volatile int16_t *)(base + object + 0x18u),
                    *(const volatile int16_t *)(base + object + 0x1Au),
                    *(const volatile int16_t *)(base + object + 0x1Cu),
                    *(const volatile int16_t *)(base + object + 0x1Eu));
        }
        request = *(const volatile uint32_t *)(base + object + 0x10u);
        if (request >= 0x00010000u && request < 0x04000000u) {
            const uint32_t request_state =
                *(const volatile uint32_t *)(base + request + 0x18u);
            const uint32_t read =
                *(const volatile uint32_t *)(base + request + 0x14u);
            if (request_state < 3u) ++request_states[request_state];
            if (read >= 0x00010000u && read < 0x04000000u) {
                const uint32_t read_state =
                    *(const volatile uint32_t *)(base + read + 0x20u);
                if (read_state < 3u) ++read_states[read_state];
            }
        } else {
            ++null_requests;
        }
        ++objects;
        next = *(const volatile uint32_t *)(base + node);
        if (next == node) break;
        node = next;
    }
    fprintf(stderr,
            "[%s SUMMARY] pending=%u objects=%u null_requests=%u "
            "request_states=%u/%u/%u read_states=%u/%u/%u "
            "object_states=%u/%u/%u/%u/%u/%u/%u/%u\n",
            label, *(const volatile uint32_t *)(base + manager_va + 0x30u),
            objects, null_requests,
            request_states[0], request_states[1], request_states[2],
            read_states[0], read_states[1], read_states[2],
            object_states[0], object_states[1], object_states[2],
            object_states[3], object_states[4], object_states[5],
            object_states[6], object_states[7]);
}

static void trace_dump_counted_wait_summaries(void)
{
    trace_dump_counted_manager_summary("TEXTURE", 0x007934C8u);
    trace_dump_counted_manager_summary("MODEL", 0x00643860u);
}

static void trace_dump_recent_functions(void)
{
    const uint32_t end = g_recomp_recent_func_idx;
    const uint32_t count = end < 64u ? end : 64u;
    uint32_t i;

    fprintf(stderr, "[RECENT FUNCS] count=%u\n", count);
    for (i = 0; i < count; ++i) {
        const uint32_t index = (end - count + i) & 63u;
        fprintf(stderr, "  [%02u] 0x%08X\n", i,
                g_recomp_recent_funcs[index]);
    }
}

static void trace_dump_recent_game_functions(void)
{
    const uint32_t end = g_recomp_recent_game_func_idx;
    const uint32_t count = end < 256u ? end : 256u;
    uint32_t i;

    recomp_dump_frontend_post_lua_state();
    fprintf(stderr, "[RECENT GAME FUNCS] count=%u\n", count);
    for (i = 0; i < count; ++i) {
        const uint32_t index = (end - count + i) & 255u;
        fprintf(stderr, "  [%03u] 0x%08X\n", i,
                g_recomp_recent_game_funcs[index]);
    }
}

static void trace_dump_current_disc_file(void)
{
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    const uint32_t object = g_ecx;

    /* PblDiscFile::Tell is a two-instruction leaf, so ECX still identifies
     * the file object while this function is current. Other PblDiscFile
     * methods reuse ECX internally and are not safe to sample this way. */
    if (g_recomp_current_func != 0x00209B80u || base == 0u ||
        object < 0x00010000u || object > 0x03FFFF00u)
        return;

    fprintf(stderr,
            "[CURRENT DISC FILE] object=%08X handle=%08X offset=%08X "
            "size=%08X cache=%08X name=%.*s\n",
            object,
            *(const volatile uint32_t *)(base + object + 0x88u),
            *(const volatile uint32_t *)(base + object + 0x8Cu),
            *(const volatile uint32_t *)(base + object + 0x90u),
            *(const volatile uint32_t *)(base + object + 0xCCu),
            120, (const char *)(base + object + 0x10u));
}

static void trace_dump_recent_asset_requests(void)
{
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    const uint32_t end = g_recomp_recent_asset_request_idx;
    const uint32_t count = end < 512u ? end : 512u;
    uint32_t seen[512];
    uint32_t seen_count = 0u;
    uint32_t states[8] = {0};
    uint32_t i;

    if (base == 0u) return;
    for (i = 0u; i < count; ++i) {
        const uint32_t request =
            g_recomp_recent_asset_requests[(end - count + i) & 511u];
        uint32_t j;
        if (request < 0x00010000u || request >= 0x04000000u) continue;
        for (j = 0u; j < seen_count; ++j)
            if (seen[j] == request) break;
        if (j != seen_count) continue;
        seen[seen_count++] = request;
    }
    fprintf(stderr, "[RECENT ASSET REQUESTS] unique=%u samples=%u\n",
            seen_count, count);
    for (i = 0u; i < seen_count; ++i) {
        const uint32_t request = seen[i];
        const uint32_t state =
            *(const volatile uint32_t *)(base + request + 0x18u);
        const uint32_t read =
            *(const volatile uint32_t *)(base + request + 0x14u);
        const uint32_t read_state =
            (read >= 0x00010000u && read < 0x04000000u) ?
                *(const volatile uint32_t *)(base + read + 0x20u) : 0xFFFFFFFFu;
        if (state < 8u) ++states[state];
        fprintf(stderr,
                "  req=%08X name=%08X type=%08X state=%u read=%08X read_state=%u\n",
                request,
                *(const volatile uint32_t *)(base + request),
                *(const volatile uint32_t *)(base + request + 4u),
                state, read, read_state);
    }
    fprintf(stderr,
            "[RECENT ASSET STATES] 0=%u 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u\n",
            states[0], states[1], states[2], states[3], states[4],
            states[5], states[6], states[7]);
}
void trace_dump_stream_manager(void)
{
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    const uint32_t manager = *(const volatile uint32_t *)(base + 0x0085BF18u);
    const uint32_t active = *(const volatile uint32_t *)(base + manager + 0x218u);
    const uint32_t needs_start = *(const volatile uint32_t *)(base + manager + 4u);
    const uint32_t needs_end = *(const volatile uint32_t *)(base + manager + 8u);
    const uint32_t pending_start = *(const volatile uint32_t *)(base + manager + 0x110u);
    const uint32_t pending_end = *(const volatile uint32_t *)(base + manager + 0x114u);
    const uint32_t needs_count = *(const volatile uint8_t *)(base + manager) ?
        64u : (needs_end + 64u - needs_start) % 64u;
    const uint32_t pending_count = *(const volatile uint8_t *)(base + manager + 0x10Cu) ?
        64u : (pending_end + 64u - pending_start) % 64u;
    fprintf(stderr,
            "[STREAM MANAGER] needs=%u indices=%u/%u pending=%u indices=%u/%u active=%08X "
            "unused_reads=%u unused_io=%u files=%u\n",
            needs_count, needs_start, needs_end,
            pending_count, pending_start, pending_end,
            active,
            *(const volatile uint32_t *)(base + manager + 0x228u),
            *(const volatile uint32_t *)(base + manager + 0x238u),
            *(const volatile uint32_t *)(base + manager + 0x244u));
    if (active >= 0x00010000u && active < 0x04000000u) {
        fprintf(stderr,
                "[STREAM ACTIVE] buffer=%08X length=%u type=%u transfer=%u "
                "sectors=%u..%u file=%08X use=%u state=%u owner=%08X\n",
                *(const volatile uint32_t *)(base + active + 0x0Cu),
                *(const volatile uint32_t *)(base + active + 0x14u),
                *(const volatile uint32_t *)(base + active + 0x18u),
                *(const volatile uint32_t *)(base + active + 0x1Cu),
                *(const volatile uint32_t *)(base + active + 0x20u),
                *(const volatile uint32_t *)(base + active + 0x24u),
                *(const volatile uint32_t *)(base + active + 0x28u),
                *(const volatile uint32_t *)(base + active + 0x44u),
                *(const volatile uint32_t *)(base + active + 0x48u),
                *(const volatile uint32_t *)(base + active + 0x4Cu));
    }
    {
        const uint32_t files_data = *(const volatile uint32_t *)(base + manager + 0x23Cu);
        const uint32_t file_count = *(const volatile uint32_t *)(base + manager + 0x244u);
        uint32_t i;

        if (files_data >= 0x00010000u && files_data < 0x04000000u && file_count <= 16u) {
            for (i = 0u; i < file_count; ++i) {
                const uint32_t file = *(const volatile uint32_t *)(base + files_data + i * 4u);
                uint32_t read_node = 0u, io_node = 0u, read = 0u, io = 0u;
                if (file < 0x00010000u || file >= 0x04000000u) continue;
                read_node = *(const volatile uint32_t *)(base + file + 0x10u);
                io_node = *(const volatile uint32_t *)(base + file + 0x20u);
                if (read_node >= 0x00010000u && read_node < 0x04000000u)
                    read = *(const volatile uint32_t *)(base + read_node + 8u);
                if (io_node >= 0x00010000u && io_node < 0x04000000u)
                    io = *(const volatile uint32_t *)(base + io_node + 8u);
                fprintf(stderr,
                        "[STREAM FILE %u] file=%08X lsn=%u hash=%08X handle=%08X size=%u "
                        "read_node=%08X read=%08X io_node=%08X io=%08X\n",
                        i, file,
                        *(const volatile uint32_t *)(base + file),
                        *(const volatile uint32_t *)(base + file + 4u),
                        *(const volatile uint32_t *)(base + file + 8u),
                        *(const volatile uint32_t *)(base + file + 0xCu),
                        read_node, read, io_node, io);
                if (read >= 0x00010000u && read < 0x04000000u) {
                    fprintf(stderr,
                            "[STREAM READ %u] request=%08X file_id=%08X sectors=%u..%u "
                            "offset=%u length=%u status=%u data=%08X io=%08X owner=%08X\n",
                            i, read,
                            *(const volatile uint32_t *)(base + read + 0x0Cu),
                            *(const volatile uint32_t *)(base + read + 0x10u),
                            *(const volatile uint32_t *)(base + read + 0x14u),
                            *(const volatile uint32_t *)(base + read + 0x18u),
                            *(const volatile uint32_t *)(base + read + 0x1Cu),
                            *(const volatile uint32_t *)(base + read + 0x20u),
                            *(const volatile uint32_t *)(base + read + 0x24u),
                            *(const volatile uint32_t *)(base + read + 0x28u),
                            *(const volatile uint32_t *)(base + read + 0x2Cu));
                }
                if (io >= 0x00010000u && io < 0x04000000u) {
                    fprintf(stderr,
                            "[STREAM IO %u] request=%08X buffer=%08X length=%u type=%u transfer=%u "
                            "sectors=%u..%u file=%08X use=%u state=%u owner=%08X\n",
                            i, io,
                            *(const volatile uint32_t *)(base + io + 0x0Cu),
                            *(const volatile uint32_t *)(base + io + 0x14u),
                            *(const volatile uint32_t *)(base + io + 0x18u),
                            *(const volatile uint32_t *)(base + io + 0x1Cu),
                            *(const volatile uint32_t *)(base + io + 0x20u),
                            *(const volatile uint32_t *)(base + io + 0x24u),
                            *(const volatile uint32_t *)(base + io + 0x28u),
                            *(const volatile uint32_t *)(base + io + 0x44u),
                            *(const volatile uint32_t *)(base + io + 0x48u),
                            *(const volatile uint32_t *)(base + io + 0x4Cu));
                }
            }
        }
    }
}
static void trace_dump_effect_hash_stall(void)
{
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    const uint32_t sp = g_esp;
    uint32_t table;
    uint32_t object;
    uint32_t occupied = 0;
    uint32_t i;

    if (g_recomp_current_func != 0x001F2A60u || sp > 0x03FFFFC0u) return;

    table = *(const volatile uint32_t *)(base + sp + 0x0Cu);
    if (table < 0x00010410u || table > 0x03FFFBEFu) return;
    object = table - 0x410u;
    for (i = 0; i < 256u; ++i) {
        if (*(const volatile uint32_t *)(base + table + i * 4u) != 0u) ++occupied;
    }

    fprintf(stderr,
            "[EFFECT HASH STALL] key=%08X table=%08X mask=%u object=%08X "
            "count=%u occupied=%u loop=%u declared=%u value=%08X\n",
            *(const volatile uint32_t *)(base + sp + 0x08u), table,
            *(const volatile uint32_t *)(base + sp + 0x10u), object,
            *(const volatile uint32_t *)(base + object + 0x08u), occupied,
            *(const volatile uint32_t *)(base + sp + 0x14u),
            *(const volatile uint32_t *)(base + sp + 0x3Cu),
            *(const volatile uint32_t *)(base + sp + 0x20u));
    fprintf(stderr,
            "[EFFECT TABL CHUNK] %08X %08X %08X %08X %08X %08X %08X\n",
            *(const volatile uint32_t *)(base + sp + 0x50u),
            *(const volatile uint32_t *)(base + sp + 0x54u),
            *(const volatile uint32_t *)(base + sp + 0x58u),
            *(const volatile uint32_t *)(base + sp + 0x5Cu),
            *(const volatile uint32_t *)(base + sp + 0x60u),
            *(const volatile uint32_t *)(base + sp + 0x64u),
            *(const volatile uint32_t *)(base + sp + 0x68u));
}

static volatile DWORD g_trace_watchdog_game_thread_id;

static DWORD WINAPI trace_watchdog_thread(LPVOID parameter){
    DWORD timeout_ms = (DWORD)(uintptr_t)parameter;

    Sleep(timeout_ms);
    {
        HANDLE game_thread = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                THREAD_QUERY_INFORMATION,
            FALSE, g_trace_watchdog_game_thread_id);
        if (game_thread != NULL) {
            CONTEXT context;
            HMODULE module = GetModuleHandleW(NULL);
            int have_context = 0;
            memset(&context, 0, sizeof(context));
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(game_thread) != (DWORD)-1) {
                have_context = GetThreadContext(game_thread, &context) != 0;
                ResumeThread(game_thread);
            }
#if defined(_M_X64)
            if (have_context) {
                fprintf(stderr,
                        "[TRACE WATCHDOG HOST] tid=%lu rip=%p rva=%08llX "
                        "rsp=%p rax=%p rbx=%p rcx=%p rdx=%p "
                        "rsi=%p rdi=%p\n",
                        (unsigned long)g_trace_watchdog_game_thread_id,
                        (void *)(uintptr_t)context.Rip,
                        (unsigned long long)(context.Rip -
                            (DWORD64)(uintptr_t)module),
                        (void *)(uintptr_t)context.Rsp,
                        (void *)(uintptr_t)context.Rax,
                        (void *)(uintptr_t)context.Rbx,
                        (void *)(uintptr_t)context.Rcx,
                        (void *)(uintptr_t)context.Rdx,
                        (void *)(uintptr_t)context.Rsi,
                        (void *)(uintptr_t)context.Rdi);
            }
#endif
            CloseHandle(game_thread);
        }
    }
    fprintf(stderr,
            "[TRACE WATCHDOG] guest state after %lu ms: current=0x%08X "
            "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "ebp=%08X esp=%08X\n",
            (unsigned long)timeout_ms, g_recomp_current_func,
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_seh_ebp, g_esp);
    fprintf(stderr,
            "[PFIFO WATCHDOG] stage=%u submission=%u get=%08X put=%08X "
            "word=%08X subchannel=%u method=%04X\n",
            g_nv2a_pfifo_diag_stage, g_nv2a_pfifo_diag_submission,
            g_nv2a_pfifo_diag_get, g_nv2a_pfifo_diag_put,
            g_nv2a_pfifo_diag_word, g_nv2a_pfifo_diag_subchannel,
            g_nv2a_pfifo_diag_method);
    fprintf(stderr,
            "[MMIO PUT WATCHDOG] hits=%llu value=%08X rip=%p->%p\n",
            (unsigned long long)g_nv2a_mmio_put_hits,
            g_nv2a_mmio_put_last_value,
            (void *)g_nv2a_mmio_put_last_rip_before,
            (void *)g_nv2a_mmio_put_last_rip_after);
    {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t sp = g_esp;
        unsigned int i;

        fprintf(stderr, "[GUEST STACK] esp=%08X", sp);
        if (sp >= 0x00010000u && sp <= 0x03FFFFC0u) {
            for (i = 0; i < 16u; ++i) {
                if ((i & 3u) == 0u)
                    fprintf(stderr, "\n  +%02X:", i * 4u);
                fprintf(stderr, " %08X",
                        *(const volatile uint32_t *)(base + sp + i * 4u));
            }
        }
        fprintf(stderr, "\n");
    }
    {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        fprintf(stderr,
                "[SHELL STATE] main=%08X sub=%08X force_load=%u "
                "load_screen_active=%u\n",
                *(const volatile uint32_t *)(base + 0x00413F6Cu),
                *(const volatile uint32_t *)(base + 0x00413F68u),
                *(const volatile uint32_t *)(base + 0x00413FF4u),
                *(const volatile uint8_t *)(base + 0x00365D30u));
    }
    trace_dump_counted_wait_summaries();
    trace_dump_stream_manager();
    trace_dump_effect_hash_stall();
    trace_dump_recent_asset_requests();
    trace_dump_recent_functions();
    trace_dump_recent_game_functions();
    trace_dump_current_disc_file();
#if defined(ENABLE_RECOMP_FUNC_TRACE)
    recomp_func_trace_dump();
#endif
    fflush(stderr);
    return 0;
}

static void start_trace_watchdog(void)
{
    const char *value = getenv("MERCENARIES_TRACE_TIMEOUT_MS");
    char *end = NULL;
    unsigned long timeout_ms;
    HANDLE thread;

    if (!value || value[0] == '\0') return;
    timeout_ms = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || timeout_ms == 0 ||
        timeout_ms > 0xFFFFFFFFul) {
        fprintf(stderr, "Ignoring invalid MERCENARIES_TRACE_TIMEOUT_MS=%s\n",
                value);
        return;
    }

    g_trace_watchdog_game_thread_id = GetCurrentThreadId();
    thread = CreateThread(NULL, 0, trace_watchdog_thread,
                          (LPVOID)(uintptr_t)timeout_ms, 0, NULL);
    if (thread) CloseHandle(thread);
}

static HWND g_host_window;
static IDirect3DDevice8 *g_host_d3d_device;
static int g_host_tools_open, g_host_inactive, g_host_resizing, g_host_ready;
#define WM_RECOMP_DISPLAY (WM_APP + 0x52)


static void host_apply_recomp_options(uint32_t changes)
{
    uint32_t output_width, output_height;
    uint32_t internal_width, internal_height;
    uint32_t aspect_width, aspect_height;
    recomp_display_mode display_mode;
    DWORD style;
    RECT rect;

    d3d8_SetFrameCap(recomp_options_fps_cap());
    d3d8_SetVSync(recomp_options_vsync());
    d3d8_SetForceAnisotropic16x(recomp_options_anisotropic_16x());
    pgraph_d3d11_set_haze_mode(recomp_options_authentic_haze());
    recomp_options_presentation_aspect(&aspect_width, &aspect_height);
    d3d8_SetPresentationAspect(aspect_width, aspect_height);
    xbox_set_widescreen_enabled(recomp_options_aspect_ratio() != 0);

    if (changes & (RECOMP_OPTIONS_CHANGE_ASPECT | RECOMP_OPTIONS_CHANGE_FOV))
        recomp_options_refresh_retail_camera_projection();

    if (changes & (RECOMP_OPTIONS_CHANGE_ASPECT |
                   RECOMP_OPTIONS_CHANGE_RESOLUTION |
                   RECOMP_OPTIONS_CHANGE_SSAA)) {
        recomp_options_internal_resolution_size(&internal_width,
                                                &internal_height);
        pgraph_d3d11_set_internal_resolution(internal_width,
                                             internal_height);
    }
    if (!(changes & (RECOMP_OPTIONS_CHANGE_RESOLUTION |
                     RECOMP_OPTIONS_CHANGE_DISPLAY)) || !g_host_window)
        return;

    recomp_options_resolution_size(&output_width, &output_height);
    display_mode = recomp_options_display_mode();
    style = display_mode == RECOMP_DISPLAY_WINDOWED ?
        WS_OVERLAPPEDWINDOW : WS_POPUP;
    rect.left = rect.top = 0;
    rect.right = (LONG)output_width;
    rect.bottom = (LONG)output_height;
    MONITORINFO monitor={sizeof(monitor)};
    GetMonitorInfoA(MonitorFromWindow(g_host_window,MONITOR_DEFAULTTONEAREST),&monitor);
    int exclusive=display_mode==RECOMP_DISPLAY_FULLSCREEN && !g_host_tools_open && !g_host_inactive;
    if (display_mode != RECOMP_DISPLAY_WINDOWED && !exclusive) {
        output_width = (uint32_t)(monitor.rcMonitor.right-monitor.rcMonitor.left);
        output_height = (uint32_t)(monitor.rcMonitor.bottom-monitor.rcMonitor.top);
        rect.right = (LONG)output_width;
        rect.bottom = (LONG)output_height;
    } else if (display_mode == RECOMP_DISPLAY_WINDOWED) {
        AdjustWindowRect(&rect, style, FALSE);
    }
    if(g_host_resizing)return;
    g_host_resizing=1;
    /* Release exclusive mode before resizing the HWND for the desktop/panel. */
    if (!d3d8_ResizePresentation(output_width,output_height,exclusive)) {
        fprintf(stderr,"[RECOMP-OPTIONS] presentation apply failed; retaining desktop-sized window\n");
        if(display_mode!=RECOMP_DISPLAY_WINDOWED){
            output_width=(uint32_t)(monitor.rcMonitor.right-monitor.rcMonitor.left);
            output_height=(uint32_t)(monitor.rcMonitor.bottom-monitor.rcMonitor.top);
            rect.right=(LONG)output_width;rect.bottom=(LONG)output_height;
            d3d8_ResizePresentation(output_width,output_height,FALSE);
        }
    }
    SetWindowLongPtrA(g_host_window, GWL_STYLE, (LONG_PTR)style);
    SetWindowPos(g_host_window, NULL,
                 display_mode == RECOMP_DISPLAY_WINDOWED ? rect.left : monitor.rcMonitor.left,
                 display_mode == RECOMP_DISPLAY_WINDOWED ? rect.top : monitor.rcMonitor.top,
                 rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    g_host_resizing=0;
}
static void host_dev_display(int open){
    g_host_tools_open=open;
    if(recomp_options_display_mode()!=RECOMP_DISPLAY_FULLSCREEN)return;
    if(open)host_apply_recomp_options(RECOMP_OPTIONS_CHANGE_DISPLAY);
    else PostMessageA(g_host_window,WM_RECOMP_DISPLAY,0,0);
}


static void dev_bookmark(void) { preview_snapshot("USER-MARKER-DEV-MENU"); }

static LRESULT CALLBACK host_window_proc(HWND hwnd, UINT message,
                                        WPARAM wparam, LPARAM lparam)
{
    if(recomp_controls_message(hwnd,message,wparam,lparam))return message==WM_SETCURSOR ? TRUE : 0;
    switch (message) {
    case WM_ACTIVATEAPP:
        g_host_inactive=!wparam;
        if(g_host_ready && !g_host_resizing && recomp_options_display_mode()!=RECOMP_DISPLAY_WINDOWED)
            PostMessageA(hwnd,WM_RECOMP_DISPLAY,0,0);
        break;
    case WM_DISPLAYCHANGE:
        if(g_host_ready && !g_host_resizing && recomp_options_display_mode()!=RECOMP_DISPLAY_WINDOWED)
            PostMessageA(hwnd,WM_RECOMP_DISPLAY,0,0);
        break;
    case WM_RECOMP_DISPLAY:
        if(g_host_ready && !g_host_resizing)host_apply_recomp_options(RECOMP_OPTIONS_CHANGE_DISPLAY);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F11 && recomp_dev_menu_allowed()) {
            if (!(lparam & (1L << 30))) recomp_dev_freecam_toggle();
            return 0;
        }
        if (wparam == VK_F9 && recomp_dev_menu_allowed()) {
            if (!(lparam & (1L << 30))) recomp_dev_menu_toggle();
            return 0;
        }
        if (wparam == VK_F8 && !(lparam & (1L << 30))) {
            preview_snapshot("USER-MARKER-F8");
            return 0;
        }
        break;
    case WM_CLOSE:
        xbox_preview_log_event("session", "window close requested");
        fprintf(stderr, "[HOST-WINDOW] WM_CLOSE hwnd=%p\n", (void *)hwnd);
        fflush(stderr);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        fprintf(stderr, "[HOST-WINDOW] WM_DESTROY hwnd=%p\n", (void *)hwnd);
        fflush(stderr);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

static BOOL host_graphics_init(HINSTANCE instance, int show_command)
{
    static const char class_name[] = "MercenariesRecompWindow";
    WNDCLASSA window_class;
    RECT rect;
    D3DPRESENT_PARAMETERS present;
    IDirect3D8 *d3d;
    HRESULT hr;
    const char *hide_window = getenv("MERCENARIES_HIDE_WINDOW");
    uint32_t output_width, output_height;
    uint32_t internal_width, internal_height;
    uint32_t aspect_width, aspect_height;
    recomp_display_mode display_mode;
    DWORD window_style;
    int window_x = CW_USEDEFAULT, window_y = CW_USEDEFAULT;

    recomp_options_init();
    d3d8_SetFrameCap(recomp_options_fps_cap());
    d3d8_SetVSync(recomp_options_vsync());
    d3d8_SetForceAnisotropic16x(recomp_options_anisotropic_16x());
    pgraph_d3d11_set_haze_mode(recomp_options_authentic_haze());
    recomp_options_resolution_size(&output_width, &output_height);
    recomp_options_internal_resolution_size(&internal_width, &internal_height);
    recomp_options_presentation_aspect(&aspect_width, &aspect_height);
    d3d8_SetPresentationAspect(aspect_width, aspect_height);
    xbox_set_widescreen_enabled(recomp_options_aspect_ratio() != 0);
    display_mode = recomp_options_display_mode();
    window_style = display_mode == RECOMP_DISPLAY_WINDOWED ?
        WS_OVERLAPPEDWINDOW : WS_POPUP;
    rect.left = rect.top = 0;
    rect.right = (LONG)output_width;
    rect.bottom = (LONG)output_height;
    if (display_mode == RECOMP_DISPLAY_BORDERLESS) {
        window_x = window_y = 0;
        output_width = (uint32_t)GetSystemMetrics(SM_CXSCREEN);
        output_height = (uint32_t)GetSystemMetrics(SM_CYSCREEN);
        rect.right = (LONG)output_width;
        rect.bottom = (LONG)output_height;
    } else if (display_mode == RECOMP_DISPLAY_FULLSCREEN) {
        window_x = window_y = 0;
    }

    memset(&window_class, 0, sizeof(window_class));
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = host_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorA(NULL, IDC_ARROW);
    window_class.hIcon = mercenaries_app_icon(instance, 1);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = class_name;

    if (!RegisterClassA(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fprintf(stderr, "Host graphics: RegisterClass failed (%lu)\n",
                GetLastError());
        return FALSE;
    }

    AdjustWindowRect(&rect, window_style, FALSE);
    g_host_window = CreateWindowExA(
        0, class_name, "Mercenaries Recomp", window_style,
        window_x, window_y, rect.right - rect.left,
        rect.bottom - rect.top, NULL, NULL, instance, NULL);
    if (!g_host_window) {
        fprintf(stderr, "Host graphics: CreateWindow failed (%lu)\n",
                GetLastError());
        return FALSE;
    }

    SendMessageA(g_host_window, WM_SETICON, ICON_BIG,
        (LPARAM)mercenaries_app_icon(instance, 1));
    SendMessageA(g_host_window, WM_SETICON, ICON_SMALL,
        (LPARAM)mercenaries_app_icon(instance, 0));

    memset(&present, 0, sizeof(present));
    present.BackBufferWidth = output_width;
    present.BackBufferHeight = output_height;
    present.BackBufferFormat = D3DFMT_A8R8G8B8;
    present.BackBufferCount = 1;
    present.MultiSampleType = D3DMULTISAMPLE_NONE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = g_host_window;
    present.Windowed = display_mode != RECOMP_DISPLAY_FULLSCREEN;
    present.EnableAutoDepthStencil = TRUE;
    present.AutoDepthStencilFormat = D3DFMT_D24S8;

    d3d = xbox_Direct3DCreate8(0);
    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 0, g_host_window, 0,
                                   &present, &g_host_d3d_device);
    if (FAILED(hr)) {
        fprintf(stderr, "Host graphics: CreateDevice failed (0x%08lX)\n", hr);
        DestroyWindow(g_host_window);
        g_host_window = NULL;
        return FALSE;
    }

    pgraph_d3d11_init();
    pgraph_d3d11_set_internal_resolution(internal_width, internal_height);
    recomp_options_set_apply_callback(host_apply_recomp_options);
    recomp_dev_menu_init(g_host_window, dev_bookmark);
    recomp_dev_menu_set_display_callback(host_dev_display);
    recomp_dev_overlay_init();
    g_host_ready=1;
    recomp_controls_init(g_host_window);
    ShowWindow(g_host_window,
               hide_window && hide_window[0] != '\0' ? SW_HIDE : show_command);
    UpdateWindow(g_host_window);
    return TRUE;
}

typedef struct allocator_watch_arm_args {
    DWORD target_thread_id;
    HANDLE ready_event;
    uint32_t guest_va;
} allocator_watch_arm_args;

static volatile uint32_t g_redscene_watchpoint_guest_va;
static volatile uint32_t g_redscene_watchpoint_old_value;
static volatile LONG g_redscene_collision_watch_live;
static volatile uint32_t g_havok_watchpoint_guest_va;
static volatile uint32_t g_havok_watchpoint_floor;
static volatile uint32_t g_havok_watchpoint_old_value;
static volatile LONG g_havok_watchpoint_lifecycle_reset;
static volatile uint32_t g_global_pointer_watchpoint_guest_va;
static volatile uint32_t g_global_pointer_watchpoint_old_value;
static int g_global_pointer_watchpoint_custom;
static volatile uint32_t g_transition_pool_watchpoint_guest_va;
static volatile uint32_t g_transition_pool_watchpoint_old_value;

static DWORD WINAPI arm_allocator_watchpoint_thread(void *opaque)
{
    allocator_watch_arm_args *args = (allocator_watch_arm_args *)opaque;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                               THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, args->target_thread_id);
    if (thread != NULL) {
        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT context;
            memset(&context, 0, sizeof(context));
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = (DWORD64)((uintptr_t)g_xbox_mem_offset +
                                        args->guest_va);
                context.Dr6 = 0;
                context.Dr7 = (context.Dr7 & ~(DWORD64)0xF000Fu) |
                              (DWORD64)0xD0001u;
                SetThreadContext(thread, &context);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    SetEvent(args->ready_event);
    return 0;
}

void recomp_arm_allocator_watchpoint(void)
{
    static volatile LONG armed;
    allocator_watch_arm_args args;
    HANDLE worker;

    if (InterlockedCompareExchange(&armed, 1, 0) != 0)
        return;
    args.target_thread_id = GetCurrentThreadId();
    args.ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    args.guest_va = 0x00FA67C8u;
    if (args.ready_event == NULL)
        return;
    worker = CreateThread(NULL, 0, arm_allocator_watchpoint_thread, &args,
                          0, NULL);
    if (worker == NULL) {
        CloseHandle(args.ready_event);
        return;
    }
    WaitForSingleObject(args.ready_event, INFINITE);
    CloseHandle(worker);
    CloseHandle(args.ready_event);
    fprintf(stderr, "[ALLOCATOR-WATCH] armed guest=00FA67C8 native=%p\n",
            (void *)((uintptr_t)g_xbox_mem_offset + 0x00FA67C8u));
    fflush(stderr);
}
void recomp_arm_redscene_watchpoint(uint32_t guest_va)
{
    static volatile LONG armed;
    const int collision_retarget =
        getenv("MERCENARIES_COLLISION_WRITE_SLOT") != NULL;
    allocator_watch_arm_args args;
    HANDLE worker;

    if (InterlockedCompareExchange(&armed, 1, 0) != 0 &&
        !collision_retarget)
        return;
    args.target_thread_id = GetCurrentThreadId();
    args.ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    args.guest_va = guest_va;
    if (args.ready_event == NULL)
        return;
    worker = CreateThread(NULL, 0, arm_allocator_watchpoint_thread, &args,
                          0, NULL);
    if (worker == NULL) {
        CloseHandle(args.ready_event);
        return;
    }
    WaitForSingleObject(args.ready_event, INFINITE);
    CloseHandle(worker);
    CloseHandle(args.ready_event);
    InterlockedExchange((volatile LONG *)&g_redscene_watchpoint_guest_va,
                        (LONG)guest_va);
    InterlockedExchange((volatile LONG *)&g_redscene_watchpoint_old_value,
                        (LONG)*(const uint32_t *)(
                            (uintptr_t)g_xbox_mem_offset + guest_va));
    InterlockedExchange(&g_redscene_collision_watch_live,
                        collision_retarget ? 1 : 0);
    fprintf(stderr, "[REDSCENE-WATCH] armed guest=%08X native=%p\n",
            guest_va,
            (void *)((uintptr_t)g_xbox_mem_offset + guest_va));
    fflush(stderr);
}
void recomp_arm_havok_stack_watchpoint(uint32_t memory)
{
    static volatile LONG armed;
    allocator_watch_arm_args args;
    HANDLE worker;
    uint32_t guest_va;

    if (memory < 0x00010000u || memory > 0x03FFFFE8u ||
        InterlockedCompareExchange(&armed, 1, 0) != 0)
        return;
    guest_va = memory + 8u;
    args.target_thread_id = GetCurrentThreadId();
    args.ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    args.guest_va = guest_va;
    if (args.ready_event == NULL)
        return;
    worker = CreateThread(NULL, 0, arm_allocator_watchpoint_thread, &args,
                          0, NULL);
    if (worker == NULL) {
        CloseHandle(args.ready_event);
        return;
    }
    WaitForSingleObject(args.ready_event, INFINITE);
    CloseHandle(worker);
    CloseHandle(args.ready_event);
    InterlockedExchange((volatile LONG *)&g_havok_watchpoint_guest_va,
                        (LONG)guest_va);
    InterlockedExchange((volatile LONG *)&g_havok_watchpoint_floor,
                        (LONG)*(const uint32_t *)(
                            (uintptr_t)g_xbox_mem_offset + guest_va));
    InterlockedExchange((volatile LONG *)&g_havok_watchpoint_old_value,
                        (LONG)*(const uint32_t *)(
                            (uintptr_t)g_xbox_mem_offset + guest_va));
    InterlockedExchange(&g_havok_watchpoint_lifecycle_reset, 0);
    fprintf(stderr,
            "[HAVOK-STACK-WATCH] armed memory=%08X current=%08X free=%u "
            "native=%p\n",
            memory, g_havok_watchpoint_floor,
            *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + memory + 0xCu),
            (void *)((uintptr_t)g_xbox_mem_offset + guest_va));
    fflush(stderr);
}
static void arm_global_pointer_watchpoint(uint32_t guest_va)
{
    allocator_watch_arm_args args;
    HANDLE worker;

    args.target_thread_id = GetCurrentThreadId();
    args.ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    args.guest_va = guest_va;
    if (args.ready_event == NULL)
        return;
    worker = CreateThread(NULL, 0, arm_allocator_watchpoint_thread, &args,
                          0, NULL);
    if (worker == NULL) {
        CloseHandle(args.ready_event);
        return;
    }
    WaitForSingleObject(args.ready_event, INFINITE);
    CloseHandle(worker);
    CloseHandle(args.ready_event);
    InterlockedExchange(
        (volatile LONG *)&g_global_pointer_watchpoint_guest_va,
        (LONG)guest_va);
    InterlockedExchange(
        (volatile LONG *)&g_global_pointer_watchpoint_old_value,
        (LONG)*(const uint32_t *)((uintptr_t)g_xbox_mem_offset + guest_va));
    xbox_preview_log_event("GLOBAL-POINTER-WATCH", "armed guest=%08X value=%08X",
            guest_va, *(const uint32_t *)((uintptr_t)g_xbox_mem_offset +
                                         guest_va));
    fflush(stderr);
}
void recomp_arm_transition_pool_watchpoint(uint32_t guest_va)
{
    static volatile LONG armed;
    allocator_watch_arm_args args;
    HANDLE worker;

    if (getenv("MERCENARIES_TRACE_TRANSITION_POOL_COUNT") == NULL ||
        guest_va < 0x00010000u || guest_va > 0x03FFFFFCu ||
        InterlockedCompareExchange(&armed, 1, 0) != 0)
        return;
    args.target_thread_id = GetCurrentThreadId();
    args.ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    args.guest_va = guest_va;
    if (args.ready_event == NULL)
        return;
    worker = CreateThread(NULL, 0, arm_allocator_watchpoint_thread, &args,
                          0, NULL);
    if (worker == NULL) {
        CloseHandle(args.ready_event);
        return;
    }
    WaitForSingleObject(args.ready_event, INFINITE);
    CloseHandle(worker);
    CloseHandle(args.ready_event);
    InterlockedExchange(
        (volatile LONG *)&g_transition_pool_watchpoint_guest_va,
        (LONG)guest_va);
    InterlockedExchange(
        (volatile LONG *)&g_transition_pool_watchpoint_old_value,
        (LONG)*(const uint32_t *)((uintptr_t)g_xbox_mem_offset + guest_va));
    fprintf(stderr,
            "[TRANSITION-POOL-WATCH] armed guest=%08X count=%u\n",
            guest_va,
            *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + guest_va));
    fflush(stderr);
}
void recomp_transition_this_checkpoint(uint32_t stage, uint32_t expected,
                                       uint32_t actual)
{
    if (getenv("MERCENARIES_TRACE_TRANSITION_POOL_COUNT") == NULL ||
        expected == actual)
        return;
    fprintf(stderr,
            "[TRANSITION-THIS-CLOBBER] stage=%u expected=%08X actual=%08X "
            "current=%08X esp=%08X\n",
            stage, expected, actual, g_recomp_current_func, g_esp);
    trace_dump_recent_functions();
    trace_dump_recent_game_functions();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0xE6u);
}
void recomp_transition_callee_checkpoint(uint32_t stage,
                                         uint32_t expected_esp,
                                         uint32_t actual_esp,
                                         uint32_t expected_esi,
                                         uint32_t saved_esi,
                                         uint32_t pending_esi)
{
    if (getenv("MERCENARIES_TRACE_TRANSITION_POOL_COUNT") == NULL ||
        (expected_esp == actual_esp && expected_esi == pending_esi))
        return;
    fprintf(stderr,
            "[TRANSITION-CALLEE-ABI] stage=%u expected_esp=%08X "
            "actual_esp=%08X "
            "expected_esi=%08X saved_esi=%08X pending_esi=%08X "
            "current=%08X\n",
            stage, expected_esp, actual_esp, expected_esi, saved_esi,
            pending_esi,
            g_recomp_current_func);
    trace_dump_recent_functions();
    trace_dump_recent_game_functions();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0xE7u);
}
void recomp_transition_pool_reset_checkpoint(uint32_t guest_va)
{
    uint32_t count;
    if (getenv("MERCENARIES_TRACE_TRANSITION_POOL_COUNT") == NULL ||
        guest_va < 0x00010000u || guest_va > 0x03FFFFFCu)
        return;
    count = *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + guest_va);
    fprintf(stderr,
            "[TRANSITION-POOL-RESET] guest=%08X count=%u current=%08X\n",
            guest_va, count, g_recomp_current_func);
    fflush(stderr);
    if (count > 32u) {
        trace_dump_recent_functions();
        trace_dump_recent_game_functions();
        TerminateProcess(GetCurrentProcess(), 0xE5u);
    }
}
/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
static void capture_crash_guest_memory(void)
{
    static volatile LONG attempted;
    static unsigned char chunk[65536];
    const char *path = getenv("MERCENARIES_DUMP_CRASH_GUEST_PATH");
    uint32_t offset = 0u;
    HANDLE file;

    if (path == NULL || path[0] == '\0' || g_xbox_mem_offset == 0 ||
        InterlockedCompareExchange(&attempted, 1, 0) != 0)
        return;
    /* Windows minidumps may omit file-mapped guest RAM. Preserve that RAM
     * separately for opt-in diagnostics. Never overwrite an earlier capture,
     * allocate heap storage, or write back into the crashed game's memory. */
    file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[CRASH-GUEST] create failed error=%lu path=%s\n",
                (unsigned long)GetLastError(), path);
        return;
    }
    while (offset < 0x04000000u) {
        SIZE_T copied = 0u;
        DWORD written = 0u;
        const void *source = (const void *)((uintptr_t)g_xbox_mem_offset + offset);
        if (!ReadProcessMemory(GetCurrentProcess(), source, chunk,
                               sizeof(chunk), &copied) || copied != sizeof(chunk) ||
            !WriteFile(file, chunk, (DWORD)sizeof(chunk), &written, NULL) ||
            written != sizeof(chunk))
            break;
        offset += (uint32_t)sizeof(chunk);
    }
    CloseHandle(file);
    fprintf(stderr, "[CRASH-GUEST] bytes=%u expected=67108864 path=%s\n", offset, path);
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    typedef struct havok_watch_event {
        uint64_t rva;
        uint32_t old_value, value, current_func;
        uint32_t eax, ebx, ecx, edx, esi, edi, esp;
    } havok_watch_event;
    static havok_watch_event havok_events[64];
    static uint32_t havok_event_index;    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP &&
        (ep->ContextRecord->Dr6 & 1u) != 0u) {
        uintptr_t module_base = (uintptr_t)GetModuleHandleA(NULL);
        const uint32_t havok_guest_va =
            (uint32_t)InterlockedCompareExchange(
                (volatile LONG *)&g_havok_watchpoint_guest_va, 0, 0);
        const uint32_t global_pointer_guest_va =
            (uint32_t)InterlockedCompareExchange(
                (volatile LONG *)&g_global_pointer_watchpoint_guest_va, 0, 0);
        const uint32_t transition_pool_guest_va =
            (uint32_t)InterlockedCompareExchange(
                (volatile LONG *)&g_transition_pool_watchpoint_guest_va,
                0, 0);
        const uint32_t redscene_guest_va =
            (uint32_t)InterlockedCompareExchange(
                (volatile LONG *)&g_redscene_watchpoint_guest_va, 0, 0);
        const uint32_t watched_guest_va = transition_pool_guest_va != 0u ?
            transition_pool_guest_va : global_pointer_guest_va != 0u ?
            global_pointer_guest_va : havok_guest_va != 0u ?
            havok_guest_va :
            (redscene_guest_va != 0u ? redscene_guest_va : 0x00FA67C8u);
        const uint32_t value = *(const uint32_t *)(
            (uintptr_t)g_xbox_mem_offset + watched_guest_va);
        if (transition_pool_guest_va != 0u) {
            const uint32_t old_value = (uint32_t)InterlockedExchange(
                (volatile LONG *)&g_transition_pool_watchpoint_old_value,
                (LONG)value);
            fprintf(stderr,
                    "[TRANSITION-POOL-WATCH] guest=%08X old=%u value=%u "
                    "rva=%llX func=%08X eax=%08X ebx=%08X ecx=%08X "
                    "edx=%08X esi=%08X edi=%08X esp=%08X\n",
                    watched_guest_va, old_value, value,
                    (unsigned long long)(ep->ContextRecord->Rip - module_base),
                    g_recomp_current_func, g_eax, g_ebx, g_ecx, g_edx,
                    g_esi, g_edi, g_esp);
            fflush(stderr);
            if (value > 32u) {
                trace_dump_recent_functions();
                trace_dump_recent_game_functions();
                TerminateProcess(GetCurrentProcess(), 0xE4u);
            }
            ep->ContextRecord->Dr6 = 0u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (global_pointer_guest_va != 0u) {
            const uint32_t old_value = (uint32_t)InterlockedExchange(
                (volatile LONG *)&g_global_pointer_watchpoint_old_value,
                (LONG)value);
            xbox_preview_log_sample("GLOBAL-POINTER-WATCH",
                    "guest=%08X old=%08X value=%08X "
                    "rva=%llX func=%08X",
                    watched_guest_va, old_value, value,
                    (unsigned long long)(ep->ContextRecord->Rip - module_base),
                    g_recomp_current_func);
            fflush(stderr);
            if (!g_global_pointer_watchpoint_custom && value == 0xFFFFFFFFu) {
                trace_dump_recent_functions();
                trace_dump_recent_game_functions();
                TerminateProcess(GetCurrentProcess(), 0xE3u);
            }
            ep->ContextRecord->Dr6 = 0u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }        if (havok_guest_va != 0u) {
            const uint32_t floor = (uint32_t)InterlockedCompareExchange(
                (volatile LONG *)&g_havok_watchpoint_floor, 0, 0);
            const uint32_t old_value = (uint32_t)InterlockedExchange(
                (volatile LONG *)&g_havok_watchpoint_old_value, (LONG)value);
            if (InterlockedCompareExchange(
                    &g_havok_watchpoint_lifecycle_reset, 0, 0) != 0) {
                if (value == 0u) {
                    ep->ContextRecord->Dr6 = 0u;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
                if (value >= 0x00010000u && value < 0x04000000u) {
                    InterlockedExchange(
                        (volatile LONG *)&g_havok_watchpoint_floor,
                        (LONG)value);
                    InterlockedExchange(&g_havok_watchpoint_lifecycle_reset,
                                        0);
                    fprintf(stderr,
                            "[HAVOK-STACK-WATCH] reinitialized current=%08X\n",
                            value);
                    fflush(stderr);
                    ep->ContextRecord->Dr6 = 0u;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
            if (value >= floor && value < 0x04000000u) {
                havok_watch_event *event =
                    &havok_events[havok_event_index++ & 63u];
                event->rva = ep->ContextRecord->Rip - module_base;
                event->old_value = old_value;
                event->value = value;
                event->current_func = g_recomp_current_func;
                event->eax = g_eax;
                event->ebx = g_ebx;
                event->ecx = g_ecx;
                event->edx = g_edx;
                event->esi = g_esi;
                event->edi = g_edi;
                event->esp = g_esp;
                ep->ContextRecord->Dr6 = 0u;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (value == 0u && old_value == floor &&
                g_recomp_current_func == 0x001F2A90u) {
                /* sub_001F2A90 is the retail 32-bit zero-fill leaf.  The
                 * hardware data breakpoint fires from inside its translated
                 * native loop, where the recompiler's guest register globals
                 * have not yet been written back; reading g_esp arguments
                 * here therefore observes stale caller state.  The captured
                 * retail caller clears 0x40 dwords beginning at this address
                 * while constructing the next scene object, after the stack
                 * has returned to its armed floor.  The backing allocation is
                 * being reused; this is not a downward stack allocation. */
                fprintf(stderr,
                        "[HAVOK-STACK-WATCH] lifecycle memset-zero "
                        "current=%08X\n",
                        old_value);
                fflush(stderr);
                InterlockedExchange(&g_havok_watchpoint_lifecycle_reset, 1);
                InterlockedExchange(
                    (volatile LONG *)&g_havok_watchpoint_floor, 0);
                ep->ContextRecord->Dr6 = 0u;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            fprintf(stderr,
                    "[HAVOK-STACK-UNDERFLOW-WATCH] guest=%08X rip=%p "
                    "rva=%llX current_func=%08X floor=%08X old=%08X "
                    "value=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X "
                    "esi=%08X edi=%08X esp=%08X seh_ebp=%08X dr6=%llX\n",
                    watched_guest_va,
                    (void *)(uintptr_t)ep->ContextRecord->Rip,
                    (unsigned long long)(ep->ContextRecord->Rip - module_base),
                    g_recomp_current_func, floor, old_value, value,
                    g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp,
                    g_seh_ebp, (unsigned long long)ep->ContextRecord->Dr6);
            {
                const uint32_t shown = havok_event_index < 24u ?
                    havok_event_index : 24u;
                fprintf(stderr, "[HAVOK-STACK-WATCH-HISTORY] count=%u\n", shown);
                for (uint32_t i = shown; i != 0u; --i) {
                    const havok_watch_event *event = &havok_events[
                        (havok_event_index - i) & 63u];
                    fprintf(stderr,
                            "  [%02u] rva=%llX func=%08X %08X->%08X "
                            "eax=%08X ebx=%08X ecx=%08X edx=%08X "
                            "esi=%08X edi=%08X esp=%08X\n",
                            shown - i, (unsigned long long)event->rva,
                            event->current_func, event->old_value, event->value,
                            event->eax, event->ebx, event->ecx, event->edx,
                            event->esi, event->edi, event->esp);
                }
            }
            trace_dump_recent_functions();
            trace_dump_recent_game_functions();
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0xE2u);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        const uint32_t old_value = (uint32_t)InterlockedCompareExchange(
            (volatile LONG *)&g_redscene_watchpoint_old_value, 0, 0);
        if (redscene_guest_va != 0u &&
            getenv("MERCENARIES_COLLISION_WRITE_SLOT") != NULL) {
            const uint32_t object = watched_guest_va - 0x20u;
            const int valid_collision_state =
                value == 0u || value == object || value == object + 8u ||
                value == object + 0x10u || value == object + 0x18u;
            if (InterlockedCompareExchange(
                    &g_redscene_collision_watch_live, 0, 0) == 0) {
                ep->ContextRecord->Dr6 = 0u;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (valid_collision_state) {
                InterlockedExchange(
                    (volatile LONG *)&g_redscene_watchpoint_old_value,
                    (LONG)value);
                fprintf(stderr,
                        "[COLLISION-WATCH-VALID] guest=%08X old=%08X "
                        "value=%08X rva=%llX func=%08X\n",
                        watched_guest_va, old_value, value,
                        (unsigned long long)(ep->ContextRecord->Rip -
                                             module_base),
                        g_recomp_current_func);
                fflush(stderr);
                if (value == 0u)
                    InterlockedExchange(&g_redscene_collision_watch_live, 0);
                ep->ContextRecord->Dr6 = 0u;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        fprintf(stderr,
                "[%s-WATCH-HIT] guest=%08X rip=%p rva=%llX current=%08X "
                "old=%08X value=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X esp=%08X seh_ebp=%08X dr6=%llX\n",
                redscene_guest_va != 0u ? "REDSCENE" : "ALLOCATOR",
                watched_guest_va,
                (void *)(uintptr_t)ep->ContextRecord->Rip,
                (unsigned long long)(ep->ContextRecord->Rip - module_base),
                g_recomp_current_func, old_value, value,
                g_eax, g_ebx, g_ecx, g_edx,
                g_esi, g_edi, g_esp, g_seh_ebp,
                (unsigned long long)ep->ContextRecord->Dr6);
        trace_dump_recent_functions();
        trace_dump_recent_game_functions();
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0xE1u);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];
        uint32_t fault_xbox_va =
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);
        int is_write =
            ep->ExceptionRecord->ExceptionInformation[0] != 0;

        if (fault_xbox_va >= 0x80000000u &&
            fault_xbox_va < 0xC0000000u &&
            xbox_MapPhysicalAlias(fault_xbox_va)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (fault_xbox_va >= 0xFD000000 &&
            fault_xbox_va < 0xFE000000 &&
            nv2a_hook_handle_mmio(ep->ContextRecord, fault_addr,
                                  fault_xbox_va, is_write)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (fault_xbox_va >= 0xFE800000u &&
            fault_xbox_va < 0xFE880000u &&
            apu_hook_handle_mmio(ep->ContextRecord, fault_addr,
                                 fault_xbox_va, is_write)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (fault_xbox_va >= 0xFEC00000u &&
            fault_xbox_va < 0xFEC01000u &&
            ac97_hook_handle_mmio(ep->ContextRecord, fault_addr,
                                  fault_xbox_va, is_write)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (fault_xbox_va >= 0xF0000000 &&
            fault_xbox_va < 0xFD000000 &&
            nv2a_hook_handle_vram(fault_addr, fault_xbox_va)) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        uintptr_t module_base = (uintptr_t)GetModuleHandleA(NULL);
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)module_base;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(
            module_base + (uintptr_t)dos->e_lfanew);
        uintptr_t module_end =
            module_base + (uintptr_t)nt->OptionalHeader.SizeOfImage;
        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        xbox_preview_log_crash(ep->ExceptionRecord->ExceptionCode,
            (uintptr_t)ep->ContextRecord->Rip,g_recomp_current_func,
            g_eax,g_ecx,g_edx,g_esp);
        capture_crash_guest_memory();
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        if (g_esp >= 0x00010000u && g_esp <= 0x03FFFFC0u) {
            const uint32_t *guest_stack = (const uint32_t *)(
                (uintptr_t)g_xbox_mem_offset + g_esp);
            fprintf(stderr, "  Guest stack:");
            for (int i = 0; i < 16; ++i)
                fprintf(stderr, " %08X", guest_stack[i]);
            fputc('\n', stderr);
        }
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            fault_xbox_va);
        fprintf(stderr, "  Native module: base=0x%llX, fault RVA=0x%llX\n",
            (unsigned long long)module_base,
            (unsigned long long)(ep->ContextRecord->Rip - module_base));
        fprintf(stderr, "  Current guest function: 0x%08X\n",
                g_recomp_current_func);
        if (g_esi >= 0x00010000u && g_esi <= 0x03FFFC00u) {
            const uint8_t *object = (const uint8_t *)(
                (uintptr_t)g_xbox_mem_offset + g_esi);
            fprintf(stderr,
                    "  ESI object: base=%08X vtable=%08X "
                    "ring[+254]=%08X ring[+258]=%08X "
                    "ring[+260]=%08X\n",
                    g_esi, *(const uint32_t *)(object + 0x000u),
                    *(const uint32_t *)(object + 0x254u),
                    *(const uint32_t *)(object + 0x258u),
                    *(const uint32_t *)(object + 0x260u));
        }
        trace_dump_recent_functions();
        trace_dump_recent_game_functions();
        recomp_lua_dump_poscall_ring_on_crash();
        recomp_func_trace_dump();

        {
            const uint32_t d3d_device_va =
                *(const uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x00299378u);

            fprintf(stderr, "  D3D device: 0x%08X\n", d3d_device_va);
            if (d3d_device_va >= 0x00010000u && d3d_device_va < 0x04000000u) {
                const uint32_t *device = (const uint32_t *)(
                    (uintptr_t)g_xbox_mem_offset + d3d_device_va);

                fprintf(stderr,
                        "  D3D push: cur=%08X end=%08X flags=%08X\n",
                        device[0], device[1], device[2]);
                fprintf(stderr,
                        "  D3D lists: first=%08X second=%08X count=%08X "
                        "entries=%08X,%08X,%08X,%08X\n",
                        device[0x1A04u / 4u], device[0x1A08u / 4u],
                        device[0x1A10u / 4u], device[0x1A14u / 4u],
                        device[0x1A18u / 4u], device[0x1A1Cu / 4u],
                        device[0x1A20u / 4u]);
            }
        }

        /* Print native stack return addresses for debugging */
        {
            uintptr_t *sp = (uintptr_t *)ep->ContextRecord->Rsp;
            fprintf(stderr, "  Native stack (first 8 return addrs):\n");
            for (int i = 0; i < 64 && sp[i]; i++) {
                if (sp[i] >= module_base && sp[i] < module_end) {
                    fprintf(stderr, "    [%d] 0x%llX\n", i, (unsigned long long)sp[i]);
                }
            }
        }
        fflush(stderr);
    }

    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        xbox_preview_log_crash(ep->ExceptionRecord->ExceptionCode,
            (uintptr_t)ep->ContextRecord->Rip,g_recomp_current_func,
            g_eax,g_ecx,g_edx,g_esp);
        uintptr_t module_base = (uintptr_t)GetModuleHandleA(NULL);
        fprintf(stderr,
                "[CRASH] Guest breakpoint at RIP=0x%llX (module RVA=0x%llX) "
                "current=0x%08X\n",
                (unsigned long long)ep->ContextRecord->Rip,
                (unsigned long long)(ep->ContextRecord->Rip - module_base),
                g_recomp_current_func);
        fprintf(stderr,
                "  Xbox regs: eax=%08X ebx=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X ebp=%08X esp=%08X\n",
                g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi,
                g_seh_ebp, g_esp);
        trace_dump_recent_functions();
        trace_dump_recent_game_functions();
        recomp_lua_dump_poscall_ring_on_crash();
        recomp_func_trace_dump();
        fflush(stderr);
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        xbox_preview_log_crash(ep->ExceptionRecord->ExceptionCode,
            (uintptr_t)ep->ContextRecord->Rip,g_recomp_current_func,
            g_eax,g_ecx,g_edx,g_esp);
        uintptr_t module_base = (uintptr_t)GetModuleHandleA(NULL);
        fprintf(stderr,
                "[CRASH] Integer divide by zero at RIP=0x%llX (module RVA=0x%llX)\n",
                (unsigned long long)ep->ContextRecord->Rip,
                (unsigned long long)(ep->ContextRecord->Rip - module_base));
        fprintf(stderr,
                "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X\n",
                g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Current guest function: 0x%08X\n",
                g_recomp_current_func);
        if (g_esi >= 0x00010000u && g_esi <= 0x03FFFC00u) {
            const uint8_t *object = (const uint8_t *)(
                (uintptr_t)g_xbox_mem_offset + g_esi);
            fprintf(stderr,
                    "  ESI object: base=%08X vtable=%08X "
                    "ring[+254]=%08X ring[+258]=%08X "
                    "ring[+260]=%08X\n",
                    g_esi, *(const uint32_t *)(object + 0x000u),
                    *(const uint32_t *)(object + 0x254u),
                    *(const uint32_t *)(object + 0x258u),
                    *(const uint32_t *)(object + 0x260u));
        }
        trace_dump_recent_functions();        recomp_func_trace_dump();
        fflush(stderr);
    }


    return EXCEPTION_CONTINUE_SEARCH;
}

/* ── WinMain ───────────────────────────────────────────────── */

static BOOL game_dir_has_xbe(const char *directory)
{
    char path[MAX_PATH];
    DWORD attributes;

    if (!directory || directory[0] == '\0' ||
        snprintf(path, sizeof(path), "%s\\default.xbe", directory) < 0) {
        return FALSE;
    }
    attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static const char *diagnostic_save_directory(const char *normal_directory)
{
    const char *isolated = getenv("MERCENARIES_TEST_ISOLATE_INPUT");
    const char *directory;
    if (!isolated || !*isolated || *isolated == '0')
        return normal_directory;
    directory = getenv("MERCENARIES_TEST_SAVE_DIR");
    return directory && *directory ? directory : normal_directory;
}

static BOOL select_game_dir(const char *directory)
{
    DWORD length;

    if (!game_dir_has_xbe(directory)) return FALSE;
    length = GetFullPathNameA(directory, (DWORD)sizeof(g_game_dir_storage),
                              g_game_dir_storage, NULL);
    if (length == 0 || length >= sizeof(g_game_dir_storage)) return FALSE;
    g_game_dir = g_game_dir_storage;
    {
        char root[MAX_PATH];
        char *separator;
        int written;

        written = snprintf(root, sizeof(root), "%s", g_game_dir_storage);
        if (written > 0 && (size_t)written < sizeof(root)) {
            separator = strrchr(root, '\\');
            if (separator) {
                *separator = '\0';
                separator = strrchr(root, '\\');
            }
            if (separator) {
                *separator = '\0';
                written = snprintf(g_save_dir_storage,
                    sizeof(g_save_dir_storage), "%s\\%s", root,
                    DEFAULT_SAVE_DIR);
                if (written > 0 &&
                    (size_t)written < sizeof(g_save_dir_storage))
                    g_save_dir = g_save_dir_storage;
            }
        }
    }
    return TRUE;
}

static BOOL find_default_game_dir(void)
{
    char executable_dir[MAX_PATH];
    char candidate[MAX_PATH];
    DWORD length;
    unsigned int depth;

    if (select_game_dir(DEFAULT_GAME_DIR)) return TRUE;

    length = GetModuleFileNameA(NULL, executable_dir,
                                (DWORD)sizeof(executable_dir));
    if (length == 0 || length >= sizeof(executable_dir)) return FALSE;
    {
        char *separator = strrchr(executable_dir, '\\');
        if (!separator) return FALSE;
        *separator = '\0';
    }

    for (depth = 0; depth < 8u; ++depth) {
        int written = snprintf(candidate, sizeof(candidate), "%s\\%s",
                               executable_dir, DEFAULT_GAME_DIR);
        if (written > 0 && (size_t)written < sizeof(candidate) &&
            select_game_dir(candidate)) {
            return TRUE;
        }
        {
            char *separator = strrchr(executable_dir, '\\');
            if (!separator || separator == executable_dir + 2) break;
            *separator = '\0';
        }
    }
    return FALSE;
}

static BOOL verify_retail_xbe(const void *data, size_t size)
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    unsigned char *hash_object = NULL;
    DWORD hash_object_size = 0;
    DWORD digest_size = 0;
    DWORD result_size = 0;
    NTSTATUS status;
    BOOL matches = FALSE;

    if (!data || size == 0 || size > 0xFFFFFFFFu) return FALSE;
    status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                         NULL, 0);
    if (status < 0) goto cleanup;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                               (PUCHAR)&hash_object_size,
                               sizeof(hash_object_size), &result_size, 0);
    if (status < 0) goto cleanup;
    status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                               (PUCHAR)&digest_size, sizeof(digest_size),
                               &result_size, 0);
    if (status < 0 || digest_size != sizeof(digest)) goto cleanup;
    hash_object = (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                             hash_object_size);
    if (!hash_object) goto cleanup;
    status = BCryptCreateHash(algorithm, &hash, hash_object, hash_object_size,
                              NULL, 0, 0);
    if (status < 0) goto cleanup;
    status = BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0);
    if (status < 0) goto cleanup;
    status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (status < 0) goto cleanup;
    matches = memcmp(digest, g_required_retail_xbe_sha256,
                     sizeof(digest)) == 0;

cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (hash_object) HeapFree(GetProcessHeap(), 0, hash_object);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return matches;
}

/* Direct launches also offer the selector. The launcher's explicit child flag
 * prevents recursion and keeps Launch Vanilla independent of saved selections. */
static int redirect_to_mod_selector(void)
{
    if (getenv("MERCENARIES_MOD_BOOTSTRAPPED") || getenv("MERCENARIES_TEST_ISOLATE_INPUT"))
        return 0;
    WCHAR root[MAX_PATH], pattern[MAX_PATH], launcher[MAX_PATH], command[MAX_PATH + 4];
    DWORD n = GetModuleFileNameW(NULL, root, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return 0;
    WCHAR *slash = wcsrchr(root, L'\\');
    if (!slash)
        return 0;
    *slash = 0;
    if (swprintf_s(pattern, MAX_PATH, L"%s\\mods\\*", root) < 0)
        return 0;
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(pattern, &entry);
    BOOL found = FALSE;
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(entry.cFileName, L".") &&
            wcscmp(entry.cFileName, L".."))
        {
            found = TRUE;
            break;
        }
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    if (!found)
        return 0;
    if (swprintf_s(launcher, MAX_PATH, L"%s\\Mercenaries Recompiled.exe", root) < 0)
        return -1;
    swprintf_s(command, MAX_PATH + 4, L"\"%s\"", launcher);
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(launcher, command, NULL, NULL, FALSE, 0, NULL, root, &si, &pi))
    {
        MessageBoxW(NULL,
                    L"The mod selector could not start. Keep Mercenaries Recompiled.exe alongside "
                    L"the game executable.",
                    L"Mercenaries", MB_OK | MB_ICONERROR);
        return -1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    int redirected=redirect_to_mod_selector();
    if(redirected) return redirected>0?0:1;
    recomp_register_shader_warmup();
    void *xbe_data = NULL;
    size_t xbe_size = 0;
    const char *env_game_dir = getenv("MERCENARIES_GAME_DIR");

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    if (env_game_dir && env_game_dir[0] != '\0') {
        if (!select_game_dir(env_game_dir))
            g_game_dir = env_game_dir;
    } else {
        find_default_game_dir();
    }
    const char *selected_saves = getenv("MERCENARIES_SAVE_DIR");
    if (selected_saves && *selected_saves) g_save_dir = selected_saves;
    if (!xbox_mod_overlay_init()) {
        MessageBoxA(NULL, "The selected mod overlay could not be loaded. Launch again from the mod selector.", "Mercenaries", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (snprintf(g_xbe_path, sizeof(g_xbe_path), "%s\\default.xbe",
                 g_game_dir) < 0)
        return 1;

    /* The opt-in route probe batches each diagnostic record before its
       existing fflush. Unbuffered UCRT fprintf can issue tiny WriteFile calls
       and materially stall the simulation when tracing to redirected files.
       Keep the normal preview's crash-log policy unchanged for this A/B test. */
    if (getenv("MERCENARIES_DIAGNOSTIC_BUFFERED_LOGS") != NULL) {
        static char diagnostic_stdout_buffer[16384];
        static char diagnostic_stderr_buffer[16384];
        setvbuf(stdout, diagnostic_stdout_buffer, _IOFBF,
                sizeof(diagnostic_stdout_buffer));
        setvbuf(stderr, diagnostic_stderr_buffer, _IOFBF,
                sizeof(diagnostic_stderr_buffer));
    } else {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    int logging_enabled = recomp_developer_logging_enabled();
    xbox_kernel_set_logging_enabled(logging_enabled);
    if (!logging_enabled) {
        FILE *sink;
        freopen_s(&sink, "NUL", "w", stdout);
        freopen_s(&sink, "NUL", "w", stderr);
    }
    if (logging_enabled) {
        wchar_t directory[MAX_PATH]; DWORD length;
        length=GetEnvironmentVariableW(L"MERCENARIES_PREVIEW_LOG_DIR",directory,MAX_PATH);
        if(!length) {
            length=GetModuleFileNameW(NULL,directory,MAX_PATH);
            wchar_t *slash=length && length<MAX_PATH?wcsrchr(directory,L'\\'):NULL;
            if(slash && slash-directory<MAX_PATH-20) wcscpy(slash+1,L"preview-logs");
            else directory[0]=0;
        } else if(length>=MAX_PATH) directory[0]=0;
        if(directory[0] && xbox_preview_log_init(directory)) atexit(xbox_preview_log_shutdown);
    }
    printf("=== Mercenaries - Static Recompilation ===\n");
    printf("Game directory: %s\n", g_game_dir);
    printf("Loading XBE: %s\n", g_xbe_path);

    /* Install VEH handler (first handler in chain) */
    AddVectoredExceptionHandler(1, veh_handler);

    /* Step 1: Load XBE */
    if (!load_xbe(g_xbe_path, &xbe_data, &xbe_size)) {
        MessageBoxA(NULL, "Failed to load default.xbe.\n"
                    "Pass the extracted game directory as the first argument.",
                    "Mercenaries Recomp", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);
    if (!verify_retail_xbe(xbe_data, xbe_size)) {
        MessageBoxA(NULL,
                    "default.xbe is not the supported retail Mercenaries XBE.\n"
                    "Required SHA-256:\n"
                    "AA08EA21D952AC35F49C02C7E2ED08AA25AD7535FBBBCCC95636775F37BE99D7",
                    "Mercenaries Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }
    printf("Retail XBE SHA-256 verified.\n");
    xbox_preview_log_event("session", "supported retail XBE SHA256 verified");

    recomp_mod_compatibility_init();

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        const char *reason = xbox_GetMemoryLayoutError();
        char message[640];
        snprintf(message, sizeof(message), "Failed to initialize Xbox memory layout.\n\n%s", reason);
        xbox_preview_log_event("memory-init", "%s", reason);
        MessageBoxA(NULL, message, "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }
    if (!host_graphics_init(hInstance, nCmdShow)) {
        fprintf(stderr,
                "Host graphics unavailable; continuing without presentation.\n");
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);
    {
        const char *watch_text = getenv("MERCENARIES_TRACE_POINTER_WATCH_VA");
        if (watch_text != NULL && watch_text[0] != '\0') {
            char *end = NULL;
            unsigned long address = strtoul(watch_text, &end, 0);
            if (end != watch_text && *end == '\0' && address >= 0x10000ul &&
                address <= 0x03FFFFFCul && (address & 3ul) == 0ul) {
                g_global_pointer_watchpoint_custom = 1;
                arm_global_pointer_watchpoint((uint32_t)address);
            } else {
                fprintf(stderr, "[GLOBAL-POINTER-WATCH] rejected invalid guest address\n");
            }
        } else if (getenv("MERCENARIES_TRACE_GLOBAL_POINTER_WATCH") != NULL) {
            arm_global_pointer_watchpoint(0x00643844u);
        }
    }

    printf("Initializing NV2A MMIO bridge...\n");
    nv2a_hook_init(g_xbox_mem_offset, xbox_GetMappingHandle());

    printf("Initializing MCPX APU MMIO bridge...\n");
    g_apu_state = mcpx_apu_init_standalone(
        (uint8_t *)xbox_GetMemoryBase());
    if (!g_apu_state)
        fprintf(stderr, "MCPX APU unavailable; continuing without audio.\n");

    printf("Initializing MCPX AC97 MMIO bridge...\n");
    ac97_hook_init();

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();
    xbox_kernel_set_host_service_callback(mercenaries_host_service);

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        /* Hidden diagnostics have their own title/user/cache storage. The
         * ordinary preview retains its existing saves and settings location. */
        xbox_path_init(g_game_dir, diagnostic_save_directory(g_save_dir));
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();
    if (!start_kernel_tick_thread()) {
        fprintf(stderr, "Kernel tick thread unavailable (error %lu).\n",
                GetLastError());
    }

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", MERCENARIES_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    g_recomp_entry_trace_enabled =
        getenv("MERCENARIES_TRACE_RECOMP_ENTRIES") != NULL ||
        getenv("MERCENARIES_TRACK_RECOMP_ENTRIES") != NULL ||
        getenv("MERCENARIES_TRACE_PARTICLE_LIST") != NULL;
    g_recomp_irq_entry_safepoint_enabled =
        getenv("MERCENARIES_DISABLE_IRQ_ENTRY_SAFEPOINT") == NULL;
    g_recomp_particle_list_watch_enabled =
        getenv("MERCENARIES_TRACE_PARTICLE_LIST") != NULL;
    g_recomp_target_call_trace_enabled =
        getenv("MERCENARIES_TRACE_RECOMP_ENTRIES") != NULL ||
        getenv("MERCENARIES_TRACE_TARGET_CALLS") != NULL ||
        getenv("MERCENARIES_TRACE_UPDATE_ICALL") != NULL ||
        getenv("MERCENARIES_TRACE_TRANSITION_POOL_COUNT") != NULL ||
        getenv("MERCENARIES_TRACE_REDSCENE_WRITE_WATCH") != NULL;
    start_trace_watchdog();
    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    pgraph_d3d11_shutdown();
    if (g_host_d3d_device) {
        g_host_d3d_device->lpVtbl->Release(g_host_d3d_device);
        g_host_d3d_device = NULL;
    }
    if (g_host_window) {
        DestroyWindow(g_host_window);
        g_host_window = NULL;
    }

    if (g_apu_state) {
        mcpx_apu_shutdown(g_apu_state);
        g_apu_state = NULL;
    }
    stop_kernel_tick_thread();
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    if (argc > 1 && argv[1][0] != '\0')
        _putenv_s("MERCENARIES_GAME_DIR", argv[1]);
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
}

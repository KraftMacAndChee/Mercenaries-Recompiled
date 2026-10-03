#include "recomp_controls.h"
#include "preview_log.h"
#include "dev_menu.h"
/* Manual guest-function overrides and indirect-call diagnostics.
 *
 * Ordinary indirect calls resolve manual overrides before generated dispatch.
 * System-thread startup in kernel_bridge.c currently resolves generated first.
 * Overrides provide
 * host integration, correct demonstrated translation defects, or instrument
 * guest calls. Keep the guest calling convention and state transitions intact;
 * returning success from an unimplemented function can hide the actual fault.
 * The trace ring and bounded checkpoints support diagnosis without full tracing.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <float.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <xbox/xboxrecomp.h>
#include "nv2a_pgraph_d3d11.h"
#include "xinput_xbox.h"
#include "recomp_options.h"
#include "apu_mmio_hook.h"

/*
 * Diagnostic and test switches are process-launch configuration. Many of the
 * checkpoints below run once per guest update, so asking the CRT to rescan the
 * complete environment on every call is both unnecessary and surprisingly
 * expensive. Cache both present and absent values per host thread. All call
 * sites in this translation unit use static environment-name strings.
 */
#define RECOMP_ENV_CACHE_CAPACITY 1024u
typedef struct RecompEnvCacheEntry {
    const char *name;
    const char *value;
} RecompEnvCacheEntry;

#if defined(_MSC_VER)
static __declspec(thread) RecompEnvCacheEntry
    g_recomp_env_cache[RECOMP_ENV_CACHE_CAPACITY];
#else
static _Thread_local RecompEnvCacheEntry
    g_recomp_env_cache[RECOMP_ENV_CACHE_CAPACITY];
#endif

static const char *recomp_cached_getenv(const char *name)
{
    uintptr_t key;
    uint32_t index;
    uint32_t probe;

    if (name == NULL)
        return NULL;
    /* Callers use process-lifetime literals. Hash their identity, not every
     * character of the diagnostic name on every guest call. */
    key = (uintptr_t)name;
    index = (uint32_t)((key >> 4u) ^ (key >> 12u)) &
        (RECOMP_ENV_CACHE_CAPACITY - 1u);
    for (probe = 0u; probe < RECOMP_ENV_CACHE_CAPACITY; ++probe) {
        RecompEnvCacheEntry *entry = &g_recomp_env_cache[index];
        if (entry->name == NULL) {
            entry->name = name;
            entry->value = getenv(name);
            return entry->value;
        }
        if (entry->name == name)
            return entry->value;
        index = (index + 1u) & (RECOMP_ENV_CACHE_CAPACITY - 1u);
    }
    return getenv(name);
}

static uint32_t guest_u32(uint32_t xbox_va);

/* Applies to the implementation headers below as well as this file. Moving
 * one to another translation unit changes its launch-option cache context. */
#define getenv(name) recomp_cached_getenv(name)
void recomp_music_checkpoint(uint32_t stage, uint32_t state,
                             uint32_t value);

/* ICALL trace ring buffer */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 indirect call targets.
 *
 * The runtime owns these definitions; this port only reads them.
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
void recomp_icall_fail_log(uint32_t va);
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_ebx, g_esi, g_edi;
extern uint32_t g_seh_ebp;
extern double g_fp_stack[8];
extern uint32_t g_fp_top;
extern uint16_t g_x87_control_word, g_x87_status_word;
extern float g_xmm0[4], g_xmm1[4], g_xmm2[4], g_xmm3[4];
extern float g_xmm4[4], g_xmm5[4], g_xmm6[4], g_xmm7[4];
extern uint64_t g_mm0, g_mm1, g_mm2, g_mm3;
extern uint64_t g_mm4, g_mm5, g_mm6, g_mm7;
extern ptrdiff_t g_xbox_mem_offset;

/* Preserve every APU side effect while avoiding an OS exception and native
 * instruction decode for each known retail voice-parameter register access. */
uint32_t recomp_apu_read32(uint32_t address)
{
    uint32_t value;
    if (apu_hook_try_read32(address, &value)) return value;
    return *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + address);
}

void recomp_apu_write32(uint32_t address, uint32_t value)
{
    if (!apu_hook_try_write32(address, value))
        *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + address) = value;
}

extern uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
extern volatile uint32_t g_recomp_trace_dump_requested;
extern volatile uint32_t g_recomp_current_func;
extern volatile uint32_t g_recomp_entry_trace_enabled;
extern volatile uint32_t g_recomp_watchdog_heartbeat_enabled;
extern volatile uint32_t g_recomp_watchdog_heartbeat;
extern volatile uint32_t g_recomp_recent_funcs[64];
extern volatile uint32_t g_recomp_recent_func_idx;
extern volatile uint32_t g_recomp_recent_game_funcs[256];
extern volatile uint32_t g_recomp_recent_game_func_idx;

void recomp_pose_traversal_overflow(uint32_t pose, uint32_t joint,
                                    uint32_t count, uint32_t next_joint)
{
    static volatile LONG samples;
    const LONG sample = InterlockedIncrement(&samples);

    if (sample <= 32) {
        const uint32_t end = g_recomp_recent_game_func_idx;
        const uint32_t recent_count = end < 24u ? end : 24u;

        fprintf(stderr,
                "[REDPOSE-STACK] sample=%ld pose=%08X joint=%08X "
                "count=%u next=%08X current=%08X "
                "eax=%08X ebx=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X esp=%08X; traversal aborted safely\n",
                sample, pose, joint, count, next_joint,
                (uint32_t)g_recomp_current_func,
                g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);

        /* The first few samples preserve enough translated-game history to
         * identify the malformed hierarchy's producer without making a
         * recurring bad asset flood stderr or perturb frame pacing. */
        if (sample <= 8 && recent_count != 0u) {
            fprintf(stderr, "[REDPOSE-STACK] recent[%u]", recent_count);
            for (uint32_t i = 0u; i < recent_count; ++i) {
                const uint32_t index = (end - recent_count + i) & 255u;
                fprintf(stderr, " %08X",
                        (uint32_t)g_recomp_recent_game_funcs[index]);
            }
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
}

volatile uint32_t g_recomp_func_trace_armed = 1u;
static volatile uint32_t g_frontend_post_lua_target;
static volatile uint32_t g_frontend_post_lua_count;
static volatile uint32_t g_frontend_post_lua_state[512][9];
volatile uint32_t g_recomp_notification_owner_watch_enabled;
static uint32_t g_notification_owner_watch_address;
static uint32_t g_notification_owner_watch_expected;
static uint32_t g_notification_owner_watch_previous_func;
static int g_notification_owner_watch_armed;
#define NOTIFICATION_OWNER_AUTO_CAPACITY 256u
static uint32_t g_notification_owner_auto_addresses[
    NOTIFICATION_OWNER_AUTO_CAPACITY];
static uint32_t g_notification_owner_auto_expected[
    NOTIFICATION_OWNER_AUTO_CAPACITY];
static uint32_t g_notification_owner_auto_previous_func[
    NOTIFICATION_OWNER_AUTO_CAPACITY];
static uint32_t g_notification_owner_auto_count;
static int g_notification_owner_auto_enabled;
static int g_notification_owner_dispatch_watch_enabled;
volatile uint32_t g_recomp_14fa2b_entry;
volatile uint32_t g_mercenaries_gameplay_capture_active;
volatile uint32_t g_mercenaries_standup_complete;
volatile uint32_t g_mercenaries_roadblock_model_draw_active;
volatile uint32_t g_mercenaries_hq_briefing_active;
static uint32_t g_recomp_live_human_actor;
static uint32_t g_recomp_live_vehicle_actor;
static uint32_t g_recomp_live_player_ai;
extern void sub_001E2640(void);
extern void sub_001E8EC0(void);
extern void sub_001DF0B0(void);
extern void sub_002A1954(void);
extern void sub_002A1774(void);
extern void sub_002A795F(void);

extern void d3d8_DebugArmFlipCapture(const char *path);
extern void d3d8_DebugCaptureFrameToPath(const char *path);
extern void d3d8_DebugQueueFrameCapture(const char *path);
extern void d3d8_DebugStartDisplayCapture(const char *prefix,
                                          UINT interval_ms, UINT limit);
extern void d3d8_PresentFrame(void);
extern void d3d8_DebugSetPresentCallback(void (*callback)(void));
extern void recomp_arm_redscene_watchpoint(uint32_t guest_va);
extern void recomp_arm_havok_stack_watchpoint(uint32_t memory);
static uint32_t g_release_watch_va;
static uint32_t g_release_watch_last;
static uint32_t g_release_watch_previous_func;
static int g_release_watch_initialized;
static int g_release_watch_seen;
static volatile uint32_t g_shl_followup_budget;
static volatile uint32_t g_shl_followup_index;
static volatile uint32_t g_menu_text_trace_armed;
static uint32_t merc_heap_allocate(uint32_t heap, uint32_t flags,
                                   uint32_t requested);
static uint32_t merc_heap_free(uint32_t address);

/* Diagnostic-only ownership history for the first retail small-block pool.
 * Observe allocator entries; never repair links, counts, or guest memory. */
#define MERC_SMALL_POOL_WATCH_SLOTS 16384u
typedef struct merc_small_pool_watch_slot {
    uint32_t state, next, owner[16]; /* 0 unknown, 1 free, 2 allocated */
} merc_small_pool_watch_slot;
static merc_small_pool_watch_slot g_small_pool_watch[MERC_SMALL_POOL_WATCH_SLOTS];

static void recomp_small_pool_watch(uint32_t function)
{
    static int enabled = -1;
    static uint32_t pool_start, reports;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t start, end, head, stride, count, free_count, address, next, index;
    merc_small_pool_watch_slot *slot = NULL;
    const char *reason = NULL;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_SMALL_POOL_OWNERSHIP") != NULL;
    if (!enabled || reports >= 16u || base == 0u || g_ecx != 0x00643928u ||
        (function != 0x001F66C0u && function != 0x001F6AB0u))
        return;
    start = *(volatile uint32_t *)(base + g_ecx);
    end = *(volatile uint32_t *)(base + g_ecx + 4u);
    head = *(volatile uint32_t *)(base + g_ecx + 8u);
    stride = *(volatile uint32_t *)(base + g_ecx + 12u);
    count = *(volatile uint32_t *)(base + g_ecx + 16u);
    free_count = *(volatile uint32_t *)(base + g_ecx + 20u);
    if (start < 0x10000u || end > 0x04000000u || end <= start ||
        stride != 16u || count > MERC_SMALL_POOL_WATCH_SLOTS ||
        end - start != count * stride)
        return;
    if (pool_start != start) {
        memset(g_small_pool_watch, 0, sizeof(g_small_pool_watch));
        pool_start = start;
    }
    address = head;
    if (function == 0x001F6AB0u) {
        if (g_esp < 0x10000u || g_esp > 0x03FFFFF8u)
            return;
        address = *(volatile uint32_t *)(base + g_esp + 4u);
    }
    next = 0u;
    if (address == 0u && function == 0x001F66C0u && free_count == 0u)
        return; /* A genuinely empty pool is not corruption. */
    if (address < start || address >= end || (address - start) % stride != 0u) {
        reason = "invalid-or-empty-head";
    } else {
        index = (address - start) / stride;
        slot = &g_small_pool_watch[index];
        next = *(volatile uint32_t *)(base + address);
        if (function == 0x001F66C0u) {
            if (slot->state == 2u) reason = "allocate-already-live";
            else if (slot->state == 1u && next != slot->next) reason = "free-link-overwritten";
            else if ((next == 0u && free_count > 1u) ||
                     (next != 0u && (next < start || next >= end ||
                                    (next - start) % stride != 0u)))
                reason = "invalid-next-link";
        } else if (slot->state == 1u) reason = "double-free";
    }
    if (reason != NULL) {
        fprintf(stderr, "[SMALL-POOL-OWNERSHIP] reason=%s fn=%08X block=%08X "
                "head=%08X next=%08X free=%u count=%u expected=%08X state=%u "
                "esp=%08X ebx=%08X esi=%08X edi=%08X\n", reason, function,
                address, head, next, free_count, count, slot ? slot->next : 0u,
                slot ? slot->state : 0u, g_esp, g_ebx, g_esi, g_edi);
        if (slot != NULL) {
            fprintf(stderr, "  last-owner:");
            for (uint32_t i = 0u; i < 16u; ++i)
                fprintf(stderr, " %08X", slot->owner[i]);
            fputc('\n', stderr);
        }
        fprintf(stderr, "  current:");
        for (uint32_t i = 0u; i < 32u && i < g_recomp_recent_game_func_idx; ++i)
            fprintf(stderr, " %08X", g_recomp_recent_game_funcs[
                (g_recomp_recent_game_func_idx - 1u - i) & 255u]);
        fputc('\n', stderr);
        fflush(stderr);
        ++reports;
    }
    if (slot != NULL) {
        slot->state = function == 0x001F66C0u ? 2u : 1u;
        slot->next = function == 0x001F66C0u ? next : head;
        for (uint32_t i = 0u; i < 16u; ++i)
            slot->owner[i] = i < g_recomp_recent_game_func_idx ?
                g_recomp_recent_game_funcs[(g_recomp_recent_game_func_idx - 1u - i) & 255u] : 0u;
    }
}

void recomp_guest_watch_trace(uint32_t xbox_va)
{
    static int terrain_watch_enabled = -1;
    static int terrain_watch_seen;
    static uint32_t terrain_watch_last;
    static uint32_t terrain_watch_previous_func;
    if (terrain_watch_enabled < 0)
        terrain_watch_enabled =
            getenv("MERCENARIES_TRACE_TERRAIN_STATE") != NULL;
    if (terrain_watch_enabled) {
        /* RedTerrain owns a static RedRenderable at 0x00643370. Bit zero of
           its +0x14 flags word is the scene-active bit. Watching that one
           word at guest function boundaries is cheap enough for long runs
           and catches the exact render-off/collision-on failure mode. */
        const uint32_t flags = guest_u32(0x00643384u);
        if (!terrain_watch_seen || flags != terrain_watch_last) {
            fprintf(stderr,
                    "[TERRAIN-STATE] previous=%08X next=%08X old=%08X "
                    "new=%08X active=%u eax=%08X ecx=%08X esp=%08X\n",
                    terrain_watch_previous_func, xbox_va,
                    terrain_watch_seen ? terrain_watch_last : 0xFFFFFFFFu,
                    flags, flags & 1u, g_eax, g_ecx, g_esp);
            fflush(stderr);
            terrain_watch_seen = 1;
            terrain_watch_last = flags;
        }
        terrain_watch_previous_func = xbox_va;
    }
    static int seh_watch_enabled = -1;
    static uint32_t seh_watch_last;
    static uint32_t seh_watch_previous_func;
    if (seh_watch_enabled < 0)
        seh_watch_enabled = getenv("MERCENARIES_TRACE_SEH_EBP") != NULL;
    if (seh_watch_enabled) {
        if (g_seh_ebp != seh_watch_last) {
            fprintf(stderr,
                    "[SEH-EBP] previous=%08X next=%08X old=%08X new=%08X "
                    "esp=%08X stack=%08X,%08X,%08X,%08X\n",
                    seh_watch_previous_func, xbox_va, seh_watch_last,
                    g_seh_ebp, g_esp, guest_u32(g_esp),
                    guest_u32(g_esp + 4u), guest_u32(g_esp + 8u),
                    guest_u32(g_esp + 0xCu));
            fflush(stderr);
            seh_watch_last = g_seh_ebp;
        }
        seh_watch_previous_func = xbox_va;
    }
    if (xbox_va == 0x001F66C0u || xbox_va == 0x001F6AB0u)
        recomp_small_pool_watch(xbox_va);
    static int allocator_watch_enabled = -1;
    static uint32_t allocator_watch_last;
    static uint32_t allocator_watch_previous_func;
    if (allocator_watch_enabled < 0)
        allocator_watch_enabled =
            getenv("MERCENARIES_TRACE_SCENE_ALLOCATOR") != NULL;
    if (allocator_watch_enabled) {
        const uint32_t allocator =
            *(volatile uint32_t *)((uintptr_t)0x004409ACu +
                                   g_xbox_mem_offset);
        if (allocator == 0x00FA67C0u) {
            const uint32_t current =
                *(volatile uint32_t *)((uintptr_t)(allocator + 8u) +
                                       g_xbox_mem_offset);
            if (current >= 0x10000000u &&
                allocator_watch_last < 0x10000000u) {
                fprintf(stderr,
                        "[SCENE-ALLOCATOR-CORRUPT] previous=%08X next=%08X "
                        "old=%08X new=%08X eax=%08X ebx=%08X ecx=%08X "
                        "edx=%08X esi=%08X edi=%08X esp=%08X ebp=%08X\n",
                        allocator_watch_previous_func, xbox_va,
                        allocator_watch_last, current, g_eax, g_ebx,
                        g_ecx, g_edx, g_esi, g_edi, g_esp, g_seh_ebp);
                fflush(stderr);
            }
            allocator_watch_last = current;
        }
        allocator_watch_previous_func = xbox_va;
    }
    if ((xbox_va == 0x00039390u || xbox_va == 0x000393A4u ||
         xbox_va == 0x000393B2u || xbox_va == 0x00039710u ||
         xbox_va == 0x000393B6u || xbox_va == 0x0003971Bu ||
         xbox_va == 0x00173A60u || xbox_va == 0x00173A7Eu ||
         xbox_va == 0x00173A81u || xbox_va == 0x00173AB8u ||
         xbox_va == 0x00173AC4u || xbox_va == 0x00173AD2u ||
         xbox_va == 0x00173AF5u || xbox_va == 0x00174080u ||
         xbox_va == 0x00174644u || xbox_va == 0x0017464Au ||
         xbox_va == 0x00174650u || xbox_va == 0x0017465Bu ||
         xbox_va == 0x001EE381u || xbox_va == 0x001EE38Fu ||
         xbox_va == 0x001EE395u || xbox_va == 0x001EE39Bu) &&
        getenv("MERCENARIES_TRACE_OBJECT_LOOKUP") != NULL) {
        fprintf(stderr,
                "[OBJECT-LOOKUP] point=%08X eax=%08X ebx=%08X context=%08X asset=%08X target=%08X ebp=%08X "
                "esp=%08X stack=%08X,%08X,%08X,%08X\n",
                xbox_va, g_eax, g_ebx, g_edi, g_esi,
                (g_eax >= 0x00010000u && g_eax < 0x03FFFFF0u)
                    ? guest_u32(g_eax + 0xCu) : 0xFFFFFFFFu,
                g_seh_ebp, g_esp, guest_u32(g_esp), guest_u32(g_esp + 4u),
                guest_u32(g_esp + 8u), guest_u32(g_esp + 0xCu));
        fflush(stderr);
    }
    if (xbox_va == 0x00231434u &&
        getenv("MERCENARIES_TRACE_SIMD_CONTINUATION") != NULL) {
        fprintf(stderr,
                "[SIMD-CONTINUATION] va=%08X eax=%08X ebx=%08X ecx=%08X "
                "edx=%08X esi=%08X edi=%08X esp=%08X ebp=%08X "
                "stack=%08X,%08X,%08X,%08X,%08X,%08X "
                "allocator=%08X/%08X\n",
                xbox_va, g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi,
                g_esp, g_seh_ebp, guest_u32(g_esp), guest_u32(g_esp + 4u),
                guest_u32(g_esp + 8u), guest_u32(g_esp + 0xCu),
                guest_u32(g_esp + 0x10u), guest_u32(g_esp + 0x14u),
                guest_u32(0x00FA67C8u), guest_u32(0x00FA67CCu));
        fflush(stderr);
    }
    if (g_shl_followup_budget != 0u) {
        fprintf(stderr,
                "[SHL-FOLLOW] #%u va=%08X eax=%08X ebx=%08X ecx=%08X "
                "edx=%08X esi=%08X edi=%08X esp=%08X\n",
                g_shl_followup_index++, xbox_va, g_eax, g_ebx, g_ecx,
                g_edx, g_esi, g_edi, g_esp);
        --g_shl_followup_budget;
        fflush(stderr);
    }    if (!g_release_watch_initialized) {
        const char *watch_text = getenv("MERCENARIES_WATCH_GUEST_DWORD");
        g_release_watch_initialized = 1;
        if (watch_text != NULL)
            g_release_watch_va = (uint32_t)strtoul(watch_text, NULL, 16);
    }
    if (g_release_watch_va >= 0x00010000u &&
        g_release_watch_va <= 0x03FFFFFCu && g_xbox_mem_offset != 0) {
        const uint32_t value = *(volatile uint32_t *)(
            (uintptr_t)g_xbox_mem_offset + g_release_watch_va);
        if (!g_release_watch_seen || value != g_release_watch_last) {
            fprintf(stderr,
                    "[GUEST WATCH] va=%08X %08X -> %08X after=%08X before=%08X "
                    "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                    g_release_watch_va,
                    g_release_watch_seen ? g_release_watch_last : 0xFFFFFFFFu,
                    value, g_release_watch_previous_func, xbox_va,
                    g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
            if (g_release_watch_seen &&
                g_release_watch_last == 0x3F800000u && value == 0u &&
                getenv("MERCENARIES_TRACE_WATCH_RECENT") != NULL) {
                const uint32_t recent_end = g_recomp_recent_func_idx;
                for (uint32_t recent = 0u;
                     recent < 64u && recent < recent_end; ++recent) {
                    const uint32_t ring =
                        (recent_end - 1u - recent) & 63u;
                    fprintf(stderr, "  watch-recent[-%u]=%08X\n",
                            recent, g_recomp_recent_funcs[ring]);
                }
            }
            fflush(stderr);
            g_release_watch_last = value;
            g_release_watch_seen = 1;
        }
        g_release_watch_previous_func = xbox_va;
    }
}

#if defined(ENABLE_RECOMP_FUNC_TRACE)
#define FUNC_TRACE_SIZE 256u
#define FUNC_TRACE_STREAK_LIMIT 2000000u
#define EARLY_FUNC_TRACE_SIZE 16384u

static volatile uint32_t g_func_transitions[FUNC_TRACE_SIZE];
static volatile uint32_t g_early_func_transitions[EARLY_FUNC_TRACE_SIZE];
static volatile uint32_t g_early_func_transition_count;
static volatile DWORD g_early_func_trace_thread_id;
static int g_early_func_trace_enabled = -1;
static volatile uint64_t g_func_trace_count;
static volatile uint32_t g_func_transition_count;
static volatile uint32_t g_func_last_va;
static volatile uint32_t g_func_same_va_streak;
static volatile uint32_t g_d3d_watch_last_count = UINT32_MAX;
static volatile uint32_t g_av_watch_budget;
static volatile uint32_t g_d3d_wait_samples;
static volatile uint32_t g_d3d_ring_samples;
static volatile uint32_t g_d3d_sync_samples;
static volatile uint32_t g_d3d_ramin_setup_samples;
static volatile uint32_t g_splash_trace_samples;
static volatile uint32_t g_asset_flow_trace_samples;
static volatile uint32_t g_event_stack_trace_samples;
static volatile uint32_t g_model_flow_trace_samples;
static volatile uint32_t g_report_trace_samples;
static uint32_t g_red_alloc_size;
static uint32_t g_red_alloc_description;
static uint32_t g_red_alloc_is_array;
static uint32_t g_red_alloc_is_temp;
static volatile uint32_t g_red_oom_trace_samples;
static volatile uint32_t g_red_free_trace_samples;
static uint32_t g_guest_watch_va;
static uint32_t g_guest_watch_last;
static uint32_t g_guest_watch_previous_func;
static int g_guest_watch_initialized;
static int g_guest_watch_seen;
static uint32_t g_havok_allocator_watch_last;
static uint32_t g_havok_allocator_watch_previous_func;
static int g_havok_allocator_watch_seen;
static volatile uint32_t g_aligned_free_trace_samples;
static volatile uint32_t g_registry_leaf_trace_count;
static uint32_t g_lua_lexstate_watch;
static uint32_t g_lua_lexstate_expected;
static uint32_t g_lua_lexstate_reports;
void recomp_func_trace_dump(void);

void recomp_func_trace(uint32_t xbox_va)
{
    if (!g_recomp_func_trace_armed)
        return;

    uint64_t count = ++g_func_trace_count;
    uint32_t write_index;
    if (getenv("MERCENARIES_TRACE_STARTUP_REENTRY") != NULL &&
        (xbox_va == 0x0022964Au || xbox_va == 0x0022B43Fu ||
         xbox_va == 0x002295D6u || xbox_va == 0x001806A0u)) {
        fprintf(stderr,
                "[STARTUP-ENTRY] count=%llu tid=%lu fn=%08X esp=%08X "
                "eax=%08X ecx=%08X edx=%08X ebp=%08X\n",
                (unsigned long long)count, (unsigned long)GetCurrentThreadId(),
                xbox_va, g_esp, g_eax, g_ecx, g_edx, g_seh_ebp);
        if (xbox_va == 0x002295D6u) {
            const uint32_t startup_end = g_recomp_recent_game_func_idx;
            const uint32_t startup_count = startup_end < 32u ? startup_end : 32u;
            fprintf(stderr, "[STARTUP-PREVIOUS]");
            for (uint32_t startup_i = startup_count; startup_i > 0u;
                 --startup_i)
                fprintf(stderr, " %08X",
                        g_recomp_recent_game_funcs[
                            (startup_end - startup_i) & 255u]);
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
    if (g_early_func_trace_enabled < 0)
        g_early_func_trace_enabled =
            getenv("MERCENARIES_TRACE_EARLY_FUNC_TRANSITIONS") != NULL;
    if (getenv("MERCENARIES_TRACE_HAVOK_ALLOCATOR_UNDERFLOW") != NULL &&
        g_xbox_mem_offset != 0) {
        const uint32_t arena_base = 0x00FA67E0u;
        const uint32_t value = *(volatile uint32_t *)(
            (uintptr_t)g_xbox_mem_offset + 0x00FA67C8u);
        if (value >= 0x00F00000u && value < arena_base &&
            (!g_havok_allocator_watch_seen ||
             g_havok_allocator_watch_last >= arena_base)) {
            fprintf(stderr,
                    "[HAVOK-ALLOCATOR-UNDERFLOW] %08X->%08X "
                    "after=%08X before=%08X current=%08X "
                    "eax=%08X ebx=%08X ecx=%08X edx=%08X "
                    "esi=%08X edi=%08X esp=%08X seh_ebp=%08X\n",
                    g_havok_allocator_watch_last, value,
                    g_havok_allocator_watch_previous_func, xbox_va,
                    (uint32_t)g_recomp_current_func, g_eax, g_ebx,
                    g_ecx, g_edx, g_esi, g_edi, g_esp, g_seh_ebp);
            fflush(stderr);
            recomp_func_trace_dump();
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0xE3u);
        }
        g_havok_allocator_watch_last = value;
        g_havok_allocator_watch_previous_func = xbox_va;
        g_havok_allocator_watch_seen = 1;
    }
    if (!g_guest_watch_initialized) {
        const char *watch_text = getenv("MERCENARIES_WATCH_GUEST_DWORD");
        g_guest_watch_initialized = 1;
        if (watch_text != NULL)
            g_guest_watch_va = (uint32_t)strtoul(watch_text, NULL, 16);
    }
    if (g_guest_watch_va >= 0x00010000u &&
        g_guest_watch_va <= 0x03FFFFFCu && g_xbox_mem_offset != 0) {
        const uint32_t value = *(volatile uint32_t *)(
            (uintptr_t)g_xbox_mem_offset + g_guest_watch_va);
        if (!g_guest_watch_seen || value != g_guest_watch_last) {
            fprintf(stderr,
                    "[GUEST WATCH] va=%08X %08X -> %08X after=%08X before=%08X "
                    "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                    g_guest_watch_va,
                    g_guest_watch_seen ? g_guest_watch_last : 0xFFFFFFFFu,
                    value, g_guest_watch_previous_func, xbox_va,
                    g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
            g_guest_watch_last = value;
            g_guest_watch_seen = 1;
        }
        g_guest_watch_previous_func = xbox_va;
    }    if (getenv("MERCENARIES_TRACE_ALIGNED_FREE") != NULL &&
        xbox_va == 0x0018DBB0u && g_aligned_free_trace_samples++ < 1000000u) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        uint32_t aligned_va = 0xFFFFFFFFu;
        uint32_t header = 0xFFFFFFFFu;
        uint32_t raw_va = 0xFFFFFFFFu;
        uint32_t arg1 = 0xFFFFFFFFu;
        uint32_t arg2 = 0xFFFFFFFFu;
        if (g_esp >= 0x00010000u && g_esp <= 0x03FFFFE0u) {
            aligned_va = *(volatile uint32_t *)(base + g_esp + 4u);
            arg1 = *(volatile uint32_t *)(base + g_esp + 8u);
            arg2 = *(volatile uint32_t *)(base + g_esp + 12u);
            if (aligned_va >= 0x00010004u && aligned_va < 0x04000000u) {
                header = *(volatile uint32_t *)(base + aligned_va - 4u);
                raw_va = aligned_va - header;
            }
        }
        if (aligned_va >= 0x01000000u || header != 0x10u) {
            fprintf(stderr,
                    "[ALIGNED FREE] aligned=%08X header=%08X raw=%08X arg1=%08X arg2=%08X "
                    "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                    aligned_va, header, raw_va, arg1, arg2,
                    g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
        }
    }    if (getenv("MERCENARIES_TRACE_EVENT_STACK") != NULL &&
        g_event_stack_trace_samples < 128u &&
        (xbox_va == 0x00186E90u || xbox_va == 0x00187020u ||
         xbox_va == 0x00187430u || xbox_va == 0x001874B0u)) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        uint32_t arg0 = 0xFFFFFFFFu;
        if (g_esp >= 0x00010000u && g_esp < 0x04000000u)
            arg0 = *(volatile uint32_t *)(base + g_esp + 4u);
        fprintf(stderr,
                "[EVENT-STACK] #%u fn=%08X esp=%08X ecx=%08X eax=%08X "
                "edx=%08X esi=%08X edi=%08X arg0=%08X\n",
                g_event_stack_trace_samples++, xbox_va, g_esp, g_ecx,
                g_eax, g_edx, g_esi, g_edi, arg0);
    }
    if (getenv("MERCENARIES_TRACE_SHL_LUA") != NULL) {
        static uint32_t shl_lua_trace_samples;
        static int shl_lua_trace_active;
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        uint32_t arg0 = 0xFFFFFFFFu;
        uint32_t arg1 = 0xFFFFFFFFu;
        if (xbox_va == 0x00112020u ||
            (xbox_va == 0x001DC520u && base != 0u &&
             g_esp >= 0x00010000u && g_esp < 0x03FFFFF0u &&
             *(volatile uint32_t *)(base + g_esp + 12u) == 2331u))
            shl_lua_trace_active = 1;
        if (shl_lua_trace_active && shl_lua_trace_samples < 128u &&
            (xbox_va == 0x001DC520u || xbox_va == 0x001DD610u ||
             xbox_va == 0x001DD5A0u || xbox_va == 0x001DF4F0u ||
             xbox_va == 0x001DC4B0u || xbox_va == 0x00112020u ||
             xbox_va == 0x00113A50u || xbox_va == 0x001DC970u ||
             xbox_va == 0x001DD000u || xbox_va == 0x001DD190u ||
             xbox_va == 0x001DCB30u || xbox_va == 0x001DC980u ||
             xbox_va == 0x001135E0u ||
             xbox_va == 0x00120A30u || xbox_va == 0x00175820u ||
             xbox_va == 0x00119FE0u || xbox_va == 0x00114D60u ||
             xbox_va == 0x00114DB0u) && base != 0u &&
            g_esp >= 0x00010000u && g_esp < 0x03FFFFF4u) {
            arg0 = *(volatile uint32_t *)(base + g_esp + 4u);
            arg1 = *(volatile uint32_t *)(base + g_esp + 8u);
            fprintf(stderr,
                    "[SHL-LUA] #%u fn=%08X esp=%08X arg0=%08X arg1=%08X "
                    "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
                    shl_lua_trace_samples++, xbox_va, g_esp, arg0, arg1,
                    g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi);
            fflush(stderr);
        }
    }
    if (getenv("MERCENARIES_TRACE_LUA_LEX") != NULL) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;

        if (xbox_va == 0x001E7D70u) {
            fprintf(stderr,
                    "[LUA LEX] parser L=%08X z=%08X buff=%08X esp=%08X\n",
                    *(volatile uint32_t *)(base + g_esp + 4u),
                    *(volatile uint32_t *)(base + g_esp + 8u),
                    *(volatile uint32_t *)(base + g_esp + 12u), g_esp);
        } else if (xbox_va == 0x001E39D0u) {
            g_lua_lexstate_expected =
                *(volatile uint32_t *)(base + g_esp + 4u);
            g_lua_lexstate_watch =
                *(volatile uint32_t *)(base + g_esp + 8u);
            fprintf(stderr,
                    "[LUA LEX] setinput L=%08X ls=%08X z=%08X source=%08X esp=%08X\n",
                    g_lua_lexstate_expected, g_lua_lexstate_watch,
                    *(volatile uint32_t *)(base + g_esp + 12u),
                    *(volatile uint32_t *)(base + g_esp + 16u), g_esp);
        }

        if (g_lua_lexstate_watch >= 0x00010000u &&
            g_lua_lexstate_watch < 0x04000000u) {
            const uint32_t actual = *(volatile uint32_t *)(
                base + g_lua_lexstate_watch + 0x34u);
            if (actual != g_lua_lexstate_expected &&
                g_lua_lexstate_reports++ < 16u) {
                fprintf(stderr,
                        "[LUA LEX] L changed before %08X: ls=%08X expected=%08X actual=%08X "
                        "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                        xbox_va, g_lua_lexstate_watch,
                        g_lua_lexstate_expected, actual,
                        g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
            }
        }

        if (xbox_va == 0x001E3E90u) {
            const uint32_t ls = *(volatile uint32_t *)(base + g_esp + 4u);
            fprintf(stderr,
                    "[LUA LEX] error ls=%08X L=%08X token=%08X current=%08X esp=%08X\n",
                    ls,
                    (ls >= 0x00010000u && ls < 0x04000000u) ?
                        *(volatile uint32_t *)(base + ls + 0x34u) : 0u,
                    (ls >= 0x00010000u && ls < 0x04000000u) ?
                        *(volatile uint32_t *)(base + ls + 0x10u) : 0u,
                    (ls >= 0x00010000u && ls < 0x04000000u) ?
                        *(volatile uint32_t *)(base + ls) : 0u,
                    g_esp);
        }
    }
    if (getenv("MERCENARIES_TRACE_RED_FREE") != NULL &&
        (xbox_va == 0x001F6970u || xbox_va == 0x001F67A0u ||
         xbox_va == 0x001F6820u) && g_red_free_trace_samples < 1000000u) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t pool_va = g_ecx;
        const uint32_t sample = g_red_free_trace_samples++;
        uint32_t arg0 = 0xFFFFFFFFu;
        uint32_t arg1 = 0xFFFFFFFFu;
        uint32_t arg2 = 0xFFFFFFFFu;
        uint32_t pool_start = 0xFFFFFFFFu;
        uint32_t pool_size = 0xFFFFFFFFu;
        uint32_t free_size = 0xFFFFFFFFu;
        uint32_t low_free = 0xFFFFFFFFu;
        uint32_t blocks = 0xFFFFFFFFu;
        uint32_t num_blocks = 0xFFFFFFFFu;
        uint32_t free_index = 0xFFFFFFFFu;
        uint32_t alloc_index = 0xFFFFFFFFu;
        uint32_t alignment_mask = 0xFFFFFFFFu;
        uint32_t tracking_level = 0xFFFFFFFFu;
        uint32_t invariant_flags = 0u;

        if (g_esp >= 0x00010000u && g_esp <= 0x03FFFFE0u) {
            arg0 = *(volatile uint32_t *)(base + g_esp + 4u);
            arg1 = *(volatile uint32_t *)(base + g_esp + 8u);
            arg2 = *(volatile uint32_t *)(base + g_esp + 12u);
        } else {
            invariant_flags |= 1u;
        }
        if (pool_va >= 0x00010000u && pool_va <= 0x03FFFFD8u) {
            pool_start = *(volatile uint32_t *)(base + pool_va);
            pool_size = *(volatile uint32_t *)(base + pool_va + 4u);
            free_size = *(volatile uint32_t *)(base + pool_va + 8u);
            low_free = *(volatile uint32_t *)(base + pool_va + 0xCu);
            blocks = *(volatile uint32_t *)(base + pool_va + 0x10u);
            num_blocks = *(volatile uint32_t *)(base + pool_va + 0x14u);
            free_index = *(volatile uint32_t *)(base + pool_va + 0x18u);
            alloc_index = *(volatile uint32_t *)(base + pool_va + 0x1Cu);
            alignment_mask = *(volatile uint32_t *)(base + pool_va + 0x20u);
            tracking_level = *(volatile uint32_t *)(base + pool_va + 0x24u);
            if (free_index > num_blocks)
                invariant_flags |= 2u;
            if (alloc_index > num_blocks)
                invariant_flags |= 4u;
            if (free_index > alloc_index)
                invariant_flags |= 8u;
            if (blocks < 0x00010000u || blocks >= 0x04000000u)
                invariant_flags |= 16u;
            if (pool_start < 0x00010000u || pool_start >= 0x04000000u ||
                pool_size > 0x04000000u || pool_start + pool_size < pool_start ||
                pool_start + pool_size > 0x04000000u)
                invariant_flags |= 32u;
        } else {
            invariant_flags |= 64u;
        }
        fprintf(stderr,
                "[RED FREE] #%u fn=%08X pool=%08X arg0=%08X arg1=%08X arg2=%08X "
                "start=%08X size=%u free=%u low=%u blocks=%08X count=%u "
                "free_index=%u alloc_index=%u align=%08X tracking=%u flags=%02X "
                "eax=%08X edx=%08X ebx=%08X esi=%08X edi=%08X esp=%08X\n",
                sample, xbox_va, pool_va, arg0, arg1, arg2,
                pool_start, pool_size, free_size, low_free, blocks, num_blocks,
                free_index, alloc_index, alignment_mask, tracking_level,
                invariant_flags, g_eax, g_edx, g_ebx, g_esi, g_edi, g_esp);
    }
    if (getenv("MERCENARIES_TRACE_RED_OOM") != NULL) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        if (xbox_va == 0x001F7070u) {
            g_red_alloc_size = *(volatile uint32_t *)(base + g_esp + 4u);
            g_red_alloc_description = *(volatile uint32_t *)(base + g_esp + 8u);
            g_red_alloc_is_array = *(volatile uint32_t *)(base + g_esp + 12u);
            g_red_alloc_is_temp = *(volatile uint32_t *)(base + g_esp + 16u);
        } else if (xbox_va == 0x001F6E10u && g_red_oom_trace_samples++ < 8u) {
            const uint32_t pool_va = 0x00645984u;
            const uint32_t block_va = *(volatile uint32_t *)(base + pool_va + 0x10u);
            const uint32_t num_blocks = *(volatile uint32_t *)(base + pool_va + 0x14u);
            const uint32_t free_index = *(volatile uint32_t *)(base + pool_va + 0x18u);
            const uint32_t alloc_index = *(volatile uint32_t *)(base + pool_va + 0x1Cu);
            uint32_t largest_free = 0u;
            uint32_t i;
            const char *description =
                (g_red_alloc_description >= 0x00010000u &&
                 g_red_alloc_description < 0x04000000u) ?
                    (const char *)(base + g_red_alloc_description) : "<invalid>";
            if (block_va >= 0x00010000u && block_va < 0x04000000u &&
                free_index <= num_blocks) {
                for (i = 0u; i < free_index; ++i) {
                    const uint32_t block_size =
                        *(volatile uint32_t *)(base + block_va + i * 8u + 4u);
                    if (block_size > largest_free)
                        largest_free = block_size;
                }
            }
            fprintf(stderr,
                    "[RED OOM] size=%u (0x%X) desc=%08X '%.*s' array=%u temp=%u level=%u "
                    "pool_start=%08X pool_size=%u free=%u low=%u largest=%u "
                    "blocks=%08X count=%u free_index=%u alloc_index=%u\n",
                    g_red_alloc_size, g_red_alloc_size, g_red_alloc_description,
                    240, description, g_red_alloc_is_array, g_red_alloc_is_temp,
                    *(volatile uint32_t *)(base + g_esp + 4u),
                    *(volatile uint32_t *)(base + pool_va),
                    *(volatile uint32_t *)(base + pool_va + 4u),
                    *(volatile uint32_t *)(base + pool_va + 8u),
                    *(volatile uint32_t *)(base + pool_va + 0xCu), largest_free,
                    block_va, num_blocks, free_index, alloc_index);
            if (getenv("MERCENARIES_TRACE_RED_OOM_STACK") != NULL &&
                g_red_oom_trace_samples == 1u)
                g_recomp_trace_dump_requested = 1u;
        }
    }
    if (getenv("MERCENARIES_TRACE_REGISTRY") != NULL &&
        xbox_va == 0x00121350u) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t count_now = g_registry_leaf_trace_count++;
        const uint32_t chunk_va = *(volatile uint32_t *)(base + g_esp + 4u);
        if (chunk_va >= 0x00010000u && chunk_va < 0x04000000u &&
            (count_now < 32u || (count_now & 0xFFu) == 0u)) {
            const uint32_t file_va = *(volatile uint32_t *)(base + chunk_va + 0x18u);
            fprintf(stderr,
                    "[REGISTRY] leaf=%u chunk=%08X id=%08X start=%d end=%d size=%d next=%d "
                    "file=%08X file_size=%d file_pos=%d char_blocks=%u\n",
                    count_now, chunk_va,
                    *(volatile uint32_t *)(base + chunk_va),
                    *(volatile int32_t *)(base + chunk_va + 8u),
                    *(volatile int32_t *)(base + chunk_va + 0xCu),
                    *(volatile int32_t *)(base + chunk_va + 0x10u),
                    *(volatile int32_t *)(base + chunk_va + 0x14u), file_va,
                    (file_va >= 0x00010000u && file_va < 0x04000000u) ?
                        *(volatile int32_t *)(base + file_va + 8u) : -1,
                    (file_va >= 0x00010000u && file_va < 0x04000000u) ?
                        *(volatile int32_t *)(base + file_va + 0x10u) : -1,
                    *(volatile uint32_t *)(base + 0x00371DA8u));
        }
    }    if (getenv("MERCENARIES_TRACE_REPORT") != NULL &&
        xbox_va == 0x00208E00u && g_report_trace_samples < 128u) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t string_va = *(volatile uint32_t *)(base + g_esp + 4u);
        const char *string =
            (string_va >= 0x00010000u && string_va < 0x04000000u) ?
                (const char *)(base + string_va) : "<invalid>";
        if (getenv("MERCENARIES_TRACE_REPORT_LOOP") != NULL &&
            string_va == 0x002F4950u && g_report_trace_samples != 0u) {
            fprintf(stderr, "[REPORT] repeated report start; requesting function trace dump\n");
            g_recomp_trace_dump_requested = 1;
        }
        fprintf(stderr, "[REPORT] #%u writer=%08X string=%08X '%.*s'\n",
                g_report_trace_samples++, g_ecx, string_va, 240, string);
    }
    if (getenv("MERCENARIES_TRACE_INIT_PATH") != NULL &&
        (xbox_va == 0x00187BA0u || xbox_va == 0x001789B0u ||
         xbox_va == 0x002370B8u)) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t arg0 = *(volatile uint32_t *)(base + g_esp + 4u);
        const uint32_t arg1 = *(volatile uint32_t *)(base + g_esp + 8u);
        const uint32_t arg2 = *(volatile uint32_t *)(base + g_esp + 12u);
        const uint32_t arg3 = *(volatile uint32_t *)(base + g_esp + 16u);
        const char *s0 = (arg0 >= 0x00010000u && arg0 < 0x04000000u) ?
            (const char *)(base + arg0) : "<invalid>";
        const char *s1 = (arg1 >= 0x00010000u && arg1 < 0x04000000u) ?
            (const char *)(base + arg1) : "<invalid>";
        const char *s2 = (arg2 >= 0x00010000u && arg2 < 0x04000000u) ?
            (const char *)(base + arg2) : "<invalid>";
        const char *s3 = (arg3 >= 0x00010000u && arg3 < 0x04000000u) ?
            (const char *)(base + arg3) : "<invalid>";
        if (xbox_va == 0x00187BA0u || xbox_va == 0x001789B0u ||
            arg1 == 0x002E8434u) fprintf(stderr,
                "[INIT-PATH] fn=%08X esp=%08X args=%08X/%08X/%08X/%08X "
                "strings='%.*s' | '%.*s' | '%.*s' | '%.*s'\n",
                xbox_va, g_esp, arg0, arg1, arg2, arg3,
                160, s0, 160, s1, 160, s2, 160, s3);
    }    if (getenv("MERCENARIES_TRACE_TEXTURE_READ") != NULL &&
        xbox_va == 0x002163B0u) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t chunk = *(volatile uint32_t *)(base + g_esp + 4u);
        const uint32_t file =
            (chunk >= 0x00010000u && chunk < 0x04000000u) ?
                *(volatile uint32_t *)(base + chunk + 0x18u) : 0u;
        const uint32_t vtable =
            (file >= 0x00010000u && file < 0x04000000u) ?
                *(volatile uint32_t *)(base + file) : 0u;
        fprintf(stderr,
                "[TEXTURE-READ] obj=%08X chunk=%08X file=%08X "
                "vtable=%08X read=%08X data=%08X size=%u pos=%u open=%08X\n",
                g_ecx, chunk, file, vtable,
                vtable ? *(volatile uint32_t *)(base + vtable + 0x10u) : 0u,
                file ? *(volatile uint32_t *)(base + file + 4u) : 0u,
                file ? *(volatile uint32_t *)(base + file + 8u) : 0u,
                file ? *(volatile uint32_t *)(base + file + 0x10u) : 0u,
                file ? *(volatile uint32_t *)(base + file + 0x14u) : 0u);
    }    if (getenv("MERCENARIES_TRACE_ASSET_FLOW") != NULL &&
        g_asset_flow_trace_samples < 160u &&
        (xbox_va == 0x001796B0u || xbox_va == 0x00222A10u ||
         xbox_va == 0x001F78D0u || xbox_va == 0x001F72B0u ||
         xbox_va == 0x002181C0u)) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        uint32_t manager_root = 0u;
        uint32_t pending_object = 0u;
        uint32_t request = 0u;
        uint32_t read_request = 0u;

        if (base != 0u) {
            if (xbox_va == 0x001F78D0u &&
                g_ecx >= 0x00010000u && g_ecx < 0x04000000u) {
                manager_root = *(volatile uint32_t *)(base + g_ecx + 0x24u);
                if (manager_root >= 0x00010004u && manager_root < 0x04000004u) {
                    pending_object = manager_root - 4u;
                }
            } else if (xbox_va == 0x001F72B0u &&
                       g_ecx >= 0x00010000u && g_ecx < 0x04000000u) {
                pending_object = g_ecx;
            } else if (xbox_va == 0x002181C0u &&
                       g_ecx >= 0x00010000u && g_ecx < 0x04000000u) {
                request = g_ecx;
            }
            if (pending_object >= 0x00010000u && pending_object < 0x04000000u) {
                request = *(volatile uint32_t *)(base + pending_object + 0x10u);
            }
            if (request >= 0x00010000u && request < 0x04000000u) {
                read_request = *(volatile uint32_t *)(base + request + 0x14u);
            }
        }
        fprintf(stderr,
                "[ASSET-FLOW] #%u fn=%08X ecx=%08X eax=%08X esp=%08X "
                "root=%08X object=%08X obj_states=%02X/%02X request=%08X "
                "request_state=%u read=%08X read_state=%u\n",
                g_asset_flow_trace_samples, xbox_va, g_ecx, g_eax, g_esp,
                manager_root, pending_object,
                pending_object ? *(volatile uint8_t *)(base + pending_object + 0x20u) : 0u,
                pending_object ? *(volatile uint8_t *)(base + pending_object + 0x21u) : 0u,
                request,
                request ? *(volatile uint32_t *)(base + request + 0x18u) : 0u,
                read_request,
                read_request ? *(volatile uint32_t *)(base + read_request + 0x20u) : 0u);
        ++g_asset_flow_trace_samples;
    }
    if (getenv("MERCENARIES_TRACE_MODEL_FLOW") != NULL &&
        g_model_flow_trace_samples < 512u &&
        *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x00643890u) != 0u &&
        (xbox_va == 0x001F78D0u || xbox_va == 0x001F72B0u ||
         xbox_va == 0x002181C0u)) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        uint32_t object = 0u;
        uint32_t request = 0u;
        uint32_t read_request = 0u;

        if (xbox_va == 0x001F78D0u && g_ecx == 0x00643860u) {
            const uint32_t node = *(volatile uint32_t *)(base + g_ecx + 0x24u);
            if (node >= 0x00010004u && node < 0x04000004u)
                object = node - 4u;
        } else if (xbox_va == 0x001F72B0u &&
                   g_ecx >= 0x00010000u && g_ecx < 0x04000000u) {
            object = g_ecx;
        } else if (xbox_va == 0x002181C0u &&
                   g_ecx >= 0x00010000u && g_ecx < 0x04000000u) {
            request = g_ecx;
        }
        if (object != 0u)
            request = *(volatile uint32_t *)(base + object + 0x10u);
        if (request >= 0x00010000u && request < 0x04000000u)
            read_request = *(volatile uint32_t *)(base + request + 0x14u);

        fprintf(stderr,
                "[MODEL-FLOW] #%u fn=%08X pending=%u ecx=%08X eax=%08X "
                "object=%08X object_state=%u/%u list=%u request=%08X "
                "request_state=%u read=%08X read_state=%u\n",
                g_model_flow_trace_samples, xbox_va,
                *(volatile uint32_t *)(base + 0x00643890u), g_ecx, g_eax,
                object,
                object ? *(volatile uint8_t *)(base + object + 0x20u) : 0u,
                object ? *(volatile uint8_t *)(base + object + 0x21u) : 0u,
                object ? *(volatile uint8_t *)(base + object + 0x22u) : 0u,
                request,
                request ? *(volatile uint32_t *)(base + request + 0x18u) : 0u,
                read_request,
                read_request ? *(volatile uint32_t *)(base + read_request + 0x20u) : 0u);
        ++g_model_flow_trace_samples;
    }
    if (getenv("MERCENARIES_TRACE_SPLASH") != NULL &&
        g_splash_trace_samples < 64u &&
        (xbox_va == 0x0010CAD0u || xbox_va == 0x0020FAE0u ||
         xbox_va == 0x0028CAA0u || xbox_va == 0x0028CC10u ||
         xbox_va == 0x00290450u || xbox_va == 0x00209810u ||
         xbox_va == 0x00209A50u || xbox_va == 0x002092A0u ||
         xbox_va == 0x0010C5F0u || xbox_va == 0x0010C790u ||
         xbox_va == 0x0028BF60u || xbox_va == 0x0028BFB0u ||
         xbox_va == 0x0028C030u || xbox_va == 0x0028AB00u ||
         xbox_va == 0x0028BDB0u || xbox_va == 0x0028A530u)) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        uint32_t arg0 = 0u;
        uint32_t arg1 = 0u;
        uint32_t device = 0u;
        uint32_t texture = 0u;
        uint32_t vertex_buffer = 0u;

        if (base != 0u && g_esp >= 0x00010000u && g_esp < 0x04000000u) {
            arg0 = *(volatile uint32_t *)(base + g_esp + 4u);
            arg1 = *(volatile uint32_t *)(base + g_esp + 8u);
            device = *(volatile uint32_t *)(base + 0x007AD604u);
            texture = *(volatile uint32_t *)(base + 0x00365D50u);
            vertex_buffer = *(volatile uint32_t *)(base + 0x00365EA4u);
        }
        fprintf(stderr,
                "[SPLASH-TRACE] #%u fn=%08X eax=%08X ecx=%08X esp=%08X "
                "arg0=%08X arg1=%08X device=%08X texture=%08X vb=%08X\n",
                g_splash_trace_samples, xbox_va, g_eax, g_ecx, g_esp,
                arg0, arg1, device, texture, vertex_buffer);
        ++g_splash_trace_samples;
    }
    if ((xbox_va == 0x002956A0u || xbox_va == 0x002956D9u) &&
        g_d3d_ramin_setup_samples < 32u &&
        g_xbox_mem_offset != 0 &&
        g_ecx >= 0x00010000u && g_ecx < 0x04000000u) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t mmio_base = *(volatile uint32_t *)(base + g_ecx);
        const uint32_t ramht_cpu = *(volatile uint32_t *)(base + g_ecx + 0x130u);
        const uint32_t arg0 = *(volatile uint32_t *)(base + g_esp + 4u);
        const uint32_t arg1 = *(volatile uint32_t *)(base + g_esp + 8u);
        const uint32_t arg2 = *(volatile uint32_t *)(base + g_esp + 12u);
        const uint32_t arg3 = *(volatile uint32_t *)(base + g_esp + 16u);
        const uint32_t arg4 = *(volatile uint32_t *)(base + g_esp + 20u);

        fprintf(stderr,
                "[D3D RAMIN SETUP] fn=%08X ctx=%08X mmio=%08X "
                "ramht_cpu=%08X args=%08X,%08X,%08X,%08X,%08X\n",
                xbox_va, g_ecx, mmio_base, ramht_cpu,
                arg0, arg1, arg2, arg3, arg4);
        ++g_d3d_ramin_setup_samples;
    }

    if (xbox_va == 0x00289350u || xbox_va == 0x002893A0u ||
        xbox_va == 0x0028B430u || xbox_va == 0x0028F8B0u ||
        xbox_va == 0x00291085u || xbox_va == 0x00292836u) {
        g_av_watch_budget = 64u;
        fprintf(stderr,
                "[AV WATCH] entering ordinal-2 caller 0x%08X at entry %llu, esp=%08X\n",
                xbox_va, (unsigned long long)count, g_esp);
    }
    if (xbox_va == 0x0028FF80u) {
        g_av_watch_budget = 512u;
        fprintf(stderr, "[D3D CREATE] entry ecx=%08X ebx=%08X esp=%08X\n",
                g_ecx, g_ebx, g_esp);
    }

    if (xbox_va == 0x0028F9E0u) {
        uint32_t global_device = 0u;
        uint32_t presentation = 0u;

        if (g_xbox_mem_offset != 0) {
            global_device = *(volatile uint32_t *)(
                (uintptr_t)g_xbox_mem_offset + 0x00299378u);
            presentation = *(volatile uint32_t *)(
                (uintptr_t)g_xbox_mem_offset + g_esp + 4u);
        }
        g_av_watch_budget = 96u;
        fprintf(stderr,
                "[D3D INIT] entry ecx=%08X global=%08X presentation=%08X esp=%08X\n",
                g_ecx, global_device, presentation, g_esp);
    }
    if (xbox_va == 0x0028E340u && g_d3d_wait_samples < 8u &&
        g_xbox_mem_offset != 0) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t device = *(volatile uint32_t *)(base + 0x00299378u);
        uint32_t push = 0u;
        uint32_t flags = 0u;
        uint32_t control = 0u;
        uint32_t put = 0u;
        uint32_t get = 0u;

        if (device >= 0x00010000u && device < 0x04000000u) {
            push = *(volatile uint32_t *)(base + device);
            flags = *(volatile uint32_t *)(base + device + 8u);
            control = *(volatile uint32_t *)(base + device + 0x1C20u);
            if (control >= 0x00010000u && control < 0x04000000u) {
                put = *(volatile uint32_t *)(base + control + 0x40u);
                get = *(volatile uint32_t *)(base + control + 0x44u);
            }
        }
        fprintf(stderr,
                "[D3D WAIT] sample=%u device=%08X push=%08X flags=%08X "
                "control=%08X put=%08X get=%08X ecx=%08X\n",
                g_d3d_wait_samples, device, push, flags, control, put, get,
                g_ecx);
        ++g_d3d_wait_samples;
    }
    if (xbox_va == 0x0028E4D0u && g_d3d_ring_samples < 4u &&
        g_xbox_mem_offset != 0) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t device = *(volatile uint32_t *)(base + 0x00299378u);
        uint32_t current = 0u;
        uint32_t end = 0u;
        uint32_t index = 0u;
        uint32_t mask = 0u;
        uint32_t wrap = 0u;
        uint32_t table = 0u;
        uint32_t entry = 0u;
        uint32_t entry_end = 0u;

        if (device >= 0x00010000u && device < 0x04000000u) {
            current = *(volatile uint32_t *)(base + device + 0x00u);
            end = *(volatile uint32_t *)(base + device + 0x2Cu);
            index = *(volatile uint32_t *)(base + device + 0x34u);
            mask = *(volatile uint32_t *)(base + device + 0x38u);
            wrap = *(volatile uint32_t *)(base + device + 0x44u);
            table = *(volatile uint32_t *)(base + device + 0x48u);
            if (table >= 0x00010000u && table < 0x04000000u &&
                index <= mask && mask < 0x10000u) {
                entry = *(volatile uint32_t *)(base + table + index * 8u);
                entry_end = *(volatile uint32_t *)(base + table + index * 8u + 4u);
            }
        }
        fprintf(stderr,
                "[D3D RING] sample=%u input=%08X arg=%08X device=%08X "
                "current=%08X end=%08X index=%08X mask=%08X wrap=%08X "
                "table=%08X entry=%08X entry_end=%08X\n",
                g_d3d_ring_samples, g_eax,
                *(volatile uint32_t *)(base + g_esp + 4u), device,
                current, end, index, mask, wrap, table, entry, entry_end);
        ++g_d3d_ring_samples;
    }
    if (xbox_va == 0x0028E650u && g_d3d_sync_samples < 4u &&
        g_xbox_mem_offset != 0) {
        const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
        const uint32_t device = *(volatile uint32_t *)(base + g_esp + 4u);
        const uint32_t wait_for_gpu = *(volatile uint32_t *)(base + g_esp + 8u);
        uint32_t token_ptr = 0u;
        uint32_t token = 0u;
        uint32_t pgraph_base = 0u;

        if (device >= 0x00010000u && device < 0x04000000u) {
            token_ptr = *(volatile uint32_t *)(base + device + 0x30u);
            pgraph_base = *(volatile uint32_t *)(base + device + 0x934u);
            if (token_ptr >= 0x00010000u && token_ptr < 0x04000000u) {
                token = *(volatile uint32_t *)(base + token_ptr);
            }
        }
        fprintf(stderr,
                "[D3D SYNC] sample=%u device=%08X wait=%08X token_ptr=%08X "
                "token=%08X expected_patt=%08X pgraph_base=%08X\n",
                g_d3d_sync_samples, device, wait_for_gpu, token_ptr, token,
                token << 2, pgraph_base);
        ++g_d3d_sync_samples;
    }






    if (g_av_watch_budget != 0u) {
        fprintf(stderr,
                "[AV WATCH] entry 0x%08X eax=%08X ecx=%08X edx=%08X esp=%08X\n",
                xbox_va, g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "[REG WATCH] ebx=%08X esi=%08X edi=%08X\n",
                g_ebx, g_esi, g_edi);
        --g_av_watch_budget;
    }


    if (xbox_va == g_func_last_va) {
        ++g_func_same_va_streak;
    } else {
        g_func_last_va = xbox_va;
        g_func_same_va_streak = 1u;
        write_index = g_func_transition_count++;
        g_func_transitions[write_index & (FUNC_TRACE_SIZE - 1u)] = xbox_va;
        if (g_early_func_trace_enabled) {
            const DWORD current_thread_id = GetCurrentThreadId();
            if (g_early_func_trace_thread_id == 0u)
                InterlockedCompareExchange(
                    (volatile LONG *)&g_early_func_trace_thread_id,
                    (LONG)current_thread_id, 0);
            if (g_early_func_trace_thread_id == current_thread_id) {
                uint32_t early_index = g_early_func_transition_count++;
                if (early_index < EARLY_FUNC_TRACE_SIZE)
                    g_early_func_transitions[early_index] = xbox_va;
            }
        }
    }

    if (g_xbox_mem_offset != 0) {
        const uint32_t d3d_device = *(volatile uint32_t *)(
            (uintptr_t)g_xbox_mem_offset + 0x00299378u);

        if (d3d_device >= 0x00010000u && d3d_device < 0x04000000u) {
            const uint32_t list_count = *(volatile uint32_t *)(
                (uintptr_t)g_xbox_mem_offset + d3d_device + 0x1A10u);

            if (list_count != g_d3d_watch_last_count) {
                fprintf(stderr,
                        "[D3D WATCH] count %08X -> %08X before 0x%08X at entry %llu\n",
                        g_d3d_watch_last_count, list_count, xbox_va,
                        (unsigned long long)count);
                g_d3d_watch_last_count = list_count;
            }

            if (list_count > 4u) {
                fprintf(stderr,
                        "[D3D WATCH] count became 0x%08X before 0x%08X "
                        "after %llu entries\n",
                        list_count, xbox_va,
                        (unsigned long long)count);
                recomp_func_trace_dump();
                fflush(stderr);
                exit(87);
            }
        }
    }
    count += (count == 5000000u);
    if (count == 5000000u) {
        fprintf(stderr,
                "[FUNC TRACE] total-entry cutoff at 0x%08X; registers: "
                "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
                "ebp=%08X esp=%08X\n",
                xbox_va, g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi,
                g_seh_ebp, g_esp);
        recomp_func_trace_dump();
        fflush(stderr);
        exit(88);
    }


    if (g_func_same_va_streak == FUNC_TRACE_STREAK_LIMIT) {
        uint32_t transition_count = g_func_transition_count;
        uint32_t available = transition_count < 64u ? transition_count : 64u;

        fprintf(stderr,
                "[FUNC TRACE] 0x%08X repeated %u times after %llu entries; recent transitions:\n",
                xbox_va, FUNC_TRACE_STREAK_LIMIT,
                (unsigned long long)count);
        for (uint32_t i = 0; i < available; ++i) {
            uint32_t index = (transition_count - available + i) &
                             (FUNC_TRACE_SIZE - 1u);
            fprintf(stderr, "  [%02u] 0x%08X\n", i,
                    g_func_transitions[index]);
        }
        fprintf(stderr,
                "[FUNC TRACE] eax=%08X ebx=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X ebp=%08X esp=%08X\n",
                g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi,
                g_seh_ebp, g_esp);
        fflush(stderr);
        exit(86);
    }

    if (g_recomp_trace_dump_requested) {
        g_recomp_trace_dump_requested = 0u;
        fprintf(stderr, "[FUNC TRACE] requested after bounded delay trace at 0x%08X\n", xbox_va);
        recomp_func_trace_dump();
        fflush(stderr);
        exit(89);
    }
}

void recomp_func_trace_dump(void)
{
    uint32_t transition_count = g_func_transition_count;
    uint32_t available = transition_count < 64u ? transition_count : 64u;

    fprintf(stderr,
            "[FUNC TRACE] %llu entries, %u transitions, last=0x%08X streak=%u:\n",
            (unsigned long long)g_func_trace_count, transition_count,
            g_func_last_va, g_func_same_va_streak);
    for (uint32_t i = 0; i < available; ++i) {
        uint32_t index = (transition_count - available + i) &
                         (FUNC_TRACE_SIZE - 1u);
        fprintf(stderr, "  [%02u] 0x%08X\n", i, g_func_transitions[index]);
    }
    if (g_early_func_trace_enabled) {
        const uint32_t early_count =
            g_early_func_transition_count < EARLY_FUNC_TRACE_SIZE
                ? g_early_func_transition_count : EARLY_FUNC_TRACE_SIZE;
        fprintf(stderr,
                "[EARLY FUNC TRANSITIONS] thread=%lu stored=%u observed=%u\n",
                (unsigned long)g_early_func_trace_thread_id, early_count,
                g_early_func_transition_count);
        for (uint32_t i = 0; i < early_count; ++i)
            fprintf(stderr, "  [E%05u] 0x%08X\n", i,
                    g_early_func_transitions[i]);
    }
}
#else
void recomp_func_trace_dump(void)
{
}
#endif

/* Register state (defined in xbox_memory_layout.c) */

extern uint32_t g_eax, g_esp;
extern uint32_t g_esi, g_edi;
extern uint32_t g_seh_ebp;
extern ptrdiff_t g_xbox_mem_offset;



extern volatile uint32_t g_recomp_recent_funcs[64];
extern volatile uint32_t g_recomp_recent_func_idx;

void recomp_stack_collapse_trace(uint32_t xbox_va)
{
    static LONG reported;
    uint32_t recent_index;

    if (InterlockedCompareExchange(&reported, 1, 0) != 0) return;

    recent_index = g_recomp_recent_func_idx;
    fprintf(stderr,
            "[STACK-COLLAPSE] first low-stack function=%08X esp=%08X "
            "ebp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X "
            "esi=%08X edi=%08X recent_index=%u\n",
            xbox_va, g_esp, g_seh_ebp, g_eax, g_ebx, g_ecx, g_edx,
            g_esi, g_edi, recent_index);
    for (uint32_t i = 0; i < 16u && i < recent_index; ++i) {
        uint32_t index = (recent_index - 1u - i) & 63u;
        fprintf(stderr, "  stack-collapse-recent[-%u]=%08X\n", i,
                g_recomp_recent_funcs[index]);
    }
    fflush(stderr);
}

void recomp_target_call_trace(uint32_t xbox_va, const char *caller,
                              uint32_t line, uint32_t stack,
                              uint32_t frame, uint32_t saved_stack)
{
    static uint32_t samples;
    static uint32_t state_compiler_samples;
    static uint32_t render_method_samples;
    static uint32_t render_object_ring[16];
    static uint32_t render_object_index;
    if (caller != NULL &&
        strcmp(caller, "sub_001940D0") == 0 &&
        getenv("MERCENARIES_TRACE_HAVOK_HEIGHTFIELD") != NULL) {
        const int manual = recomp_lookup_manual(xbox_va) != NULL;
        const int generated = recomp_lookup(xbox_va) != NULL;
        const int kernel = recomp_lookup_kernel(xbox_va) != NULL;
        fprintf(stderr,
                "[HEIGHTFIELD-ICALL] target=%08X caller=%s:%u "
                "esp=%08X saved=%08X resolved=%d/%d/%d "
                "eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
                xbox_va, caller, line, stack, saved_stack,
                manual, generated, kernel, g_eax, g_ecx, g_edx,
                g_esi, g_edi);
        fflush(stderr);
    }
    if (xbox_va == 0x002376FCu) {
        const uint32_t object = g_ecx;
        const uint32_t vtable =
            object >= 0x00010000u && object <= 0x03FFFFFCu ?
                guest_u32(object) : 0u;
        fprintf(stderr,
                "[GUEST-PURECALL] caller=%s:%u object=%08X vtable=%08X "
                "current=%08X esp=%08X saved=%08X eax=%08X ebx=%08X "
                "ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X\n",
                caller != NULL ? caller : "<unknown>", line, object, vtable,
                (uint32_t)g_recomp_current_func, stack, saved_stack, g_eax,
                g_ebx, g_ecx, g_edx, g_esi, g_edi, frame);
        fflush(stderr);
    }
    if (xbox_va == 0x00146730u &&
        stack >= 0x00010000u && stack <= 0x03FFFFF4u) {
        static LONG overlap_reported;
        static LONG heightfield_samples;
        const uint32_t input = guest_u32(stack + 4u);
        const uint32_t output = guest_u32(stack + 8u);
        const uint32_t spheres = guest_u32(input);
        const uint32_t num_spheres = guest_u32(input + 4u);
        const uint64_t output_end =
            (uint64_t)output + (uint64_t)num_spheres * 16u;
        const uint32_t space = 0x00692218u + 0x14u;
        const LONG heightfield_sample = InterlockedIncrement(&heightfield_samples);
        uint32_t levels = guest_u32(space);
        if (getenv("MERCENARIES_TRACE_HAVOK_HEIGHTFIELD") != NULL &&
            heightfield_sample <= 128) {
            const uint32_t memory = guest_u32(0x004409ACu);
            fprintf(stderr,
                    "[HAVOK-HEIGHTFIELD] n=%ld caller=%s:%u this=%08X "
                    "input=%08X spheres=%08X count=%u output=%08X..%08llX "
                    "memory=%08X current=%08X free=%u prev=%08X base=%08X "
                    "esp=%08X saved=%08X\n",
                    heightfield_sample, caller, line, g_ecx, input, spheres,
                    num_spheres, output, (unsigned long long)output_end,
                    memory, guest_u32(memory + 8u),
                    guest_u32(memory + 0xCu), guest_u32(memory + 0x10u),
                    guest_u32(memory + 0x14u), stack, saved_stack);
            fflush(stderr);
        }
        if (levels > 8u) levels = 8u;
        for (uint32_t level = 0u; level < levels; ++level) {
            const uint32_t level_object = space + 4u + level * 0x1Cu;
            const uint32_t items = guest_u32(level_object + 4u);
            const uint32_t capacity = guest_u32(level_object + 0xCu);
            const uint64_t items_end =
                (uint64_t)items + (uint64_t)capacity * 4u;
            if (output < items_end && items < output_end &&
                InterlockedCompareExchange(&overlap_reported, 1, 0) == 0) {
                const uint32_t memory = guest_u32(0x004409ACu);
                fprintf(stderr,
                        "[HAVOK-HEIGHTFIELD-OVERLAP] caller=%s:%u this=%08X "
                        "input=%08X spheres=%08X count=%u output=%08X..%08llX "
                        "level=%u items=%08X..%08llX capacity=%u "
                        "memory=%08X current=%08X free=%u prev=%08X base=%08X "
                        "esp=%08X saved=%08X\n",
                        caller, line, g_ecx, input, spheres, num_spheres,
                        output, (unsigned long long)output_end, level, items,
                        (unsigned long long)items_end, capacity, memory,
                        guest_u32(memory + 8u), guest_u32(memory + 0xCu),
                        guest_u32(memory + 0x10u), guest_u32(memory + 0x14u),
                        stack, saved_stack);
                fflush(stderr);
            }
        }
    }
    if (xbox_va == 0x00043A60u) {
        const uint32_t object = g_ecx;
        const uint32_t vtable =
            object >= 0x00010000u && object <= 0x03FFFFFCu ?
                guest_u32(object) : 0u;
        const uint32_t embedded =
            object >= 0x00010000u && object <= 0x03FFF918u ?
                guest_u32(object + 0x6E8u) : 0u;
        fprintf(stderr,
                "[TRANSITION-RESET-ICALL] caller=%s:%u object=%08X "
                "vtable=%08X embedded=%08X esp=%08X saved=%08X\n",
                caller, line, object, vtable, embedded, stack, saved_stack);
        fflush(stderr);
    }
    if (getenv("MERCENARIES_TRACE_UPDATE_ICALL") != NULL &&
        xbox_va == 0x0006C420u &&
        g_ecx >= 0x00010000u && g_ecx <= 0x03FFF9FFu) {
        fprintf(stderr,
                "[UPDATE-ICALL] object=%08X vtable=%08X "
                "ring=%08X/%08X fallback=%08X\n",
                g_ecx, guest_u32(g_ecx), guest_u32(g_ecx + 0x254u),
                guest_u32(g_ecx + 0x258u), guest_u32(g_ecx + 0x260u));
    }

    if (xbox_va == 0x001C8990u &&
        stack >= 0x00010000u && stack < 0x04000000u) {
        const uint32_t pair = guest_u32(stack + 4u);
        const uint32_t left = guest_u32(pair);
        const uint32_t right = guest_u32(pair + 4u);
        if (pair != 0u && (left < 0x00010000u || right < 0x00010000u)) {
            fprintf(stderr,
                    "[SCENE-PAIR-ICALL] caller=%s:%u object=%08X pair=%08X "
                    "left=%08X right=%08X esp=%08X saved=%08X\n",
                    caller, line, g_ecx, pair, left, right, stack,
                    saved_stack);
            fflush(stderr);
        }
    }
    if (xbox_va == 0x0020DAF0u) {
        const uint32_t render_index = render_object_index++;
        render_object_ring[render_index & 15u] = g_ecx;
        if (g_ecx < 0x00300000u) {
            fprintf(stderr, "[RENDER-OBJECT-HISTORY]");
            for (uint32_t i = 1u; i <= 16u && i <= render_object_index; ++i)
                fprintf(stderr, " %08X",
                        render_object_ring[(render_object_index - i) & 15u]);
            fputc('\n', stderr);
        }
    }
    if (xbox_va == 0x0020DAF0u &&
        (g_ecx < 0x00300000u || render_method_samples++ < 16u)) {
        fprintf(stderr,
                "[RENDER-METHOD-CALL] target=%08X caller=%s:%u esp=%08X "
                "saved=%08X eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
                xbox_va, caller, line, stack, saved_stack, g_eax, g_ecx,
                g_edx, g_esi, g_edi);
        fflush(stderr);
    }

    if (caller != NULL && strncmp(caller, "296", 3) == 0) {
        if (state_compiler_samples++ >= 128u) return;
    } else {
        if (samples++ >= 256u) return;
    }
    fprintf(stderr,
            "[TARGET-CALL] target=%08X caller=%s:%u esp=%08X "
            "ebp=%08X saved_esp=%08X\n",
            xbox_va, caller, line, stack, frame, saved_stack);
    fflush(stderr);
}

void recomp_teardown_call_checkpoint(uint32_t site, uint32_t expected_esp,
                                      uint32_t expected_esi)
{
    static int initialized, enabled;
    static unsigned reports, matched_reports;
    if (!initialized) {
        enabled = getenv("MERCENARIES_TRACE_TEARDOWN_STACK") != NULL;
        initialized = 1;
    }
    if (!enabled || reports >= 256u) return;
    if (g_esp == expected_esp && g_esi == expected_esi && matched_reports++ >= 8u)
        return;
    ++reports;
    fprintf(stderr, "[TEARDOWN-STACK] site=%08X esp=%08X expected=%08X "
            "esi=%08X expected_esi=%08X ecx=%08X eax=%08X mismatch=%u\n",
            site, g_esp, expected_esp, g_esi, expected_esi, g_ecx, g_eax,
            g_esp != expected_esp || g_esi != expected_esi);
    fflush(stderr);
}

void recomp_ai_call_checkpoint(uint32_t site, uint32_t expected_esp,
                              uint32_t expected_esi, uint32_t expected_edi)
{
    static int enabled = -1;
    static unsigned reports;
    if (g_esp == expected_esp && g_esi == expected_esi && g_edi == expected_edi)
        return;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_AI_UPDATE_STACK") != NULL;
    if (!enabled || reports >= 32u) return;
    ++reports;
    fprintf(stderr, "[AI-CALL-ABI] site=%08X esp=%08X expected=%08X "
            "esi=%08X expected=%08X edi=%08X expected=%08X "
            "eax=%08X ecx=%08X edx=%08X ebx=%08X\n",
            site, g_esp, expected_esp, g_esi, expected_esi,
            g_edi, expected_edi, g_eax, g_ecx, g_edx, g_ebx);
    fflush(stderr);
}

void recomp_teardown_item_checkpoint(uint32_t site, uint32_t actor)
{
    static int initialized, enabled;
    static unsigned reports;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    if (!initialized) {
        enabled = getenv("MERCENARIES_TRACE_TEARDOWN_STACK") != NULL;
        initialized = 1;
    }
    if (!enabled || reports >= 2048u || actor < 0x10000u || actor > 0x04000000u - 0xA80u)
        return;
    ++reports;
    fprintf(stderr, "[TEARDOWN-ITEM] site=%08X actor=%08X vt=%08X name=%08X "
            "spore=%08X ammo=%08X model=%08X tick=%04X links=%08X,%08X "
            "inventory=%08X,%08X,%08X,%08X esp=%08X esi=%08X\n",
            site, actor, *(uint32_t *)(base + actor), *(uint32_t *)(base + actor + 4),
            *(uint32_t *)(base + actor + 8), *(uint32_t *)(base + actor + 0x1A4),
            *(uint32_t *)(base + actor + 0x1B0), *(uint16_t *)(base + actor + 0x1B4),
            *(uint32_t *)(base + actor + 0x20C), *(uint32_t *)(base + actor + 0x210),
            *(uint32_t *)(base + actor + 0x7B8), *(uint32_t *)(base + actor + 0x7C8),
            *(uint32_t *)(base + actor + 0x970), *(uint32_t *)(base + actor + 0xA7C),
            g_esp, g_esi);
}

void recomp_icall_stack_mismatch_trace(uint32_t xbox_va, const char *caller,
                                       uint32_t line, uint32_t expected_stack,
                                       uint32_t actual_stack, uint32_t frame)
{
    static LONG reported;

    if (InterlockedCompareExchange(&reported, 1, 0) != 0) return;
    fprintf(stderr,
            "[ICALL-STACK-COLLAPSE] target=%08X caller=%s:%u "
            "expected=%08X actual=%08X ebp=%08X\n",
            xbox_va, caller, line, expected_stack, actual_stack, frame);
    fflush(stderr);
}

void recomp_event_stack_trace(uint32_t xbox_va)
{
    static int enabled = -1;
    static uint32_t samples;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t arg0 = 0xFFFFFFFFu;
    uint32_t stack0 = 0xFFFFFFFFu;
    uint32_t stack2 = 0xFFFFFFFFu;
    uint32_t stack3 = 0xFFFFFFFFu;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_EVENT_STACK") != NULL;
    if (!enabled || samples >= 512u) return;
    if (g_esp >= 0x00010000u && g_esp < 0x04000000u) {
        stack0 = *(volatile uint32_t *)(base + g_esp);
        arg0 = *(volatile uint32_t *)(base + g_esp + 4u);
        stack2 = *(volatile uint32_t *)(base + g_esp + 8u);
        stack3 = *(volatile uint32_t *)(base + g_esp + 12u);
    }
    fprintf(stderr,
            "[EVENT-STACK-LITE] #%u fn=%08X esp=%08X ecx=%08X eax=%08X "
            "edx=%08X esi=%08X edi=%08X ebp=%08X stack=%08X/%08X/%08X/%08X\n",
            samples++, xbox_va, g_esp, g_ecx, g_eax, g_edx,
            g_esi, g_edi, g_seh_ebp, stack0, arg0, stack2, stack3);
}

void recomp_script_use_checkpoint(uint32_t xbox_va)
{
    static int auto_y_armed_from_use_only;

    if (xbox_va == 0x0011AAD0u && !auto_y_armed_from_use_only &&
        getenv("MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY") != NULL) {
        auto_y_armed_from_use_only = 1;
        if (getenv("MERCENARIES_CAPTURE_GAMEPLAY_AFTER_USE_ONLY") != NULL)
            g_mercenaries_gameplay_capture_active = 1u;
        xbox_InputArmTestAutoY();
        fprintf(stderr, "[SCRIPT-USE-TRACE] Stand Up prompt ready\n");
        {
            const char *capture_path =
                getenv("MERCENARIES_CAPTURE_STANDUP_PROMPT_PATH");
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugArmFlipCapture(capture_path);
        }
        if (getenv("MERCENARIES_TRACE_APU_VOICES") != NULL ||
            getenv("MERCENARIES_TRACE_APU_ROUTE_STAGES") != NULL)
            mcpx_apu_debug_dump_stream_voices();
        fflush(stderr);
        /* The targeted entry trace is only needed to find the point where
         * the chair interaction becomes usable.  Turn it off before the
         * stand-up animation and walking probe so tracing does not distort
         * the retail script/physics cadence we are trying to measure. */
        if (getenv("MERCENARIES_KEEP_RECOMP_ENTRY_TRACE_AFTER_USE") == NULL)
            g_recomp_entry_trace_enabled = 0u;
    }
}


extern void trace_dump_stream_manager(void);

void recomp_stream_update_trace(void)
{
    static int enabled = -1;
    static uint32_t samples;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_STREAM_UPDATE") != NULL;
    if (!enabled) return;
    if ((samples++ & 0xFFu) == 0u) {
        fprintf(stderr, "[STREAM-UPDATE-LITE] sample=%u esp=%08X\n", samples - 1u, g_esp);
        trace_dump_stream_manager();
    }
}
void recomp_disk_error_trace(void)
{
    static LONG reported;
    uint32_t recent_index;

    if (getenv("MERCENARIES_TRACE_DISK_ERROR") == NULL ||
        InterlockedCompareExchange(&reported, 1, 0) != 0)
        return;

    recent_index = g_recomp_recent_func_idx;
    fprintf(stderr,
            "[DISK-ERROR] callback requested esp=%08X eax=%08X ecx=%08X "
            "edx=%08X recent_index=%u\n",
            g_esp, g_eax, g_ecx, g_edx, recent_index);
    for (uint32_t i = 0; i < 32u && i < recent_index; ++i) {
        const uint32_t index = (recent_index - 1u - i) & 63u;
        fprintf(stderr, "  disk-error-recent[-%u]=%08X\n", i,
                g_recomp_recent_funcs[index]);
    }
    fflush(stderr);
}
void recomp_disc_cache_trace(uint32_t xbox_va)
{
    static uint32_t samples;
    static uint32_t four_byte_samples;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    const uint32_t object = g_ecx;
    const uint32_t buffer = base != 0u ?
        *(volatile uint32_t *)(base + g_esp + 4u) : 0u;
    const uint32_t requested = base != 0u ?
        *(volatile uint32_t *)(base + g_esp + 8u) : 0u;
    uint32_t cache = 0u;
    uint32_t offset = 0u;
    uint32_t size = 0u;

    if (getenv("MERCENARIES_TRACE_DISC_CACHE") == NULL || base == 0u)
        return;
    if (requested == 4u) {
        if (four_byte_samples++ >= 32u)
            return;
    } else if (samples++ >= 64u) {
        return;
    }
    if (object >= 0x00010000u && object < 0x04000000u) {
        cache = *(volatile uint32_t *)(base + object + 0xCCu);
        offset = *(volatile uint32_t *)(base + object + 0x8Cu);
        size = *(volatile uint32_t *)(base + object + 0x90u);
    }
    fprintf(stderr,
            "[DISC-CACHE] fn=%08X esp=%08X object=%08X buffer=%08X requested=%u "
            "cache=%08X offset=%08X size=%08X setup=%u count=%u max=%u "
            "entries=%08X/%08X/%08X\n",
            xbox_va, g_esp, object, buffer, requested, cache, offset, size,
            *(volatile uint8_t *)(base + 0x007AB5A8u),
            *(volatile uint32_t *)(base + 0x0030F194u),
            *(volatile uint32_t *)(base + 0x0030F198u),
            *(volatile uint32_t *)(base + 0x0030F180u),
            *(volatile uint32_t *)(base + 0x0030F184u),
            *(volatile uint32_t *)(base + 0x0030F188u));
    if (xbox_va == 0x00209200u) {
        const uint32_t recent_index = g_recomp_recent_func_idx;
        for (uint32_t i = 0; i < 10u && i < recent_index; ++i) {
            const uint32_t index = (recent_index - 1u - i) & 63u;
            fprintf(stderr, "  disc-cache-dtor-recent[-%u]=%08X\n", i,
                    g_recomp_recent_funcs[index]);
        }
    }
    fflush(stderr);
}
void recomp_pbl_file_checkpoint(uint32_t stage, uint32_t object,
                                uint32_t value)
{
    static uint32_t samples;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t handle = 0u;
    uint32_t offset = 0u;
    uint32_t size = 0u;
    uint32_t is_open = 0u;

    if (getenv("MERCENARIES_TRACE_PBL_FILE") == NULL || base == 0u ||
        samples++ >= 64u)
        return;
    if (object >= 0x00010000u && object < 0x03FFFF70u) {
        is_open = *(volatile uint8_t *)(base + object + 4u);
        handle = *(volatile uint32_t *)(base + object + 0x88u);
        offset = *(volatile uint32_t *)(base + object + 0x8Cu);
        size = *(volatile uint32_t *)(base + object + 0x90u);
    }
    fprintf(stderr,
            "[PBL-FILE] stage=%u object=%08X value=%08X open=%u "
            "handle=%08X offset=%08X size=%08X esp=%08X\n",
            stage, object, value, is_open, handle, offset, size, g_esp);
    fflush(stderr);
}
void recomp_stream_header_trace(uint32_t phase, uint32_t anchor)
{
    static uint32_t samples;
    static int header_active;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t stack10 = 0xFFFFFFFFu;
    uint32_t stack14 = 0xFFFFFFFFu;
    uint32_t stack144 = 0xFFFFFFFFu;

    if (getenv("MERCENARIES_TRACE_STREAM_HEADER") == NULL || base == 0u)
        return;
    if (phase == 1u)
        header_active = 1;
    if (!header_active || samples++ >= 64u)
        return;
    if (g_esp >= 0x00010000u && g_esp < 0x04000000u - 0x148u) {
        stack10 = *(volatile uint32_t *)(base + g_esp + 0x10u);
        stack14 = *(volatile uint32_t *)(base + g_esp + 0x14u);
        stack144 = *(volatile uint32_t *)(base + g_esp + 0x144u);
    }
    fprintf(stderr,
            "[STREAM-HEADER] phase=%u esp=%08X anchor=%08X eax=%08X "
            "ecx=%08X edx=%08X s10=%08X s14=%08X s144=%08X\n",
            phase, g_esp, anchor, g_eax, g_ecx, g_edx,
            stack10, stack14, stack144);
    fflush(stderr);
    if (phase == 4u)
        header_active = 0;
}
void recomp_lua_concat_trace(uint32_t state, uint32_t total,
                             uint32_t last, uint32_t top,
                             uint32_t left, uint32_t right)
{
    static int enabled = -1;
    static uint32_t reports;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_ERROR") != NULL;
    if (!enabled || base == 0u || reports++ >= 32u)
        return;

    fprintf(stderr,
            "[LUA CONCAT ERROR] L=%08X total=%u last=%u top=%08X "
            "left=%08X type=%u lo=%08X hi=%08X "
            "right=%08X type=%u lo=%08X hi=%08X\n",
            state, total, last, top,
            left,
            left < 0x03FFFFF0u ? *(volatile uint32_t *)(base + left) : 0xFFFFFFFFu,
            left < 0x03FFFFF0u ? *(volatile uint32_t *)(base + left + 8u) : 0u,
            left < 0x03FFFFF0u ? *(volatile uint32_t *)(base + left + 0x0Cu) : 0u,
            right,
            right < 0x03FFFFF0u ? *(volatile uint32_t *)(base + right) : 0xFFFFFFFFu,
            right < 0x03FFFFF0u ? *(volatile uint32_t *)(base + right + 8u) : 0u,
            right < 0x03FFFFF0u ? *(volatile uint32_t *)(base + right + 0x0Cu) : 0u);
    fflush(stderr);
}
void recomp_lua_vm_register_trace(uint32_t tag, uint32_t stack,
                                  uint32_t state, uint32_t instruction,
                                  uint32_t destination)
{
    static int enabled = -1;
    static uint32_t reports;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_VM_REGISTERS") != NULL;
    if (!enabled || base == 0u || state != 0x008DC320u ||
        stack >= 0x008BF600u || reports++ >= 512u)
        return;

    fprintf(stderr,
            "[LUA VM REG] tag=%u esp=%08X L=%08X ins=%08X dst=%08X "
            "L8=%08X Lc=%08X L14=%08X L1c=%08X "
            "stack=%08X,%08X,%08X,%08X\n",
            tag, stack, state, instruction, destination,
            state < 0x03FFFFE0u ? *(volatile uint32_t *)(base + state + 8u) : 0xFFFFFFFFu,
            state < 0x03FFFFE0u ? *(volatile uint32_t *)(base + state + 0x0Cu) : 0xFFFFFFFFu,
            state < 0x03FFFFE0u ? *(volatile uint32_t *)(base + state + 0x14u) : 0xFFFFFFFFu,
            state < 0x03FFFFE0u ? *(volatile uint32_t *)(base + state + 0x1Cu) : 0xFFFFFFFFu,
            stack < 0x03FFFFF0u ? *(volatile uint32_t *)(base + stack) : 0xFFFFFFFFu,
            stack < 0x03FFFFECu ? *(volatile uint32_t *)(base + stack + 4u) : 0xFFFFFFFFu,
            stack < 0x03FFFFE8u ? *(volatile uint32_t *)(base + stack + 8u) : 0xFFFFFFFFu,
            stack < 0x03FFFFE4u ? *(volatile uint32_t *)(base + stack + 12u) : 0xFFFFFFFFu);
    fflush(stderr);
}
void recomp_math_table_trace(uint32_t tag, uint32_t stack, uint32_t frame,
                             uint32_t output, uint32_t slot,
                             uint32_t counter, uint32_t source,
                             uint32_t cursor)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_MATH_TABLE") != NULL;
    if (!enabled || reports++ >= 256u)
        return;

    fprintf(stderr,
            "[MATH TABLE] tag=%u esp=%08X ebp=%08X output=%08X "
            "slot=%08X count=%u source=%08X cursor=%08X fp_top=%u\n",
            tag, stack, frame, output, slot, counter, source, cursor,
            g_fp_top);
    fflush(stderr);
}
void recomp_asset_lookup_trace(uint32_t tag, uint32_t stack,
                               uint32_t object, uint32_t owner,
                               uint32_t table, uint32_t search,
                               uint32_t key)
{
    static int enabled = -1;
    static uint32_t reports;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t data = 0xFFFFFFFFu;
    uint32_t capacity = 0xFFFFFFFFu;
    uint32_t count = 0xFFFFFFFFu;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_ASSET_LOOKUP") != NULL;
    if (!enabled || base == 0u)
        return;

    if (search >= 0x00010000u && search < 0x03FFFFF4u) {
        data = *(volatile uint32_t *)(base + search);
        capacity = *(volatile uint32_t *)(base + search + 4u);
        count = *(volatile uint32_t *)(base + search + 8u);
    }
    if ((tag == 2u && data != 0u && count <= 0x00010000u) ||
        reports++ >= 128u)
        return;
    fprintf(stderr,
            "[ASSET LOOKUP] tag=%u esp=%08X object=%08X owner=%08X "
            "table=%08X search=%08X data=%08X capacity=%08X count=%08X "
            "key=%08X\n",
            tag, stack, object, owner, table, search, data, capacity, count,
            key);
    fflush(stderr);
}
void recomp_lua_gc_trace(uint32_t tag, uint32_t stack,
                         uint32_t state, uint32_t global,
                         uint32_t main_thread, uint32_t gc_state)
{
    static int enabled = -1;
    static uint32_t reports;
    static uint32_t normal_reports[10];
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t top = 0xFFFFFFFFu, ci = 0xFFFFFFFFu;
    uint32_t stack_base = 0xFFFFFFFFu, base_ci = 0xFFFFFFFFu;
    int invalid = 0;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_GC") != NULL;
    if (!enabled || base == 0u)
        return;

    if (state != 0u) {
        invalid = state < 0x00010000u || state >= 0x04000000u;
        if (!invalid) {
            top = *(volatile uint32_t *)(base + state + 0x08u);
            ci = *(volatile uint32_t *)(base + state + 0x14u);
            stack_base = *(volatile uint32_t *)(base + state + 0x1Cu);
            base_ci = *(volatile uint32_t *)(base + state + 0x28u);
            invalid = top < 0x00010000u || top >= 0x04000000u ||
                      ci < 0x00010000u || ci >= 0x04000000u ||
                      stack_base < 0x00010000u || stack_base >= 0x04000000u ||
                      base_ci < 0x00010000u || base_ci >= 0x04000000u;
        }
    }
    if (global != 0u)
        invalid |= global < 0x00010000u || global >= 0x04000000u;
    if (main_thread != 0u)
        invalid |= main_thread < 0x00010000u || main_thread >= 0x04000000u;
    if (gc_state != 0u)
        invalid |= gc_state < 0x00010000u || gc_state >= 0x04000000u;
    if (!invalid && tag < 10u && normal_reports[tag]++ >= 2u)
        return;
    if (reports++ >= 128u)
        return;
    fprintf(stderr,
            "[LUA GC] tag=%u esp=%08X state=%08X global=%08X "
            "main=%08X gcstate=%08X top=%08X ci=%08X stack=%08X "
            "base_ci=%08X invalid=%u\n",
            tag, stack, state, global, main_thread, gc_state,
            top, ci, stack_base, base_ci, invalid != 0);
    fflush(stderr);
}
void recomp_pose_buffer_trace(uint32_t stage, uint32_t stack,
                              uint32_t object, uint32_t value)
{
    static int enabled = -1;
    static uint32_t reports;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t source = 0xFFFFFFFFu;
    uint32_t count = 0xFFFFFFFFu;
    int invalid = 0;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_POSE_BUFFER") != NULL;
    if (!enabled || base == 0u || reports++ >= 256u)
        return;

    if (object < 0x00010000u || object >= 0x03FFFFFCu) {
        invalid = 1;
    } else {
        source = *(volatile uint32_t *)(base + object);
        if (source < 0x00010000u || source >= 0x03FFFFF4u) {
            invalid = source != 0u;
        } else {
            count = *(volatile uint32_t *)(base + source + 8u);
            invalid = count > 0x00010000u;
        }
    }

    fprintf(stderr,
            "[POSE BUFFER] stage=%u esp=%08X object=%08X source=%08X "
            "count=%08X value=%08X invalid=%u eax=%08X ecx=%08X "
            "edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            stage, stack, object, source, count, value, invalid != 0,
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi);
    fflush(stderr);
}
void recomp_global_list_validate(uint32_t stage, uint32_t stack)
{
    static int enabled = -1;
    static int reported;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    const uint32_t sentinel = 0x30F074u;
    uint32_t node;
    uint32_t count;
    uint32_t next = 0xFFFFFFFFu;
    uint32_t previous = 0xFFFFFFFFu;
    uint32_t object = 0xFFFFFFFFu;
    uint32_t order = 0xFFFFFFFFu;
    uint32_t bad = 0u;
    uint32_t step;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_GLOBAL_HASH") != NULL;
    if (!enabled || reported || base == 0u)
        return;

    node = *(volatile uint32_t *)(base + sentinel);
    count = *(volatile uint32_t *)(base + 0x30F084u);
    for (step = 0u; step < 4096u; ++step) {
        if (node == sentinel)
            break;
        if (node < 0x00010000u || node > 0x03FFFFEFu) {
            bad = 1u;
            break;
        }
        next = *(volatile uint32_t *)(base + node);
        previous = *(volatile uint32_t *)(base + node + 4u);
        object = *(volatile uint32_t *)(base + node + 8u);
        order = *(volatile uint32_t *)(base + node + 12u);
        if (object < 0x00010000u || object > 0x03FFFFFFu) {
            bad = 2u;
            break;
        }
        if (object + 4u != node) {
            bad = 5u;
            break;
        }
        if (next < 0x00010000u || next > 0x03FFFFFFu ||
            previous < 0x00010000u || previous > 0x03FFFFFFu) {
            bad = 6u;
            break;
        }
        if (*(volatile uint32_t *)(base + next + 4u) != node ||
            *(volatile uint32_t *)(base + previous) != node) {
            bad = 7u;
            break;
        }
        node = next;
        if (step > count + 2u) {
            bad = 3u;
            break;
        }
    }
    if (step == 4096u)
        bad = 4u;
    if (!bad)
        return;

    reported = 1;
    fprintf(stderr,
            "[GLOBAL LIST INVALID] stage=%u esp=%08X count=%08X step=%u "
            "bad=%u node=%08X next=%08X prev=%08X object=%08X order=%08X\n",
            stage, stack, count, step, bad, node, next, previous, object, order);
    fprintf(stderr, "[GLOBAL LIST INVALID CHAIN]");
    node = *(volatile uint32_t *)(base + sentinel);
    for (step = 0u; step != 32u; ++step) {
        fprintf(stderr, " %08X", node);
        if (node == sentinel || node < 0x00010000u || node > 0x03FFFFEFu)
            break;
        node = *(volatile uint32_t *)(base + node);
    }
    fputc('\n', stderr);
    {
        const uint32_t recent_index = g_recomp_recent_game_func_idx;
        const uint32_t recent_count = recent_index < 48u ? recent_index : 48u;
        fprintf(stderr, "[GLOBAL LIST INVALID RECENT]");
        for (step = recent_count; step != 0u; --step)
            fprintf(stderr, " %08X",
                    g_recomp_recent_game_funcs[(recent_index - step) & 255u]);
        fputc('\n', stderr);
    }
    fflush(stderr);
}void recomp_global_hash_trace(uint32_t stage, uint32_t stack,
                              uint32_t object, uint32_t key)
{
    static int enabled = -1;
    static uint32_t reports;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t capacity = 0xFFFFFFFFu;
    uint32_t values = 0xFFFFFFFFu;
    uint32_t table = 0xFFFFFFFFu;
    uint32_t output = 0xFFFFFFFFu;
    uint32_t words[6] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                          0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    uint32_t i;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_GLOBAL_HASH") != NULL;
    if (!enabled || base == 0u)
        return;
    {
        const char *minimum = getenv("MERCENARIES_TRACE_GLOBAL_STAGE_MIN");
        const char *maximum = getenv("MERCENARIES_TRACE_GLOBAL_STAGE_MAX");
        if ((minimum && stage < (uint32_t)strtoul(minimum, NULL, 10)) ||
            (maximum && stage > (uint32_t)strtoul(maximum, NULL, 10)))
            return;
    }
    if (stage < 80u && reports++ >= 1024u)
        return;

    capacity = *(volatile uint32_t *)(base + 0x6438A8u);
    values = *(volatile uint32_t *)(base + 0x6438B0u);
    table = *(volatile uint32_t *)(base + 0x6438B4u);
    output = *(volatile uint32_t *)(base + 0x643844u);
    if (stack >= 0x00010000u && stack <= 0x03FFFFE8u)
        for (i = 0; i != 6u; ++i)
            words[i] = *(volatile uint32_t *)(base + stack + i * 4u);
    fprintf(stderr,
            "[GLOBAL HASH] stage=%u esp=%08X object=%08X key=%08X "
            "capacity=%08X values=%08X table=%08X output=%08X "
            "stack=%08X,%08X,%08X,%08X,%08X,%08X "
            "eax=%08X ecx=%08X edx=%08X ebx=%08X esi=%08X edi=%08X\n",
            stage, stack, object, key, capacity, values, table, output,
            words[0], words[1], words[2], words[3], words[4], words[5],
            g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi);
    if (stage >= 80u) {
        const uint32_t recent_index = g_recomp_recent_game_func_idx;
        const uint32_t count = recent_index < 24u ? recent_index : 24u;
        fprintf(stderr, "[GLOBAL LIST RECENT]");
        for (i = count; i != 0u; --i)
            fprintf(stderr, " %08X",
                    g_recomp_recent_game_funcs[(recent_index - i) & 255u]);
        fputc('\n', stderr);
    }
    fflush(stderr);
}

#define RECOMP_LUA_HOST_JMP_MAX 64u

typedef struct recomp_lua_host_jmp_entry {
    uint32_t guest_buffer;
    jmp_buf environment;
} recomp_lua_host_jmp_entry;

static recomp_lua_host_jmp_entry
    g_recomp_lua_host_jmps[RECOMP_LUA_HOST_JMP_MAX];
static uint32_t g_recomp_lua_host_jmp_depth;

void *recomp_lua_host_jmp_register(uint32_t guest_buffer)
{
    recomp_lua_host_jmp_entry *entry;

    if (g_recomp_lua_host_jmp_depth >= RECOMP_LUA_HOST_JMP_MAX) {
        fprintf(stderr, "[LUA-HOST-JMP] protected-call depth overflow\n");
        abort();
    }
    entry = &g_recomp_lua_host_jmps[g_recomp_lua_host_jmp_depth++];
    entry->guest_buffer = guest_buffer;
    return &entry->environment;
}

void recomp_lua_host_jmp_pop(uint32_t guest_buffer)
{
    if (g_recomp_lua_host_jmp_depth == 0u)
        return;
    if (g_recomp_lua_host_jmps[g_recomp_lua_host_jmp_depth - 1u].guest_buffer !=
        guest_buffer) {
        fprintf(stderr,
                "[LUA-HOST-JMP] pop mismatch expected=%08X actual=%08X depth=%u\n",
                guest_buffer,
                g_recomp_lua_host_jmps[g_recomp_lua_host_jmp_depth - 1u].guest_buffer,
                g_recomp_lua_host_jmp_depth);
        return;
    }
    --g_recomp_lua_host_jmp_depth;
}

uint32_t recomp_lua_host_longjmp(uint32_t guest_buffer, uint32_t value)
{
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t index;

    for (index = g_recomp_lua_host_jmp_depth; index != 0u; --index) {
        recomp_lua_host_jmp_entry *entry =
            &g_recomp_lua_host_jmps[index - 1u];
        if (entry->guest_buffer == guest_buffer) {
            const uint32_t result = value != 0u ? value : 1u;

            g_recomp_lua_host_jmp_depth = index;
            g_seh_ebp = *(volatile uint32_t *)(base + guest_buffer);
            g_ebx = *(volatile uint32_t *)(base + guest_buffer + 4u);
            g_edi = *(volatile uint32_t *)(base + guest_buffer + 8u);
            g_esi = *(volatile uint32_t *)(base + guest_buffer + 0x0Cu);
            g_esp = *(volatile uint32_t *)(base + guest_buffer + 0x10u) + 4u;
            *(volatile uint32_t *)(base + 0u) =
                *(volatile uint32_t *)(base + guest_buffer + 0x18u);
            g_eax = result;
            longjmp(entry->environment, (int)result);
        }
    }
    return 0u;
}

double recomp_native_strtod(uint32_t guest_string,
                            uint32_t guest_end_pointer)
{
    static unsigned int trace_count;
    const char *source;
    char *end;
    double result;

    if (guest_string < 0x00010000u || guest_string >= 0x04000000u) {
        if (guest_end_pointer >= 0x00010000u &&
            guest_end_pointer <= 0x03FFFFFCu)
            *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + guest_end_pointer) = guest_string;
        return 0.0;
    }

    source = (const char *)((uintptr_t)g_xbox_mem_offset + guest_string);
    result = strtod(source, &end);
    if (getenv("MERCENARIES_TRACE_NATIVE_STRTOD") != NULL &&
        (trace_count++ < 64u ||
         (source[0] == '0' && source[1] == '.' && source[2] == '4'))) {
        fprintf(stderr,
                "[NATIVE-STRTOD] source=%08X end-pointer=%08X "
                "text=%.32s result=%.17g consumed=%td\n",
                guest_string, guest_end_pointer, source, result,
                end - source);
        fflush(stderr);
    }
    if (guest_end_pointer >= 0x00010000u &&
        guest_end_pointer <= 0x03FFFFFCu) {
        const ptrdiff_t consumed = end - source;
        *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + guest_end_pointer) =
            guest_string + (uint32_t)consumed;
    }
    return result;
}

uint32_t recomp_native_format_lua_number(uint32_t guest_buffer,
                                         double value)
{
    char *destination;
    int length;

    if (guest_buffer < 0x00010000u || guest_buffer > 0x03FFFFDFu)
        return 0u;

    destination = (char *)((uintptr_t)g_xbox_mem_offset + guest_buffer);
    length = snprintf(destination, 32u, "%.14g", value);
    if (length < 0 || length >= 32) {
        destination[0] = '\0';
        return 0u;
    }
    return (uint32_t)length;
}

void recomp_lua_error_trace(uint32_t xbox_va)
{
    static int enabled = -1;
    static uint32_t reports;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t state, top, stack, error_jmp, error_offset;
    uint32_t error_object = 0u, error_type = 0xFFFFFFFFu, error_value = 0u;
    uint32_t handler_object = 0u, handler_type = 0xFFFFFFFFu;
    uint32_t error_code = 0xFFFFFFFFu;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_ERROR") != NULL;
    if (!enabled || base == 0u || reports++ >= 16u ||
        g_esp < 0x00010000u || g_esp >= 0x03FFFFF8u)
        return;

    state = *(volatile uint32_t *)(base + g_esp + 4u);
    if (xbox_va == 0x001DE930u)
        error_code = *(volatile uint32_t *)(base + g_esp + 8u);
    if (state < 0x00010000u || state >= 0x03FFFFA0u) {
        fprintf(stderr,
                "[LUA ERROR] fn=%08X invalid-state=%08X code=%u esp=%08X\n",
                xbox_va, state, error_code, g_esp);
        return;
    }

    top = *(volatile uint32_t *)(base + state + 0x08u);
    stack = *(volatile uint32_t *)(base + state + 0x1Cu);
    error_jmp = *(volatile uint32_t *)(base + state + 0x58u);
    error_offset = *(volatile uint32_t *)(base + state + 0x5Cu);
    if (top >= 0x00010010u && top < 0x04000000u) {
        error_object = top - 0x10u;
        error_type = *(volatile uint32_t *)(base + error_object);
        error_value = *(volatile uint32_t *)(base + error_object + 8u);
    }
    if (error_offset != 0u && stack >= 0x00010000u &&
        stack < 0x04000000u && error_offset < 0x04000000u - stack) {
        handler_object = stack + error_offset;
        if (handler_object < 0x03FFFFF0u)
            handler_type = *(volatile uint32_t *)(base + handler_object);
    }

    fprintf(stderr,
            "[LUA ERROR] fn=%08X code=%u L=%08X top=%08X stack=%08X "
            "errorJmp=%08X errfunc=%08X handler=%08X handlerType=%u "
            "errorObj=%08X errorType=%u value=%08X message=",
            xbox_va, error_code, state, top, stack, error_jmp, error_offset,
            handler_object, handler_type, error_object, error_type, error_value);
    if (error_type == 4u && error_value >= 0x00010000u &&
        error_value < 0x03FFFFF0u) {
        uint32_t length = *(volatile uint32_t *)(base + error_value + 0x0Cu);
        uint32_t shown = length < 512u ? length : 512u;
        uint32_t i;
        if (shown <= 0x04000000u - error_value - 0x10u) {
            for (i = 0u; i < shown; ++i) {
                const uint8_t ch = *(volatile uint8_t *)(
                    base + error_value + 0x10u + i);
                fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
            }
            if (shown < length)
                fputs("...", stderr);
        } else {
            fputs("<invalid-string-length>", stderr);
        }
    } else {
        fputs("<non-string>", stderr);
    }
    fputc('\n', stderr);
    {
        const uint32_t count = g_recomp_recent_game_func_idx < 96u
            ? g_recomp_recent_game_func_idx : 96u;
        const uint32_t start = g_recomp_recent_game_func_idx - count;
        uint32_t i;

        fprintf(stderr, "[LUA ERROR RECENT] count=%u\n", count);
        for (i = 0u; i < count; ++i) {
            const uint32_t index = start + i;
            fprintf(stderr, "  [%02u] %08X\n", i,
                    g_recomp_recent_game_funcs[index & 255u]);
        }
    }
    {
        const uint32_t base_ci = *(volatile uint32_t *)(base + state + 0x28u);
        const uint32_t current_ci = *(volatile uint32_t *)(base + state + 0x14u);
        uint32_t first_ci = base_ci;
        uint32_t ci_count = 0u;

        if (base_ci >= 0x00010000u && current_ci >= base_ci &&
            current_ci < 0x03FFFFE8u &&
            ((current_ci - base_ci) % 0x18u) == 0u) {
            ci_count = (current_ci - base_ci) / 0x18u + 1u;
            if (ci_count > 24u) {
                first_ci = current_ci - 23u * 0x18u;
                ci_count = 24u;
            }
            fprintf(stderr, "[LUA ERROR CALLINFO] base=%08X current=%08X count=%u\n",
                    base_ci, current_ci, ci_count);
            for (uint32_t frame_index = 0u; frame_index < ci_count;
                 ++frame_index) {
                const uint32_t ci = first_ci + frame_index * 0x18u;
                const uint32_t ci_base = *(volatile uint32_t *)(base + ci);
                const uint32_t ci_top = *(volatile uint32_t *)(base + ci + 4u);
                const uint32_t ci_state = *(volatile uint32_t *)(base + ci + 8u);
                const uint32_t saved_pc = *(volatile uint32_t *)(base + ci + 0x0Cu);
                uint32_t function_object = 0u;
                uint32_t function_type = 0xFFFFFFFFu;
                uint32_t closure = 0u;
                uint32_t is_c = 0xFFFFFFFFu;
                uint32_t proto = 0u;
                uint32_t code = 0u;
                uint32_t source = 0u;
                uint32_t pc_index = 0xFFFFFFFFu;
                uint32_t line = 0xFFFFFFFFu;

                if (ci_base >= 0x00010010u && ci_base < 0x04000000u) {
                    function_object = ci_base - 0x10u;
                    function_type = *(volatile uint32_t *)(base + function_object);
                    closure = *(volatile uint32_t *)(base + function_object + 8u);
                }
                if (function_type == 6u && closure >= 0x00010000u &&
                    closure < 0x03FFFFE0u) {
                    is_c = *(volatile uint8_t *)(base + closure + 6u);
                    if (is_c == 0u) {
                        proto = *(volatile uint32_t *)(base + closure + 0x0Cu);
                        if (proto >= 0x00010000u && proto < 0x03FFFFC0u) {
                            code = *(volatile uint32_t *)(base + proto + 0x0Cu);
                            source = *(volatile uint32_t *)(base + proto + 0x20u);
                            if (saved_pc >= code + 4u && saved_pc < 0x04000000u) {
                                pc_index = (saved_pc - code) / 4u - 1u;
                                const uint32_t size_code =
                                    *(volatile uint32_t *)(base + proto + 0x2Cu);
                                const uint32_t line_info =
                                    *(volatile uint32_t *)(base + proto + 0x14u);
                                if (pc_index < size_code &&
                                    line_info >= 0x00010000u &&
                                    line_info <= 0x03FFFFFCu - pc_index * 4u) {
                                    line = *(volatile uint32_t *)(
                                        base + line_info + pc_index * 4u);
                                }
                            }
                        }
                    }
                }
                fprintf(stderr,
                        "  ci[%02u]=%08X base=%08X top=%08X state=%02X "
                        "funcObj=%08X type=%u closure=%08X isC=%u "
                        "proto=%08X pc=%08X op=%u line=%u source=",
                        frame_index, ci, ci_base, ci_top, ci_state,
                        function_object, function_type, closure, is_c,
                        proto, saved_pc, pc_index, line);
                if (source >= 0x00010000u && source < 0x03FFFFF0u) {
                    const uint32_t source_length =
                        *(volatile uint32_t *)(base + source + 0x0Cu);
                    const uint32_t shown = source_length < 240u
                        ? source_length : 240u;
                    for (uint32_t i = 0u; i < shown; ++i) {
                        const uint8_t ch = *(volatile uint8_t *)(
                            base + source + 0x10u + i);
                        fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                    }
                } else {
                    fputs("<none>", stderr);
                }
                fputc('\n', stderr);
            }
        }
    }
    {
        uint32_t dump_start = stack;
        if (top >= stack && top - stack > 24u * 0x10u)
            dump_start = top - 24u * 0x10u;
        fprintf(stderr, "[LUA ERROR STACK] from=%08X to=%08X\n", dump_start, top);
        for (uint32_t object = dump_start;
             object + 0x10u <= top && object < 0x03FFFFF0u;
             object += 0x10u) {
            const uint32_t type = *(volatile uint32_t *)(base + object);
            const uint32_t value = *(volatile uint32_t *)(base + object + 8u);
            fprintf(stderr, "  obj=%08X type=%u lo=%08X hi=%08X text=",
                    object, type, value,
                    *(volatile uint32_t *)(base + object + 0x0Cu));
            if (type == 4u && value >= 0x00010000u && value < 0x03FFFFF0u) {
                const uint32_t length = *(volatile uint32_t *)(base + value + 0x0Cu);
                const uint32_t shown = length < 160u ? length : 160u;
                for (uint32_t i = 0u; i < shown; ++i) {
                    const uint8_t ch = *(volatile uint8_t *)(
                        base + value + 0x10u + i);
                    fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
                }
            } else {
                fputs("-", stderr);
            }
            fputc('\n', stderr);
        }
    }
    fflush(stderr);
}
void recomp_dump_frontend_post_lua_state(void)
{
    const uint32_t observed = g_frontend_post_lua_count;
    const uint32_t count = observed < 512u ? observed : 512u;
    if (g_frontend_post_lua_target == 0u || observed == 0u)
        return;
    fprintf(stderr, "[FRONTEND-POST-LUA-DUMP] target=%u observed=%u ring=%u\n",
            (uint32_t)g_frontend_post_lua_target, observed, count);
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t index = (observed - count + i) & 511u;
        fprintf(stderr,
                "  [L%03u] va=%08X eax=%08X ebx=%08X ecx=%08X "
                "edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X\n",
                i,
                (uint32_t)g_frontend_post_lua_state[index][0],
                (uint32_t)g_frontend_post_lua_state[index][1],
                (uint32_t)g_frontend_post_lua_state[index][2],
                (uint32_t)g_frontend_post_lua_state[index][3],
                (uint32_t)g_frontend_post_lua_state[index][4],
                (uint32_t)g_frontend_post_lua_state[index][5],
                (uint32_t)g_frontend_post_lua_state[index][6],
                (uint32_t)g_frontend_post_lua_state[index][7],
                (uint32_t)g_frontend_post_lua_state[index][8]);

    }
    fflush(stderr);
}
/* Lightweight Release-build watch for the Lua parser's stack LexState. */
void recomp_lua_lex_trace(uint32_t xbox_va)
{
    static int enabled = -1;
    if (g_frontend_post_lua_target != 0u) {
        const uint32_t post_index = g_frontend_post_lua_count++;
        if (post_index < 2u || (post_index & 63u) == 63u) {
            fprintf(stderr, "[FRONTEND-POST-LUA-PROGRESS] index=%u va=%08X\n",
                    post_index, xbox_va);
            fflush(stderr);
        }
        if (xbox_va == 0x001DEE30u &&
            g_esp >= 0x00010000u && g_esp <= 0x03FFFFF4u) {
            const uint32_t state = guest_u32(g_esp + 4u);
            const uint32_t object = guest_u32(g_esp + 8u);
            fprintf(stderr, "[FRONTEND-POST-LUA-MEM] index=%u args=%08X/%08X L=",
                    post_index, state, object);
            if (state >= 0x00010000u && state <= 0x03FFFFCCu) {
                for (uint32_t word = 0u; word < 13u; ++word)
                    fprintf(stderr, "%s%08X", word == 0u ? "" : ",",
                            guest_u32(state + word * 4u));
            } else {
                fputc('-', stderr);
            }
            fputs(" obj=", stderr);
            if (object >= 0x00010000u && object <= 0x03FFFFE0u) {
                for (uint32_t word = 0u; word < 8u; ++word)
                    fprintf(stderr, "%s%08X", word == 0u ? "" : ",",
                            guest_u32(object + word * 4u));
            } else {
                fputc('-', stderr);
            }
            fputc('\n', stderr);
            if (object >= 0x00010000u && object <= 0x03FFFFF0u) {
                const uint32_t closure = guest_u32(object + 8u);
                const uint32_t ci = state >= 0x00010000u &&
                    state <= 0x03FFFFE8u ? guest_u32(state + 0x14u) : 0u;
                const uint32_t base = state >= 0x00010000u &&
                    state <= 0x03FFFFF0u ? guest_u32(state + 0x0Cu) : 0u;
                fprintf(stderr,
                        "[FRONTEND-POST-LUA-DEEP] index=%u closure=%08X data=",
                        post_index, closure);
                if (closure >= 0x00010000u && closure <= 0x03FFFFACu) {
                    for (uint32_t word = 0u; word < 21u; ++word)
                        fprintf(stderr, "%s%08X", word == 0u ? "" : ",",
                                guest_u32(closure + word * 4u));
                } else {
                    fputc('-', stderr);
                }
                {
                    const uint32_t call_closure =
                        base >= 0x00010008u && base <= 0x03FFFFF8u ?
                        guest_u32(base - 8u) : 0u;
                    fprintf(stderr, " call=%08X:", call_closure);
                    if (call_closure >= 0x00010000u &&
                        call_closure <= 0x03FFFFC0u) {
                        for (uint32_t word = 0u; word < 16u; ++word)
                            fprintf(stderr, "%s%08X", word == 0u ? "" : ",",
                                    guest_u32(call_closure + word * 4u));
                    } else {
                        fputc('-', stderr);
                    }
                }
                fprintf(stderr, " ci=%08X:", ci);
                if (ci >= 0x00010000u && ci <= 0x03FFFFE8u) {
                    for (uint32_t word = 0u; word < 6u; ++word)
                        fprintf(stderr, "%s%08X", word == 0u ? "" : ",",
                                guest_u32(ci + word * 4u));
                } else {
                    fputc('-', stderr);
                }
                fprintf(stderr, " base=%08X:", base);
                if (base >= 0x00010010u && base <= 0x03FFFFE0u) {
                    for (int32_t word = -4; word < 8; ++word)
                        fprintf(stderr, "%s%08X", word == -4 ? "" : ",",
                                guest_u32(base + (uint32_t)(word * 4)));
                } else {
                    fputc('-', stderr);
                }
                fputc('\n', stderr);
            }
            fflush(stderr);
        }
        g_frontend_post_lua_state[post_index & 511u][0] = xbox_va;
        g_frontend_post_lua_state[post_index & 511u][1] = g_eax;
        g_frontend_post_lua_state[post_index & 511u][2] = g_ebx;
        g_frontend_post_lua_state[post_index & 511u][3] = g_ecx;
        g_frontend_post_lua_state[post_index & 511u][4] = g_edx;
        g_frontend_post_lua_state[post_index & 511u][5] = g_esi;
        g_frontend_post_lua_state[post_index & 511u][6] = g_edi;
        g_frontend_post_lua_state[post_index & 511u][7] = g_seh_ebp;
        g_frontend_post_lua_state[post_index & 511u][8] = g_esp;

        if (post_index + 1u >= g_frontend_post_lua_target) {
            recomp_dump_frontend_post_lua_state();
            ExitProcess(86u);
        }
    }
    static uint32_t lexstate;
    static uint32_t expected_state;
    static uint32_t reports;
    static uint32_t previous_va;
    const uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t caller_va;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_LEX") != NULL;
    if (!enabled || base == 0u)
        return;

    caller_va = previous_va;
    previous_va = xbox_va;

    if (xbox_va == 0x001DC520u) {
        const uint32_t buffer = *(volatile uint32_t *)(base + g_esp + 8u);
        const uint32_t size = *(volatile uint32_t *)(base + g_esp + 12u);
        const uint32_t name = *(volatile uint32_t *)(base + g_esp + 16u);
        uint32_t i;
        fprintf(stderr, "[LUA BUFFER] ptr=%08X size=%u name=%08X text=", buffer, size, name);
        if (buffer >= 0x00010000u && buffer < 0x04000000u && size < 0x01000000u) {
            const uint32_t shown = size;
            for (i = 0; i < shown; ++i) {
                const uint8_t ch = *(volatile uint8_t *)(base + buffer + i);
                fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
            }
        }
        fputc('\n', stderr);
        if (buffer >= 0x00010000u && buffer < 0x04000000u &&
            size < 0x01000000u) {
            const uint32_t first = size > 512u ? size - 512u : 0u;
            fprintf(stderr, "[LUA BUFFER TAIL] offset=%u text=", first);
            for (i = first; i < size; ++i) {
                const uint8_t ch = *(volatile uint8_t *)(base + buffer + i);
                fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
            }
            fputc('\n', stderr);
        }
    }

    if (xbox_va == 0x001E3800u) {
        const uint32_t block = *(volatile uint32_t *)(base + g_esp + 8u);
        const uint32_t old_size = *(volatile uint32_t *)(base + g_esp + 12u);
        const uint32_t size = *(volatile uint32_t *)(base + g_esp + 16u);
        if ((block != 0u && (block < 0x00010000u || block >= 0x04000000u)) ||
            old_size >= 0x01000000u || size >= 0x01000000u) {
            fprintf(stderr,
                    "[LUA ALLOC BADARGS] L=%08X block=%08X old=%u size=%u esp=%08X\n",
                    *(volatile uint32_t *)(base + g_esp + 4u), block,
                    old_size, size, g_esp);
        }
    }
    if (xbox_va == 0x001E7C90u) {
        const uint32_t ls = *(volatile uint32_t *)(base + g_esp + 4u);
        const uint32_t token = *(volatile uint32_t *)(base + ls + 0x10u);
        if (token == 0x106u || token == 0x11Fu)
            fprintf(stderr, "[LUA BLOCK] enter token=%08X ls=%08X esp=%08X from=%08X\n",
                    token, ls, g_esp, caller_va);
    } else if (xbox_va == 0x001E7B60u) {
        static uint32_t statement_reports;
        const uint32_t token = *(volatile uint32_t *)(base + g_ebx + 0x10u);
        if (statement_reports++ < 256u)
            fprintf(stderr, "[LUA STMT] dispatch token=%08X ls=%08X esp=%08X from=%08X\n",
                    token, g_ebx, g_esp, caller_va);
    } else if (xbox_va == 0x001E6030u) {
        static uint32_t prefix_reports;
        const uint32_t token = *(volatile uint32_t *)(base + g_eax + 0x10u);
        if (prefix_reports++ < 256u)
            fprintf(stderr, "[LUA PREFIX] token=%08X ls=%08X esp=%08X from=%08X\n",
                    token, g_eax, g_esp, caller_va);
    }
    if (xbox_va == 0x001E4F20u) {
        static uint32_t next_reports;
        const uint32_t token = *(volatile uint32_t *)(base + g_eax + 0x10u);
        if (next_reports++ < 1024u)
            fprintf(stderr, "[LUA NEXT] consume=%08X ls=%08X esp=%08X from=%08X\n",
                    token, g_eax, g_esp, caller_va);
    }
    if (xbox_va == 0x001E4F70u) {
        static uint32_t match_reports;
        const uint32_t token = *(volatile uint32_t *)(base + g_esi + 0x10u);
        if (match_reports++ < 256u)
            fprintf(stderr,
                    "[LUA MATCH] token=%08X expected=%08X line=%u ls=%08X esp=%08X from=%08X\n",
                    token, g_edi, g_eax, g_esi, g_esp, caller_va);
    }
    if (xbox_va == 0x001DD610u) {
        fprintf(stderr,
                "[LUA LIGHT] lua_load from=%08X L=%08X reader=%08X data=%08X name=%08X esp=%08X\n",
                caller_va,
                *(volatile uint32_t *)(base + g_esp + 4u),
                *(volatile uint32_t *)(base + g_esp + 8u),
                *(volatile uint32_t *)(base + g_esp + 12u),
                *(volatile uint32_t *)(base + g_esp + 16u), g_esp);
    }
    if (xbox_va == 0x001DF380u) {
        fprintf(stderr,
                "[LUA LIGHT] protectedparser L=%08X z=%08X bin=%08X esp=%08X\n",
                *(volatile uint32_t *)(base + g_esp + 4u),
                *(volatile uint32_t *)(base + g_esp + 8u),
                *(volatile uint32_t *)(base + g_esp + 12u), g_esp);
    } else if (xbox_va == 0x001DE970u) {
        fprintf(stderr,
                "[LUA LIGHT] rawprotected L=%08X func=%08X ud=%08X esp=%08X\n",
                *(volatile uint32_t *)(base + g_esp + 4u),
                *(volatile uint32_t *)(base + g_esp + 8u),
                *(volatile uint32_t *)(base + g_esp + 12u), g_esp);
    } else if (xbox_va == 0x001DF2F0u) {
        fprintf(stderr,
                "[LUA LIGHT] f_parser L=%08X ud=%08X esp=%08X\n",
                *(volatile uint32_t *)(base + g_esp + 4u),
                *(volatile uint32_t *)(base + g_esp + 8u), g_esp);
    } else if (xbox_va == 0x001E7D70u) {
        fprintf(stderr,
                "[LUA LIGHT] parser L=%08X z=%08X buff=%08X esp=%08X\n",
                *(volatile uint32_t *)(base + g_esp + 4u),
                *(volatile uint32_t *)(base + g_esp + 8u),
                *(volatile uint32_t *)(base + g_esp + 12u), g_esp);
    }

    if (xbox_va == 0x001E39D0u) {
        expected_state = *(volatile uint32_t *)(base + g_esp + 4u);
        lexstate = *(volatile uint32_t *)(base + g_esp + 8u);
        fprintf(stderr,
                "[LUA LIGHT] setinput L=%08X ls=%08X z=%08X source=%08X esp=%08X\n",
                expected_state, lexstate,
                *(volatile uint32_t *)(base + g_esp + 12u),
                *(volatile uint32_t *)(base + g_esp + 16u), g_esp);
        return;
    }

    if (lexstate >= 0x00010000u && lexstate < 0x04000000u) {
        const uint32_t actual = *(volatile uint32_t *)(base + lexstate + 0x34u);
        if (actual != expected_state && reports++ < 16u) {
            fprintf(stderr,
                    "[LUA LIGHT] L changed before %08X: ls=%08X expected=%08X actual=%08X "
                    "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                    xbox_va, lexstate, expected_state, actual,
                    g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
        }
    }

    if (xbox_va == 0x001E3E90u) {
        const uint32_t ls = *(volatile uint32_t *)(base + g_esp + 4u);
        const uint32_t message = *(volatile uint32_t *)(base + g_esp + 8u);
        uint32_t i;
        fprintf(stderr,
                "[LUA LIGHT] error ls=%08X L=%08X token=%08X current=%08X "
                "message=%08X esp=%08X from=%08X text=",
                ls,
                (ls >= 0x00010000u && ls < 0x04000000u) ?
                    *(volatile uint32_t *)(base + ls + 0x34u) : 0u,
                (ls >= 0x00010000u && ls < 0x04000000u) ?
                    *(volatile uint32_t *)(base + ls + 0x10u) : 0u,
                (ls >= 0x00010000u && ls < 0x04000000u) ?
                    *(volatile uint32_t *)(base + ls) : 0u,
                message, g_esp, caller_va);
        if (message >= 0x00010000u && message < 0x04000000u) {
            for (i = 0; i < 200u; ++i) {
                const uint8_t ch = *(volatile uint8_t *)(base + message + i);
                if (ch == 0u)
                    break;
                fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
            }
        }
        fputc('\n', stderr);
    }
}

/* Manual function overrides */

static void *guest_ptr(uint32_t xbox_va)
{
    return (void *)((uintptr_t)xbox_va + g_xbox_mem_offset);
}

static uint32_t guest_u32(uint32_t xbox_va)
{
    return *(volatile uint32_t *)guest_ptr(xbox_va);
}

static uint16_t guest_u16(uint32_t xbox_va)
{
    return *(volatile uint16_t *)guest_ptr(xbox_va);
}

static uint8_t guest_u8(uint32_t xbox_va)
{
    return *(volatile uint8_t *)guest_ptr(xbox_va);
}

typedef struct recomp_saved_guest_cpu_context {
    uint32_t eax, ecx, edx, esp, ebx, esi, edi, seh_ebp;
    double fp_stack[8];
    uint32_t fp_top;
    uint16_t x87_control_word, x87_status_word;
    float xmm[8][4];
    uint64_t mm[8];
    uint32_t current_func;
} recomp_saved_guest_cpu_context;

static void recomp_save_guest_cpu_context(
    recomp_saved_guest_cpu_context *context)
{
    context->eax = g_eax; context->ecx = g_ecx; context->edx = g_edx;
    context->esp = g_esp; context->ebx = g_ebx; context->esi = g_esi;
    context->edi = g_edi; context->seh_ebp = g_seh_ebp;
    memcpy(context->fp_stack, g_fp_stack, sizeof(context->fp_stack));
    context->fp_top = g_fp_top;
    context->x87_control_word = g_x87_control_word;
    context->x87_status_word = g_x87_status_word;
    memcpy(context->xmm[0], g_xmm0, sizeof(context->xmm[0]));
    memcpy(context->xmm[1], g_xmm1, sizeof(context->xmm[1]));
    memcpy(context->xmm[2], g_xmm2, sizeof(context->xmm[2]));
    memcpy(context->xmm[3], g_xmm3, sizeof(context->xmm[3]));
    memcpy(context->xmm[4], g_xmm4, sizeof(context->xmm[4]));
    memcpy(context->xmm[5], g_xmm5, sizeof(context->xmm[5]));
    memcpy(context->xmm[6], g_xmm6, sizeof(context->xmm[6]));
    memcpy(context->xmm[7], g_xmm7, sizeof(context->xmm[7]));
    context->mm[0] = g_mm0; context->mm[1] = g_mm1;
    context->mm[2] = g_mm2; context->mm[3] = g_mm3;
    context->mm[4] = g_mm4; context->mm[5] = g_mm5;
    context->mm[6] = g_mm6; context->mm[7] = g_mm7;
    context->current_func = g_recomp_current_func;
}

static void recomp_restore_guest_cpu_context(
    const recomp_saved_guest_cpu_context *context)
{
    g_eax = context->eax; g_ecx = context->ecx; g_edx = context->edx;
    g_esp = context->esp; g_ebx = context->ebx; g_esi = context->esi;
    g_edi = context->edi; g_seh_ebp = context->seh_ebp;
    memcpy(g_fp_stack, context->fp_stack, sizeof(context->fp_stack));
    g_fp_top = context->fp_top;
    g_x87_control_word = context->x87_control_word;
    g_x87_status_word = context->x87_status_word;
    memcpy(g_xmm0, context->xmm[0], sizeof(context->xmm[0]));
    memcpy(g_xmm1, context->xmm[1], sizeof(context->xmm[1]));
    memcpy(g_xmm2, context->xmm[2], sizeof(context->xmm[2]));
    memcpy(g_xmm3, context->xmm[3], sizeof(context->xmm[3]));
    memcpy(g_xmm4, context->xmm[4], sizeof(context->xmm[4]));
    memcpy(g_xmm5, context->xmm[5], sizeof(context->xmm[5]));
    memcpy(g_xmm6, context->xmm[6], sizeof(context->xmm[6]));
    memcpy(g_xmm7, context->xmm[7], sizeof(context->xmm[7]));
    g_mm0 = context->mm[0]; g_mm1 = context->mm[1];
    g_mm2 = context->mm[2]; g_mm3 = context->mm[3];
    g_mm4 = context->mm[4]; g_mm5 = context->mm[5];
    g_mm6 = context->mm[6]; g_mm7 = context->mm[7];
    g_recomp_current_func = context->current_func;
}

static void recomp_guest_push_u32(uint32_t value)
{
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = value;
}

static void recomp_guard_cancel_owner(uint32_t owner);
static void recomp_guard_request(uint32_t vm, uint32_t handle);
static void recomp_guard_callback(uint32_t vm);
static void recomp_guard_error(uint32_t vm);
static void recomp_guard_poll(void);
static void recomp_guard_preview_snapshot(void);
int recomp_xact_low_level_handle_exists(uint32_t handle);
static void recomp_guard_call_end(void);
static int recomp_test_guard_heap_failure;
#include "voice_callback_queue.h"
extern void sub_00113A50(void);
uint64_t recomp_voice_call_begin(void) { return voice_call_begin(); }
void recomp_voice_cancel_owner(uint32_t owner) { voice_discard(owner, 0, 0); recomp_guard_cancel_owner(owner); }
static void recomp_voice_dispatch_deferred(uint32_t owner, uint32_t vm, const char *name)
{
    if (owner < 0x10000u || owner > 0x03ffffe0u ||
        guest_u32(owner + 0x1Cu) != vm) return;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    size_t length = strlen(name) + 1;
    if (length > 4096 || g_esp < 0x20000u || g_esp >= 0x4000000u) return;
    /* Own scratch below the caller's frame; the callee grows below it. */
    g_esp = (g_esp - (uint32_t)length - 32u) & ~15u;
    uint32_t text = g_esp;
    memcpy(guest_ptr(text), name, length);
    recomp_guest_push_u32(text);
    recomp_guest_push_u32(0);
    g_ecx = owner;
    xbox_preview_log_event("voice-fallback", "dispatch owner=%08X vm=%08X callback=%.100s", owner, vm, name);
    sub_00113A50();
    recomp_restore_guest_cpu_context(&saved);
}
void recomp_voice_call_end(uint64_t mark, int success)
{
    voice_call_end(mark, success, recomp_voice_dispatch_deferred);
    if (!voice_depth) recomp_test_guard_heap_failure = 0;
    recomp_guard_call_end();
}
/* Replacement only for the two synchronous failure branches of
 * Audio_PlayVoiceoverCB. Successful playback keeps the audio stop callback. */
void recomp_voice_fallback_call(void)
{
    uint32_t owner = g_ecx, text = guest_u32(g_esp + 4u);
    if (owner >= 0x10000u && owner <= 0x03ffffe0u &&
        text >= 0x10000u && text < 0x03fff000u) {
        const char *name = (const char *)guest_ptr(text);
        if (memchr(name, 0, 4096) && voice_defer(owner, guest_u32(owner + 0x1Cu), name)) {
            xbox_preview_log_event("voice-fallback", "deferred owner=%08X callback=%.100s", owner, name);
            g_esp += 8u; /* same thiscall ret 4 as InvokeLuaFunction */
            return;
        }
    }
    sub_00113A50();
}
/* Failure injection is private-harness-only; never enabled by user settings. */
static uint32_t recomp_test_hq_managed_failure_hash;
int recomp_test_fail_hq_voice(uint32_t cue)
{
    if (!getenv("MERCENARIES_TEST_GAMEPAD_FILE") || !getenv("MERCENARIES_TEST_FAIL_HQ_VOICE") ||
        cue < 0x10000u || cue >= 0x03fff000u) return 0;
    const char *name = (const char *)guest_ptr(cue);
    int matches = !strncmp(name,"sks.bousks0",10) || !strncmp(name,"mso.boumso0",10) ||
           !strncmp(name,"aso.bouaso0",10) || !strncmp(name,"cso.boucso0",10);
    if (matches && !strcmp(getenv("MERCENARIES_TEST_FAIL_HQ_VOICE"), "managed")) {
        recomp_test_hq_managed_failure_hash = g_ebx; /* caller's PblHash(cue) */
        return 0;
    }
    return matches;
}
void recomp_hq_callback_checkpoint(uint32_t vm, uint32_t text, uint32_t type)
{
    if (text < 0x10000u || text >= 0x03ffff00u) return;
    const char *name = (const char *)guest_ptr(text);
    if (strcmp(name, "PreBriefing") && strcmp(name, "EnterBriefing")) return;
    recomp_guard_callback(vm);
    xbox_preview_log_event("hq-entry", "script-callback vm=%08X name=%s is_function=%u", vm, name, type == 6);
}
static void recomp_hq_note_script_error(uint32_t vm);
/* Record protected-call errors before retail discards the error object. This
 * is read-only and covers failures after the HQ has already relocated us. */
void recomp_lua_pcall_checkpoint(uint32_t vm, uint32_t status, uint32_t description)
{
    if (!status || vm < 0x10000u || vm > 0x03ffffa0u) return;
    recomp_hq_note_script_error(vm);
    recomp_guard_error(vm);
    uint32_t top = guest_u32(vm + 8u);
    char message[385] = "<non-string error>";
    if (top >= 0x10010u && top <= 0x04000000u && guest_u32(top - 16u) == 4u) {
        uint32_t string = guest_u32(top - 8u);
        if (string >= 0x10000u && string <= 0x03ffffefu) {
            uint32_t length = guest_u32(string + 12u);
            if (length <= 0x04000000u - string - 16u) {
                if (length > sizeof(message)-1u) length = sizeof(message)-1u;
                memcpy(message, guest_ptr(string + 16u), length);
                message[length] = 0;
            }
        }
    }
    const char *desc = description >= 0x10000u && description < 0x03ffff80u
        ? (const char *)guest_ptr(description) : (description ? "<invalid>" : "<script load>");
    xbox_preview_log_event("lua-error", "vm=%08X status=%u caller=%.120s message=%s",
                           vm, status, desc, message);
}
/* Fixed globals only, sampled by the existing preview timeline. Never advances
 * scripts or changes the fade/control state while diagnosing a failed entry. */
void recomp_hq_preview_snapshot(void)
{
    recomp_guard_preview_snapshot();
    if (!g_xbox_mem_offset || !guest_u8(0x403384u)) return;
    uint32_t owner = guest_u32(0x403378u);
    uint32_t vm = owner >= 0x10000u && owner <= 0x03ffffe0u
        ? guest_u32(owner + 0x1cu) : 0u;
    xbox_preview_log_event("hq-state",
        "scene=%08X script_done=%u owner=%08X vm=%08X actors=%u "
        "clock=%u rate=%.6g joystick=%u/%u fade_alpha=%u fade_in=%.6g "
        "fade_out=%.6g duration=%.6g stamp=%u leave_canvas=%u "
        "callback_owner=%08X callback=%.99s",
        guest_u32(0x403370u), guest_u8(0x403385u), owner, vm,
        guest_u32(0x403380u), guest_u32(0x4140ccu),
        *(float *)guest_ptr(0x4140d4u), guest_u32(0x323accu),
        guest_u32(0x323ad0u), guest_u8(0x4034acu),
        *(float *)guest_ptr(0x403424u), *(float *)guest_ptr(0x403428u),
        *(float *)guest_ptr(0x40342cu), guest_u32(0x40341cu),
        guest_u8(0x403430u), guest_u32(0x403640u),
        (const char *)guest_ptr(0x403644u));
}
void recomp_hq_init_checkpoint(uint32_t phase, uint32_t vm)
{
    /* Private integration test: fail either ScriptInit or its later timer.
     * Retail Lua raises and unwinds the error; recovery does not fake status. */
    static int injected;
    const char *fault = getenv("MERCENARIES_TEST_FAIL_HQ_INIT");
    if (!injected && fault && getenv("MERCENARIES_TEST_GAMEPAD_FILE")) {
        uint32_t owner = guest_u32(0x403378u), top = guest_u32(vm + 8u);
        if (owner >= 0x10000u && owner <= 0x03ffffd0u &&
            guest_u32(owner + 0x28u) == 0x21c08419u &&
            top >= 0x10010u && top <= 0x04000000u) {
            if (!phase && strcmp(fault, "event")) {
                injected = 1;
                *(uint32_t *)guest_ptr(top - 16u) = 0; /* LUA_TNIL */
            } else if (phase && !strcmp(fault, "event")) {
                injected = 1;
                extern void sub_001DD000(void), sub_001DCF90(void), sub_001DD380(void);
                recomp_saved_guest_cpu_context saved;
                recomp_save_guest_cpu_context(&saved);
                g_esp = (g_esp - 64u) & ~15u;
                uint32_t name = g_esp;
                memcpy(guest_ptr(name), "PlayBriefingScript", sizeof("PlayBriefingScript"));
                /* PlayBriefingScript = 0: the existing event invokes a number. */
                recomp_guest_push_u32(name); recomp_guest_push_u32(vm);
                recomp_guest_push_u32(0); sub_001DD000(); g_esp += 8;
                recomp_guest_push_u32(0); recomp_guest_push_u32(0); /* double 0 */
                recomp_guest_push_u32(vm); recomp_guest_push_u32(0);
                sub_001DCF90(); g_esp += 12;
                recomp_guest_push_u32(0xffffd8efu); recomp_guest_push_u32(vm);
                recomp_guest_push_u32(0); sub_001DD380(); g_esp += 8;
                recomp_restore_guest_cpu_context(&saved);
            }
        }
    }
    xbox_preview_log_sample("hq-entry", "briefing-script phase=%s vm=%08X owner=%08X scene=%08X",
        phase ? "returned" : "begin", vm, guest_u32(0x403378u), guest_u32(0x403370u));
}
void recomp_voice_request_checkpoint(uint32_t vm, uint32_t handle, uint32_t cue, uint32_t callback)
{
    if (callback < 0x10000u || callback >= 0x03fff000u) return;
    const char *name = (const char *)guest_ptr(callback);
    if (!memchr(name, 0, 100) || strcmp(name, "PreBriefing")) return;
    const char *cue_name = cue >= 0x10000u && cue < 0x03fff000u ? (const char *)guest_ptr(cue) : "<invalid>";
    xbox_preview_log_event("hq-entry", "voice-request vm=%08X handle=%08X cue=%.100s callback=%s", vm, handle, cue_name, name);
    recomp_guard_request(vm, handle);
}

/* A guard's PreBriefing continuation is mandatory while its control lock is
 * held. Audio cancellation normally suppresses callbacks; for this one wait,
 * reconcile a provably retired cue after event iteration, never by timeout.
 * Non-HQ callbacks, live/queued audio and VM/world teardown stay untouched. */
typedef struct recomp_guard_wait {
    uint32_t owner, vm, handle, game, play, map;
    unsigned defining_depth;
} recomp_guard_wait;
static recomp_guard_wait guard_wait;
static void mercenaries_voiceover_stop_callback(void);

static uint32_t recomp_guard_callback_record(uint32_t handle)
{
    const uint32_t pool = 0x00371BB0u;
    unsigned count = guest_u16(pool);
    if (!handle || count > 4u) return 0u;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t entry = pool + 0x1A8u + i * 8u;
        if (guest_u32(entry) != handle) continue;
        uint32_t record = guest_u32(entry + 4u);
        if (record < pool + 8u || record > pool + 0x1A8u - 104u ||
            (record - pool - 8u) % 104u) return 0u;
        if (!memchr(guest_ptr(record + 4u), 0, 100u) ||
            strcmp((const char *)guest_ptr(record + 4u), "PreBriefing")) return 0u;
        return record;
    }
    return 0u;
}
static uint32_t recomp_guard_managed_cue(uint32_t handle)
{
    uint32_t pool = guest_u32(0x00690AC0u);
    if (pool < 0x10000u || pool > 0x04000000u - 0x1648u ||
        guest_u16(pool) > 64u) return UINT32_MAX;
    for (unsigned i = 0; i < 64u; ++i) {
        uint32_t cue = pool + 0x44u + i * 0x50u;
        if (guest_u8(pool + 4u + i) && guest_u32(cue + 4u) == handle) return cue;
    }
    return 0u;
}
static void recomp_guard_cancel_owner(uint32_t owner)
{
    if (guard_wait.owner == owner) memset(&guard_wait, 0, sizeof(guard_wait));
}
static void recomp_guard_callback(uint32_t vm)
{
    if (guard_wait.vm == vm) memset(&guard_wait, 0, sizeof(guard_wait));
}
static void recomp_guard_error(uint32_t vm)
{
    /* Never dispatch a continuation from a script that did not finish defining
     * it. The Lua allocation fallback prevents the observed memory failure. */
    if (guard_wait.vm == vm && guard_wait.defining_depth) {
        xbox_preview_log_event("hq-guard", "script-error vm=%08X handle=%08X; cancelling recovery", vm, guard_wait.handle);
        memset(&guard_wait, 0, sizeof(guard_wait));
    }
}
static void recomp_guard_call_end(void)
{
    if (voice_depth < guard_wait.defining_depth) guard_wait.defining_depth = 0;
}
static void recomp_guard_request(uint32_t vm, uint32_t handle)
{
    uint32_t record = recomp_guard_callback_record(handle);
    if (!record || guest_u8(0x403384u) || guest_u32(0x323ACCu) != 15u) return;
    uint32_t owner = guest_u32(record);
    if (owner < 0x10000u || owner > 0x03ffffd0u ||
        guest_u32(owner + 0x1cu) != vm) return;
    guard_wait = (recomp_guard_wait){owner, vm, handle, guest_u32(0x413f6cu),
                                    guest_u32(0x413f68u), guest_u32(0x403970u), voice_depth};
    /* Faults are available only to the private isolated gameplay harness. */
    const char *fault = getenv("MERCENARIES_TEST_HQ_GUARD_FAILURE");
    if (!fault || !getenv("MERCENARIES_TEST_GAMEPAD_FILE")) return;
    if (!strcmp(fault, "heap")) {
        recomp_test_guard_heap_failure = 1;
        xbox_preview_log_event("hq-guard", "private-test fail title allocations until guard script returns");
    } else if (!strcmp(fault, "stopped")) {
        uint32_t cue = recomp_guard_managed_cue(handle);
        if (!cue || cue == UINT32_MAX) return;
        recomp_saved_guest_cpu_context saved;
        recomp_save_guest_cpu_context(&saved);
        extern void sub_00206660(void);
        recomp_guest_push_u32(1u); /* actual retail immediate stop */
        recomp_guest_push_u32(0u);
        g_ecx = cue;
        sub_00206660();
        recomp_restore_guest_cpu_context(&saved);
        xbox_preview_log_event("hq-guard", "private-test interrupted registered welcome handle=%08X", handle);
    }
}
static void recomp_guard_poll(void)
{
    if (!guard_wait.handle || voice_depth || g_esp < 0x20000u || g_esp >= 0x04000000u) return;
    if (getenv("MERCENARIES_TEST_GAMEPAD_FILE") && getenv("MERCENARIES_TEST_GUARD_NO_RECOVERY")) return;
    uint32_t owner = guard_wait.owner, vm = guard_wait.vm, handle = guard_wait.handle;
    if (guest_u32(owner + 0x1cu) != vm || guest_u8(0x403384u) ||
        guest_u32(0x323ACCu) != 15u || guest_u32(0x413f6cu) != guard_wait.game ||
        guest_u32(0x413f68u) != guard_wait.play || guest_u32(0x403970u) != guard_wait.map) {
        memset(&guard_wait, 0, sizeof(guard_wait)); return;
    }
    uint32_t record = recomp_guard_callback_record(handle);
    if (!record || guest_u32(record) != owner) {
        memset(&guard_wait, 0, sizeof(guard_wait)); return;
    }
    uint32_t cue = recomp_guard_managed_cue(handle);
    if (cue == UINT32_MAX) return;
    if (cue && guest_u32(cue + 0x38u) != 4u) return;
    memset(&guard_wait, 0, sizeof(guard_wait));
    if (cue && guest_u32(cue + 0x4cu) == 0x0011EA00u)
        *(uint32_t *)guest_ptr(cue + 0x4cu) = 0u;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    xbox_preview_log_event("hq-guard", "retired welcome recovered handle=%08X vm=%08X", handle, vm);
    recomp_guest_push_u32(handle);
    recomp_guest_push_u32(0u);
    mercenaries_voiceover_stop_callback(); /* ordinary find/invoke/release */
    recomp_restore_guest_cpu_context(&saved);
}

/* A failed room script must not leave its retained entry fade on screen.
 * Defer the normal exit until all Lua calls and event iteration have returned.
 * This is error recovery, not a timeout: healthy or merely slow briefings do
 * not enter this path. The C-17 and other non-HQ scripts are excluded. */
static uint32_t hq_error_owner, hq_error_vm, hq_error_scene;
static int hq_recovering;
static int recomp_hq_has_entry_fade(void)
{
    return guest_u8(0x403430u) && guest_u8(0x4034acu) == 255u &&
        !strcmp((const char *)guest_ptr(0x403644u), "EnterBriefing");
}
static void recomp_hq_note_script_error(uint32_t vm)
{
    uint32_t owner = guest_u32(0x403378u);
    if (hq_recovering || !guest_u8(0x403384u) || owner < 0x10000u || owner > 0x03ffffd0u ||
        guest_u32(owner + 0x1cu) != vm ||
        guest_u32(owner + 0x28u) != 0x21c08419u ||
        !guest_u32(0x403370u) || !recomp_hq_has_entry_fade()) return;
    hq_error_owner = owner;
    hq_error_vm = vm;
    hq_error_scene = guest_u32(0x403370u);
}
void recomp_hq_recover_script_error(void)
{
    recomp_guard_poll();
    if (!hq_error_owner || voice_depth || g_esp < 0x20000u || g_esp >= 0x4000000u) return;
    uint32_t owner = hq_error_owner;
    if (!guest_u8(0x403384u) || guest_u32(0x403378u) != owner ||
        guest_u32(owner + 0x1cu) != hq_error_vm ||
        guest_u32(owner + 0x28u) != 0x21c08419u ||
        guest_u32(0x403370u) != hq_error_scene || !recomp_hq_has_entry_fade()) {
        hq_error_owner = 0;
        return;
    }
    if (!guest_u8(0x403385u)) return; /* finish room construction first */
    hq_error_owner = 0; /* exit can dispatch more scripts; never recurse */
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    /* A contract may have started before an unrelated callback failed. Do not
     * turn recovery into an implicit mission acceptance/transition. */
    extern void sub_00121BD0(void), sub_0010FAB0(void);
    g_ecx = 0x414150u;
    recomp_guest_push_u32(0x4c713ab3u); /* mission_accepted */
    recomp_guest_push_u32(0);
    sub_00121BD0();
    if (!g_eax) {
        xbox_preview_log_event("hq-recovery", "script failed before fade-in; requesting original exit scene=%08X vm=%08X",
                               hq_error_scene, hq_error_vm);
        /* Cancel pending chair/briefing timers before cleaning up a seated
         * player. A normal door exit assumes the player already stood up. */
        recomp_guest_push_u32(hq_error_vm);
        recomp_guest_push_u32(0);
        sub_0010FAB0(); /* CancelAllEventsOfMatchingLuaState (cdecl) */
        g_esp += 4;
        const char *callbacks[] = { "ThreeStageInteractionCleanup", "onUseExit" };
        g_esp = (g_esp - 64u) & ~15u;
        uint32_t name = g_esp;
        hq_recovering = 1;
        for (unsigned i = 0; i < 2u; ++i) {
            memcpy(guest_ptr(name), callbacks[i], strlen(callbacks[i]) + 1u);
            g_ecx = owner;
            recomp_guest_push_u32(name);
            recomp_guest_push_u32(0);
            /* InvokeLuaFunction ignores missing globals. The chair cleanup
             * exists only when an interaction was started in this fresh VM. */
            sub_00113A50();
        }
        hq_recovering = 0;
    }
    recomp_restore_guest_cpu_context(&saved);
}

/* Guard-side state was absent from the old room-only timeline. Observe the
 * exact registered welcome without inferring silence/completion from its age.
 * Runs on the existing preview cadence, never from the audio worker. */
static float guest_f32(uint32_t xbox_va);
static int recomp_guard_audio_range(uint32_t address, uint32_t size)
{
    return address >= 0x10000u && size <= 0x4000000u && address <= 0x4000000u-size;
}
static void recomp_guard_preview_snapshot(void)
{
    static uint64_t last_sample;
    static uint32_t last_handle, last_vm;
    if (!g_xbox_mem_offset || !xbox_preview_log_enabled() || !guard_wait.handle) return;
    uint64_t now = GetTickCount64();
    if (last_handle == guard_wait.handle && last_vm == guard_wait.vm && now-last_sample < 5000u) return;
    last_sample=now; last_handle=guard_wait.handle; last_vm=guard_wait.vm;
    uint32_t managed=recomp_guard_managed_cue(guard_wait.handle);
    if (!managed || managed==UINT32_MAX) {
        xbox_preview_log_event("hq-guard-audio", "handle=%08X vm=%08X managed=%08X controls=%u",
            guard_wait.handle,guard_wait.vm,managed,guest_u32(0x323accu));
        return;
    }
    char record[704];
    size_t used=0;
#define GUARD_AUDIO_APPEND(...) do { \
    if (used<sizeof(record)) { \
        int added=snprintf(record+used,sizeof(record)-used,__VA_ARGS__); \
        if (added>0) used+=(size_t)added; \
    } \
} while (0)
    uint32_t handle=guest_u32(managed+8u), wrapper=0, pool=0x85c600u+0x7568u;
    for (unsigned i=0;i<128u;++i) {
        uint32_t candidate=pool+0x84u+i*0x38u;
        if (handle && guest_u8(pool+4u+i) && guest_u32(candidate+4u)==handle) {wrapper=candidate;break;}
    }
    GUARD_AUDIO_APPEND("handle=%08X vm=%08X managed=%08X state=%u low_handle=%08X wrapper=%08X "
        "controls=%u clock=%u rate=%.6g length=%.6g remaining=%.6g callback=%08X lua_depth=%u defining_depth=%u",
        guard_wait.handle,guard_wait.vm,managed,guest_u32(managed+0x38u),handle,wrapper,
        guest_u32(0x323accu),guest_u32(0x4140ccu),(double)guest_f32(0x4140d4u),
        (double)guest_f32(managed+0x40u),(double)guest_f32(managed+0x44u),guest_u32(managed+0x4cu),
        voice_depth,guard_wait.defining_depth);
    if (!wrapper) goto emit;
    uint32_t cue=guest_u32(wrapper+8u), sound=0;
    int valid=recomp_guard_audio_range(cue,0x40u);
    GUARD_AUDIO_APPEND(" | low_state=%u hash=%08X src=%d cat=%u delay=%.6g cue=%08X "
        "flags=%08X children=%08X/%08X/%08X",
        guest_u32(wrapper),guest_u32(wrapper+0x1cu),(int32_t)guest_u32(wrapper+0xcu),
        guest_u32(wrapper+0x24u),(double)guest_f32(wrapper+0x2cu),cue,
        valid?guest_u32(cue+0x20u):UINT32_MAX,valid?guest_u32(cue+0x28u):0u,
        valid?guest_u32(cue+0x2cu):0u,valid?guest_u32(cue+0x30u):0u);
    if (!valid) goto emit;
    for (unsigned i=0;i<3u && !sound;++i) sound=guest_u32(cue+0x28u+4u*i);
    if (!recomp_guard_audio_range(sound,0x48u) || guest_u32(sound+0x44u)!=cue) goto emit;
    uint32_t track=guest_u32(sound+0x34u),config=guest_u32(sound+0x10u);
    GUARD_AUDIO_APPEND(" | sound=%08X state=%u flags=%08X active=%u track=%08X tracks=%u",
        sound,guest_u16(sound+0x30u),guest_u32(sound+0x38u),guest_u32(sound+0x40u),track,
        recomp_guard_audio_range(config,9u)?guest_u8(config+8u):0u);
    /* The welcome's first track is sufficient to distinguish blocked file
     * reads from queued DirectSound packets. Never follow arbitrary lists. */
    if (!recomp_guard_audio_range(track,0x88u)) goto emit;
    uint32_t stream=guest_u32(track+0x40u);
    GUARD_AUDIO_APPEND(" | track=%08X flags=%04X voice=%08X stream=%08X remaining=%u offset=%u",
        track,guest_u16(track),guest_u32(track+4u),stream,guest_u32(track+0x44u),guest_u32(track+0x48u));
    if (!recomp_guard_audio_range(stream,0x50u)) goto emit;
    uint32_t first=stream+0x10u, second=stream+0x30u;
    GUARD_AUDIO_APPEND(" | packet0=%08X/%u/%08X/%08X/%u/%u packet1=%08X/%u/%08X/%08X/%u/%u",
        guest_u32(first),guest_u32(first+4u),guest_u32(first+8u),guest_u32(first+0xcu),
        guest_u32(first+0x10u),guest_u32(first+0x14u),
        guest_u32(second),guest_u32(second+4u),guest_u32(second+8u),guest_u32(second+0xcu),
        guest_u32(second+0x10u),guest_u32(second+0x14u));
emit:
    xbox_preview_log_event("hq-guard-audio", "%s", record);
#undef GUARD_AUDIO_APPEND
}

/* Retail RedMovie::Update contains this operation, but BrushMovie calls
 * UpdateMovie instead. Apply Music-category attenuation before the decoder
 * queues its first packet and whenever the volume changes. XMV is a mixed
 * soundtrack, not separate dialogue/SFX/music stems. */
void recomp_movie_apply_volume(uint32_t movie)
{
    if (movie < 0x10000u || movie > 0x03ffffbcu ||
        !guest_u8(movie + 0x14u)) return;
    uint32_t count = guest_u32(movie + 0x24u);
    if (count > 4u) return; /* RedMovie::iMaxAudioStreams */
    float level = *(float *)guest_ptr(0x00842130u); /* eMusic category */
    if (!isfinite(level)) return;
    int32_t volume = (int32_t)fmaxf(-10000.0f, fminf(0.0f, level));
    recomp_func_t set_volume = recomp_lookup(0x0029E155u);
    if (!set_volume) return;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t stream = guest_u32(movie + 4u + 4u * i);
        if (stream < 0x10000u || stream > 0x03ffffd8u) continue;
        uint32_t settings = guest_u32(stream + 0x14u);
        if (settings < 0x10000u || settings > 0x03ffffdcu) continue;
        int32_t current = (int32_t)guest_u32(settings + 0x1cu);
        int32_t headroom = (int32_t)guest_u32(settings + 0x20u);
        if ((int64_t)current + headroom == volume) continue;
        /* Opt-in, bounded evidence for movie/audio-option regression runs. */
        { static int trace = -1; static unsigned traced;
          if (trace < 0) trace = getenv("MERCENARIES_TRACE_FMV_VOLUME") != NULL;
          if (trace && traced++ < 16u)
              fprintf(stderr, "[FMV-VOLUME] movie=%08X stream=%08X category_db=%d previous_db=%d headroom=%d\n",
                      movie, stream, volume, current, headroom); }
        recomp_guest_push_u32((uint32_t)volume);
        recomp_guest_push_u32(stream);
        recomp_guest_push_u32(0u);
        set_volume();
        recomp_restore_guest_cpu_context(&saved);
    }
    recomp_restore_guest_cpu_context(&saved);
}

static uint32_t recomp_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/* Binding icons share the retail menu canvas, clip, fade and row layout.
 * Draw after the selected-row background so that it cannot cover the icon. */
void recomp_controls_paint_binding_icons(uint32_t brush,float start_y,float row_height)
{
    int recomp_controls_row_binding(uint32_t,int*,unsigned short*);
    void recomp_prompts_binding_row(uint32_t);
    if(brush<0x10000u || brush>0x03ffff00u || !(row_height>0.f))return;
    recomp_func_t begin=recomp_lookup(0x000E4E00u),box=recomp_lookup(0x0020A1C0u);
    if(!begin || !box)return;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    unsigned count=guest_u32(brush+0x3cu);
    if(count>16)count=16;
    for(unsigned row=0;row<count;++row){
        uint32_t hash=guest_u32(brush+0x40u+row*4u);
        if(!recomp_controls_row_binding(hash,NULL,NULL))continue;
        float per=*(float*)guest_ptr(brush+0xc8u);
        float fade=per>0.f?(*(float*)guest_ptr(brush+0xc4u)-*(float*)guest_ptr(brush+0x80u+row*4u))/per:1.f;
        if(!(fade>0.f))continue;if(fade>1.f)fade=1.f;
        recomp_prompts_binding_row(hash);
        g_esp=saved.esp;
        recomp_guest_push_u32(0);recomp_guest_push_u32(0);recomp_guest_push_u32(0);
        recomp_guest_push_u32(((uint32_t)(128.f*fade)<<24)|0x00808080u);
        recomp_guest_push_u32(0xCCDC1675u);
        recomp_guest_push_u32(0);g_ecx=brush;begin();
        float size=row_height*.95f;
        float y=start_y+row_height*row+*(float*)guest_ptr(0x351f58u)-size*.5f;
        recomp_guest_push_u32(0x3f800000u);recomp_guest_push_u32(0x3f800000u);
        recomp_guest_push_u32(0);recomp_guest_push_u32(0);
        recomp_guest_push_u32(recomp_float_bits(size));recomp_guest_push_u32(recomp_float_bits(size));
        recomp_guest_push_u32(recomp_float_bits(y));recomp_guest_push_u32(recomp_float_bits(69.f-size*.5f));
        recomp_guest_push_u32(0);g_ecx=brush;box();
        recomp_prompts_binding_row(0);
    }
    recomp_restore_guest_cpu_context(&saved);
}

/* Use the retail untextured quad path so the slider shares menu transforms,
 * clipping and fade. The thumb's entire width stays inside the track. */
void recomp_options_paint_fov_slider(uint32_t brush, float start_y, float row_height)
{
    if (brush < 0x10000u || brush > 0x03FFFF00u || !(row_height > 0.f)) return;
    unsigned count = guest_u32(brush + 0x3Cu);
    if (count > 16u) return;
    recomp_func_t begin = recomp_lookup(0x0020A0D0u);
    recomp_func_t box = recomp_lookup(0x0020A1C0u);
    if (!begin || !box) return;
    for (unsigned row = 0; row < count; ++row) {
        if (guest_u32(brush + 0x40u + row * 4u) != RECOMP_OPTIONS_FOV_HASH) continue;
        float per = *(float *)guest_ptr(brush + 0xC8u);
        float fade = per > 0.f ? (*(float *)guest_ptr(brush + 0xC4u) -
                     *(float *)guest_ptr(brush + 0x80u + row * 4u)) / per : 1.f;
        if (!(fade > 0.f)) return;
        if (fade > 1.f) fade = 1.f;
        const float left = 190.f, width = 110.f, thumb = 4.f;
        const float y = start_y + row_height * row + *(float *)guest_ptr(0x351F58u);
        const float position = recomp_options_fov_slider_position();
        recomp_saved_guest_cpu_context saved;
        recomp_save_guest_cpu_context(&saved);
        for (unsigned part = 0; part < 2u; ++part) {
            const float x = part ? left + (width - thumb) * position : left;
            const float w = part ? thumb : width;
            const float h = part ? row_height * .65f : 1.f;
            g_esp = saved.esp;
            recomp_guest_push_u32(0); recomp_guest_push_u32(0); recomp_guest_push_u32(0);
            recomp_guest_push_u32(((uint32_t)(128.f * fade) << 24) | 0x00FFFFFFu);
            recomp_guest_push_u32(0); /* no texture */
            recomp_guest_push_u32(0); g_ecx = brush; begin();
            recomp_guest_push_u32(0x3F800000u); recomp_guest_push_u32(0x3F800000u);
            recomp_guest_push_u32(0); recomp_guest_push_u32(0);
            recomp_guest_push_u32(recomp_float_bits(h)); recomp_guest_push_u32(recomp_float_bits(w));
            recomp_guest_push_u32(recomp_float_bits(y - h * .5f)); recomp_guest_push_u32(recomp_float_bits(x));
            recomp_guest_push_u32(0); g_ecx = brush; box();
        }
        recomp_restore_guest_cpu_context(&saved);
        return;
    }
}

/* Other cameras (including offscreen rendering) retain their authored FOV. */
float recomp_options_camera_fov(uint32_t camera, float fov)
{
    if (camera >= 0x10000u && camera <= 0x03FFFF00u &&
        guest_u32(camera) == 0x00300DC4u)
        return recomp_options_scale_fov(fov);
    return fov;
}

#include "dev_spawn.h"
#include "dev_battle_guest.h"
#include "dev_factions_guest.h"
#include "dev_mission_guest.h"
#include "boids_guest.h"
#include "freecam_streaming.h"
#include "cargo_helicopters.h"
#include "ai_boarding.h"
#include "stream_admission.h"
int recomp_stream_admit_idle(uint32_t manager,uint32_t bytes)
{
    if(manager<0x10000u||manager>0x4000000u-0x3C8u)return 0;
    uint32_t queued=guest_u32(manager+0x10C)&0xffu;
    queued|=guest_u32(manager+0x110)!=guest_u32(manager+0x114);
    int admit=recomp_stream_idle_admission(guest_u32(manager+0x3B4),bytes,
                                         guest_u32(manager+0x218),queued);
    if(admit)xbox_preview_log_event("stream-admission","idle budget relief used=%u request=%u",guest_u32(manager+0x3B4),bytes);
    return admit;
}


/* Rebuild the retail RedCamera projection when Aspect Ratio is applied while
 * gameplay is already running.  The title normally chooses these values once
 * when RedCamera is constructed from the Xbox EEPROM widescreen flag.  Merely
 * resizing the host presentation later therefore stretches the existing 4:3
 * matrix until the camera is recreated.  Invoke the original SetPerspective
 * method on the live camera, with all translated CPU state preserved, so the
 * same retail matrix path is used immediately. */
float recomp_options_camera_far_plane(uint32_t camera, float far_plane) {
    /* Expand the view frustum only. RedCamera::_fDrawDistance still controls
     * the terrain cache and must remain at its original 300 metre budget. */
    if (camera >= 0x10000u && camera <= 0x03FFFF00u &&
        guest_u32(camera) == 0x00300DC4u &&
        far_plane == *(float *)guest_ptr(camera + 0xA0u))
        return far_plane * recomp_options_object_distance_multiplier();
    return far_plane;
}

void recomp_options_refresh_retail_camera_projection(void)
{
    const uint32_t camera_global = 0x006437B4u;
    const uint32_t video_widescreen_flag = 0x006437D0u;
    const uint32_t red_camera_vtable = 0x00300DC4u;
    const uint32_t retail_fov_4_3_address = 0x00300DC8u;
    const uint32_t retail_aspect_4_3_address = 0x00300DCCu;
    recomp_saved_guest_cpu_context saved;
    recomp_func_t set_perspective;
    uint32_t camera;
    uint32_t far_plane;
    float fov;
    float aspect;

    if (g_xbox_mem_offset == 0 || g_esp < 0x00010020u)
        return;

    *(volatile uint8_t *)guest_ptr(video_widescreen_flag) =
        recomp_options_aspect_ratio() != 0 ? 1u : 0u;
    camera = guest_u32(camera_global);
    if (camera < 0x00010000u || camera > 0x03FFFF00u ||
        guest_u32(camera) != red_camera_vtable)
        return;

    set_perspective = recomp_lookup(0x001F8F10u);
    if (set_perspective == NULL)
        return;

    fov = *(volatile float *)guest_ptr(retail_fov_4_3_address);
    aspect = *(volatile float *)guest_ptr(retail_aspect_4_3_address);
    fov = recomp_options_perspective_fov(fov, aspect);
    aspect = recomp_options_perspective_aspect(aspect);
    far_plane = guest_u32(camera + 0xA0u);

    recomp_save_guest_cpu_context(&saved);
    recomp_guest_push_u32(recomp_float_bits(aspect));
    recomp_guest_push_u32(recomp_float_bits(fov));
    recomp_guest_push_u32(far_plane);
    recomp_guest_push_u32(0x3E4CCCCDu); /* retail near plane: 0.2f */
    recomp_guest_push_u32(0u);          /* translated call return slot */
    g_ecx = camera;
    set_perspective();
    recomp_restore_guest_cpu_context(&saved);

    fprintf(stderr,
            "[RECOMP-OPTIONS] refreshed retail camera projection "
            "camera=%08X fov=%.6f aspect=%.6f\n",
            camera, fov, aspect);
}

static void recomp_print_red_lookup_string(uint32_t source)
{
    uint32_t emitted = 0u;

    while (emitted < 80u) {
        uint32_t token = guest_u8(source);
        if (token == 0u)
            break;
        if ((token & 0x80u) != 0u) {
            uint32_t length = token & 0x1Fu;
            uint32_t offset;
            uint32_t copy;
            source += 1u;
            offset = guest_u8(source);
            source += 1u;
            offset |= (token & 0x60u) << 3u;
            copy = source - offset - 2u;
            while (length-- != 0u && emitted < 80u) {
                fputc((int)guest_u8(copy++), stderr);
                emitted += 1u;
            }
        } else {
            fputc((int)token, stderr);
            source += 1u;
            emitted += 1u;
        }
    }
}
void recomp_xbox_debug_print(uint32_t text_address, uint32_t text_length)
{
    fprintf(stderr, "[XBOX-DBG] ");
    for (uint32_t i = 0; i < text_length; ++i)
        fputc((int)guest_u8(text_address + i), stderr);
    fflush(stderr);
}

static float guest_f32(uint32_t xbox_va)
{
    float value;
    const uint32_t bits = guest_u32(xbox_va);
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void recomp_extraction_probe(uint32_t stage, uint32_t subject, uint32_t value)
{
    static int enabled = -1;
    static unsigned counts[6];
    uint32_t ai, actor;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_EXTRACTION_FLIGHT") != NULL;
    if (!enabled || stage >= 6u || subject < 0x10000u ||
        subject > 0x04000000u - 0x100u)
        return;
    if (stage < 5u) {
        if (counts[stage]++ >= 64u) return;
        fprintf(stderr, "[EXTRACTION-SPAWN] stage=%u vector=%08X "
                "pos=(%.8g,%.8g,%.8g) value=%08X collision-y=%.8g\n",
                stage, subject, guest_f32(subject), guest_f32(subject + 4u),
                guest_f32(subject + 8u), value, guest_f32(0x378EE4u));
    } else {
        if (counts[stage]++ >= 2048u || (counts[stage] & 7u) != 1u) return;
        ai = guest_u32(subject + 8u);
        if (ai < 0x10000u || ai > 0x04000000u - 0x600u) return;
        actor = guest_u32(ai + 0x10u);
        if (actor < 0x10000u || actor > 0x04000000u - 0x100u) return;
        fprintf(stderr, "[EXTRACTION-FLIGHT] state=%08X phase=%u ai=%08X "
                "actor=%08X pos=(%.8g,%.8g,%.8g) hp=%.8g "
                "target=(%.8g,%.8g,%.8g) collective=%.8g prior-lift=%.8g "
                "avoid=(%.8g,%.8g,%.8g) avoid-active=%u dt=%.8g\n",
                subject, guest_u32(subject + 0x18u), ai, actor,
                guest_f32(actor + 0xE0u), guest_f32(actor + 0xE4u),
                guest_f32(actor + 0xE8u), guest_f32(actor + 0x98u),
                guest_f32(subject + 0xCu), guest_f32(subject + 0x10u),
                guest_f32(subject + 0x14u), guest_f32(ai + 0x5A4u),
                guest_f32(ai + 0x5A8u), guest_f32(ai + 0x5ACu),
                guest_f32(ai + 0x5B0u), guest_f32(ai + 0x5B4u),
                guest_u8(ai + 0x5BCu), guest_f32(0x413FA0u));
    }
    fflush(stderr);
}

void recomp_extraction_use_probe(uint32_t stage, uint32_t actor,
                                 uint32_t user, uint32_t data,
                                 uint32_t count)
{
    static int enabled = -1;
    static unsigned reports;
    static unsigned calls[5];
    unsigned i;
    int extraction_actor;
    int load_action = 0;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_EXTRACTION_USE") != NULL;
    if (!enabled || stage >= 5u || reports >= 4096u ||
        actor < 0x10000u || actor > 0x04000000u - 0x1CEEu ||
        user < 0x10000u || user > 0x04000000u - 0x800u ||
        data < 0x10000u || data > 0x04000000u - 0x70u)
        return;

    /* GetBestUsableActor runs every frame. Preserve the first eight samples
     * at each boundary, then retain one in eight so a normal walk from the
     * 15 m gather radius to the dock cannot exhaust the bounded evidence. */
    if (calls[stage] >= 8192u)
        return;
    if (calls[stage] >= 8u && (calls[stage] & 7u) != 0u) {
        ++calls[stage];
        return;
    }
    ++calls[stage];

    /* +0x1CED is a helicopter-only field; arbitrary actors can contain a
     * nonzero byte at that offset.  Restrict selection-stage filtering to the
     * retail RsActorVehicleHelicopter vtable before reading its extraction
     * flag, otherwise ordinary nearby actors exhaust the bounded log. */
    extraction_actor = guest_u32(actor) == 0x002E21A8u &&
        guest_u8(actor + 0x1CEDu) != 0;
    if (count <= 4u) {
        for (i = 0; i < count; ++i) {
            if ((guest_u32(data + i * 0x1Cu + 0x14u) & 0x7Fu) == 0xBu) {
                load_action = 1;
                break;
            }
        }
    }
    if (!extraction_actor && !load_action)
        return;

    ++reports;
    fprintf(stderr,
            "[EXTRACTION-USE] stage=%u actor=%08X user=%08X count=%u "
            "actor-pos=(%.7g,%.7g,%.7g) user-pos=(%.7g,%.7g,%.7g) "
            "data=%08X matrix/action-pos=(%.7g,%.7g,%.7g) "
            "radius=%.7g action=%u flags=%02X carry=%08X\n",
            stage, actor, user, count,
            guest_f32(actor + 0xE0u), guest_f32(actor + 0xE4u),
            guest_f32(actor + 0xE8u), guest_f32(user + 0xE0u),
            guest_f32(user + 0xE4u), guest_f32(user + 0xE8u), data,
            guest_f32(data), guest_f32(data + 4u), guest_f32(data + 8u),
            stage < 2u ? 0.0f : guest_f32(data + 0xCu),
            stage < 2u ? 0u : guest_u32(data + 0x14u) & 0x7Fu,
            stage < 2u ? 0u : guest_u8(data + 0x18u),
            guest_u32(user + 0x79Cu));
    fflush(stderr);
}

/* The retail money counter advances its digits every update and plays a cue
 * on alternate updates. Batch only that counter at the original 30 Hz; its
 * fade/scale still use every frame's unmodified game-plus-UI delta. This HUD
 * is a singleton updated on the game thread. A reset guest frame serial also
 * resets the sidecar, including when the widget is reconstructed in place. */
float recomp_money_counter_dt(uint32_t widget, uint32_t frame,
                             float real_dt, float animation_dt, int *play_cue)
{
    static uint32_t owner, next_frame;
    static double cadence_time, counter_time;
    static unsigned odd_tick;
    const double period = 1.0 / 30.0;
    double ticks;
    float elapsed;

    *play_cue = 0;
    if (owner != widget || frame != next_frame) {
        cadence_time = counter_time = 0.0;
        odd_tick = 0u;
        owner = widget;
    }
    next_frame = frame + 1u;
    if (!isfinite(real_dt) || !isfinite(animation_dt) ||
        real_dt < 0.0f || animation_dt < 0.0f) {
        cadence_time = counter_time = 0.0;
        return 0.0f;
    }
    cadence_time += real_dt;
    counter_time += animation_dt;
    /* Allow float delta rounding at an exact 30 Hz boundary. */
    ticks = floor(cadence_time / period + 0.000001);
    if (ticks < 1.0)
        return 0.0f;
    cadence_time = fmax(0.0, cadence_time - ticks * period);
    elapsed = (float)counter_time;
    counter_time = 0.0;
    /* A stalled frame catches up the number without a burst of queued cues. */
    *play_cue = ticks >= 2.0 || !odd_tick;
    odd_tick ^= (unsigned)fmod(ticks, 2.0);
    return elapsed;
}

/* The main loop truncates every QPC delta to 1/3000-second ticks. Carry the
 * discarded fraction to the next update, before either the game or UI clock
 * applies its own pause/scale. Do not change the shared CRT conversion. */
double recomp_frame_ticks_with_remainder(double ticks)
{
    static double remainder;
    double whole;
    if (!isfinite(ticks) || ticks < 0.0 || ticks > 2147483647.0) {
        remainder = 0.0;
        return ticks;
    }
    ticks += remainder;
    remainder = modf(ticks, &whole);
    return whole; /* Exact integer input to the retail truncating conversion. */
}

/* The retail turbulence filter divides by frame time, amplifying its random
 * impulse above the original 30 FPS rate. Preserve that rate's damping at
 * higher refresh rates; phase, script timing and random draws still use dt.
 * RsCameraStateMario64.cpp::DoAddTurbulenceNoise is the only patched caller. */
float recomp_turbulence_damping_dt(float dt)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_DISABLE_TURBULENCE_DAMPING_FIX") == NULL;
    if (enabled && dt > 0.0f && dt < 1.0f / 30.0f)
        return 1.0f / 30.0f;
    return dt;
}

/* Recover static lights initialized while the camera is outside
 * their draw range. RsLight::Update returns immediately for FlickerType_None,
 * leaving RecalculateLight's black distance-fade result cached after relocation.
 * Reuse the original recalculation only after its full-intensity range is met.
 * This does not change authored colors, radius, activation or flicker timing. */
int recomp_static_light_refresh_needed(uint32_t light)
{
    static int enabled = -1;
    static uint32_t reports;
    uint32_t camera;
    float distance, fade, dx, dz, full;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_DISABLE_STATIC_LIGHT_REFRESH") == NULL;
    if (!enabled || light < 0x10000u || light > 0x04000000u - 0x248u)
        return 0;
    if (!(guest_u8(light + 0x1A0u) & 1u) ||
        guest_u32(light + 0x1D0u) != 0u ||
        guest_u32(light + 0x1A8u) != 0x80000000u ||
        guest_u32(light + 0x240u) != 0x80000000u ||
        !(guest_u32(light + 0x1A4u) & 0x00FFFFFFu) ||
        !(guest_f32(light + 0x23Cu) > 0.0f))
        return 0;
    camera = guest_u32(0x006437B4u);
    if (camera < 0x10000u || camera > 0x04000000u - 0x4Cu)
        return 0;
    distance = guest_f32(light + 0x154u);
    fade = guest_f32(light + 0x1B0u);
    dx = guest_f32(camera + 0x40u) - guest_f32(light + 0x70u);
    dz = guest_f32(camera + 0x48u) - guest_f32(light + 0x78u);
    full = distance - fade;
    if (!isfinite(distance) || !isfinite(fade) || full <= 0.0f ||
        !isfinite(dx) || !isfinite(dz) || dx * dx + dz * dz > full * full)
        return 0;
    if (reports++ < 64u)
        fprintf(stderr, "[STATIC-LIGHT-REFRESH] light=%08X color=%08X "
            "distance=%.6g range=%.6g radius=%.6g\n", light,
            guest_u32(light + 0x1A4u), sqrtf(dx * dx + dz * dz),
            distance, guest_f32(light + 0x23Cu));
    return 1;
}

void recomp_omni_light_trace(uint32_t index, uint32_t light,
                             uint32_t color)
{
    static int enabled = -1;
    static uint32_t sample;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_OMNI_LIGHTS") != NULL;
    if (!enabled || !g_mercenaries_standup_complete || sample >= 64u)
        return;
    if (light < 0x00010000u || light > 0x03FFFFD8u)
        return;

    ++sample;
    fprintf(stderr,
            "[RETAIL-OMNI-LIGHT] sample=%u index=%u light=%08X "
            "vtable=%08X node=(%08X,%08X) active=%08X "
            "sphere=(%.6g,%.6g,%.6g r=%.6g) color=%08X "
            "raw10=%08X raw28=%08X\n",
            sample, index, light, guest_u32(light),
            guest_u32(light + 4u), guest_u32(light + 8u),
            guest_u32(light + 0xCu),
            guest_f32(light + 0x14u), guest_f32(light + 0x18u),
            guest_f32(light + 0x1Cu), guest_f32(light + 0x20u),
            color, guest_u32(light + 0x10u), guest_u32(light + 0x28u));
    fflush(stderr);
}
static int g_recomp_input_initialized;
static uint32_t g_recomp_input_device_mask;
static ULONGLONG g_test_auto_a_stop_deadline_ms;

static void recomp_release_test_auto_a(void)
{
    const uint32_t joystick = guest_u32(0x00413F90u);

    xbox_InputStopTestAutoA();
    if (joystick >= 0x00010000u && joystick <= 0x04000000u - 0xCCu) {
        *(uint32_t *)guest_ptr(joystick + 0xC0u) &= ~0x40u;
        *(uint32_t *)guest_ptr(joystick + 0xC4u) &= ~0x40u;
        *(uint32_t *)guest_ptr(joystick + 0xC8u) |= 0x40u;
    }
    *(uint16_t *)guest_ptr(0x007AC848u) |= 0x40u;
    *(uint16_t *)guest_ptr(0x007AC85Au) |= 0x40u;
}
static ULONGLONG g_test_standup_capture_deadline_ms;
static ULONGLONG g_test_game_capture_deadline_ms;
static ULONGLONG g_test_post_aircraft_capture_deadline_ms;
static ULONGLONG g_test_standup_trace_until_ms;
static ULONGLONG g_test_standup_trace_next_ms;
static uint32_t g_test_standup_trace_samples;
static ULONGLONG g_test_camera_trace_until_ms;
static ULONGLONG g_test_camera_trace_next_ms;
static uint32_t g_test_camera_trace_samples;
static ULONGLONG g_test_interaction_capture_at_ms;
static ULONGLONG g_test_camera_return_capture_at_ms;
static ULONGLONG g_test_aircraft_target_trace_next_ms;
static uint32_t g_recomp_briefing_pda_actor;
static uint32_t g_recomp_briefing_m4_actor;
static uint32_t g_recomp_briefing_grenades_actor;
static uint32_t g_recomp_briefing_door_actor;
static uint32_t g_recomp_briefing_usehumvee_actor;
static float g_test_pre_interaction_yaw;
static int g_test_pre_interaction_yaw_valid;
static int g_test_post_pda_camera_return;

static uint32_t recomp_lookup_entity_by_hash(uint32_t hash,
                                             uint32_t *spore_out);
static void recomp_trace_aircraft_targets(ULONGLONG now);

/* Record only active vehicle recenter steps, including short-update completion. */
void recomp_vehicle_recenter_checkpoint(uint32_t state, float max_step,
                                        float raw_angle, float step)
{
    static int enabled = -1;
    static uint32_t count;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_CAMERA_TRANSITION") != NULL;
    if (!enabled || count >= 1024u || state < 0x10000u ||
        state > 0x04000000u - 0x160u)
        return;
    ++count;
    fprintf(stderr,
            "[VEHICLE-RECENTER-STEP] sample=%u tick=%llu state=%08X "
            "mode=%08X max-step=%.9g raw-angle=%.9g step=%.9g "
            "soft=%u legacy-premature=%u\n",
            count, (unsigned long long)GetTickCount64(), state,
            guest_u32(state + 0x158u), max_step, raw_angle, step,
            (unsigned)guest_u8(state + 0x15Cu),
            (unsigned)(max_step > 0.0f && fabsf(step) < .005f &&
                       fabsf(raw_angle) >= .005f));
}

void recomp_camera_mode_checkpoint(uint32_t setter)
{
    static uint32_t sequence;
    static int pickup_capture_armed;
    const uint32_t camera = g_ecx;
    const int camera_valid = camera >= 0x00010000u &&
                             camera <= 0x04000000u - 0x58u;
    const int stack_valid = g_esp >= 0x00010000u &&
                            g_esp <= 0x04000000u - 8u;
    const int cinematic = setter == 0x000977B0u;
    const uint32_t target = camera_valid ?
        guest_u32(camera + (cinematic ? 0x30u : 0x38u)) : 0u;
    float transition = 0.0f;
    const ULONGLONG now = GetTickCount64();

    if (stack_valid)
        transition = guest_f32(g_esp + 4u);

    if (getenv("MERCENARIES_TRACE_CAMERA_MODE") != NULL) {
        fprintf(stderr,
                "[CAMERA-MODE] seq=%u mode=%s setter=%08X camera=%08X "
                "current=%08X target=%08X transition=%.7g lock=%u "
                "standup=%u esp=%08X return=%08X\n",
                ++sequence, cinematic ? "CINEMATIC" : "MARIO64",
                setter, camera,
                camera_valid ? guest_u32(camera + 0x24u) : 0u,
                target, transition,
                camera_valid ? (unsigned int)guest_u8(camera + 0x54u) : 0u,
                g_mercenaries_standup_complete, g_esp,
                stack_valid ? guest_u32(g_esp) : 0u);
        fflush(stderr);
    }

    if (!g_mercenaries_standup_complete)
        return;

    if (getenv("MERCENARIES_TRACE_CAMERA_TRANSITION") != NULL ||
        getenv("MERCENARIES_TRACE_CAMERA_MODE") != NULL) {
        g_test_camera_trace_until_ms = now + 5000u;
        g_test_camera_trace_next_ms = now;
        g_test_camera_trace_samples = 0u;
    }

    if (cinematic && !pickup_capture_armed) {
        const uint32_t mario64 = camera_valid ?
            guest_u32(camera + 0x38u) : 0u;
        const char *pre_path =
            getenv("MERCENARIES_CAPTURE_PRE_INTERACTION_PATH");
        const char *path = getenv("MERCENARIES_CAPTURE_CAMERA_MODE_PATH");
        if (mario64 >= 0x00010000u &&
            mario64 <= 0x04000000u - 0x114u) {
            g_test_pre_interaction_yaw = guest_f32(mario64 + 0x10Cu);
            g_test_pre_interaction_yaw_valid = 1;
        }
        /* The setter runs before the new scripted camera has rendered. A
         * synchronous capture here records the final known-good Mario64
         * frame immediately before the pickup camera takes over. */
        if (pre_path != NULL && pre_path[0] != '\0')
            d3d8_DebugCaptureFrameToPath(pre_path);
        if (path != NULL && path[0] != '\0')
            d3d8_DebugArmFlipCapture(path);
        if (getenv("MERCENARIES_CAPTURE_INTERACTION_PATH") != NULL)
            g_test_interaction_capture_at_ms = now + 500u;
        pickup_capture_armed = 1;
    } else if (!cinematic && pickup_capture_armed == 1) {
        const char *delay_text;
        unsigned long delay_ms;

        g_test_post_pda_camera_return = 1;
        if (getenv("MERCENARIES_CAPTURE_POST_PDA_DRAW_PREFIX") != NULL)
            pgraph_d3d11_reset_gameplay_capture_series();
        if (getenv("MERCENARIES_CAPTURE_CAMERA_RETURN_PATH") != NULL) {
            delay_text = getenv("MERCENARIES_CAPTURE_CAMERA_RETURN_DELAY_MS");
            delay_ms = delay_text && delay_text[0] != '\0' ?
                strtoul(delay_text, NULL, 0) : 1000u;
            g_test_camera_return_capture_at_ms = now + delay_ms;
        }
        pickup_capture_armed = 2;
    }
}

void recomp_camera_mode_post_checkpoint(uint32_t setter)
{
    const uint32_t camera = 0x004140E8u;
    const uint32_t mario64 = guest_u32(camera + 0x38u);

    if (setter != 0x000976E0u ||
        getenv("MERCENARIES_TEST_RESTORE_PRE_PDA_YAW") == NULL ||
        !g_test_pre_interaction_yaw_valid ||
        mario64 < 0x00010000u || mario64 > 0x04000000u - 0x114u)
        return;

    *(float *)guest_ptr(mario64 + 0x10Cu) = g_test_pre_interaction_yaw;
    *(float *)guest_ptr(mario64 + 0x110u) = g_test_pre_interaction_yaw;
    *(float *)guest_ptr(mario64 + 0x114u) = 0.0f;
    fprintf(stderr,
            "[CAMERA-YAW-EXPERIMENT] restored pre-interaction yaw %.7g\n",
            g_test_pre_interaction_yaw);
    fflush(stderr);
    g_test_pre_interaction_yaw_valid = 0;
}
static uint32_t g_weapon_render_trace_all_budget;

void recomp_player_loadout_checkpoint(uint32_t stage, uint32_t owner,
                                      uint32_t template_hash,
                                      uint32_t value)
{
    static int enabled = -1;
    const int owner_valid = owner >= 0x00010000u &&
                            owner <= 0x04000000u - 0x814u;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_PLAYER_LOADOUT") != NULL;
    if (!enabled || !owner_valid)
        return;

    fprintf(stderr,
            "[PLAYER-LOADOUT] stage=%u owner=%08X template=%08X "
            "value=%08X count=%d equipped=%08X "
            "items=%08X/%08X/%08X/%08X/%08X\n",
            stage, owner, template_hash, value,
            (int32_t)guest_u32(owner + 0x7B8u),
            guest_u32(owner + 0x7A0u),
            guest_u32(owner + 0x7A4u), guest_u32(owner + 0x7A8u),
            guest_u32(owner + 0x7ACu), guest_u32(owner + 0x7B0u),
            guest_u32(owner + 0x7B4u));
    fflush(stderr);
}

void recomp_weapon_render_toggle_checkpoint(uint32_t stage, uint32_t owner,
                                            uint32_t enable)
{
    static int enabled = -1;
    const int owner_valid = owner >= 0x00010000u &&
                            owner <= 0x04000000u - 0x814u;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_WEAPON_ITEM") != NULL;
    if (!enabled || !owner_valid)
        return;

    g_weapon_render_trace_all_budget = 128u;
    fprintf(stderr,
            "[WEAPON-RENDER] stage=%u owner=%08X requested=%u "
            "enabled=%u count=%d equipped=%08X "
            "items=%08X/%08X/%08X/%08X/%08X\n",
            stage, owner, enable != 0u,
            (unsigned int)guest_u8(owner + 0x810u),
            (int32_t)guest_u32(owner + 0x7B8u),
            guest_u32(owner + 0x7A0u),
            guest_u32(owner + 0x7A4u), guest_u32(owner + 0x7A8u),
            guest_u32(owner + 0x7ACu), guest_u32(owner + 0x7B0u),
            guest_u32(owner + 0x7B4u));
    fflush(stderr);
}

void recomp_inventory_item_checkpoint(uint32_t stage, uint32_t owner,
                                      uint32_t item, uint32_t dock,
                                      uint32_t renderable,
                                      uint32_t matrix)
{
    static int enabled = -1;
    static uint32_t reports;
    static uint32_t last_owner;
    static uint32_t last_equipped = UINT32_MAX;
    static int32_t last_count = INT32_MIN;
    static uint32_t detail_budget;
    static uint32_t last_active = UINT32_MAX;
    static uint32_t last_model = UINT32_MAX;
    static uint32_t last_render_matrix = UINT32_MAX;
    const int owner_valid = owner >= 0x00010000u &&
                            owner <= 0x04000000u - 0x7CCu;
    const int item_valid = item >= 0x00010000u &&
                           item <= 0x04000000u - 0xF0u;
    const int renderable_valid = renderable >= 0x00010000u &&
                                 renderable <= 0x04000000u - 0x24u;
    const int matrix_valid = matrix >= 0x00010000u &&
                             matrix <= 0x04000000u - 0x40u;
    uint32_t equipped;
    int32_t count;
    uint32_t active = 0u;
    uint32_t model = 0u;
    uint32_t render_matrix = 0u;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_WEAPON_ITEM") != NULL;
    if (!enabled || !owner_valid || reports >= 2048u)
        return;

    equipped = guest_u32(owner + 0x7A0u);
    count = (int32_t)guest_u32(owner + 0x7B8u);
    if (stage == 0u) {
        if (owner == last_owner && equipped == last_equipped &&
            count == last_count)
            return;
        last_owner = owner;
        last_equipped = equipped;
        last_count = count;
        detail_budget = 32u;
    } else {
        if (item != equipped && g_weapon_render_trace_all_budget == 0u)
            return;
        if (renderable_valid) {
            active = guest_u32(renderable + 0x14u) & 1u;
            model = guest_u32(renderable + 0x1Cu);
            render_matrix = guest_u32(renderable + 0x18u);
        }
        if (stage == 2u &&
            (active != last_active || model != last_model ||
             render_matrix != last_render_matrix)) {
            last_active = active;
            last_model = model;
            last_render_matrix = render_matrix;
            if (detail_budget < 16u)
                detail_budget = 16u;
        }
        if (g_weapon_render_trace_all_budget != 0u) {
            --g_weapon_render_trace_all_budget;
        } else if (detail_budget == 0u) {
            return;
        } else {
            --detail_budget;
        }
    }

    fprintf(stderr,
            "[WEAPON-ITEM] report=%u stage=%u owner=%08X count=%d "
            "equipped=%08X item=%08X item-vtable=%08X dock=%08X "
            "renderable=%08X active=%u model=%08X render-matrix=%08X "
            "matrix=%08X matrix-pos=(%.7g,%.7g,%.7g) "
            "stored-pos=(%.7g,%.7g,%.7g)\n",
            ++reports, stage, owner, count,
            equipped, item, item_valid ? guest_u32(item) : 0u, dock,
            renderable,
            renderable_valid ? active : 0u,
            renderable_valid ? model : 0u,
            renderable_valid ? render_matrix : 0u,
            matrix,
            matrix_valid ? guest_f32(matrix + 0x30u) : 0.0f,
            matrix_valid ? guest_f32(matrix + 0x34u) : 0.0f,
            matrix_valid ? guest_f32(matrix + 0x38u) : 0.0f,
            item_valid ? guest_f32(item + 0xE0u) : 0.0f,
            item_valid ? guest_f32(item + 0xE4u) : 0.0f,
            item_valid ? guest_f32(item + 0xE8u) : 0.0f);
    fflush(stderr);
}
static uint32_t g_camera_symmetric_cast_trace_pending;

void recomp_mopp_linear_cast_checkpoint(uint32_t stage, uint32_t phantom,
                                        uint32_t self_type,
                                        uint32_t other_type,
                                        uint32_t collidable,
                                        uint32_t target,
                                        uint32_t input,
                                        uint32_t cast_collector,
                                        uint32_t start_collector)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_MOPP_LINEAR_CAST") != NULL;
    if (!enabled || (self_type != 19u && other_type != 19u) ||
        reports >= 512u)
        return;

    fprintf(stderr,
            "[MOPP-LINEAR-CAST] report=%u stage=%u phantom=%08X "
            "types=(%u,%u) collidable=%08X target=%08X input=%08X "
            "cast=%08X cast-data=(%08X,%08X,%08X,%08X,%08X,%08X) "
            "start=%08X start-data=(%08X,%08X,%08X,%08X,%08X,%08X)\n",
            ++reports, stage, phantom, self_type, other_type, collidable,
            target, input, cast_collector,
            cast_collector ? guest_u32(cast_collector + 0u) : 0u,
            cast_collector ? guest_u32(cast_collector + 4u) : 0u,
            cast_collector ? guest_u32(cast_collector + 8u) : 0u,
            cast_collector ? guest_u32(cast_collector + 12u) : 0u,
            cast_collector ? guest_u32(cast_collector + 16u) : 0u,
            cast_collector ? guest_u32(cast_collector + 20u) : 0u,
            start_collector,
            start_collector ? guest_u32(start_collector + 0u) : 0u,
            start_collector ? guest_u32(start_collector + 4u) : 0u,
            start_collector ? guest_u32(start_collector + 8u) : 0u,
            start_collector ? guest_u32(start_collector + 12u) : 0u,
            start_collector ? guest_u32(start_collector + 16u) : 0u,
            start_collector ? guest_u32(start_collector + 20u) : 0u);
    fflush(stderr);
}

void recomp_mopp_vm_hit_checkpoint(uint32_t stage, uint32_t machine,
                                   uint32_t shape_key,
                                   uint32_t allowed,
                                   uint32_t child_shape,
                                   uint32_t child_type,
                                   uint32_t target,
                                   uint32_t cast_collector,
                                   uint32_t start_collector)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_MOPP_VM_HITS") != NULL;
    if (!enabled || reports >= 1024u ||
        cast_collector < 0x00010000u ||
        cast_collector > 0x04000000u - 4u ||
        guest_u32(cast_collector) != 0x002F110Cu)
        return;

    if (stage == 31u && target == 0x001A87C0u)
        g_camera_symmetric_cast_trace_pending = 1u;

    fprintf(stderr,
            "[MOPP-VM-HIT] report=%u stage=%u machine=%08X key=%08X "
            "allowed=%u child=%08X child-type=%u target=%08X "
            "cast=%08X cast-data=(%08X,%08X,%08X,%08X) "
            "start=%08X start-data=(%08X,%08X,%08X,%08X)\n",
            ++reports, stage, machine, shape_key, allowed, child_shape,
            child_type, target, cast_collector,
            cast_collector ? guest_u32(cast_collector + 0u) : 0u,
            cast_collector ? guest_u32(cast_collector + 4u) : 0u,
            cast_collector ? guest_u32(cast_collector + 8u) : 0u,
            cast_collector ? guest_u32(cast_collector + 12u) : 0u,
            start_collector,
            start_collector ? guest_u32(start_collector + 0u) : 0u,
            start_collector ? guest_u32(start_collector + 4u) : 0u,
            start_collector ? guest_u32(start_collector + 8u) : 0u,
            start_collector ? guest_u32(start_collector + 12u) : 0u);
    fflush(stderr);
}

void recomp_havok_linear_cast_checkpoint(uint32_t stage, uint32_t body_a,
                                         uint32_t body_b, uint32_t input,
                                         uint32_t cast_collector,
                                         uint32_t start_collector,
                                         uint32_t target)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_HAVOK_LINEAR_CAST") != NULL;
    if (!enabled || !g_camera_symmetric_cast_trace_pending ||
        reports >= 256u || cast_collector < 0x00010000u ||
        cast_collector > 0x04000000u - 8u ||
        guest_u32(cast_collector) != 0x002F70FCu)
        return;

    fprintf(stderr,
            "[HAVOK-LINEAR-CAST] report=%u stage=%u body-a=%08X "
            "body-b=%08X input=%08X target=%08X cast=%08X "
            "cast-data=(%08X,%08X,%08X,%08X) start=%08X "
            "start-data=(%08X,%08X,%08X,%08X)\n",
            ++reports, stage, body_a, body_b, input, target,
            cast_collector,
            guest_u32(cast_collector + 0u),
            guest_u32(cast_collector + 4u),
            guest_u32(cast_collector + 8u),
            guest_u32(cast_collector + 12u),
            start_collector,
            start_collector ? guest_u32(start_collector + 0u) : 0u,
            start_collector ? guest_u32(start_collector + 4u) : 0u,
            start_collector ? guest_u32(start_collector + 8u) : 0u,
            start_collector ? guest_u32(start_collector + 12u) : 0u);
    fflush(stderr);
    if (stage == 41u)
        g_camera_symmetric_cast_trace_pending = 0u;
}

void recomp_havok_iterative_checkpoint(uint32_t body_a, uint32_t body_b,
                                       uint32_t input, uint32_t collector,
                                       uint32_t closest_output,
                                       uint32_t closest_target)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_HAVOK_LINEAR_CAST") != NULL;
    if (!enabled || !g_camera_symmetric_cast_trace_pending || reports >= 128u)
        return;

    fprintf(stderr,
            "[HAVOK-ITERATIVE] report=%u bodies=(%08X,%08X) input=%08X "
            "collector=%08X closest-target=%08X valid=%u distance=%.9g "
            "normal=(%.9g,%.9g,%.9g) point=(%.9g,%.9g,%.9g) "
            "path=(%.9g,%.9g,%.9g) tolerance=%.9g "
            "extra-penetration=%.9g start-tolerance=%.9g\n",
            ++reports, body_a, body_b, input, collector, closest_target,
            (unsigned int)guest_u8(closest_output + 8u),
            guest_f32(closest_output + 0x1Cu),
            guest_f32(closest_output + 0x10u),
            guest_f32(closest_output + 0x14u),
            guest_f32(closest_output + 0x18u),
            guest_f32(closest_output + 0x20u),
            guest_f32(closest_output + 0x24u),
            guest_f32(closest_output + 0x28u),
            guest_f32(input + 0x10u), guest_f32(input + 0x14u),
            guest_f32(input + 0x18u), guest_f32(input + 8u),
            guest_f32(input + 0x20u), guest_f32(input + 0x24u));
    {
        uint32_t body;
        unsigned int side;
        for (side = 0u; side < 2u; ++side) {
            unsigned int depth;
            body = side == 0u ? body_a : body_b;
            for (depth = 0u; depth < 5u &&
                              body >= 0x00010000u &&
                              body <= 0x04000000u - 0x10u;
                 ++depth) {
                const uint32_t shape = guest_u32(body + 0u);
                const uint32_t motion = guest_u32(body + 8u);
                const uint32_t parent = guest_u32(body + 0xCu);
                unsigned int type = 0xFFFFFFFFu;
                if (shape >= 0x00010000u &&
                    shape <= 0x04000000u - 8u)
                    type = guest_u8(shape + 4u);
                fprintf(stderr,
                        "[HAVOK-BODY-CHAIN] report=%u side=%c depth=%u "
                        "body=%08X shape=%08X type=%u key=%08X "
                        "motion=%08X parent=%08X\n",
                        reports, side == 0u ? 'A' : 'B', depth,
                        body, shape, type, guest_u32(body + 4u),
                        motion, parent);
                if (parent == body)
                    break;
                body = parent;
            }
        }
    }
    {
        const uint32_t shape_a = guest_u32(body_a + 0u);
        const uint32_t shape_b = guest_u32(body_b + 0u);
        const uint32_t motion_a = guest_u32(body_a + 8u);
        const uint32_t motion_b = guest_u32(body_b + 8u);
        if (shape_a >= 0x00010000u && shape_a <= 0x04000000u - 0x40u &&
            shape_b >= 0x00010000u && shape_b <= 0x04000000u - 0x40u &&
            motion_a >= 0x00010000u && motion_a <= 0x04000000u - 0x60u &&
            motion_b >= 0x00010000u && motion_b <= 0x04000000u - 0x60u) {
            fprintf(stderr,
                    "[HAVOK-CASE] report=%u shapes=(%08X,%08X) radius=(%.9g,%.9g) "
                    "box-half=(%.9g,%.9g,%.9g) "
                    "tri=((%.9g,%.9g,%.9g),(%.9g,%.9g,%.9g),(%.9g,%.9g,%.9g)) "
                    "ta=((%.9g,%.9g,%.9g),(%.9g,%.9g,%.9g),(%.9g,%.9g,%.9g),(%.9g,%.9g,%.9g)) "
                    "tb=((%.9g,%.9g,%.9g),(%.9g,%.9g,%.9g),(%.9g,%.9g,%.9g),(%.9g,%.9g,%.9g))\n",
                    reports, shape_a, shape_b,
                    guest_f32(shape_a + 0x0Cu), guest_f32(shape_b + 0x0Cu),
                    guest_f32(shape_b + 0x10u), guest_f32(shape_b + 0x14u),
                    guest_f32(shape_b + 0x18u),
                    guest_f32(shape_a + 0x10u), guest_f32(shape_a + 0x14u), guest_f32(shape_a + 0x18u),
                    guest_f32(shape_a + 0x20u), guest_f32(shape_a + 0x24u), guest_f32(shape_a + 0x28u),
                    guest_f32(shape_a + 0x30u), guest_f32(shape_a + 0x34u), guest_f32(shape_a + 0x38u),
                    guest_f32(motion_a + 0x20u), guest_f32(motion_a + 0x24u), guest_f32(motion_a + 0x28u),
                    guest_f32(motion_a + 0x30u), guest_f32(motion_a + 0x34u), guest_f32(motion_a + 0x38u),
                    guest_f32(motion_a + 0x40u), guest_f32(motion_a + 0x44u), guest_f32(motion_a + 0x48u),
                    guest_f32(motion_a + 0x50u), guest_f32(motion_a + 0x54u), guest_f32(motion_a + 0x58u),
                    guest_f32(motion_b + 0x20u), guest_f32(motion_b + 0x24u), guest_f32(motion_b + 0x28u),
                    guest_f32(motion_b + 0x30u), guest_f32(motion_b + 0x34u), guest_f32(motion_b + 0x38u),
                    guest_f32(motion_b + 0x40u), guest_f32(motion_b + 0x44u), guest_f32(motion_b + 0x48u),
                    guest_f32(motion_b + 0x50u), guest_f32(motion_b + 0x54u), guest_f32(motion_b + 0x58u));
        }
    }
    fflush(stderr);
}

void recomp_havok_box_feature_checkpoint(uint32_t result,
                                         uint32_t detector,
                                         uint32_t stack,
                                         uint32_t work,
                                         uint32_t feature,
                                         uint32_t output)
{
    static int enabled = -1;
    static uint32_t reports;
    unsigned int i;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_HAVOK_BOX_FEATURE") != NULL;
    if (!enabled || !g_camera_symmetric_cast_trace_pending || reports >= 128u)
        return;

    fprintf(stderr,
            "[HAVOK-BOX-FEATURE] report=%u result=%u detector=%08X "
            "stack=%08X work=%08X feature=%08X output=%08X "
            "selected=(%u,%u)\n",
            ++reports, result, detector, stack, work, feature, output,
            guest_u16(stack + 0x88u), guest_u16(stack + 0x8Au));
    fprintf(stderr, "[HAVOK-BOX-FEATURE-WORK] report=%u", reports);
    for (i = 0u; i < 16u; ++i)
        fprintf(stderr, " %08X", guest_u32(work + i * 4u));
    fprintf(stderr, "\n[HAVOK-BOX-FEATURE-DESC] report=%u", reports);
    for (i = 0u; i < 8u; ++i)
        fprintf(stderr, " %04X", guest_u16(feature + i * 2u));
    fprintf(stderr, "\n[HAVOK-BOX-FEATURE-OUT] report=%u", reports);
    for (i = 0u; i < 12u; ++i)
        fprintf(stderr, " %.9g", guest_f32(output + i * 4u));
    fprintf(stderr, "\n");
    fflush(stderr);
}

void recomp_mopp_long_ray_return_checkpoint(
    uint32_t machine, uint32_t hidden_result, uint32_t frame,
    uint32_t stack_after, uint32_t ebx_before, uint32_t esi_before,
    uint32_t edi_before, uint32_t ebx_after, uint32_t esi_after,
    uint32_t edi_after)
{
    static int enabled = -1;
    static uint32_t reports;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_MOPP_LONG_RAY_RETURN") != NULL;
    if (!enabled || reports >= 96u)
        return;
    ++reports;
    fprintf(stderr,
            "[MOPP-LONG-RAY-RETURN] report=%u machine=%08X hit=%u "
            "hidden=%08X/%u frame=%08X stack=%08X "
            "ebx=%08X/%08X esi=%08X/%08X edi=%08X/%08X\n",
            reports, machine, guest_u8(machine + 0x50u), hidden_result,
            guest_u8(hidden_result), frame, stack_after,
            ebx_before, ebx_after, esi_before, esi_after,
            edi_before, edi_after);
    fflush(stderr);
}

void recomp_mopp_opcode_checkpoint(uint32_t program, uint32_t opcode,
                                   uint32_t mapped_index, uint32_t target,
                                   uint32_t stack, uint32_t frame)
{
    static int enabled = -1;
    static uint64_t dispatches;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_MOPP_OPCODES") != NULL;
    if (!enabled)
        return;

    ++dispatches;
    if (dispatches <= 512u || (dispatches % 100000u) == 0u) {
        fprintf(stderr,
                "[MOPP-OPCODE] dispatch=%llu program=%08X opcode=%02X "
                "mapped=%u target=%08X esp=%08X ebp=%08X\n",
                (unsigned long long)dispatches, program, opcode & 0xFFu,
                mapped_index, target, stack, frame);
        fflush(stderr);
    }
}

static void recomp_trace_live_human_physics(ULONGLONG now)
{
    static int continuous = -1;
    static uint32_t observed_actor;
    static int observed_disabled;
    const uint32_t actor = g_recomp_live_human_actor;
    const uint32_t physics = actor + 0x280u;
    uint32_t phantom;
    uint32_t proxy;
    uint32_t overlap_array;
    uint32_t overlap_count;
    uint32_t manifold_count;
    int disable_count;
    unsigned int physics_active;

    if (continuous < 0)
        continuous =
            getenv("MERCENARIES_TRACE_HUMAN_PHYSICS_CONTINUOUS") != NULL;
    if (now < g_test_standup_trace_next_ms)
        return;
    if ((!continuous && g_test_standup_trace_until_ms == 0u) ||
        actor < 0x00010000u || actor > 0x04000000u - 0x800u)
        return;
    if (!continuous && now > g_test_standup_trace_until_ms) {
        g_test_standup_trace_until_ms = 0u;
        return;
    }

    disable_count = (int)(int8_t)guest_u8(actor + 0x6C1u);
    physics_active = (unsigned int)guest_u8(physics + 0xA5u);
    phantom = guest_u32(physics + 0x118u);
    proxy = guest_u32(physics + 0x11Cu);
    overlap_array = phantom >= 0x00010000u &&
                    phantom <= 0x04000000u - 0xC8u
                        ? guest_u32(phantom + 0xC0u) : 0u;
    overlap_count = phantom >= 0x00010000u &&
                    phantom <= 0x04000000u - 0xC8u
                        ? guest_u32(phantom + 0xC4u) : 0xFFFFFFFFu;
    manifold_count = proxy >= 0x00010000u &&
                     proxy <= 0x04000000u - 0x6Cu
                         ? guest_u32(proxy + 0x68u) : 0xFFFFFFFFu;
    if (actor != observed_actor) {
        observed_actor = actor;
        observed_disabled = 0;
    }
    if (disable_count > 0)
        observed_disabled = 1;
    else if (observed_disabled && physics_active != 0u) {
        /* EnableCollision() only restores the human's auxiliary obstacle.
         * The character is actually ready to walk once EndCannedSequence()
         * has also restored the Havok character controller. */
        xbox_InputArmTestAutoMove();
        observed_disabled = 0;
    }

    fprintf(stderr,
            "[HUMAN-PHYSICS] sample=%u actor=%08X disable=%d "
            "stored=(%.7g,%.7g,%.7g) active=%u moved=%u "
            "ray-use=%u ray-y=%.7g ray-t=%.7g dt=(%.7g,%.7g,%.7g) "
            "position=(%.7g,%.7g,%.7g) velocity=(%.7g,%.7g,%.7g) "
            "collision-v=(%.7g,%.7g,%.7g) offset=(%.7g,%.7g,%.7g) "
            "phantom=%08X overlaps=%u proxy=%08X manifold=%u\n",
            ++g_test_standup_trace_samples, actor,
            disable_count,
            guest_f32(actor + 0xE0u), guest_f32(actor + 0xE4u),
            guest_f32(actor + 0xE8u),
            physics_active,
            (unsigned int)guest_u8(physics + 0x108u),
            (unsigned int)guest_u8(physics + 0xA7u),
            guest_f32(physics + 0xA8u), guest_f32(physics + 0xACu),
            guest_f32(physics + 0xB4u), guest_f32(physics + 0xB8u),
            guest_f32(physics + 0xBCu),
            guest_f32(physics + 0xD8u), guest_f32(physics + 0xDCu),
            guest_f32(physics + 0xE0u),
            guest_f32(physics + 0xE4u), guest_f32(physics + 0xE8u),
            guest_f32(physics + 0xECu),
            guest_f32(physics + 0xF0u), guest_f32(physics + 0xF4u),
            guest_f32(physics + 0xF8u),
            guest_f32(physics + 0xFCu), guest_f32(physics + 0x100u),
            guest_f32(physics + 0x104u),
            phantom, overlap_count, proxy, manifold_count);
    if (physics_active != 0u && overlap_count <= 8u &&
        overlap_array >= 0x00010000u &&
        overlap_array <= 0x04000000u - overlap_count * 4u) {
        uint32_t i;
        for (i = 0u; i < overlap_count; ++i) {
            const uint32_t collidable = guest_u32(overlap_array + i * 4u);
            const int collidable_valid = collidable >= 0x00010000u &&
                collidable <= 0x04000000u - 0x24u;
            const uint32_t shape = collidable_valid ?
                guest_u32(collidable + 0x00u) : 0u;
            const uint32_t motion = collidable_valid ?
                guest_u32(collidable + 0x08u) : 0u;
            const uint32_t owner = collidable_valid ?
                guest_u32(collidable + 0x20u) : 0u;
            const int shape_valid = shape >= 0x00010000u &&
                shape <= 0x04000000u - 4u;
            const int motion_valid = motion >= 0x00010000u &&
                motion <= 0x04000000u - 0x60u;
            const int owner_valid = owner >= 0x00010000u &&
                owner <= 0x04000000u - 4u;
            fprintf(stderr,
                    "[HUMAN-OVERLAP] sample=%u index=%u collidable=%08X "
                    "shape=%08X shape-vtable=%08X motion=%08X "
                    "motion-pos=(%.7g,%.7g,%.7g) unique=%08X type=%u "
                    "filter=%08X owner=%08X owner-vtable=%08X\n",
                    g_test_standup_trace_samples, i, collidable,
                    shape, shape_valid ? guest_u32(shape) : 0u, motion,
                    motion_valid ? guest_f32(motion + 0x50u) : 0.0f,
                    motion_valid ? guest_f32(motion + 0x54u) : 0.0f,
                    motion_valid ? guest_f32(motion + 0x58u) : 0.0f,
                    collidable_valid ? guest_u32(collidable + 0x14u) : 0u,
                    collidable_valid ? guest_u32(collidable + 0x18u) : 0u,
                    collidable_valid ? guest_u32(collidable + 0x1Cu) : 0u,
                    owner, owner_valid ? guest_u32(owner) : 0u);
        }
    }
    fflush(stderr);
    g_test_standup_trace_next_ms = now + 250u;
}

static float recomp_axis_len2(uint32_t matrix, uint32_t row)
{
    const float x = guest_f32(matrix + row);
    const float y = guest_f32(matrix + row + 4u);
    const float z = guest_f32(matrix + row + 8u);
    return x * x + y * y + z * z;
}

static void recomp_trace_live_camera_transition(ULONGLONG now)
{
    static uint32_t previous_draws;
    static uint32_t previous_vertices;
    static uint32_t previous_clears;
    PgraphD3D11Stats render_stats;
    uint32_t draw_delta;
    uint32_t vertex_delta;
    uint32_t clear_delta;
    const uint32_t camera = 0x004140E8u;
    const uint32_t red_camera = guest_u32(camera + 0x1Cu);
    const uint32_t current = guest_u32(camera + 0x24u);
    const uint32_t mario64 = guest_u32(camera + 0x38u);
    const int red_valid = red_camera >= 0x00010000u &&
                          red_camera <= 0x04000000u - 0x100u;
    const int current_valid = current >= 0x00010000u &&
                              current <= 0x04000000u - 0x1D0u;
    const int mario_valid = mario64 >= 0x00010000u &&
                            mario64 <= 0x04000000u - 0x1D0u;
    const uint32_t current_vtable = current_valid ? guest_u32(current) : 0u;
    const int current_script = current_vtable == 0x002E70A0u;
    const uint32_t animated = current_script ? guest_u32(current + 0x74u) : 0u;
    const int animated_valid = animated >= 0x00010000u &&
                               animated <= 0x04000000u - 0xF0u;

    pgraph_d3d11_get_stats(&render_stats);
    draw_delta = render_stats.draw_calls - previous_draws;
    vertex_delta = render_stats.vertices_submitted - previous_vertices;
    clear_delta = render_stats.clears - previous_clears;
    previous_draws = render_stats.draw_calls;
    previous_vertices = render_stats.vertices_submitted;
    previous_clears = render_stats.clears;

    if (g_test_camera_trace_until_ms == 0u ||
        now < g_test_camera_trace_next_ms)
        return;
    if (now > g_test_camera_trace_until_ms) {
        g_test_camera_trace_until_ms = 0u;
        return;
    }

    /* Retail StateMario64::Enter is 0x0009CD00; its recenter flag is
     * +0x15C. Record it during the existing bounded transition window. */
    if (mario_valid) {
        fprintf(stderr,
                "[CAMERA-RECENTER] mode=%08X soft=%u auto=%u "
                "yaw=%.7g delta-yaw=%.7g tilt=%.7g delta-tilt=%.7g "
                "old-pos=(%.7g,%.7g,%.7g) free-yaw=%.7g free-in=%.7g\n",
                guest_u32(mario64 + 0x158u),
                (unsigned)guest_u8(mario64 + 0x15Cu),
                (unsigned)guest_u8(mario64 + 0x15Du),
                guest_f32(mario64 + 0x10Cu), guest_f32(mario64 + 0x114u),
                guest_f32(mario64 + 0x100u), guest_f32(mario64 + 0x108u),
                guest_f32(mario64 + 0x11Cu), guest_f32(mario64 + 0x120u),
                guest_f32(mario64 + 0x124u), guest_f32(mario64 + 0x1A4u),
                guest_f32(mario64 + 0x1A8u));
    }

    fprintf(stderr,
            "[CAMERA-FRAME-STATS] sample=%u draws=%u vertices=%u clears=%u "
            "totals=%u/%u/%u\n",
            g_test_camera_trace_samples + 1u, draw_delta, vertex_delta,
            clear_delta, render_stats.draw_calls,
            render_stats.vertices_submitted, render_stats.clears);
    fprintf(stderr,
            "[CAMERA-TRANSITION] sample=%u camera=%08X red=%08X "
            "current=%08X/%08X mario=%08X/%08X blend=%.7g/%.7g lock=%u "
            "red-pos=(%.7g,%.7g,%.7g) red-axis-len2=(%.7g,%.7g,%.7g) "
            "frustum=(near=%.7g far=%.7g width=%.7g height=%.7g) "
            "draw=(%.7g inv=%.7g sq=%.7g inv-sq=%.7g) "
            "zoom=(%.7g inv=%.7g) cull=(sin-x=%.7g sin-y=%.7g "
            "cot-x=%.7g cot-y=%.7g cos-x=%.7g tan-x=%.7g tan-y=%.7g) "
            "state-pos=(%.7g,%.7g,%.7g) state-axis-len2=(%.7g,%.7g,%.7g) "
            "mario-guid=%08X mario-pos=(%.7g,%.7g,%.7g) "
            "noise-free-pos=(%.7g,%.7g,%.7g) actor-pos=(%.7g,%.7g,%.7g) "
            "script-listener=%u anim=%08X/%08X anim-attach=%08X "
            "anim-hp=%u camera-attach=%08X/%08X "
            "anim-pos=(%.7g,%.7g,%.7g)\n",
            ++g_test_camera_trace_samples, camera, red_camera,
            current, current_vtable,
            mario64, mario_valid ? guest_u32(mario64) : 0u,
            guest_f32(camera + 0x4Cu), guest_f32(camera + 0x50u),
            (unsigned int)guest_u8(camera + 0x54u),
            red_valid ? guest_f32(red_camera + 0x40u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x44u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x48u) : 0.0f,
            red_valid ? recomp_axis_len2(red_camera + 0x10u, 0u) : 0.0f,
            red_valid ? recomp_axis_len2(red_camera + 0x10u, 0x10u) : 0.0f,
            red_valid ? recomp_axis_len2(red_camera + 0x10u, 0x20u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x90u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x94u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x98u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x9Cu) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xA0u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xA4u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xA8u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xACu) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xB0u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xB4u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xB8u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xBCu) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xC0u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xC4u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xC8u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xCCu) : 0.0f,
            red_valid ? guest_f32(red_camera + 0xD0u) : 0.0f,
            current_valid ? guest_f32(current + 0x40u) : 0.0f,
            current_valid ? guest_f32(current + 0x44u) : 0.0f,
            current_valid ? guest_f32(current + 0x48u) : 0.0f,
            current_valid ? recomp_axis_len2(current + 0x10u, 0u) : 0.0f,
            current_valid ? recomp_axis_len2(current + 0x10u, 0x10u) : 0.0f,
            current_valid ? recomp_axis_len2(current + 0x10u, 0x20u) : 0.0f,
            mario_valid ? guest_u32(mario64 + 0x60u) : 0u,
            mario_valid ? guest_f32(mario64 + 0x40u) : 0.0f,
            mario_valid ? guest_f32(mario64 + 0x44u) : 0.0f,
            mario_valid ? guest_f32(mario64 + 0x48u) : 0.0f,
            mario_valid ? guest_f32(mario64 + 0xA0u) : 0.0f,
            mario_valid ? guest_f32(mario64 + 0xA4u) : 0.0f,
            mario_valid ? guest_f32(mario64 + 0xA8u) : 0.0f,
            g_recomp_live_human_actor ? guest_f32(g_recomp_live_human_actor + 0xE0u) : 0.0f,
            g_recomp_live_human_actor ? guest_f32(g_recomp_live_human_actor + 0xE4u) : 0.0f,
            g_recomp_live_human_actor ? guest_f32(g_recomp_live_human_actor + 0xE8u) : 0.0f,
            current_script ? guest_u32(current + 0x6Cu) != 0u : 0u,
            animated, animated_valid ? guest_u32(animated) : 0u,
            current_script ? guest_u32(current + 0x78u) : 0u,
            current_script ? (unsigned int)guest_u8(current + 0x7Cu) : 0u,
            current_script ? guest_u32(current + 0xC4u) : 0u,
            current_script ? guest_u32(current + 0xC8u) : 0u,
            animated_valid ? guest_f32(animated + 0xE0u) : 0.0f,
            animated_valid ? guest_f32(animated + 0xE4u) : 0.0f,
            animated_valid ? guest_f32(animated + 0xE8u) : 0.0f);
    if (mario_valid && g_recomp_live_human_actor >= 0x00010000u &&
        g_recomp_live_human_actor <= 0x04000000u - 0xECu) {
        const uint32_t actor = g_recomp_live_human_actor;
        const float rel_x = guest_f32(mario64 + 0x40u) -
                            guest_f32(actor + 0xE0u);
        const float rel_y = guest_f32(mario64 + 0x44u) -
                            guest_f32(actor + 0xE4u);
        const float rel_z = guest_f32(mario64 + 0x48u) -
                            guest_f32(actor + 0xE8u);
        const float actor_z_x = guest_f32(actor + 0xD0u);
        const float actor_z_y = guest_f32(actor + 0xD4u);
        const float actor_z_z = guest_f32(actor + 0xD8u);
        fprintf(stderr,
                "[CAMERA-PLACEMENT] sample=%u stick=%.7g/%.7g "
                "tilt=%.7g yaw=%.7g actor-z=(%.7g,%.7g,%.7g) "
                "rel=(%.7g,%.7g,%.7g) rel-dot-actor-z=%.7g\n",
                g_test_camera_trace_samples,
                guest_f32(mario64 + 0xF8u),
                guest_f32(mario64 + 0xFCu),
                guest_f32(mario64 + 0x100u),
                guest_f32(mario64 + 0x10Cu),
                actor_z_x, actor_z_y, actor_z_z,
                rel_x, rel_y, rel_z,
                rel_x * actor_z_x + rel_y * actor_z_y + rel_z * actor_z_z);
        if (red_valid) {
            const float red_z_x = guest_f32(red_camera + 0x30u);
            const float red_z_y = guest_f32(red_camera + 0x34u);
            const float red_z_z = guest_f32(red_camera + 0x38u);
            const float red_to_actor_x = guest_f32(actor + 0xE0u) -
                                         guest_f32(red_camera + 0x40u);
            const float red_to_actor_y = guest_f32(actor + 0xE4u) -
                                         guest_f32(red_camera + 0x44u);
            const float red_to_actor_z = guest_f32(actor + 0xE8u) -
                                         guest_f32(red_camera + 0x48u);
            const float dot_z = red_z_x * red_to_actor_x +
                                red_z_y * red_to_actor_y +
                                red_z_z * red_to_actor_z;
            fprintf(stderr,
                    "[CAMERA-ORIENTATION] sample=%u "
                    "red-z=(%.7g,%.7g,%.7g) to-actor=(%.7g,%.7g,%.7g) "
                    "dot-z=%.7g dot-minus-z=%.7g\n",
                    g_test_camera_trace_samples,
                    red_z_x, red_z_y, red_z_z,
                    red_to_actor_x, red_to_actor_y, red_to_actor_z,
                    dot_z, -dot_z);
        }
    }
    fflush(stderr);
    g_test_camera_trace_next_ms = now + 125u;
}

static void recomp_service_scheduled_camera_captures(ULONGLONG now)
{
    if (g_test_interaction_capture_at_ms != 0u &&
        now >= g_test_interaction_capture_at_ms) {
        const char *path = getenv("MERCENARIES_CAPTURE_INTERACTION_PATH");
        if (path != NULL && path[0] != '\0')
            d3d8_DebugCaptureFrameToPath(path);
        g_test_interaction_capture_at_ms = 0u;
    }
    if (g_test_camera_return_capture_at_ms != 0u &&
        now >= g_test_camera_return_capture_at_ms) {
        const char *path = getenv("MERCENARIES_CAPTURE_CAMERA_RETURN_PATH");
        if (getenv("MERCENARIES_CAPTURE_POST_PDA_STEADY_DRAW_PREFIX") != NULL)
            pgraph_d3d11_reset_gameplay_capture_series();
        if (path != NULL && path[0] != '\0')
            d3d8_DebugCaptureFrameToPath(path);
        g_test_camera_return_capture_at_ms = 0u;
    }
}

/* Opt-in, bounded recorder sampled only after successful real submissions.
 * No guest writes, per-frame allocation, file I/O or trace strings. Read the
 * ring from a private process while paused using sample_vehicle_frames.py. */
typedef struct RecompVehicleFrame {
    uint64_t serial;
    int64_t qpc;
    int64_t frequency;
    uint32_t ticks, camera, owner, actor, mode;
    float dt, stick;
    uint32_t source;
    float hull[16], chase[16], rendered[16];
} RecompVehicleFrame;
static RecompVehicleFrame *g_vehicle_frame_trace;
static volatile uint32_t g_vehicle_frame_trace_count;
extern uint32_t d3d8_DebugGetSubmissionSource(void);
static void recomp_vehicle_submission_trace(uint64_t serial, int64_t qpc, int64_t frequency)
{
    uint32_t camera = guest_u32(0x0041410Cu), spore, actor, red;
    RecompVehicleFrame frame = {0};
    if (!g_vehicle_frame_trace || camera < 0x10000u || camera > 0x4000000u - 0x15Cu ||
        guest_u32(camera) != 0x002E6FD4u) return;
    frame.owner = guest_u32(camera + 0x60u);
    actor = recomp_lookup_entity_by_hash(frame.owner, &spore);
    red = guest_u32(0x00414104u);
    if (actor < 0x10000u || actor > 0x4000000u - 0xF0u ||
        red < 0x10000u || red > 0x4000000u - 0x50u) return;
    frame.source = d3d8_DebugGetSubmissionSource();
    frame.serial = serial; frame.qpc = qpc; frame.frequency = frequency;
    frame.ticks = guest_u32(0x4140CCu);
    frame.camera = camera; frame.actor = actor;
    frame.mode = guest_u32(camera + 0x158u);
    frame.dt = guest_f32(0x413FA0u); frame.stick = guest_f32(camera + 0xF8u);
    memcpy(frame.hull, guest_ptr(actor + 0xB0u), sizeof(frame.hull));
    memcpy(frame.chase, guest_ptr(camera + 0x10u), sizeof(frame.chase));
    memcpy(frame.rendered, guest_ptr(red + 0x10u), sizeof(frame.rendered));
    g_vehicle_frame_trace[g_vehicle_frame_trace_count & 4095u] = frame;
    ++g_vehicle_frame_trace_count;
}
extern void d3d8_DebugSetSubmissionCallback(void (*callback)(uint64_t, int64_t, int64_t));

static void recomp_camera_present_trace_callback(void)
{
    const ULONGLONG now = GetTickCount64();
    recomp_trace_live_camera_transition(now);
    recomp_trace_aircraft_targets(now);
    recomp_service_scheduled_camera_captures(now);
}
static void recomp_apply_test_pda_route(XBOX_INPUT_STATE *state,
                                        ULONGLONG now)
{
    static ULONGLONG arrived_ms;
    static int used;
    static int logged_walk;
    static int logged_use;
    const uint32_t actor = g_recomp_live_human_actor;
    const uint32_t pda = g_recomp_briefing_pda_actor;
    const uint32_t camera = 0x004140E8u;
    const uint32_t red_camera = guest_u32(camera + 0x1Cu);
    float dx;
    float dz;
    float distance_squared;
    float desired_length;
    float forward_x;
    float forward_z;
    float forward_length;
    float right_x;
    float right_z;
    float input_x;
    float input_y;

    if (getenv("MERCENARIES_TEST_AUTO_PDA_ROUTE") == NULL || used ||
        !g_mercenaries_standup_complete ||
        actor < 0x00010000u || actor > 0x04000000u - 0xECu ||
        pda < 0x00010000u || pda > 0x04000000u - 0xECu)
        return;

    dx = guest_f32(pda + 0xE0u) - guest_f32(actor + 0xE0u);
    dz = guest_f32(pda + 0xE8u) - guest_f32(actor + 0xE8u);
    distance_squared = dx * dx + dz * dz;
    if (distance_squared > 1.0f) {
        /*
         * Mercenaries movement is camera-relative. A fixed backward input
         * therefore walks sideways in the aircraft as the scripted camera
         * settles. Aim the diagnostic stick toward the PDA in world space.
         */
        if (red_camera < 0x00010000u ||
            red_camera > 0x04000000u - 0x4Cu)
            return;
        desired_length = sqrtf(distance_squared);
        dx /= desired_length;
        dz /= desired_length;
        forward_x = guest_f32(actor + 0xE0u) -
                    guest_f32(red_camera + 0x40u);
        forward_z = guest_f32(actor + 0xE8u) -
                    guest_f32(red_camera + 0x48u);
        forward_length = sqrtf(forward_x * forward_x +
                               forward_z * forward_z);
        if (forward_length < 0.001f)
            return;
        forward_x /= forward_length;
        forward_z /= forward_length;
        right_x = -forward_z;
        right_z = forward_x;
        input_x = dx * right_x + dz * right_z;
        input_y = dx * forward_x + dz * forward_z;
        state->Gamepad.sThumbLX = (SHORT)(input_x * 32767.0f);
        state->Gamepad.sThumbLY = (SHORT)(input_y * 32767.0f);
        if (!logged_walk) {
            fprintf(stderr,
                    "[PDA-ROUTE] camera-relative walk player=(%.7g,%.7g) "
                    "pda=(%.7g,%.7g) distance=%.7g "
                    "forward=(%.7g,%.7g) stick=(%.7g,%.7g)\n",
                    guest_f32(actor + 0xE0u), guest_f32(actor + 0xE8u),
                    guest_f32(pda + 0xE0u), guest_f32(pda + 0xE8u),
                    desired_length, forward_x, forward_z,
                    input_x, input_y);
            fflush(stderr);
            logged_walk = 1;
        }
        return;
    }

    state->Gamepad.sThumbLX = 0;
    state->Gamepad.sThumbLY = 0;
    if (arrived_ms == 0u)
        arrived_ms = now;
    if (now - arrived_ms < 300u) {
        state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
        if (!logged_use) {
            fprintf(stderr,
                    "[PDA-ROUTE] pickup Y player=(%.7g,%.7g) "
                    "pda=(%.7g,%.7g) distance=%.7g\n",
                    guest_f32(actor + 0xE0u), guest_f32(actor + 0xE8u),
                    guest_f32(pda + 0xE0u), guest_f32(pda + 0xE8u),
                    sqrtf(distance_squared));
            fflush(stderr);
            logged_use = 1;
        }
    } else if (now - arrived_ms >= 600u) {
        used = 1;
    }
}
static void recomp_arm_game_stall_watchdog(void);
static uint32_t recomp_find_player_occupied_vehicle(void);
static int g_recomp_aircraft_route_complete;
static ULONGLONG g_recomp_aircraft_route_complete_ms;
static int g_recomp_post_aircraft_gate_route_started;
static int g_recomp_allied_hq_route_arrived;
static int g_recomp_allied_hq_from_checkpoint;
static ULONGLONG g_recomp_allied_hq_route_arrived_ms;
static void recomp_complete_test_aircraft_route(ULONGLONG now,
                                                unsigned int stage)
{
    const char *capture_path =
        getenv("MERCENARIES_CAPTURE_AIRCRAFT_ROUTE_PATH");
    const char *post_capture_path =
        getenv("MERCENARIES_CAPTURE_POST_AIRCRAFT_PATH");
    fprintf(stderr,
            "[AIRCRAFT-ROUTE] pickup, door, and Humvee interactions completed "
            "stage=%u vehicle=%08X\n",
            stage, g_recomp_live_vehicle_actor);
    g_recomp_aircraft_route_complete = 1;
    g_recomp_aircraft_route_complete_ms = now;
    if (getenv("MERCENARIES_CAPTURE_POST_AIRCRAFT_DRAW_PREFIX") != NULL)
        pgraph_d3d11_reset_gameplay_capture_series();
    if (getenv("MERCENARIES_TRACE_HAVOK_STACK_WATCH") != NULL) {
        const uint32_t memory = guest_u32(0x004409ACu);
        recomp_arm_havok_stack_watchpoint(memory);
    }
    fflush(stderr);
    if (capture_path && capture_path[0] != '\0')
        d3d8_DebugCaptureFrameToPath(capture_path);
    if (post_capture_path && post_capture_path[0] != '\0') {
        const char *delay_text = getenv(
            "MERCENARIES_CAPTURE_POST_AIRCRAFT_DELAY_MS");
        const ULONGLONG delay_ms =
            delay_text && delay_text[0] != '\0' ?
            strtoull(delay_text, NULL, 0) : 45000u;
        g_test_post_aircraft_capture_deadline_ms = now + delay_ms;
    }
}

static void recomp_apply_test_aircraft_pickup_route(XBOX_INPUT_STATE *state,
                                                     ULONGLONG now)
{
    static unsigned int stage;
    static ULONGLONG arrived_ms;
    static ULONGLONG cooldown_until_ms;
    static int logged_walk;
    static int logged_use;
    static int logged_rifle_fire;
    static int humvee_transition_capture_started;
    static int aborted;
    const uint32_t targets[] = {
        g_recomp_briefing_pda_actor,
        g_recomp_briefing_grenades_actor,
        g_recomp_briefing_m4_actor,
        g_recomp_briefing_door_actor,
        g_recomp_briefing_usehumvee_actor
    };
    const char *names[] = {
        "pda", "grenades", "m4", "opencargodoor", "usehumvee"
    };
    const uint32_t actor = g_recomp_live_human_actor;
    const uint32_t camera = 0x004140E8u;
    const uint32_t red_camera = guest_u32(camera + 0x1Cu);
    const float arrival_radius_squared = stage == 4u ? 0.81f : 1.0f;
    uint32_t target;
    float dx, dz, distance_squared;
    float desired_length, forward_x, forward_z, forward_length;
    float right_x, right_z, input_x, input_y;

    if (getenv("MERCENARIES_TEST_AUTO_AIRCRAFT_PICKUP_ROUTE") == NULL ||
        !g_mercenaries_standup_complete || stage >= 5u || aborted)
        return;

    /* Do not infer that the final Y press succeeded from elapsed time.  The
     * cargo-door conversation gates the Humvee hotspot, and a slow/missing
     * voice can leave that hotspot disabled longer than the old 12-second
     * retry window.  Only the retail player-vehicle ownership handoff proves
     * the interaction was accepted. */
    if (stage == 4u) {
        const uint32_t occupied_vehicle =
            recomp_find_player_occupied_vehicle();
        if (occupied_vehicle >= 0x00010000u &&
            occupied_vehicle <= 0x04000000u - 0x1100u) {
            g_recomp_live_vehicle_actor = occupied_vehicle;
            stage = 5u;
            recomp_complete_test_aircraft_route(now, stage);
            return;
        }
    }
    if (actor < 0x00010000u || actor > 0x04000000u - 0xECu)
        return;

    if (cooldown_until_ms != 0u) {
        if (now < cooldown_until_ms) {
            if (stage == 2u &&
                getenv("MERCENARIES_TEST_AUTO_FIRE_AFTER_AIRCRAFT_RIFLE") != NULL &&
                cooldown_until_ms - now <= 3000u) {
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 255;
                if (!logged_rifle_fire) {
                    fprintf(stderr,
                            "[AIRCRAFT-ROUTE] post-rifle firing probe started\n");
                    fflush(stderr);
                    logged_rifle_fire = 1;
                }
            }
            return;
        }
        ++stage;
        if (getenv("MERCENARIES_TRACE_APU_ROUTE_STAGES") != NULL) {
            fprintf(stderr, "[AIRCRAFT-ROUTE] APU snapshot after stage %u\n",
                    stage);
            mcpx_apu_debug_dump_stream_voices();
        }
        cooldown_until_ms = 0u;
        arrived_ms = 0u;
        logged_walk = 0;
        logged_use = 0;
        if (stage >= 5u) {
            recomp_complete_test_aircraft_route(now, stage);
            return;
        }
    }

    target = targets[stage];
    if (target < 0x00010000u || target > 0x04000000u - 0xECu)
        return;

    if (arrived_ms != 0u) {
        const ULONGLONG elapsed = now - arrived_ms;
        if (stage < 3u) {
            if (elapsed < 300u)
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
            else if (elapsed >= 600u)
                cooldown_until_ms = now + 4500u;
        } else if (stage == 3u) {
            /* The cargo-door and Humvee hotspots are enabled by Lua after
             * the preceding authored animation/callback.  Arrival can happen
             * before that state change, so retry a normal short Y pulse while
             * remaining at the hotspot instead of assuming the first pulse
             * was accepted. */
            if ((elapsed % 1500u) < 300u)
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
            if (elapsed >= 12000u)
                cooldown_until_ms = now + 8000u;
        } else {
            /* Entering the Humvee is a state transition, not a timed pickup.
             * Retry Y until the occupied-vehicle tracker confirms success.
             * A bounded 90-second abort keeps an unattended bad route from
             * running forever without manufacturing a false completion. */
            if (!humvee_transition_capture_started) {
                const char *transition_prefix = getenv(
                    "MERCENARIES_CAPTURE_HUMVEE_TRANSITION_PREFIX");
                if (transition_prefix != NULL && transition_prefix[0] != '\0')
                    d3d8_DebugStartDisplayCapture(
                        transition_prefix, 100u, 256u);
                humvee_transition_capture_started = 1;
            }
            if ((elapsed % 1500u) < 300u)
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
            if (elapsed >= 90000u) {
                const char *capture_path =
                    getenv("MERCENARIES_CAPTURE_AIRCRAFT_ROUTE_ABORT_PATH");
                fprintf(stderr,
                        "[AIRCRAFT-ROUTE-ABORT] stage=4 reason=Humvee "
                        "handoff-timeout player=%08X hotspot=%08X "
                        "vehicle=%08X\n",
                        actor, target, g_recomp_live_vehicle_actor);
                fflush(stderr);
                if (capture_path != NULL && capture_path[0] != '\0')
                    d3d8_DebugCaptureFrameToPath(capture_path);
                aborted = 1;
            }
        }
        return;
    }

    dx = guest_f32(target + 0xE0u) - guest_f32(actor + 0xE0u);
    dz = guest_f32(target + 0xE8u) - guest_f32(actor + 0xE8u);
    distance_squared = dx * dx + dz * dz;
    /* c17_briefing.lua calls Actor_EnableScriptedUse without a distance, and
     * RsLuaState.cpp documents that default as exactly one metre.  Keep the
     * test inside that real radius (with a small margin) so a Y pulse can be
     * accepted; stopping two or three metres away can never enter the car. */
    if (distance_squared > arrival_radius_squared) {
        if (red_camera < 0x00010000u ||
            red_camera > 0x04000000u - 0x4Cu)
            return;
        desired_length = sqrtf(distance_squared);
        dx /= desired_length;
        dz /= desired_length;
        forward_x = guest_f32(actor + 0xE0u) -
                    guest_f32(red_camera + 0x40u);
        forward_z = guest_f32(actor + 0xE8u) -
                    guest_f32(red_camera + 0x48u);
        forward_length = sqrtf(forward_x * forward_x +
                               forward_z * forward_z);
        if (forward_length < 0.001f)
            return;
        forward_x /= forward_length;
        forward_z /= forward_length;
        right_x = -forward_z;
        right_z = forward_x;
        input_x = dx * right_x + dz * right_z;
        input_y = dx * forward_x + dz * forward_z;
        state->Gamepad.sThumbLX = (SHORT)(input_x * 32767.0f);
        state->Gamepad.sThumbLY = (SHORT)(input_y * 32767.0f);
        if (!logged_walk) {
            fprintf(stderr,
                    "[AIRCRAFT-ROUTE] stage=%u target=%s walk "
                    "player=(%.7g,%.7g) target=(%.7g,%.7g) distance=%.7g "
                    "camera=(%.7g,%.7g) forward=(%.7g,%.7g) "
                    "desired=(%.7g,%.7g) stick=(%.7g,%.7g)\n",
                    stage, names[stage],
                    guest_f32(actor + 0xE0u), guest_f32(actor + 0xE8u),
                    guest_f32(target + 0xE0u), guest_f32(target + 0xE8u),
                    desired_length,
                    guest_f32(red_camera + 0x40u),
                    guest_f32(red_camera + 0x48u),
                    forward_x, forward_z, dx, dz, input_x, input_y);
            fflush(stderr);
            logged_walk = 1;
        }
        return;
    }

    state->Gamepad.sThumbLX = 0;
    state->Gamepad.sThumbLY = 0;
    arrived_ms = now;
    state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
    if (!logged_use) {
        const char *cloud_gate_path = stage == 3u ?
            getenv("MERCENARIES_CAPTURE_CLOUD_FLIGHT_GATE_PATH") : NULL;
        fprintf(stderr,
                "[AIRCRAFT-ROUTE] stage=%u target=%s pickup Y "
                "player=(%.7g,%.7g) distance=%.7g\n",
                stage, names[stage],
                guest_f32(actor + 0xE0u), guest_f32(actor + 0xE8u),
                sqrtf(distance_squared));
        fflush(stderr);
        /* Diagnostic-only gate at the first normal cargo-door Y pulse. The
         * authored exterior CloudFlight shot starts from this interaction;
         * creating a real frame capture here lets renderer observers arm at
         * the affected interval without changing guest state or timing. */
        if (cloud_gate_path != NULL && cloud_gate_path[0] != '\0')
            d3d8_DebugCaptureFrameToPath(cloud_gate_path);
        logged_use = 1;
    }
}

static void recomp_trace_roadblock_actor_census(void)
{
    static int completed;
    unsigned int base_humans = 0u;
    unsigned int general_humans = 0u;
    unsigned int player_humans = 0u;

    if (completed || getenv("MERCENARIES_TRACE_ROADBLOCK_ACTORS") == NULL)
        return;
    completed = 1;

    /* Retail class identities, established from constructor vtable stores:
     * 0x2E2818 RsActorVehicleHuman, 0x2E2CE0 HumanGeneral, and
     * 0x2E32B8 HumanPlayer. Actor allocations in this title are 16-byte
     * aligned. Scan once at encounter entry so a radar-only contact can be
     * separated from a spawned-but-culled human without affecting gameplay. */
    for (uint32_t actor = 0x00800000u;
         actor <= 0x03FFF000u - 0x954u; actor += 16u) {
        const uint32_t vtable = guest_u32(actor);
        float x;
        float y;
        float z;
        if (vtable != 0x002E2818u && vtable != 0x002E2CE0u &&
            vtable != 0x002E32B8u)
            continue;
        x = guest_f32(actor + 0xE0u);
        y = guest_f32(actor + 0xE4u);
        z = guest_f32(actor + 0xE8u);
        if (!isfinite(x) || !isfinite(y) || !isfinite(z) ||
            fabsf(x - 1595.0f) > 400.0f || fabsf(z - 1510.0f) > 400.0f)
            continue;
        if (vtable == 0x002E2818u)
            ++base_humans;
        else if (vtable == 0x002E2CE0u)
            ++general_humans;
        else
            ++player_humans;
        fprintf(stderr,
                "[ROADBLOCK-ACTOR] actor=%08X vtable=%08X "
                "pos=(%.7g,%.7g,%.7g) camera-vis=%.7g invisible=%u "
                "render-vtable=%08X render-flags=%08X model=%08X "
                "weapon-flags=%08X weapon-model=%08X\n",
                actor, vtable, x, y, z,
                guest_f32(actor + 0x154u),
                vtable == 0x002E2CE0u ?
                    (unsigned int)guest_u8(actor + 0x950u) : 0u,
                guest_u32(actor + 0x3Cu), guest_u32(actor + 0x50u),
                guest_u32(actor + 0x58u), guest_u32(actor + 0x8D4u),
                guest_u32(actor + 0x8DCu));
    }
    fprintf(stderr,
            "[ROADBLOCK-ACTOR] census base=%u general=%u player=%u\n",
            base_humans, general_humans, player_humans);
    fflush(stderr);
}

static void recomp_trace_reward_vehicle_census(float px, float pz)
{
    static int completed;
    const float authored_x = 1483.828f;
    const float authored_z = 1173.829f;

    if (completed || getenv("MERCENARIES_TRACE_VEHICLE_REWARD") == NULL)
        return;
    if ((px - authored_x) * (px - authored_x) +
        (pz - authored_z) * (pz - authored_z) > 35.0f * 35.0f)
        return;
    completed = 1;

    /* SWN.wld places jeep9 (nk_veh_jeepmgun, CashValue $2,000) here.
     * Inventory nearby live actor allocations without mutating them so the
     * subsequent RPG test can target the retail object and observe the
     * original culprit -> CashValue -> SetMoney path end to end. */
    for (uint32_t actor = 0x00010000u;
         actor <= 0x04000000u - 0xECu; actor += 0x10u) {
        const uint32_t vtable = guest_u32(actor);
        float x;
        float y;
        float z;
        float dx;
        float dz;

        if (vtable < 0x002D0000u || vtable > 0x002F0000u)
            continue;
        x = guest_f32(actor + 0xE0u);
        y = guest_f32(actor + 0xE4u);
        z = guest_f32(actor + 0xE8u);
        if (!isfinite(x) || !isfinite(y) || !isfinite(z))
            continue;
        dx = x - authored_x;
        dz = z - authored_z;
        if (dx * dx + dz * dz > 30.0f * 30.0f)
            continue;
        fprintf(stderr,
                "[REWARD-VEHICLE-CENSUS] actor=%08X vtable=%08X "
                "pos=(%.7g,%.7g,%.7g) hp=%.7g faction=%08X "
                "spore=%08X render=%08X model=%08X\n",
                actor, vtable, x, y, z, guest_f32(actor + 0x98u),
                guest_u32(actor + 0xF4u), guest_u32(actor + 0x38u),
                guest_u32(actor + 0x3Cu), guest_u32(actor + 0x58u));
    }
    fflush(stderr);
}

static uint32_t recomp_find_live_player_human(void)
{
    static uint32_t cached_player;

    /* HumanGeneral and HumanPlayer share enough layout that accepting the
     * most recently observed human can silently steer an enemy. Keep this
     * test-only route tied to the retail HumanPlayer class identity. */
    if (cached_player >= 0x00800000u &&
        cached_player <= 0x03FFF000u - 0x954u &&
        guest_u32(cached_player) == 0x002E32B8u)
        return cached_player;

    cached_player = 0u;
    for (uint32_t actor = 0x00800000u;
         actor <= 0x03FFF000u - 0x954u; actor += 16u) {
        if (guest_u32(actor) == 0x002E32B8u) {
            cached_player = actor;
            break;
        }
    }
    return cached_player;
}

static uint32_t recomp_find_player_occupied_vehicle(void)
{
    const uint32_t player_human_from_ai =
        (g_recomp_live_player_ai >= 0x00010000u &&
         g_recomp_live_player_ai <= 0x04000000u - 0x99Cu) ?
            guest_u32(g_recomp_live_player_ai + 0x998u) : 0u;
    const uint32_t human =
        (player_human_from_ai >= 0x00010000u &&
         player_human_from_ai <= 0x04000000u - 0x76Cu &&
         guest_u32(player_human_from_ai) == 0x002E32B8u) ?
            player_human_from_ai : recomp_find_live_player_human();
    uint32_t seat;
    uint32_t manager;
    uint32_t vehicle;

    if (human < 0x00010000u || human > 0x04000000u - 0x76Cu)
        return 0u;
    seat = guest_u32(human + 0x768u);
    if (seat < 0x00010000u || seat > 0x04000000u - 4u)
        return 0u;

    /* 
     first Seat field and _pVehicleOwner as the first RiderManager field. */
    manager = guest_u32(seat);
    if (manager < 0x00010000u || manager > 0x04000000u - 4u)
        return 0u;
    vehicle = guest_u32(manager);
    if (vehicle < 0x00010000u || vehicle > 0x03FFE000u)
        return 0u;
    return vehicle;
}

static int recomp_apply_test_roadblock_on_foot(XBOX_INPUT_STATE *state,
                                                ULONGLONG now,
                                                uint32_t vehicle)
{
    static unsigned int phase;
    static unsigned int walk_stage;
    static ULONGLONG phase_started_ms;
    static ULONGLONG last_report_ms;
    static ULONGLONG last_moved_ms;
    static ULONGLONG last_vehicle_sample_ms;
    static ULONGLONG stationary_since_ms;
    static ULONGLONG bypass_started_ms;
    static int captured_exit;
    static int captured_bypass;
    static int fire_probe_logged;
    static int fire_probe_completed_logged;
    static float last_hx;
    static float last_hz;
    static float last_vehicle_x;
    static float last_vehicle_z;
    const float walk_targets[][2] = {
        /* The roadblock encounter occupies the road around editor Z -1512.
         * Back away from the Humvee, skirt its east edge on foot, then return
         * to the authored northbound road centreline. Runtime Z is sign-flipped. */
        {1608.000f, 1530.000f},
        {1622.000f, 1510.000f},
        {1622.000f, 1475.000f},
        {1591.465f, 1462.935f}
    };
    const uint32_t player_human_from_ai =
        (g_recomp_live_player_ai >= 0x00010000u &&
         g_recomp_live_player_ai <= 0x04000000u - 0x99Cu) ?
            guest_u32(g_recomp_live_player_ai + 0x998u) : 0u;
    const uint32_t human =
        (player_human_from_ai >= 0x00010000u &&
         player_human_from_ai <= 0x04000000u - 0xECu &&
         guest_u32(player_human_from_ai) == 0x002E32B8u) ?
            player_human_from_ai : recomp_find_live_player_human();
    const uint32_t camera = 0x004140E8u;
    const uint32_t red_camera = guest_u32(camera + 0x1Cu);
    float vx, vz, hx, hz, dx, dz, distance;
    float forward_x, forward_z, forward_length, right_x, right_z;

    if (getenv("MERCENARIES_TEST_AUTO_ROADBLOCK_ON_FOOT") == NULL ||
        vehicle < 0x00010000u || vehicle > 0x04000000u - 0xECu)
        return 0;

    vx = guest_f32(vehicle + 0xE0u);
    vz = guest_f32(vehicle + 0xE8u);
    if (!isfinite(vx) || !isfinite(vz))
        return 0;

    if (phase == 0u) {
        dx = 1595.198f - vx;
        dz = 1491.332f - vz;
        if (dx * dx + dz * dz > 100.0f * 100.0f)
            return 0;
        phase = 1u;
        phase_started_ms = now;
        last_vehicle_sample_ms = now;
        last_vehicle_x = vx;
        last_vehicle_z = vz;
        fprintf(stderr,
                "[ROADBLOCK-ON-FOOT] reached authored obstruction "
                "vehicle=%08X pos=(%.7g,%.7g); braking before exit\n",
                vehicle, vx, vz);
        fflush(stderr);
    }

    state->Gamepad.sThumbLX = 0;
    state->Gamepad.sThumbLY = 0;
    if (phase == 1u) {
        state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
        if (now - last_vehicle_sample_ms >= 100u) {
            const float move_x = vx - last_vehicle_x;
            const float move_z = vz - last_vehicle_z;
            const float movement_squared = move_x * move_x + move_z * move_z;
            last_vehicle_sample_ms = now;
            last_vehicle_x = vx;
            last_vehicle_z = vz;
            if (movement_squared <= 0.01f) {
                if (stationary_since_ms == 0u)
                    stationary_since_ms = now;
                /* X is an analogue brake which becomes reverse once the car
                 * has stopped.  Transition immediately on the first low-speed
                 * sample instead of holding it long enough to reverse. */
                phase = 2u;
                phase_started_ms = now;
                fprintf(stderr,
                        "[ROADBLOCK-ON-FOOT] vehicle stopped at "
                        "(%.7g,%.7g); brake released, exit Y pulse started\n",
                        vx, vz);
                fflush(stderr);
                return 1;
            } else {
                stationary_since_ms = 0u;
            }
        }
        if (now - phase_started_ms >= 12000u) {
            phase = 2u;
            phase_started_ms = now;
            fprintf(stderr,
                    "[ROADBLOCK-ON-FOOT] vehicle stopped at "
                    "(%.7g,%.7g); exit Y pulse started\n", vx, vz);
            fflush(stderr);
        } else if (now - last_report_ms >= 1000u) {
            fprintf(stderr,
                    "[ROADBLOCK-ON-FOOT] braking vehicle=%08X "
                    "pos=(%.7g,%.7g) stationary-ms=%llu\n",
                    vehicle, vx, vz,
                    stationary_since_ms == 0u ? 0ull :
                    (unsigned long long)(now - stationary_since_ms));
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }
    if (phase == 2u) {
        const uint32_t rider_seat =
            human >= 0x00010000u && human <= 0x04000000u - 0x76Cu
                ? guest_u32(human + 0x768u) : 0u;
        /* Do not hold X here: after stopping it is reverse, not a parking
         * brake.  The Y edge must be observed while the vehicle remains below
         * the retail dismount speed limit. */
        if (now - phase_started_ms < 300u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
        if (now - last_report_ms >= 500u) {
            fprintf(stderr,
                    "[ROADBLOCK-ON-FOOT] exit-wait human=%08X seat=%08X "
                    "rider-state=%u vehicle-pos=(%.7g,%.7g)\n",
                    human, rider_seat,
                    rider_seat >= 0x00010000u &&
                            rider_seat <= 0x04000000u - 0x11Cu
                        ? guest_u32(rider_seat + 0x118u) : 0u,
                    vx, vz);
            fflush(stderr);
            last_report_ms = now;
        }
        if (now - phase_started_ms >= 3000u) {
            phase = 3u;
            phase_started_ms = now;
        }
        return 1;
    }

    if (human < 0x00010000u || human > 0x04000000u - 0xECu ||
        red_camera < 0x00010000u || red_camera > 0x04000000u - 0x4Cu) {
        if (now - last_report_ms >= 1000u) {
            fprintf(stderr,
                    "[ROADBLOCK-ON-FOOT] waiting for human/camera "
                    "human=%08X red-camera=%08X\n", human, red_camera);
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }

    hx = guest_f32(human + 0xE0u);
    hz = guest_f32(human + 0xE8u);
    if (!isfinite(hx) || !isfinite(hz))
        return 1;
    if (last_moved_ms == 0u) {
        last_hx = hx;
        last_hz = hz;
        last_moved_ms = now;
    } else if ((hx - last_hx) * (hx - last_hx) +
               (hz - last_hz) * (hz - last_hz) > 0.25f) {
        last_hx = hx;
        last_hz = hz;
        last_moved_ms = now;
    } else if (now - last_moved_ms >= 6000u &&
               walk_stage + 1u <
                   sizeof(walk_targets) / sizeof(walk_targets[0])) {
        fprintf(stderr,
                "[ROADBLOCK-ON-FOOT] stuck at walk-stage=%u "
                "pos=(%.7g,%.7g); selecting alternate waypoint\n",
                walk_stage, hx, hz);
        fflush(stderr);
        ++walk_stage;
        last_moved_ms = now;
    }
    if (!captured_exit && now - phase_started_ms >= 500u) {
        const char *capture_path =
            getenv("MERCENARIES_CAPTURE_ROADBLOCK_EXIT_PATH");
        fprintf(stderr,
                "[ROADBLOCK-ON-FOOT] control check human=%08X "
                "human-pos=(%.7g,%.7g) vehicle-pos=(%.7g,%.7g)\n",
                human, hx, hz, vx, vz);
        fflush(stderr);
        if (capture_path != NULL && capture_path[0] != '\0')
            d3d8_DebugCaptureFrameToPath(capture_path);
        captured_exit = 1;
    }

    if (walk_stage >= sizeof(walk_targets) / sizeof(walk_targets[0])) {
        if (!captured_bypass) {
            const char *capture_path =
                getenv("MERCENARIES_CAPTURE_ROADBLOCK_BYPASS_PATH");
            fprintf(stderr,
                    "[ROADBLOCK-ON-FOOT] authored roadblock bypass complete "
                    "human=%08X pos=(%.7g,%.7g)\n", human, hx, hz);
            fflush(stderr);
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            captured_bypass = 1;
            bypass_started_ms = now;
        }
        if (getenv("MERCENARIES_TEST_AUTO_FIRE_AFTER_ROADBLOCK") != NULL &&
            bypass_started_ms != 0u) {
            const ULONGLONG probe_elapsed_ms = now - bypass_started_ms;

            if (probe_elapsed_ms < 4000u) {
                if (probe_elapsed_ms >= 1000u) {
                    state->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 255;
                    if (!fire_probe_logged) {
                        fprintf(stderr,
                                "[ROADBLOCK-ON-FOOT] firing probe started "
                                "human=%08X pos=(%.7g,%.7g)\n", human, hx, hz);
                        fflush(stderr);
                        fire_probe_logged = 1;
                    }
                }
                return 1;
            }
            if (!fire_probe_completed_logged) {
                fprintf(stderr,
                        "[ROADBLOCK-ON-FOOT] firing probe complete; "
                        "releasing control to vehicle route\n");
                fflush(stderr);
                fire_probe_completed_logged = 1;
            }
        }
        /* This helper is an authored-obstruction detour, not an alternate
         * route driver.  Once the detour (and optional finite firing probe)
         * completes, relinquish input so the normal HQ route can resume. */
        return 0;
    }

    dx = walk_targets[walk_stage][0] - hx;
    dz = walk_targets[walk_stage][1] - hz;
    distance = sqrtf(dx * dx + dz * dz);
    if (distance < 2.5f) {
        fprintf(stderr,
                "[ROADBLOCK-ON-FOOT] reached walk-stage=%u "
                "human=(%.7g,%.7g) target=(%.7g,%.7g)\n",
                walk_stage, hx, hz, walk_targets[walk_stage][0],
                walk_targets[walk_stage][1]);
        fflush(stderr);
        ++walk_stage;
        return 1;
    }

    dx /= distance;
    dz /= distance;
    forward_x = hx - guest_f32(red_camera + 0x40u);
    forward_z = hz - guest_f32(red_camera + 0x48u);
    forward_length = sqrtf(forward_x * forward_x + forward_z * forward_z);
    if (!isfinite(forward_length) || forward_length < 0.001f)
        return 1;
    forward_x /= forward_length;
    forward_z /= forward_length;
    right_x = -forward_z;
    right_z = forward_x;
    state->Gamepad.sThumbLX =
        (SHORT)((dx * right_x + dz * right_z) * 32767.0f);
    state->Gamepad.sThumbLY =
        (SHORT)((dx * forward_x + dz * forward_z) * 32767.0f);
    if (now - last_report_ms >= 1000u) {
        fprintf(stderr,
                "[ROADBLOCK-ON-FOOT] walk-stage=%u human=(%.7g,%.7g) "
                "target=(%.7g,%.7g) distance=%.7g vehicle=(%.7g,%.7g) "
                "camera=(%.7g,%.7g) stick=(%d,%d)\n",
                walk_stage, hx, hz, walk_targets[walk_stage][0],
                walk_targets[walk_stage][1], distance, vx, vz,
                guest_f32(red_camera + 0x40u),
                guest_f32(red_camera + 0x48u),
                (int)state->Gamepad.sThumbLX,
                (int)state->Gamepad.sThumbLY);
        fflush(stderr);
        last_report_ms = now;
    }
    return 1;
}

static int recomp_apply_test_post_aircraft_gate_route(
    XBOX_INPUT_STATE *state, ULONGLONG now)
{
    static unsigned int stage;
    static ULONGLONG wait_until_ms;
    static ULONGLONG last_report_ms;
    static ULONGLONG last_invalid_report_ms;
    static ULONGLONG last_motion_ms;
    static ULONGLONG recovery_until_ms;
    static ULONGLONG recovery_reverse_until_ms;
    static ULONGLONG route_started_ms;
    static unsigned int recovery_count;
    static int recovery_capture_done;
    static float last_motion_x;
    static float last_motion_z;
    static float best_waypoint_distance;
    static unsigned int progress_stage = ~0u;
    static int started;
    static int completed;
    const float targets[][2] = {
        /* Runtime Z is the sign-flipped editor Z coordinate. Keep throttle
         * through loc_playerstart so the Humvee carries enough momentum to
         * break the low stone barrier. Only then pause for dmz_skgate. */
        {1548.554f, 1779.954f},
        {1548.554f, 1758.000f},
        {1555.000f, 1740.000f},
        {1563.478f, 1721.435f},
        /*
         * SW_poi_dmz_traffic.PTH path t_dmz-south_02a. Runtime Z is the
         * sign-flipped editor Z. This crosses allies0_battle1-entry, the
         * roadblock, allies0_cliff-battle-entry, and the Blackhawk area. */
        {1584.410f, 1716.458f},
        {1598.011f, 1697.908f},
        {1605.685f, 1682.274f},
        {1609.340f, 1666.298f},
        {1602.617f, 1633.424f},
        {1592.321f, 1599.297f},
        {1590.505f, 1586.130f},
        {1589.488f, 1574.308f},
        {1592.373f, 1548.214f},
        {1594.624f, 1528.525f},
        /* Run 442 measured the direct roadblock ram deflecting the Humvee
         * west and wedging it at (1582,1494). Run 476 exposed why the first
         * east-side bend was still unreliable: SWN_allies0_enc_roadblock.lyr
         * fills X=1575..1613/Z=1491..1542 with two jeeps, sandbags, barrels,
         * crates and spotlights, so the diagonal toward (1618,1520) crossed
         * the encounter instead of going around it. Keep the number of route
         * stages stable for the later HQ fence logic, but stay south at
         * runtime Z=1560, clear the authored east edge at X=1630, and only
         * then turn north past the encounter. */
        {1630.000f, 1560.000f},
        {1630.000f, 1510.000f},
        {1630.000f, 1463.000f},
        {1590.101f, 1450.495f},
        {1586.670f, 1427.974f},
        {1584.096f, 1406.873f},
        {1582.384f, 1387.119f},
        {1580.777f, 1374.376f},
        {1575.676f, 1357.388f},
        {1569.068f, 1340.120f},
        {1562.449f, 1326.501f},
        {1538.370f, 1296.641f},
        {1517.252f, 1272.955f},
        {1507.968f, 1261.153f},
        {1501.388f, 1246.865f},
        {1498.642f, 1233.921f},
        {1498.223f, 1220.721f},
        /* Exact continuation from t_dmz-south_01a. */
        {1497.005f, 1191.330f},
        {1493.916f, 1170.257f},
        {1491.651f, 1157.350f},
        {1491.141f, 1148.401f},
        /* SWN.wld places the 4,500-mass destructible global_roadblock01 at
         * editor (1442.590,-1122.390).  Runs 464/466 showed that selecting the
         * exact road Z directly from the southbound approach could leave the
         * Humvee still facing south when the westbound ram began.  Use two
         * authored-space cornering points before the exact centreline so the
         * final 45 metres are a stable, straight acceleration run. */
        {1490.000f, 1138.000f},
        {1485.000f, 1128.000f},
        /* Follow every centreline X node at the checkpoint instead of aiming
         * diagonally from t_dmz-south_01a to the far side. 
         * t_dmz-hq_03a supplies the progression; SWN.wld supplies the exact
         * runtime-Z collision corridor through global_roadblock01. The successful retail
         * recomp capture at 94-100 seconds confirms that the intended route
         * hits the crate/barrel barricade squarely, destroys it in one impact,
         * and continues straight.  Runs 457/458 approached obliquely, climbed
         * the debris or hillside, and repeatedly damaged the Humvee. */
        {1476.143f, 1122.390f},
        {1454.708f, 1122.390f},
        {1438.000f, 1122.390f},
        /* The 900-HP global_roadblock01 ends before two separate 300-HP
         * dmz_fence sections centred near X=1426.  dmz_fence.msh's authored bounds
         * and Run 467 proved the old
         * X=1431 handoff was already too far west: the Humvee cleared the
         * roadblock, then stopped about five metres east of the first fence.
         * End the ram at X=1438, north-turn while still east of the fence,
         * then cross west at runtime Z=1135.
         * fence's projected north edge at Z=1126.116, leaving nearly nine
         * metres of clearance; the following authored bend then passes north of the fence
         * without changing guest collision. */
        {1438.000f, 1135.000f},
        {1415.000f, 1135.000f},
        {1392.827f, 1106.409f},
        {1374.967f, 1098.334f},
        {1356.013f, 1088.082f},
        {1335.902f, 1075.464f},
        {1320.774f, 1065.528f},
        {1309.479f, 1056.193f},
        {1300.313f, 1047.593f},
        {1292.405f, 1038.062f},
        {1286.198f, 1023.643f},
        {1283.690f, 1011.759f},
        {1282.474f, 992.833f},
        {1285.467f, 976.480f},
        {1292.421f, 963.969f},
        {1299.264f, 950.206f},
        {1308.522f, 934.533f},
        {1319.008f, 924.504f},
        {1346.924f, 909.419f},
        {1361.132f, 899.202f},
        {1375.652f, 881.614f},
        {1379.718f, 870.151f},
        {1381.988f, 852.346f},
        /* Continue across the authored HQ road junction instead of stopping
         * at the end of t_dmz-hq_03a.  Runs 445-448 confirmed the original
         * 15-unit waypoint radius could accept both eastbound nodes while
         * the Humvee was still facing south-west.  It then reached the four
         * roadblock props at X=1399..1401 without the momentum needed to
         * break through them.  Finish the southbound turn at t_dmz-hq_08b,
         * use two staging points west of the props, and aim directly at
         * t_dmz-hq_04b on their far side. Run 474 showed that the former
         * four-metre staging leg accepted while the Humvee still faced south;
         * it hit the west edge obliquely and was deflected into the authored
         * birch/guardrail at (1397,828). Run 475 then proved that a direct turn
         * toward (1368,840) still clips the north dmz_fence at (1395,834)
         * before the vehicle can face east. Follow a wide
         * U-turn west of every fence section, settle on the exact centre of the
         * global_roadblock01 at runtime Z=842.766, then make the full eastbound
         * acceleration run. Do not drive directly from
         * that junction to loc_parking_allieshq: the straight line crosses
         * the authored hillside/chain-fence boundary and can roll the test
         * Humvee onto its side.  The remaining short approach is deliberately
         * completed on foot by recomp_apply_test_allied_hq_entry(). */
        {1365.000f, 865.000f},
        {1345.000f, 855.000f},
        {1345.000f, 842.766f},
        {1365.000f, 842.766f},
        {1380.000f, 842.766f},
        {1421.873f, 842.766f},
        {1434.000f, 840.500f}
    };
    const char *delay_text;
    const ULONGLONG elapsed = now - g_recomp_aircraft_route_complete_ms;
    ULONGLONG delay_ms;
    uint32_t actor = g_recomp_live_vehicle_actor;
    float px, pz, dx, dz, distance, forward_x, forward_z;
    float right_x, right_z, forward_dot, right_dot, steer;

    if (getenv("MERCENARIES_TEST_AUTO_DRIVE_GATE_ROUTE") == NULL ||
        completed || !g_recomp_aircraft_route_complete)
        return 0;

    delay_text = getenv("MERCENARIES_TEST_AUTO_DRIVE_AFTER_AIRCRAFT_DELAY_MS");
    delay_ms = delay_text && delay_text[0] != '\0' ?
        strtoull(delay_text, NULL, 0) : 50000u;
    if (elapsed < delay_ms)
        return 1;
    if (actor < 0x00010000u || actor > 0x04000000u - 0x1100u) {
        /* After the authored aircraft-exit cinematic, retail presents the
         * DRIVING tutorial before publishing the controllable vehicle actor.
         * This route owns the test input once its delay expires, so the
         * generic post-aircraft accelerator below cannot acknowledge that
         * modal. Pulse A here until the vehicle becomes live; the 50-second
         * delay keeps these pulses out of the preceding cinematic. */
        if (((elapsed - delay_ms) % 1500u) < 300u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        if (now - last_invalid_report_ms >= 1000u) {
            fprintf(stderr,
                    "[GATE-ROUTE-INVALID] stage=%u reason=actor actor=%08X "
                    "tutorial-A=%u\n",
                    stage, actor,
                    state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 0u);
            fflush(stderr);
            last_invalid_report_ms = now;
        }
        return 1;
    }

    px = guest_f32(actor + 0xE0u);
    pz = guest_f32(actor + 0xE8u);
    if (!isfinite(px) || !isfinite(pz)) {
        if (now - last_invalid_report_ms >= 1000u) {
            fprintf(stderr,
                    "[GATE-ROUTE-INVALID] stage=%u reason=position "
                    "actor=%08X vtable=%08X pos_bits=%08X/%08X\n",
                    stage, actor, guest_u32(actor),
                    guest_u32(actor + 0xE0u), guest_u32(actor + 0xE8u));
            fflush(stderr);
            last_invalid_report_ms = now;
        }
        return 1;
    }

    if (!started) {
        fprintf(stderr,
                "[GATE-ROUTE] started vehicle=%08X pos=(%.7g,%.7g) "
                "target=(%.7g,%.7g)\n",
                actor, px, pz, targets[0][0], targets[0][1]);
        fflush(stderr);
        started = 1;
        route_started_ms = now;
        g_recomp_post_aircraft_gate_route_started = 1;
    }

    /* A diagnostic driver must never consume an unattended machine forever.
     * Fifteen minutes after taking control is far beyond the healthy route's
     * observed duration and indicates that navigation is no longer useful. */
    if (route_started_ms != 0u && now - route_started_ms >= 900000u) {
        fprintf(stderr,
                "[GATE-ROUTE-ABORT] reason=timeout stage=%u vehicle=%08X "
                "pos=(%.7g,%.7g) elapsed=%llu\n",
                stage, actor, px, pz,
                (unsigned long long)(now - route_started_ms));
        fflush(stderr);
        completed = 1;
        state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
        return 1;
    }

    /* Sample at close encounter range for both the authored on-foot route and
     * the deliberate ram reproduction. At the earlier 100 m handoff point a
     * camera-visibility value below the human 0.04 draw threshold is expected
     * and cannot distinguish correct distance culling from the reported bug. */
    if (getenv("MERCENARIES_TRACE_ROADBLOCK_ACTORS") != NULL) {
        const float census_dx = 1595.198f - px;
        const float census_dz = 1505.167f - pz;
        if (census_dx * census_dx + census_dz * census_dz <= 45.0f * 45.0f)
            recomp_trace_roadblock_actor_census();
    }
    recomp_trace_reward_vehicle_census(px, pz);

    if (recomp_apply_test_roadblock_on_foot(state, now, actor))
        return 1;

    if (stage == 2u && wait_until_ms != 0u && now < wait_until_ms) {
        /* The low stone barrier has now been crossed at speed. Pause in the
         * space beyond it so the proximity-driven metal gate can finish
         * opening before the Humvee crosses the gate plane. */
        state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
        state->Gamepad.sThumbLX = 0;
        if (now - last_report_ms >= 1000u) {
            fprintf(stderr,
                    "[GATE-ROUTE] waiting for gate vehicle=%08X "
                    "pos=(%.7g,%.7g) remaining=%llu\n",
                    actor, px, pz,
                    (unsigned long long)(wait_until_ms - now));
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }
    wait_until_ms = 0u;

    /* HQ and live-capture regressions do not need another vehicle-ram test.
     * Hand over to normal on-foot input before the checkpoint barricades.
     * This does not move actors, disable collision, or change mission state. */
    if (stage == 35u &&
        (getenv("MERCENARIES_TEST_AUTO_TWO_CLUBS") != NULL ||
         getenv("MERCENARIES_TEST_AUTO_ENTER_ALLIED_HQ") != NULL)) {
        completed = 1;
        g_recomp_allied_hq_from_checkpoint = 1;
        g_recomp_allied_hq_route_arrived = 1;
        g_recomp_allied_hq_route_arrived_ms = now;
        fprintf(stderr, "[GATE-ROUTE] checkpoint on-foot handoff "
                "vehicle=%08X pos=(%.7g,%.7g)\n", actor, px, pz);
        fflush(stderr);
        return 1;
    }

    dx = targets[stage][0] - px;
    dz = targets[stage][1] - pz;
    distance = sqrtf(dx * dx + dz * dz);
    if (progress_stage != stage) {
        progress_stage = stage;
        best_waypoint_distance = distance;
        last_motion_ms = now;
        last_motion_x = px;
        last_motion_z = pz;
    }
    /* The authored checkpoint node at stage 39 sits beyond the destructible
     * crate/barrel barricade.  A successful head-on impact can carry the
     * Humvee several metres beyond that point between input samples, so a
     * radius-only test makes the diagnostic turn around and collide with the
     * debris it just cleared.  Crossing the westbound road plane while still
     * within the lane is the meaningful completion condition here. */
    if ((stage == 39u && px <= targets[stage][0] &&
         fabsf(pz - targets[stage][1]) <= 12.0f) ||
        distance <
            (stage == 0u ? 5.0f :
             (stage < 4u ? 7.0f :
              /* Stage 14 is the east-side clearance point around the first
               * roadblock encounter.  The general 15-metre radius could
               * accept X=1619 even though the authored props extend through
               * X=1613; the Humvee's body then clipped the encounter while
               * turning north and recovery drove it back into the blockage.
               * Reach the actual X=1630 clearance point before turning. */
              (stage == 14u ? 4.0f :
               /* Run 477 reached (1435.034,1129.221), already 3.1 metres
                * west and north of the adjacent fence's projected edge, but
                * the bumper could not close the last 1.5 metres to the
                * synthetic stage-40 point. Seven metres still keeps the
                * handoff safely north of the Z=1126.116 edge. */
               (stage == 40u ? 7.0f :
                (stage >= 35u && stage <= 41u ? 5.0f :
                 (stage >= 61u && stage <= 68u ? 4.0f :
               (stage + 1u == sizeof(targets) / sizeof(targets[0]) ?
                    4.0f : 15.0f)))))))) {
        const char *capture_path = NULL;
        fprintf(stderr,
                "[GATE-ROUTE] reached stage=%u vehicle=%08X "
                "pos=(%.7g,%.7g) target=(%.7g,%.7g) distance=%.7g\n",
                stage, actor, px, pz, targets[stage][0], targets[stage][1],
                distance);
        if (stage == 0u) {
            capture_path = getenv("MERCENARIES_CAPTURE_GATE_APPROACH_PATH");
            ++stage;
        } else if (stage == 1u) {
            ++stage;
            wait_until_ms = now + 3500u;
        } else if (stage == 2u) {
            capture_path = getenv("MERCENARIES_CAPTURE_BEYOND_GATE_PATH");
            ++stage;
        } else if (stage == 3u) {
            capture_path = getenv("MERCENARIES_CAPTURE_GATE_ROUTE_DONE_PATH");
            ++stage;
        } else if (stage + 1u < sizeof(targets) / sizeof(targets[0])) {
            ++stage;
        } else {
            capture_path = getenv(
                "MERCENARIES_CAPTURE_AUTHORED_NORTH_ROUTE_PATH");
            completed = 1;
            g_recomp_allied_hq_route_arrived = 1;
            g_recomp_allied_hq_route_arrived_ms = now;
        }
        if (capture_path != NULL && capture_path[0] != '\0')
            d3d8_DebugCaptureFrameToPath(capture_path);
        fflush(stderr);
        return 1;
    }

    dx /= distance;
    dz /= distance;
    /* 
     * negative stored-matrix Z axis.  X is its local right axis. */
    forward_x = -guest_f32(actor + 0xD0u);
    forward_z = -guest_f32(actor + 0xD8u);
    right_x = guest_f32(actor + 0xB0u);
    right_z = guest_f32(actor + 0xB8u);
    if (!isfinite(forward_x) || !isfinite(forward_z) ||
        !isfinite(right_x) || !isfinite(right_z)) {
        if (now - last_invalid_report_ms >= 1000u) {
            fprintf(stderr,
                    "[GATE-ROUTE-INVALID] stage=%u reason=orientation "
                    "actor=%08X matrix_bits=%08X/%08X/%08X/%08X\n",
                    stage, actor,
                    guest_u32(actor + 0xD0u), guest_u32(actor + 0xD8u),
                    guest_u32(actor + 0xB0u), guest_u32(actor + 0xB8u));
            fflush(stderr);
            last_invalid_report_ms = now;
        }
        return 1;
    }
    forward_dot = dx * forward_x + dz * forward_z;
    right_dot = dx * right_x + dz * right_z;
    steer = right_dot * 2.25f;
    if (steer < -1.0f)
        steer = -1.0f;
    else if (steer > 1.0f)
        steer = 1.0f;
    /* Recovery must also cover the four departure waypoints.  The scripted
     * driver can meet the aircraft ramp/stone barrier at a slightly different
     * angle as renderer load changes; previously that left diagnostic runs
     * accelerating forever at stage zero even though the game simulation was
     * healthy. */
    {
        /* Collision response can jiggle a wedged vehicle by more than half a
         * metre indefinitely.  Count only meaningful progress toward the
         * current waypoint; lateral/reverse oscillation must not continually
         * postpone recovery. */
        if (last_motion_ms == 0u ||
            distance <= best_waypoint_distance - 1.0f) {
            last_motion_ms = now;
            last_motion_x = px;
            last_motion_z = pz;
            best_waypoint_distance = distance;
        } else if (recovery_until_ms == 0u &&
                   now - last_motion_ms >= 3000u) {
            const char *recovery_capture_path =
                getenv("MERCENARIES_CAPTURE_GATE_ROUTE_STUCK_PATH");
            ++recovery_count;
            if (!recovery_capture_done && recovery_capture_path != NULL &&
                recovery_capture_path[0] != '\0') {
                d3d8_DebugCaptureFrameToPath(recovery_capture_path);
                recovery_capture_done = 1;
            }
            /* The final target is only a parking aid for the unattended
             * capture route.  A vehicle can become physically immobilized
             * beside the HQ approach while already close enough for the
             * existing on-foot entry driver to take over.  Treat that as a
             * completed diagnostic route after three failed recovery cycles;
             * this changes neither guest collision nor mission state. */
            if (recovery_count >= 3u &&
                stage + 1u == sizeof(targets) / sizeof(targets[0]) &&
                distance <= 50.0f) {
                const char *capture_path = getenv(
                    "MERCENARIES_CAPTURE_AUTHORED_NORTH_ROUTE_PATH");
                fprintf(stderr,
                        "[GATE-ROUTE-RECOVERY] accepting blocked final test "
                        "waypoint stage=%u vehicle=%08X pos=(%.7g,%.7g) "
                        "distance=%.7g\n",
                        stage, actor, px, pz, distance);
                if (capture_path != NULL && capture_path[0] != '\0')
                    d3d8_DebugCaptureFrameToPath(capture_path);
                fflush(stderr);
                completed = 1;
                /* Reuse west/south pedestrian detour from
                 * its nearby road-end node.  Value 2 distinguishes this
                 * blocked-parking fallback from the normal stage-35 mission
                 * checkpoint entry without changing retail world state. */
                g_recomp_allied_hq_from_checkpoint = 2;
                g_recomp_allied_hq_route_arrived = 1;
                g_recomp_allied_hq_route_arrived_ms = now;
                return 1;
            }
            /* Runs 612 and 619 both reached the walkable south road beside
             * the HQ fence at stage 63, but the diagnostic U-turn was highly
             * sensitive to the Humvee's residual speed.  Run 619 crossed to
             * (1389,825), then repeated reverse arcs until the retail vehicle
             * was destroyed and a newly spawned airfield vehicle inherited
             * the input.  This area is already on the verified pedestrian
             * route: checkpoint_walk_targets[29] continues east along the
             * unobstructed south road.  Hand input to the ordinary on-foot
             * driver after one proven stall instead of risking the vehicle;
             * this changes neither guest collision nor mission state. */
            if (recovery_count >= 1u && stage == 63u &&
                px >= 1340.0f && px <= 1405.0f &&
                pz >= 815.0f && pz <= 845.0f) {
                fprintf(stderr,
                        "[GATE-ROUTE-RECOVERY] accepting south-road test "
                        "handoff stage=%u vehicle=%08X pos=(%.7g,%.7g)\n",
                        stage, actor, px, pz);
                fflush(stderr);
                completed = 1;
                g_recomp_allied_hq_from_checkpoint = 3;
                g_recomp_allied_hq_route_arrived = 1;
                g_recomp_allied_hq_route_arrived_ms = now;
                return 1;
            }
            /* A reached-but-imprecise waypoint must not leave the unattended
             * diagnostic driver reversing around it forever.  This advances
             * only after three failed recovery cycles and never alters guest
             * collision or vehicle simulation. */
            if (recovery_count >= 3u &&
                (stage < 35u || stage > 41u) &&
                /* The aircraft departure's first gate can leave the Humvee
                 * beside a solid wing wall about 37 metres from the next
                 * synthetic waypoint.  That is already beyond the gate and
                 * on the authored road; allow the diagnostic driver to skip
                 * only these first four recovery waypoints within 50 metres.
                 * Later route nodes keep the tighter 35-metre bound. */
                distance <= (stage < 4u ? 50.0f : 35.0f) &&
                stage + 1u < sizeof(targets) / sizeof(targets[0])) {
                fprintf(stderr,
                        "[GATE-ROUTE-RECOVERY] skipping blocked test waypoint "
                        "stage=%u vehicle=%08X pos=(%.7g,%.7g)\n",
                        stage, actor, px, pz);
                fflush(stderr);
                ++stage;
                recovery_count = 0u;
                last_motion_ms = now;
                last_motion_x = px;
                last_motion_z = pz;
                best_waypoint_distance = 0.0f;
                progress_stage = ~0u;
                return 1;
            }
            if (recovery_count >= 6u && distance > 100.0f) {
                fprintf(stderr,
                        "[GATE-ROUTE-ABORT] reason=off-route stage=%u "
                        "vehicle=%08X pos=(%.7g,%.7g) target=(%.7g,%.7g) "
                        "distance=%.7g\n",
                        stage, actor, px, pz, targets[stage][0],
                        targets[stage][1], distance);
                fflush(stderr);
                completed = 1;
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
                return 1;
            }
            /* X is both brake and reverse on the retail Humvee.  Runs
             * 451/452 showed that a 1.5 second pulse was consumed entirely
             * by braking at the checkpoint, so the vehicle never developed
             * reverse motion and merely rocked against the same prop.  Hold
             * through the documented three-second brake transition, then
             * allow a matching forward arc before reassessing progress. */
            recovery_reverse_until_ms = now + 3500u;
            recovery_until_ms = now + 7000u;
            fprintf(stderr,
                    "[GATE-ROUTE-RECOVERY] stage=%u vehicle=%08X "
                    "pos=(%.7g,%.7g) target=(%.7g,%.7g)\n",
                    stage, actor, px, pz, targets[stage][0], targets[stage][1]);
            fflush(stderr);
        }
        if (recovery_until_ms != 0u && now < recovery_until_ms) {
            const float recovery_steer = right_dot < 0.0f ? -1.0f : 1.0f;
            if (now < recovery_reverse_until_ms) {
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
                state->Gamepad.sThumbLX = (SHORT)(-recovery_steer * 32767.0f);
            } else {
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
                state->Gamepad.sThumbLX = (SHORT)(recovery_steer * 32767.0f);
            }
            return 1;
        }
        if (recovery_until_ms != 0u) {
            recovery_until_ms = 0u;
            recovery_reverse_until_ms = 0u;
            last_motion_ms = now;
            last_motion_x = px;
            last_motion_z = pz;
            best_waypoint_distance = distance;
        }
    }
    if (((stage >= 38u && stage <= 39u) || stage >= 64u) &&
        forward_dot >= 0.985f) {
        /* Preserve momentum while aligned with either authored destructible
         * roadblock.  Stages 35-37 retain full steering through the measured
         * cornering arc; stages 38-39 then make the Humvee strike the crates
         * head-on.  Keep full steering until alignment is within
         * roughly ten degrees: run 461 entered the pile at about twenty
         * degrees when the old 0.65 threshold clamped too early. Braking or
         * full-lock steering at contact made
         * the diagnostic climb the pile.  The final HQ approach
         * similarly crosses three dmz_fence sections and two roadblock props
         * around (1393..1400,834..855). */
        if (steer < -0.3f)
            steer = -0.3f;
        else if (steer > 0.3f)
            steer = 0.3f;
        state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        state->Gamepad.sThumbLX = (SHORT)(steer * 32767.0f);
    } else if (stage >= 4u && forward_dot < 0.15f) {
        /* If a tight road node was overshot, reverse while counter-steering
         * until the target returns to the forward hemisphere. Continuing
         * to accelerate with a full steering lock can wedge the vehicle. */
        state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 200;
        state->Gamepad.sThumbLX = (SHORT)(-steer * 32767.0f);
    } else {
        const float abs_steer = fabsf(steer);
        state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
            (stage >= 4u && abs_steer > 0.65f) ? 180 : 255;
        state->Gamepad.sThumbLX = (SHORT)(steer * 32767.0f);
    }

    if (now - last_report_ms >= 500u) {
        fprintf(stderr,
                "[GATE-ROUTE] stage=%u vehicle=%08X pos=(%.7g,%.7g) "
                "target=(%.7g,%.7g) distance=%.7g forward=(%.7g,%.7g) "
                "dot=(%.7g,%.7g) steer=%.7g\n",
                stage, actor, px, pz, targets[stage][0], targets[stage][1],
                distance, forward_x, forward_z, forward_dot, right_dot,
                steer);
        fflush(stderr);
        last_report_ms = now;
    }
    return 1;
}

static uint32_t recomp_find_named_actor(uint32_t name_hash)
{
    uint32_t actor;

    /* RedWorld::Entity stores its name at +4 and Spore pointer at +8.
     * Confirmed with live HumanPlayer and HumanGeneral instances and their
     * retail constructors. +0x34 is NOT the name: scanning there selected
     * an unrelated allocation 0x30 bytes before the real bouncer, interpreting
     * an orientation vector as a world position. */
    if (name_hash == 0u)
        return 0u;
    for (actor = 0x00800000u;
         actor <= 0x03FFF000u - 0xECu; actor += 0x10u) {
        const uint32_t vtable = guest_u32(actor);
        if (guest_u32(actor + 4u) == name_hash &&
            vtable >= 0x002D0000u && vtable <= 0x002F0000u)
            return actor;
    }
    return 0u;
}

/* Opt-in screen-flash diagnostic using color 0x80ffff00, duration 1.66f,
 * and mode -1. The retail setter at 0x000F26F0 only stores these four fields; writing the
 * same values at an input safe point avoids requiring a full low-health route
 * merely to inspect the renderer state of the survivor flash.  A trigger file
 * and one-shot latch keep this entirely inert outside explicit test runs. */
static void recomp_apply_test_survivor_flash(void)
{
    static int initialized, fired;
    static const char *path;

    if (!initialized) {
        path = getenv("MERCENARIES_TEST_SURVIVOR_FLASH_FILE");
        initialized = 1;
    }
    if (fired || path == NULL || path[0] == '\0' ||
        GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        return;

    *(volatile uint32_t *)guest_ptr(0x0035CFF0u + 0x34u) = 0x80FFFF00u;
    *(volatile uint32_t *)guest_ptr(0x0035CFF0u + 0x38u) = 0x3FD47AE1u;
    *(volatile uint32_t *)guest_ptr(0x0035CFF0u + 0x3Cu) = 0x3FD47AE1u;
    *(volatile uint32_t *)guest_ptr(0x0035CFF0u + 0x40u) = 0xFFFFFFFFu;
    fired = 1;
    {
        const char *capture_prefix = getenv(
            "MERCENARIES_TEST_SURVIVOR_FLASH_CAPTURE_PREFIX");
        if (capture_prefix && *capture_prefix)
            d3d8_DebugStartDisplayCapture(capture_prefix, 100u, 12u);
    }
    fprintf(stderr,
            "[TEST-SURVIVOR-FLASH] color=80FFFF00 alpha=3FD47AE1 "
            "direction=FFFFFFFF\n");
    fflush(stderr);
}

typedef struct RecompSurvivorTraceState {
    int valid;
    uint32_t stage;
    uint32_t ai;
    uint32_t human;
    float ratio;
    float last_hitpoints;
    uint32_t state;
    uint32_t survivor_state;
    ULONGLONG tick;
} RecompSurvivorTraceState;

static RecompSurvivorTraceState g_recomp_survivor_trace_state;

static int recomp_screen_flash_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        const char *value = getenv("MERCENARIES_TRACE_SCREEN_FLASH");
        enabled = value != NULL && value[0] != '\0';
        initialized = 1;
    }
    return enabled;
}

/* Paint-time observation sees the surviving overlay after priority/clear rules.
 * Ignore timer progression when deciding whether a state changed, and sample
 * active fades at most four times per second. Disabled tracing reads no guest
 * fields; the bounded stream cannot grow without limit on a long playthrough. */
/* Trace computed satellite text colors separately from fixed frame colors. */
void recomp_satellite_color_checkpoint(uint32_t site, uint32_t color)
{
    static int enabled = -1;
    static unsigned count;
    static uint32_t sites[16], colors[16];
    unsigned i;
    if (enabled < 0) {
        const char *value = getenv("MERCENARIES_TRACE_SATELLITE_COLOR");
        enabled = value && value[0] == '1';
    }
    if (!enabled || count >= 2048u) return;
    for (i = 0; i < 16u; ++i) {
        if (sites[i] == site) {
            if (colors[i] == color) return;
            break;
        }
        if (!sites[i]) break;
    }
    if (i == 16u) return;
    sites[i] = site;
    colors[i] = color;
    fprintf(stderr, "[SATELLITE-COLOR] sample=%u tick=%llu site=%08X color=%08X fp_top=%u\n",
        ++count, (unsigned long long)GetTickCount64(), site, color, g_fp_top);
}

void recomp_screen_dimmer_checkpoint(uint32_t object)
{
    static unsigned int count;
    static uint32_t last[5];
    static ULONGLONG last_tick;
    uint32_t current[5];
    ULONGLONG now;
    float timer;
    if (!recomp_screen_flash_trace_enabled() || count >= 1024u ||
        object < 0x10000u || object > 0x04000000u - 0x44u)
        return;
    timer = guest_f32(object + 0x38u);
    current[0] = object;
    current[1] = guest_u32(object + 0x2Cu);
    current[2] = guest_u32(object + 0x30u);
    current[3] = timer > 0.0f ? guest_u32(object + 0x34u) : 0u;
    current[4] = timer > 0.0f ? guest_u32(object + 0x40u) : 0u;
    now = GetTickCount64();
    if (count && memcmp(current, last, sizeof(last)) == 0 &&
        (!(timer > 0.0f) || now - last_tick < 250u))
        return;
    memcpy(last, current, sizeof(last));
    last_tick = now;
    fprintf(stderr,
        "[SCREEN-DIMMER] sample=%u tick=%llu object=%08X "
        "dimmer=%08X flag=%d flash=%08X flag=%d timer=%g/%g "
        "main=%08X/%08X time_rate=%g\n",
        ++count, (unsigned long long)now, object, current[1],
        (int32_t)current[2], current[3], (int32_t)current[4],
        (double)timer, (double)guest_f32(object + 0x3Cu),
        guest_u32(0x00413F6Cu), guest_u32(0x00413F68u),
        (double)guest_f32(0x004140D4u));
    fflush(stderr);
}

void recomp_screen_flash_checkpoint(uint32_t object, uint32_t color,
                                    uint32_t time_bits, uint32_t render_flag)
{
    static unsigned int count;
    float time_value;
    const ULONGLONG now = GetTickCount64();

    if (!recomp_screen_flash_trace_enabled() || count >= 64u)
        return;
    memcpy(&time_value, &time_bits, sizeof(time_value));
    fprintf(stderr,
            "[SCREEN-FLASH-SET] sample=%u object=%08X color=%08X "
            "time=%g/%08X flag=%d main=%08X/%08X time_rate=%g "
            "survivor_sample=%d/%u age_ms=%llu ai=%08X human=%08X "
            "ratio=%g last_hp=%g state=%08X survivor=%08X\n",
            ++count, object, color, (double)time_value, time_bits,
            (int32_t)render_flag, guest_u32(0x00413F6Cu),
            guest_u32(0x00413F68u), guest_f32(0x004140D4u),
            g_recomp_survivor_trace_state.valid,
            g_recomp_survivor_trace_state.stage,
            g_recomp_survivor_trace_state.valid ?
                (unsigned long long)(now - g_recomp_survivor_trace_state.tick) :
                0ull,
            g_recomp_survivor_trace_state.ai,
            g_recomp_survivor_trace_state.human,
            (double)g_recomp_survivor_trace_state.ratio,
            (double)g_recomp_survivor_trace_state.last_hitpoints,
            g_recomp_survivor_trace_state.state,
            g_recomp_survivor_trace_state.survivor_state);
    fflush(stderr);
}

void recomp_survivor_state_checkpoint(uint32_t stage, uint32_t ai,
                                      uint32_t human, uint32_t vehicle,
                                      float value, float reference,
                                      uint32_t state, uint32_t survivor_state)
{
    static unsigned int count;
    if (!recomp_screen_flash_trace_enabled())
        return;
    if (stage == 1u || stage == 2u) {
        g_recomp_survivor_trace_state.valid = 1;
        g_recomp_survivor_trace_state.stage = stage;
        g_recomp_survivor_trace_state.ai = ai;
        g_recomp_survivor_trace_state.human = vehicle;
        g_recomp_survivor_trace_state.ratio = value;
        g_recomp_survivor_trace_state.last_hitpoints = reference;
        g_recomp_survivor_trace_state.state = state;
        g_recomp_survivor_trace_state.survivor_state = survivor_state;
        g_recomp_survivor_trace_state.tick = GetTickCount64();
    }
    if (count >= 128u)
        return;
    /* Stage 1 is useful only near the 19% boundary or while survivor mode is
     * already active.  Keep a long route's bounded log for transitions. */
    if (stage == 1u && value > 0.25f && state != survivor_state)
        return;
    fprintf(stderr,
            "[SURVIVOR-STATE] sample=%u stage=%u ai=%08X human=%08X "
            "context=%08X ratio=%g last_hp=%g state=%08X survivor=%08X "
            "time_rate=%g\n",
            ++count, stage, ai, human, vehicle, (double)value,
            (double)reference, state, survivor_state,
            guest_f32(0x004140D4u));
    fflush(stderr);
}

static int recomp_apply_test_gamepad_command(XBOX_INPUT_STATE *state,
                                            ULONGLONG now)
{
    static int initialized, enabled, manual;
    static const char *path;
    static unsigned int sequence, digital, analog, packet, capture_sequence;
    static int single_poll_pending;
    static int lx, ly, rx, ry;
    static ULONGLONG last_poll, expires, capture_due;
    if (!initialized) {
        path = getenv("MERCENARIES_TEST_GAMEPAD_FILE");
        enabled = path != NULL && path[0] != '\0';
        initialized = 1;
    }
    if (!enabled) return 0;
    if (last_poll == 0u || now - last_poll >= 100u) {
        FILE *file;
        char line[192];
        unsigned int lines = 0u;
        last_poll = now;
        file = fopen(path, "r");
        if (file != NULL) {
            while (lines++ < 128u && fgets(line, sizeof(line), file) != NULL) {
                unsigned int next, mode, duration, buttons, pressure, capture;
                int x, y, cx, cy;
                char trailing;
                /* Numeric controller data only: no code, guest writes,
                 * desktop keys, or mission-state operations. */
                if (sscanf(line, "%u %u %u %x %x %d %d %d %d %u %c",
                           &next, &mode, &duration, &buttons, &pressure,
                           &x, &y, &cx, &cy, &capture, &trailing) != 10 ||
                    next <= sequence || next > 0x7FFFFFFFu ||
                    mode > 1u || duration > 2000u ||
                    buttons > 0xFFu || pressure > 0xFFu || capture > 1u ||
                    x < -32768 || x > 32767 || y < -32768 || y > 32767 ||
                    cx < -32768 || cx > 32767 || cy < -32768 || cy > 32767)
                    continue;
                sequence = next; manual = mode != 0u;
                expires = now + duration; digital = buttons; analog = pressure;
                /* A zero-duration diagnostic command is one hardware sample,
                 * avoiding a hold that can cross fast retail menu states. */
                single_poll_pending = manual && duration == 0u;
                lx = x; ly = y; rx = cx; ry = cy;
                capture_due = manual && capture ? expires + 200u : 0u;
                capture_sequence = next;
                fprintf(stderr, "[TEST-GAMEPAD] seq=%u manual=%u hold_ms=%u "
                        "digital=%02X analog=%02X sticks=%d,%d,%d,%d\n",
                        next, mode, duration, buttons, pressure, x, y, cx, cy);
                fflush(stderr);
                break;
            }
            fclose(file);
        }
    }
    if (!manual) return 0;
    /* Expired/missing commands release everything and stay neutral. Automatic
     * routes resume only after an explicit newer mode=0 command. */
    memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    state->dwPacketNumber = ++packet;
    if (single_poll_pending || now < expires) {
        unsigned int i;
        state->Gamepad.wButtons = (WORD)digital;
        for (i = 0; i < 8u; ++i)
            state->Gamepad.bAnalogButtons[i] = (analog & (1u << i)) ? 255 : 0;
        state->Gamepad.sThumbLX = (SHORT)lx; state->Gamepad.sThumbLY = (SHORT)ly;
        state->Gamepad.sThumbRX = (SHORT)rx; state->Gamepad.sThumbRY = (SHORT)ry;
        single_poll_pending = 0;
    }
    if (capture_due != 0u && now >= capture_due) {
        const char *prefix = getenv("MERCENARIES_CAPTURE_TEST_GAMEPAD_PREFIX");
        if (prefix != NULL && prefix[0] != '\0') {
            char capture_path[1024];
            int length = snprintf(capture_path, sizeof(capture_path),
                                  "%s-%06u.bmp", prefix, capture_sequence);
            if (length > 0 && (size_t)length < sizeof(capture_path))
                d3d8_DebugQueueFrameCapture(capture_path);
        }
        capture_due = 0u;
    }
    return 1;
}

static int recomp_test_hq_waypoint_override(unsigned int stage, ULONGLONG now,
                                            float *target_x, float *target_z)
{
    static ULONGLONG last_poll_ms;
    static unsigned int cached_stage = UINT32_MAX;
    static unsigned int cached_sequence;
    static int cached_valid;
    static float cached_x, cached_z;
    const char *path = getenv("MERCENARIES_TEST_HQ_WAYPOINT_FILE");
    int changed = 0;

    /* Test navigation only: a bounded numeric file changes the requested
     * waypoint, never the player position, mission flags or collision. A
     * fresh sequence may resume a walking-test abort at this same stage. */
    if (path == NULL || path[0] == '\0')
        return 0;
    if (cached_stage != stage || now - last_poll_ms >= 500u) {
        FILE *file;
        char line[128];
        unsigned int lines = 0u;
        if (cached_stage != stage) {
            cached_stage = stage;
            cached_valid = 0;
        }
        last_poll_ms = now;
        file = fopen(path, "r");
        if (file != NULL) {
            while (lines++ < 128u && fgets(line, sizeof(line), file) != NULL) {
                unsigned int sequence, requested_stage;
                float x, z;
                if (sscanf(line, "%u %u %f %f", &sequence, &requested_stage,
                           &x, &z) != 4 || sequence == 0u ||
                    requested_stage != stage || !isfinite(x) || !isfinite(z) ||
                    fabsf(x) > 10000.0f || fabsf(z) > 10000.0f)
                    continue;
                changed = !cached_valid || sequence != cached_sequence;
                if (changed) {
                    cached_sequence = sequence;
                    cached_x = x;
                    cached_z = z;
                    cached_valid = 1;
                    fprintf(stderr,
                            "[ALLIED-HQ] diagnostic waypoint sequence=%u "
                            "stage=%u target=(%.7g,%.7g)\n",
                            sequence, stage, x, z);
                    fflush(stderr);
                }
                break;
            }
            fclose(file);
        }
    }
    if (cached_valid) {
        *target_x = cached_x;
        *target_z = cached_z;
    }
    return changed;
}

static int recomp_test_hq_returned_to_world(int entered_interior,
                                          int requested_starter_use,
                                          float x, float z)
{
    /* Retail onUseStarter disables (does not destroy) starter_trigger.
     * Observe the authored relocation after an actual Y use; never alter
     * mission state or mistake the initial exterior approach for acceptance. */
    return entered_interior && requested_starter_use &&
        isfinite(x) && isfinite(z) &&
        x >= 1200.0f && x <= 1800.0f && z >= 500.0f && z <= 1100.0f;
}

static int recomp_apply_test_allied_hq_entry(XBOX_INPUT_STATE *state,
                                               ULONGLONG now)
{
    static unsigned int phase;
    static unsigned int walk_stage;
    static ULONGLONG phase_started_ms;
    static ULONGLONG last_report_ms;
    static ULONGLONG last_starter_scan_ms;
    static ULONGLONG last_bouncer_scan_ms;
    static ULONGLONG last_walk_motion_ms;
    static float last_walk_motion_x;
    static float last_walk_motion_z;
    static int captured_settled_exterior;
    static int captured_interior;
    static int captured_briefing;
    static ULONGLONG hq_standup_requested_ms;
    static int hq_starter_use_requested;
    static int captured_mission_accepted;
    static int captured_subdue_probe;
    static uint32_t starter_trigger;
    static uint32_t bouncer_actor;
    static uint32_t clubs2_actor;
    static int support_cancel_sent;
    static int captured_support_menu;
    static int walk_aborted;
    static int card_walk_aborted;
    static unsigned int card_walk_stage;
    static ULONGLONG last_card_scan_ms;
    static ULONGLONG last_card_walk_motion_ms;
    static float last_card_walk_motion_x;
    static float last_card_walk_motion_z;

    /* This helper is reached from every retail XInput poll. Reject the
     * disabled diagnostic route before resolving the live player: the
     * fallback resolver scans Xbox RAM in 16-byte steps and is intentionally
     * expensive when no HumanPlayer exists (front end, loads and cutscenes).
     * Keeping this gate first is behavior-neutral for normal play. */
    if (getenv("MERCENARIES_TEST_AUTO_ENTER_ALLIED_HQ") == NULL ||
        !g_recomp_allied_hq_route_arrived) {
        g_mercenaries_hq_briefing_active = 0u;
        return 0;
    }

    const float parking_walk_targets[][2] = {
        /* Parking is west of the authored bouncer marker. SW.wld has a
         * continuous east-west divider: dmz_lowwallcorner01 at X=1436.085,
         * then four dmz_lowwall01 sections at X=1441.740, 1451.918,
         * 1461.911 and 1472.089, all around editor Z=-838.9.  The old route
         * tried to cross at X=1452 and consequently walked into solid
         * concrete forever.  Clear the west end at X=1428, then remain on the
         * south side while approaching the staircase. Run 473 proved that
         * crossing north and walking east at runtime Z=828 instead hits the
         * facade/lighting collision near X=1477. SW_Mafia5.PTH and
         * SW_poi_dmz_peds.PTH provide the original walkable approach nodes at
         * editor (1485.606,-844.731) and (1485.976,-840.913). The
         * playable-intro mission sets the bouncer
         * actionRange to only 1 unit, so the last target must be the exact
         * SW.wld loc_hq-allies-bouncer marker (1489.324,-836.714), not an
         * approximate point near the stairs. Runtime Z is sign-flipped. */
        /* The player exits on the Humvee's east/south side. First clear the
         * parked vehicle to the south, then go west; a direct line toward
         * the divider intersects the Humvee and leaves the test stick
         * pushing against its collision indefinitely. */
        {1452.000f, 865.000f},
        {1428.000f, 865.000f},
        {1428.000f, 848.000f},
        {1472.000f, 848.000f},
        {1485.606f, 844.731f},
        {1485.976f, 840.913f},
        {1489.324f, 836.714f}
    };
    const float checkpoint_walk_targets[][2] = {
        /* SWN.wld adds fences/roadblocks not present in SW.wld alone.
         * Run 482 cleared their north ends using normal walking, then
         * followed SW_poi_dmz_traffic.PTH to the HQ compound. */
        {1490.000f, 1142.000f}, {1470.000f, 1142.000f},
        /* Stay east of dmz_bld_nkguardtower and the fence row, then turn
         * west before the hillside at (1451,1109). */
        {1455.000f, 1142.000f}, {1455.000f, 1113.000f},
        {1436.000f, 1100.000f}, {1420.000f, 1098.000f},
        {1410.000f, 1100.000f},
        {1392.827f, 1106.409f}, {1374.967f, 1098.334f},
        {1356.013f, 1088.082f}, {1335.902f, 1075.464f},
        {1320.774f, 1065.528f}, {1309.479f, 1056.193f},
        {1300.313f, 1047.593f}, {1292.405f, 1038.062f},
        {1286.198f, 1023.643f}, {1283.690f, 1011.759f},
        {1282.474f, 992.833f}, {1285.467f, 976.480f},
        {1292.421f, 963.969f}, {1299.264f, 950.206f},
        {1308.522f, 934.533f}, {1319.008f, 924.504f},
        {1346.924f, 909.419f}, {1361.132f, 899.202f},
        {1375.652f, 881.614f}, {1380.000f, 870.000f},
        /* Clear the roadside rock and the full HQ fence row at
         * (1393..1395,834..855), as verified in run 482. */
        /* SW.wld's global_guardrail at (1388.410,-829.343) caught
         * run525's diagonal approach. Its observed west detour then
         * rejoined the existing route using only normal stick movement. */
        {1386.000f, 846.000f}, {1370.000f, 833.000f},
        {1412.000f, 822.000f}, {1428.000f, 848.000f},
        /* Three lowwall02 segments run from Z=839..870; the south return
         * spans X=1455..1478. Run 483 cleared the entire U-shaped enclosure
         * via its west end before crossing east and approaching the stairs. */
        {1472.000f, 852.000f}, {1450.000f, 862.000f},
        {1450.000f, 879.000f},
        {1488.000f, 879.000f}, {1488.000f, 852.000f},
        {1485.606f, 844.731f}, {1485.976f, 840.913f},
        {1489.324f, 836.714f}
    };
    const float (*walk_targets)[2] = g_recomp_allied_hq_from_checkpoint ?
        checkpoint_walk_targets : parking_walk_targets;
    const size_t walk_target_count = g_recomp_allied_hq_from_checkpoint ?
        sizeof(checkpoint_walk_targets) / sizeof(checkpoint_walk_targets[0]) :
        sizeof(parking_walk_targets) / sizeof(parking_walk_targets[0]);
    const float card_walk_targets[][2] = {
        /* Continue with normal retail movement after the briefing. These are
         * pedestrian/traffic nodes, not collision bypasses:
         * SW_Mafia5.PTH exits the HQ ramp, SW_poi_dmz_traffic.PTH's
         * t_dmz-hq_05a follows the north road, and the final point crosses the
         * west edge of SWN_allies1_enc_card.LRG/CardEncRunners02X. */
        {1489.000f, 825.000f},
        /* SW.wld puts allies_HQ at (1486.89,809.61). Run 489 hit
         * its south facade at Z=821. Leave the raised forecourt westward,
         * then pass west of the building before joining the north road. */
        {1458.000f, 827.000f},
        {1458.000f, 787.000f},
        {1487.683f, 786.920f},
        {1497.376f, 772.662f},
        {1497.969f, 754.741f},
        {1504.548f, 700.190f},
        {1506.105f, 674.558f},
        {1507.163f, 656.927f},
        /* Run491's direct east approach hit the plateau's west cliff.
         * Follow the recorded road and CardEncRunner01a/02a source paths. */
        /* Run518's direct north leg caught a road barricade near
         * (1508,614). The observed walkable east detour preserves collision
         * and rejoins the same authored road; it is test input only. */
        {1523.000f, 616.000f}, {1561.830f, 604.487f},
        {1635.000f, 604.000f}, {1675.339f, 604.933f},
        {1677.179f, 623.737f}, {1677.416f, 643.749f},
        /* Run497 verified a walkable detour around the sandbag/hedgehog
         * barrier at (1672,653); walking into its face or jumping stalls.
         * These are normal stick destinations, not changed collision. */
        {1657.000f, 646.000f}, {1657.500f, 664.000f},
        {1665.000f, 680.000f}, {1635.000f, 715.000f},
        {1593.089f, 719.803f}
    };
    const uint32_t player_human_from_ai =
        (g_recomp_live_player_ai >= 0x00010000u &&
         g_recomp_live_player_ai <= 0x04000000u - 0x99Cu) ?
            guest_u32(g_recomp_live_player_ai + 0x998u) : 0u;
    const uint32_t human =
        (player_human_from_ai >= 0x00010000u &&
         player_human_from_ai <= 0x04000000u - 0xECu &&
         guest_u32(player_human_from_ai) == 0x002E32B8u) ?
            player_human_from_ai : recomp_find_live_player_human();
    const uint32_t red_camera = guest_u32(0x004140E8u + 0x1Cu);
    float hx, hz, dx, dz, distance, target_x, target_z;
    float forward_x, forward_z, forward_length, right_x, right_z;
    float input_x, input_y;

    g_mercenaries_hq_briefing_active = phase == 3u;

    if (phase_started_ms == 0u) {
        phase_started_ms = g_recomp_allied_hq_route_arrived_ms;
        if (g_recomp_allied_hq_from_checkpoint == 2 ||
            g_recomp_allied_hq_from_checkpoint == 3) {
            /* The blocked final parking fallback exits around (1390,864).
             * checkpoint_walk_targets[27] begins the already verified detour
             * around the HQ compound's west/south low-wall enclosure. */
            walk_stage = g_recomp_allied_hq_from_checkpoint == 3 ? 29u : 27u;
            fprintf(stderr,
                    "[ALLIED-HQ] vehicle fallback begins walk mode=%d "
                    "stage=%u\n", g_recomp_allied_hq_from_checkpoint,
                    walk_stage);
            fflush(stderr);
        }
    }

    /* The world marker is only the script's requested spawn point. Follow the
     * live actor named "alliesbouncer" once it exists, because collision or
     * spawn adjustment can move the usable human away from that marker. The
     * hash 0x9A21CF9F is independently reproduced by retail 0x1F29F0;
     * see test_asset_hash_retail.py and retail-asset-hash-evidence.md. */
    if ((bouncer_actor < 0x00010000u ||
         bouncer_actor > 0x04000000u - 0xECu ||
         guest_u32(bouncer_actor + 4u) != 0x9A21CF9Fu) &&
        now - last_bouncer_scan_ms >= 1000u) {
        bouncer_actor = recomp_find_named_actor(0x9A21CF9Fu);
        last_bouncer_scan_ms = now;
        if (bouncer_actor != 0u) {
            fprintf(stderr,
                    "[ALLIED-HQ] live bouncer=%08X vtable=%08X "
                    "pos=(%.7g,%.7g,%.7g)\n",
                    bouncer_actor, guest_u32(bouncer_actor),
                    guest_f32(bouncer_actor + 0xE0u),
                    guest_f32(bouncer_actor + 0xE4u),
                    guest_f32(bouncer_actor + 0xE8u));
            fflush(stderr);
        }
    }

    if (phase == 0u) {
        const ULONGLONG elapsed = now - phase_started_ms;

        /* Settle the parked Humvee, then use the normal retail exit action.
         * X is the brake/reverse analogue button; do not hold it while Y is
         * pulsed because that can keep the rider transition from latching. */
        if (elapsed < 3000u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
        if (!g_recomp_allied_hq_from_checkpoint &&
            !captured_settled_exterior && elapsed >= 3000u) {
            const char *capture_path =
                getenv("MERCENARIES_CAPTURE_ALLIED_HQ_SETTLED_PATH");
            const uint32_t hq = recomp_find_named_actor(0x9239CAACu);
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            fprintf(stderr,
                    "[ALLIED-HQ] settled exterior captured after %llu ms "
                    "hq=%08X vtable=%08X pos=(%.7g,%.7g,%.7g) "
                    "render=%08X flags=%08X model=%08X\n",
                    (unsigned long long)elapsed, hq,
                    hq != 0u ? guest_u32(hq) : 0u,
                    hq != 0u ? guest_f32(hq + 0xE0u) : 0.0f,
                    hq != 0u ? guest_f32(hq + 0xE4u) : 0.0f,
                    hq != 0u ? guest_f32(hq + 0xE8u) : 0.0f,
                    hq != 0u ? guest_u32(hq + 0x3Cu) : 0u,
                    hq != 0u ? guest_u32(hq + 0x50u) : 0u,
                    hq != 0u ? guest_u32(hq + 0x58u) : 0u);
            fprintf(stderr,
                    "[ALLIED-HQ] name-layout-check human=%08X "
                    "human-name-hash=%08X expected-player0=%08X\n",
                    human,
                    human != 0u ? guest_u32(human + 4u) : 0u,
                    0x660E4490u);
            fflush(stderr);
            captured_settled_exterior = 1;
        }
        if (elapsed >= 3500u && elapsed < 20000u &&
            ((elapsed - 3500u) % 1500u) < 450u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
        if (elapsed >= 5500u && recomp_find_player_occupied_vehicle() == 0u) {
            if (g_recomp_allied_hq_from_checkpoint == 2 &&
                human >= 0x00010000u && human <= 0x04000000u - 0xECu &&
                guest_u32(human) == 0x002E32B8u) {
                const float exit_x = guest_f32(human + 0xE0u);
                const float exit_z = guest_f32(human + 0xE8u);

                /* The blocked-parking acceptance can leave Jacobs on either
                 * side of the final fence.  Run 618 exited southeast at
                 * (1397.6,838.4), where the north-route stage 27 points back
                 * through the fence.  That position is already east of the
                 * obstacle, so continue to the verified south-road node at
                 * stage 29.  North-side exits retain the west detour. */
                if (isfinite(exit_x) && isfinite(exit_z) &&
                    exit_x > 1390.0f && exit_z < 845.0f)
                    walk_stage = 29u;
                fprintf(stderr,
                        "[ALLIED-HQ] checkpoint exit side selected "
                        "pos=(%.7g,%.7g) walk-stage=%u\n",
                        exit_x, exit_z, walk_stage);
                fflush(stderr);
            }
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 0;
            phase = 1u;
            phase_started_ms = now;
            fprintf(stderr,
                    "[ALLIED-HQ] exit pulse complete; walking to authored "
                    "bouncer marker\n");
            fflush(stderr);
        } else if (elapsed >= 20000u) {
            fprintf(stderr, "[ALLIED-HQ-ABORT] reason=vehicle-exit-timeout\n");
            fflush(stderr);
            phase = 99u;
        }
        return 1;
    }

    if (human < 0x00010000u || human > 0x04000000u - 0xECu ||
        guest_u32(human) != 0x002E32B8u) {
        if (now - last_report_ms >= 1000u) {
            fprintf(stderr,
                    "[ALLIED-HQ-INVALID] phase=%u human=%08X vtable=%08X\n",
                    phase, human, human >= 0x00010000u ? guest_u32(human) : 0u);
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }

    hx = guest_f32(human + 0xE0u);
    hz = guest_f32(human + 0xE8u);
    if (!isfinite(hx) || !isfinite(hz))
        return 1;

    if (phase == 1u || (phase == 99u && walk_aborted)) {
        target_x = walk_targets[walk_stage][0];
        target_z = walk_targets[walk_stage][1];
        if (recomp_test_hq_waypoint_override(walk_stage, now, &target_x, &target_z)) {
            const char *prefix = getenv("MERCENARIES_CAPTURE_HQ_WAYPOINT_PREFIX");
            static unsigned int revision;
            if (prefix != NULL && prefix[0] != '\0') {
                char capture_path[MAX_PATH];
                snprintf(capture_path, sizeof(capture_path), "%s-%03u.bmp",
                         prefix, ++revision);
                d3d8_DebugCaptureFrameToPath(capture_path);
            }
            phase = 1u;
            walk_aborted = 0;
            phase_started_ms = now;
            last_walk_motion_ms = now;
            last_walk_motion_x = hx;
            last_walk_motion_z = hz;
        }
    }

    if (phase == 1u) {
        const ULONGLONG elapsed = now - phase_started_ms;
        const float motion_x = hx - last_walk_motion_x;
        const float motion_z = hz - last_walk_motion_z;

        if (last_walk_motion_ms == 0u ||
            motion_x * motion_x + motion_z * motion_z >= 1.0f) {
            last_walk_motion_ms = now;
            last_walk_motion_x = hx;
            last_walk_motion_z = hz;
        }
        if (now - last_walk_motion_ms >= 20000u) {
            const char *capture_path = getenv(
                "MERCENARIES_CAPTURE_ALLIED_HQ_WALK_ABORT_PATH");
            fprintf(stderr,
                    "[ALLIED-HQ-ABORT] reason=walk-stuck stage=%u "
                    "human=%08X pos=(%.7g,%.7g) target=(%.7g,%.7g)\n",
                    walk_stage, human, hx, hz,
                    target_x, target_z);
            fflush(stderr);
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            phase = 99u;
            walk_aborted = 1;
            return 1;
        }

        /* Keep unattended diagnostics bounded. A healthy walk from the
         * parked Humvee to the bouncer is well under two minutes even on a
         * cold shader cache. Preserve a frame at the failure point so route
         * errors cannot masquerade as guest hangs. */
        if (elapsed >= (g_recomp_allied_hq_from_checkpoint ? 300000u : 120000u)) {
            const char *capture_path = getenv(
                "MERCENARIES_CAPTURE_ALLIED_HQ_WALK_ABORT_PATH");
            fprintf(stderr,
                    "[ALLIED-HQ-ABORT] reason=walk-timeout stage=%u "
                    "human=%08X pos=(%.7g,%.7g) elapsed=%llu\n",
                    walk_stage, human, hx, hz,
                    (unsigned long long)elapsed);
            fflush(stderr);
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            phase = 99u;
            walk_aborted = 1;
            return 1;
        }

        if (walk_stage + 1u ==
                walk_target_count &&
            bouncer_actor >= 0x00010000u &&
            bouncer_actor <= 0x04000000u - 0xECu) {
            const float actor_x = guest_f32(bouncer_actor + 0xE0u);
            const float actor_z = guest_f32(bouncer_actor + 0xE8u);
            if (isfinite(actor_x) && isfinite(actor_z)) {
                target_x = actor_x;
                target_z = actor_z;
            }
        }
        dx = target_x - hx;
        dz = target_z - hz;
        distance = sqrtf(dx * dx + dz * dz);
        /* Run 483 displayed ENTER ALLIED HQ at 1.08m, where the human
         * collision capsules prevented any closer approach. Arrival only
         * switches the test to Y input; retail still decides whether use is
         * allowed. Never demand overlapping NPC collision capsules. */
        if (distance <= (walk_stage + 1u ==
                         walk_target_count ?
                             1.30f : 3.0f)) {
            fprintf(stderr,
                    "[ALLIED-HQ] reached walk stage=%u human=%08X "
                    "pos=(%.7g,%.7g) target=(%.7g,%.7g) distance=%.7g\n",
                    walk_stage, human, hx, hz,
                    target_x, target_z,
                    distance);
            fflush(stderr);
            if (++walk_stage == walk_target_count) {
                const char *capture_path = getenv(
                    "MERCENARIES_CAPTURE_ALLIED_HQ_APPROACH_PATH");
                if (capture_path != NULL && capture_path[0] != '\0')
                    d3d8_DebugCaptureFrameToPath(capture_path);
                phase = 2u;
                phase_started_ms = now;
            }
            return 1;
        }
        if (red_camera < 0x00010000u ||
            red_camera > 0x04000000u - 0x4Cu)
            return 1;
        dx /= distance;
        dz /= distance;
        forward_x = hx - guest_f32(red_camera + 0x40u);
        forward_z = hz - guest_f32(red_camera + 0x48u);
        forward_length = sqrtf(forward_x * forward_x +
                               forward_z * forward_z);
        if (forward_length < 0.001f)
            return 1;
        forward_x /= forward_length;
        forward_z /= forward_length;
        right_x = -forward_z;
        right_z = forward_x;
        input_x = dx * right_x + dz * right_z;
        input_y = dx * forward_x + dz * forward_z;
        state->Gamepad.sThumbLX = (SHORT)(input_x * 32767.0f);
        state->Gamepad.sThumbLY = (SHORT)(input_y * 32767.0f);
        if (now - last_report_ms >= 750u) {
            fprintf(stderr,
                    "[ALLIED-HQ] walking stage=%u human=%08X "
                    "pos=(%.7g,%.7g) target=(%.7g,%.7g) distance=%.7g "
                    "stick=(%.7g,%.7g)\n",
                    walk_stage, human, hx, hz,
                    target_x, target_z,
                    distance, input_x, input_y);
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }

    if (phase == 2u) {
        const ULONGLONG elapsed = now - phase_started_ms;
        float use_x = 1489.324f;
        float use_z = 836.714f;
        float marker_dx;
        float marker_dz;

        if (bouncer_actor >= 0x00010000u &&
            bouncer_actor <= 0x04000000u - 0xECu) {
            const float actor_x = guest_f32(bouncer_actor + 0xE0u);
            const float actor_z = guest_f32(bouncer_actor + 0xE8u);
            if (isfinite(actor_x) && isfinite(actor_z)) {
                use_x = actor_x;
                use_z = actor_z;
            }
        }
        marker_dx = hx - use_x;
        marker_dz = hz - use_z;

        /* The playable-intro script exposes the bouncer through a one-metre
         * scripted-use objective. Retry bounded Y pulses while standing at
         * the exact authored marker; the first accepted use fades into the
         * HQ briefing and relocates the player to the interior scene. */
        if ((elapsed % 1500u) < 350u &&
            marker_dx * marker_dx + marker_dz * marker_dz <= 2.25f)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;

        if (!captured_interior &&
            (marker_dx * marker_dx + marker_dz * marker_dz > 10000.0f)) {
            const char *capture_path =
                getenv("MERCENARIES_CAPTURE_ALLIED_HQ_INTERIOR_PATH");
            fprintf(stderr,
                    "[ALLIED-HQ] interior transition detected human=%08X "
                    "pos=(%.7g,%.7g) elapsed=%llu\n",
                    human, hx, hz, (unsigned long long)elapsed);
            fflush(stderr);
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            captured_interior = 1;
            phase = 3u;
            g_mercenaries_hq_briefing_active = 1u;
            phase_started_ms = now;
        } else if (elapsed >= 30000u) {
            fprintf(stderr,
                    "[ALLIED-HQ-ABORT] reason=bouncer-use-timeout "
                    "human=%08X pos=(%.7g,%.7g) bouncer=%08X "
                    "target=(%.7g,%.7g) distance=%.7g elapsed=%llu\n",
                    human, hx, hz, bouncer_actor, use_x, use_z,
                    sqrtf(marker_dx * marker_dx + marker_dz * marker_dz),
                    (unsigned long long)elapsed);
            fflush(stderr);
            phase = 99u;
        } else if (now - last_report_ms >= 1000u) {
            fprintf(stderr,
                    "[ALLIED-HQ] using bouncer human=%08X "
                    "pos=(%.7g,%.7g) bouncer=%08X target=(%.7g,%.7g) "
                    "distance=%.7g elapsed=%llu\n",
                    human, hx, hz, bouncer_actor, use_x, use_z,
                    sqrtf(marker_dx * marker_dx + marker_dz * marker_dz),
                    (unsigned long long)elapsed);
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }

    if (phase == 3u) {
        const ULONGLONG elapsed = now - phase_started_ms;

        /* briefing_scripts.lua gives the playable-intro
         * sw_allies1 body a 45.06666667 second duration. The retail script
         * enables starter_trigger after that body and requires normal
         * scripted use within 1.5 metres. Discover its runtime actor instead
         * of assuming an interior coordinate, then approach and press Y. */
        if ((starter_trigger == 0u ||
             starter_trigger > 0x04000000u - 0xECu ||
             guest_u32(starter_trigger + 4u) != 0x7D454737u) &&
            now - last_starter_scan_ms >= 1000u) {
            /* PblHash folds every byte with |0x20, including '_' -> 0x7f. */
            starter_trigger = recomp_find_named_actor(0x7D454737u);
            last_starter_scan_ms = now;
        }

        if (!captured_briefing && elapsed >= 15000u && starter_trigger != 0u) {
            const char *path = getenv("MERCENARIES_CAPTURE_ALLIED_HQ_BRIEFING_PATH");
            if (path != NULL && path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(path);
            captured_briefing = 1;
        }

        if (!captured_mission_accepted &&
            recomp_test_hq_returned_to_world(captured_interior,
                hq_starter_use_requested, hx, hz)) {
            const char *capture_path = getenv(
                "MERCENARIES_CAPTURE_ALLIED_MISSION_ACCEPTED_PATH");
            fprintf(stderr,
                    "[ALLIED-HQ] returned to exterior after starter Y use; "
                    "mission transition observed human=%08X pos=(%.7g,%.7g) "
                    "elapsed=%llu\n", human, hx, hz,
                    (unsigned long long)elapsed);
            fflush(stderr);
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            captured_mission_accepted = 1;
            phase = 4u;
            g_mercenaries_hq_briefing_active = 0u;
            phase_started_ms = now;
            return 1;
        }

        if (elapsed >= 180000u) {
            const char *path = getenv("MERCENARIES_CAPTURE_ALLIED_HQ_BRIEFING_PATH");
            if (path != NULL && path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(path);
            fprintf(stderr, "[ALLIED-HQ-ABORT] reason=briefing-use-timeout "
                    "starter=%08X human=%08X pos=(%.7g,%.7g)\n",
                    starter_trigger, human, hx, hz);
            fflush(stderr);
            phase = 99u;
            return 1;
        }

        if (starter_trigger != 0u &&
            starter_trigger <= 0x04000000u - 0xECu) {
            const float sx = guest_f32(starter_trigger + 0xE0u);
            const float sz = guest_f32(starter_trigger + 0xE8u);

            if (elapsed >= 47000u && isfinite(sx) && isfinite(sz)) {
                /* Run 485 captured STAND UP at HQ. Walking cannot move a
                 * seated player; issue the normal Y interaction after the
                 * briefing, then let its animation finish before approaching. */
                if (hq_standup_requested_ms == 0u) {
                    hq_standup_requested_ms = now;
                    fprintf(stderr, "[ALLIED-HQ] requesting stand-up via Y\n");
                    fflush(stderr);
                }
                if (now - hq_standup_requested_ms < 350u) {
                    state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
                    return 1;
                }
                if (now - hq_standup_requested_ms < 3500u)
                    return 1;
                dx = sx - hx;
                dz = sz - hz;
                distance = sqrtf(dx * dx + dz * dz);
                /* The trigger overlaps the visible starter NPC: stay outside
                 * his collision capsule and inside the authored 1.5m use radius. */
                if (distance > 1.30f) {
                    if (red_camera < 0x00010000u ||
                        red_camera > 0x04000000u - 0x4Cu)
                        return 1;
                    dx /= distance;
                    dz /= distance;
                    forward_x = hx - guest_f32(red_camera + 0x40u);
                    forward_z = hz - guest_f32(red_camera + 0x48u);
                    forward_length = sqrtf(forward_x * forward_x +
                                           forward_z * forward_z);
                    if (forward_length < 0.001f)
                        return 1;
                    forward_x /= forward_length;
                    forward_z /= forward_length;
                    right_x = -forward_z;
                    right_z = forward_x;
                    input_x = dx * right_x + dz * right_z;
                    input_y = dx * forward_x + dz * forward_z;
                    state->Gamepad.sThumbLX = (SHORT)(input_x * 32767.0f);
                    state->Gamepad.sThumbLY = (SHORT)(input_y * 32767.0f);
                } else if ((elapsed % 1500u) < 350u) {
                    state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
                    hq_starter_use_requested = 1;
                }
            }
        }

        if (now - last_report_ms >= 1000u) {
            fprintf(stderr,
                    "[ALLIED-HQ] briefing/starter human=%08X "
                    "pos=(%.7g,%.7g) starter=%08X starter_pos=(%.7g,%.7g) "
                    "elapsed=%llu\n",
                    human, hx, hz, starter_trigger,
                    starter_trigger != 0u ?
                        guest_f32(starter_trigger + 0xE0u) : 0.0f,
                    starter_trigger != 0u ?
                        guest_f32(starter_trigger + 0xE8u) : 0.0f,
                    (unsigned long long)elapsed);
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }

    if (phase >= 4u && getenv("MERCENARIES_TEST_AUTO_TWO_CLUBS") == NULL)
        return 1;

    if (phase == 4u || (phase == 99u && card_walk_aborted)) {
        target_x = card_walk_targets[card_walk_stage][0];
        target_z = card_walk_targets[card_walk_stage][1];
        /* The same bounded numeric navigation input uses stages 100+ here.
         * It changes a destination for ordinary stick input, never actor
         * coordinates, mission flags, collision or the capture state. */
        if (recomp_test_hq_waypoint_override(100u + card_walk_stage, now,
                                             &target_x, &target_z)) {
            phase = 4u;
            phase_started_ms = now;
            card_walk_aborted = 0;
        }
    }

    /* Bounded runs must not leave an unattended input loop at a blocked
     * route node or a missed tutorial indefinitely. */
    if (phase >= 4u && phase <= 6u && now - phase_started_ms >= 180000u) {
        const char *capture_path = getenv("MERCENARIES_CAPTURE_TWO_CLUBS_ABORT_PATH");
        fprintf(stderr, "[TWO-CLUBS-ABORT] phase=%u reason=timeout "
                "player=%08X pos=(%.7g,%.7g)\n", phase, human, hx, hz);
        fflush(stderr);
        if (capture_path != NULL && capture_path[0] != '\0')
            d3d8_DebugCaptureFrameToPath(capture_path);
        card_walk_aborted = phase == 4u;
        phase = 99u;
        return 1;
    }

    if (phase == 4u) {
        const ULONGLONG elapsed = now - phase_started_ms;

        /* The HQ briefing transition briefly leaves the player in its remote
         * interior scene. Wait until retail returns the same live player human
         * to the SW world before applying any movement. */
        if (elapsed < 4000u || hx < 1200.0f || hx > 1800.0f ||
            hz < 500.0f || hz > 1100.0f) {
            if (now - last_report_ms >= 1000u) {
                fprintf(stderr,
                        "[TWO-CLUBS] waiting for exterior player=%08X "
                        "pos=(%.7g,%.7g) elapsed=%llu\n",
                        human, hx, hz, (unsigned long long)elapsed);
                fflush(stderr);
                last_report_ms = now;
            }
            return 1;
        }

        /* Retail can raise modal tutorial/email cards while this ordinary
         * walking route is active. They leave the gameplay substate intact,
         * so blindly waiting looks exactly like collision and eventually
         * consumes the whole route deadline. Track real player motion and
         * acknowledge with A only after it has stopped for three seconds.
         * This also remains harmless at physical obstructions: it never
         * changes actor state, collision, coordinates, or mission flags. */
        if (last_card_walk_motion_ms == 0u ||
            fabsf(hx - last_card_walk_motion_x) > 0.5f ||
            fabsf(hz - last_card_walk_motion_z) > 0.5f) {
            last_card_walk_motion_ms = now;
            last_card_walk_motion_x = hx;
            last_card_walk_motion_z = hz;
        } else if (now - last_card_walk_motion_ms >= 3000u &&
                   ((now - last_card_walk_motion_ms - 3000u) % 1500u) < 300u) {
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        }

        dx = target_x - hx;
        dz = target_z - hz;
        distance = sqrtf(dx * dx + dz * dz);
        if (distance <= 3.0f) {
            fprintf(stderr,
                    "[TWO-CLUBS] reached route stage=%u player=%08X "
                    "pos=(%.7g,%.7g) target=(%.7g,%.7g) distance=%.7g\n",
                    card_walk_stage, human, hx, hz, target_x, target_z,
                    distance);
            fflush(stderr);
            /* Bound each physical leg independently. Modal cards and a long
             * but healthy route must not spend the later stages' timeout. */
            phase_started_ms = now;
            last_card_walk_motion_ms = now;
            last_card_walk_motion_x = hx;
            last_card_walk_motion_z = hz;
            if (++card_walk_stage ==
                    sizeof(card_walk_targets) /
                        sizeof(card_walk_targets[0])) {
                phase = 5u;
                phase_started_ms = now;
                fprintf(stderr,
                        "[TWO-CLUBS] entered source CardEncRunners02X "
                        "recognition region\n");
                fflush(stderr);
            }
            return 1;
        }
        if (red_camera < 0x00010000u ||
            red_camera > 0x04000000u - 0x4Cu)
            return 1;
        dx /= distance;
        dz /= distance;
        forward_x = hx - guest_f32(red_camera + 0x40u);
        forward_z = hz - guest_f32(red_camera + 0x48u);
        forward_length = sqrtf(forward_x * forward_x +
                               forward_z * forward_z);
        if (forward_length < 0.001f)
            return 1;
        forward_x /= forward_length;
        forward_z /= forward_length;
        right_x = -forward_z;
        right_z = forward_x;
        input_x = dx * right_x + dz * right_z;
        input_y = dx * forward_x + dz * forward_z;
        state->Gamepad.sThumbLX = (SHORT)(input_x * 32767.0f);
        state->Gamepad.sThumbLY = (SHORT)(input_y * 32767.0f);
        if (now - last_report_ms >= 750u) {
            fprintf(stderr,
                    "[TWO-CLUBS] walking route stage=%u player=%08X "
                    "pos=(%.7g,%.7g) target=(%.7g,%.7g) distance=%.7g\n",
                    card_walk_stage, human, hx, hz, target_x, target_z,
                    distance);
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }

    if (phase == 5u) {
        const ULONGLONG elapsed = now - phase_started_ms;

        /* PblHash("clubs2") == 0x3A69C438. Follow the real mission actor after
         * Recognition_Start_Clubs2 relocates it to allies1_clubs2_pos rather
         * than baking that mutable position into the test. */
        if ((clubs2_actor < 0x00010000u ||
            clubs2_actor > 0x04000000u - 0x6BAu ||
             guest_u32(clubs2_actor + 4u) != 0x3A69C438u) &&
            now - last_card_scan_ms >= 1000u) {
            clubs2_actor = recomp_find_named_actor(0x3A69C438u);
            last_card_scan_ms = now;
        }

        /* Recognition_Tutorial_Clubs2 appears three seconds after entering the
         * polygon. A is only used to acknowledge that retail message; normal
         * movement resumes afterward. */
        if (elapsed >= 3000u &&
            ((elapsed - 3000u) % 1500u) < 300u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;

        if (elapsed >= 4500u && clubs2_actor >= 0x00010000u &&
            clubs2_actor <= 0x04000000u - 0x6BAu) {
            target_x = guest_f32(clubs2_actor + 0xE0u);
            target_z = guest_f32(clubs2_actor + 0xE8u);
            if (isfinite(target_x) && isfinite(target_z)) {
                dx = target_x - hx;
                dz = target_z - hz;
                distance = sqrtf(dx * dx + dz * dz);
                /* IsUsableToThisActor requires <2m, not the 5m tutorial
                 * trigger radius. Close to 1.25m before attempting bash. */
                if (distance <= 1.25f) {
                    phase = 6u;
                    phase_started_ms = now;
                    fprintf(stderr,
                            "[TWO-CLUBS] bash range player=%08X "
                            "pos=(%.7g,%.7g) clubs2=%08X "
                            "target=(%.7g,%.7g) distance=%.7g\n",
                            human, hx, hz, clubs2_actor, target_x, target_z,
                            distance);
                    fflush(stderr);
                    return 1;
                }
                if (distance > 0.001f && red_camera >= 0x00010000u &&
                    red_camera <= 0x04000000u - 0x4Cu) {
                    dx /= distance;
                    dz /= distance;
                    forward_x = hx - guest_f32(red_camera + 0x40u);
                    forward_z = hz - guest_f32(red_camera + 0x48u);
                    forward_length = sqrtf(forward_x * forward_x +
                                           forward_z * forward_z);
                    if (forward_length >= 0.001f) {
                        forward_x /= forward_length;
                        forward_z /= forward_length;
                        right_x = -forward_z;
                        right_z = forward_x;
                        input_x = dx * right_x + dz * right_z;
                        input_y = dx * forward_x + dz * forward_z;
                        state->Gamepad.sThumbLX =
                            (SHORT)(input_x * 32767.0f);
                        state->Gamepad.sThumbLY =
                            (SHORT)(input_y * 32767.0f);
                    }
                }
            }
        }
        if (now - last_report_ms >= 1000u) {
            fprintf(stderr,
                    "[TWO-CLUBS] recognition/approach player=%08X "
                    "pos=(%.7g,%.7g) clubs2=%08X target=(%.7g,%.7g) "
                    "elapsed=%llu\n",
                    human, hx, hz, clubs2_actor,
                    clubs2_actor >= 0x00010000u ?
                        guest_f32(clubs2_actor + 0xE0u) : 0.0f,
                    clubs2_actor >= 0x00010000u ?
                        guest_f32(clubs2_actor + 0xE8u) : 0.0f,
                    (unsigned long long)elapsed);
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }

    if (phase == 6u) {
        const ULONGLONG elapsed = now - phase_started_ms;
        const ULONGLONG cycle = elapsed >= 3500u ?
            (elapsed - 3500u) % 3000u : 0u;

        /* Allies1_Tutorial_Bash is raised within five metres. Dismiss it with
         * A, then exercise the documented retail sequence: X bash followed
         * quickly by Y action/subdue. Repeating the pair tolerates animation
         * timing without altering the target or mission state. */
        if (elapsed < 3000u && (elapsed % 1200u) < 300u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        /* Retail SubduedState::Enter (0x00050160) writes _bIsSubdued at
         * human+0x6B9. Observe it; never synthesize capture completion. */
        const int subdued = clubs2_actor >= 0x00010000u &&
            clubs2_actor <= 0x04000000u - 0x6BAu &&
            guest_u32(clubs2_actor + 4u) == 0x3A69C438u &&
            *(const uint8_t *)guest_ptr(clubs2_actor + 0x6B9u) != 0u;
        int in_use_range = 0;
        if (!subdued && elapsed >= 3500u &&
            clubs2_actor >= 0x00010000u && clubs2_actor <= 0x04000000u - 0x6BAu &&
            guest_u32(clubs2_actor + 4u) == 0x3A69C438u) {
            dx = guest_f32(clubs2_actor + 0xE0u) - hx;
            dz = guest_f32(clubs2_actor + 0xE8u) - hz;
            distance = sqrtf(dx * dx + dz * dz);
            in_use_range = isfinite(distance) && distance < 2.0f;
            /* The target can move after the approach phase. Keep following
             * through normal controller movement instead of bashing empty
             * space for the rest of the timeout. Stop walking within 1.5m. */
            if (isfinite(distance) && distance > 1.5f &&
                red_camera >= 0x00010000u && red_camera <= 0x04000000u - 0x4Cu) {
                forward_x = hx - guest_f32(red_camera + 0x40u);
                forward_z = hz - guest_f32(red_camera + 0x48u);
                forward_length = sqrtf(forward_x * forward_x + forward_z * forward_z);
                if (forward_length >= 0.001f) {
                    forward_x /= forward_length;
                    forward_z /= forward_length;
                    input_x = (-dx * forward_z + dz * forward_x) / distance;
                    input_y = (dx * forward_x + dz * forward_z) / distance;
                    state->Gamepad.sThumbLX = (SHORT)(input_x * 32767.0f);
                    state->Gamepad.sThumbLY = (SHORT)(input_y * 32767.0f);
                }
            }
        }
        if (!subdued && in_use_range && elapsed >= 3500u && cycle < 300u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_X] = 255;
        if (!subdued && in_use_range && elapsed >= 3500u && cycle >= 650u && cycle < 1150u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;

        if (!captured_subdue_probe && subdued) {
            const char *capture_path = getenv(
                "MERCENARIES_CAPTURE_TWO_CLUBS_SUBDUE_PATH");
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            fprintf(stderr,
                    "[TWO-CLUBS] retail subdued flag observed; "
                    "player=%08X clubs2=%08X target_vtable=%08X "
                    "target_flags=%08X elapsed=%llu\n",
                    human, clubs2_actor,
                    clubs2_actor >= 0x00010000u ?
                        guest_u32(clubs2_actor) : 0u,
                    clubs2_actor >= 0x00010000u ?
                        guest_u32(clubs2_actor + 0x50u) : 0u,
                    (unsigned long long)elapsed);
            fflush(stderr);
            captured_subdue_probe = 1;
            phase = 7u;
            phase_started_ms = now;
        }
        return 1;
    }

    if (phase == 7u) {
        const ULONGLONG elapsed = now - phase_started_ms;
        const uint32_t main_state = guest_u32(0x00413F6Cu);
        const uint32_t substate = guest_u32(0x00413F68u);
        /* RsJoystickManager maps support-item selection to D-pad up/down.
         * Require the real RsMain SupportMenu transition, not merely a
         * continued input-poll heartbeat, before calling controls responsive.
         * A only acknowledges the initial retail support tutorial. */
        if (main_state == 0x4249D707u && substate == 0xDDFB69D8u &&
            elapsed >= 1000u && (elapsed % 1500u) < 300u)
            state->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
        if (elapsed >= 6000u && main_state == 0x4249D707u &&
            substate == 0xC2CBD863u &&
            ((elapsed - 6000u) % 1500u) < 300u)
            state->Gamepad.wButtons |= XBOX_GAMEPAD_DPAD_DOWN;
        if (elapsed >= 6000u && main_state == 0x4249D707u &&
            substate == 0xC739FD0Fu) {
            fprintf(stderr,
                    "[TWO-CLUBS] retail support menu opened after D-pad "
                    "input; player=%08X clubs2=%08X elapsed=%llu\n",
                    human, clubs2_actor, (unsigned long long)elapsed);
            fflush(stderr);
            phase = 8u;
            phase_started_ms = now;
            return 1;
        }
        if (elapsed >= 25000u) {
            const char *capture_path = getenv("MERCENARIES_CAPTURE_TWO_CLUBS_ABORT_PATH");
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            fprintf(stderr,
                    "[TWO-CLUBS-ABORT] reason=support-menu-open-timeout "
                    "state=%08X/%08X\n", main_state, substate);
            fflush(stderr);
            phase = 99u;
            return 1;
        }
        if (now - last_report_ms >= 1000u) {
            fprintf(stderr,
                    "[TWO-CLUBS] post-subdue input poll player=%08X "
                    "clubs2=%08X state=%08X/%08X elapsed=%llu\n",
                    human, clubs2_actor, main_state, substate,
                    (unsigned long long)elapsed);
            fflush(stderr);
            last_report_ms = now;
        }
        return 1;
    }

    if (phase == 8u) {
        const ULONGLONG elapsed = now - phase_started_ms;
        const uint32_t main_state = guest_u32(0x00413F6Cu);
        const uint32_t substate = guest_u32(0x00413F68u);
        /* Capture a rendered menu before the normal Y/cancel action.
         * Observing it open AND close rejects the reported input-loss bug
         * more strongly than a timed sequence of unacknowledged presses. */
        if (elapsed >= 750u && substate == 0xC739FD0Fu) {
            if (!captured_support_menu) {
                const char *capture_path = getenv(
                    "MERCENARIES_CAPTURE_TWO_CLUBS_SUPPORT_PATH");
                if (capture_path != NULL && capture_path[0] != '\0')
                    d3d8_DebugCaptureFrameToPath(capture_path);
                captured_support_menu = 1;
            }
            if (((elapsed - 750u) % 1500u) < 300u) {
                state->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] = 255;
                support_cancel_sent = 1;
            }
        }
        if (main_state == 0x4249D707u && substate == 0xC2CBD863u) {
            if (support_cancel_sent) {
                fprintf(stderr,
                        "[TWO-CLUBS] retail support menu opened and closed "
                        "through normal input; extraction and verification "
                        "NOT YET TESTED\n");
            } else {
                fprintf(stderr,
                        "[TWO-CLUBS-ABORT] reason=support-menu-closed-without-cancel\n");
            }
            fflush(stderr);
            phase = 99u;
            return 1;
        }
        if (elapsed >= 15000u) {
            const char *capture_path = getenv("MERCENARIES_CAPTURE_TWO_CLUBS_ABORT_PATH");
            if (capture_path != NULL && capture_path[0] != '\0')
                d3d8_DebugCaptureFrameToPath(capture_path);
            fprintf(stderr,
                    "[TWO-CLUBS-ABORT] reason=support-menu-close-timeout "
                    "state=%08X/%08X\n", main_state, substate);
            fflush(stderr);
            phase = 99u;
        }
        return 1;
    }

    return 1;
}

static void recomp_input_ensure_initialized(void)
{
    if (!g_recomp_input_initialized) {
        xbox_InputInit();
        if (getenv("MERCENARIES_TRACE_VEHICLE_FRAMES") != NULL) {
            g_vehicle_frame_trace = calloc(4096u, sizeof(RecompVehicleFrame));
            if (g_vehicle_frame_trace)
                d3d8_DebugSetSubmissionCallback(recomp_vehicle_submission_trace);
        }
        if (getenv("MERCENARIES_TRACE_CAMERA_TRANSITION") != NULL ||
            getenv("MERCENARIES_TRACE_CAMERA_MODE") != NULL ||
            getenv("MERCENARIES_TRACE_AIRCRAFT_TARGETS") != NULL)
            d3d8_DebugSetPresentCallback(
                recomp_camera_present_trace_callback);
        g_recomp_input_initialized = 1;
    }
}
static uint32_t recomp_input_scan_devices(void)
{
    uint32_t mask = 0u;
    uint32_t port;
    XBOX_INPUT_STATE state;

    recomp_input_ensure_initialized();
    for (port = 0u; port < XBOX_MAX_CONTROLLERS; ++port) {
        if (xbox_InputGetState(port, &state) == ERROR_SUCCESS)
            mask |= 1u << port;
    }
    return mask;
}

void recomp_xinput_init_devices(void)
{
    recomp_input_ensure_initialized();
}
uint32_t recomp_xinput_get_devices(void)
{
    g_recomp_input_device_mask = recomp_input_scan_devices();
    return g_recomp_input_device_mask;
}

uint32_t recomp_xinput_get_device_changes(uint32_t insertions_address,
                                           uint32_t removals_address)
{
    const uint32_t current = recomp_input_scan_devices();
    const uint32_t insertions = current & ~g_recomp_input_device_mask;
    const uint32_t removals = g_recomp_input_device_mask & ~current;

    if (insertions_address >= 0x00010000u &&
        insertions_address <= 0x04000000u - 4u)
        *(uint32_t *)guest_ptr(insertions_address) = insertions;
    if (removals_address >= 0x00010000u &&
        removals_address <= 0x04000000u - 4u)
        *(uint32_t *)guest_ptr(removals_address) = removals;
    g_recomp_input_device_mask = current;
    return (insertions | removals) != 0u;
}

uint32_t recomp_xinput_open(uint32_t port)
{
    recomp_input_ensure_initialized();
    if (port >= XBOX_MAX_CONTROLLERS || !xbox_InputIsConnected(port))
        return 0u;
    return port + 1u;
}

void recomp_xinput_close(uint32_t handle)
{
    (void)handle;
}

uint32_t recomp_xinput_get_capabilities(uint32_t handle,
                                         uint32_t capabilities_address)
{
    XBOX_INPUT_CAPABILITIES capabilities;
    DWORD result;
    const uint32_t port = handle - 1u;

    memset(&capabilities, 0, sizeof(capabilities));
    result = xbox_InputGetCapabilities(port, 0u, &capabilities);
    if (result == ERROR_SUCCESS && capabilities_address >= 0x00010000u &&
        capabilities_address <= 0x04000000u - 28u) {
        memset(guest_ptr(capabilities_address), 0, 28u);
        memcpy(guest_ptr(capabilities_address), &capabilities,
               sizeof(capabilities) < 28u ? sizeof(capabilities) : 28u);
    }
    return result;
}

uint32_t recomp_xinput_get_state(uint32_t handle, uint32_t state_address)
{
    static uint32_t call_count;
    static WORD last_buttons = 0xFFFFu;
    static BYTE last_y = 0xFFu;
    static int move_capture_started;
    static ULONGLONG move_capture_deadline_ms;
    static ULONGLONG input_poll_capture_deadline_ms;
    static int input_poll_capture_done;
    XBOX_INPUT_STATE state;
    DWORD result;
    const uint32_t port = handle - 1u;
    const ULONGLONG now = GetTickCount64();

    if (getenv("MERCENARIES_TRACE_MUSIC") != NULL) {
        static ULONGLONG last_music_poll_ms;
        if (now - last_music_poll_ms >= 1000u) {
            last_music_poll_ms = now;
            recomp_music_checkpoint(20u, 0x0037B9B4u, 0u);
        }
    }

    recomp_controls_state(guest_u32(0x00413F6Cu),guest_u32(0x00413F68u),
                          guest_u32(0x00323ACCu),guest_u8(0x0030EBA5u));
    recomp_controls_substate(guest_u32(0x00323AD0u));
    /* RsMain::_bControllerDisconnected (retail CheckControllerStatus at
     * 0x17A710). Confirm acknowledges the original Start-only handshake. */
    recomp_controls_waiting_controller(guest_u8(0x00413FBCu)!=0);
    recomp_input_ensure_initialized();
    if (getenv("MERCENARIES_TRACE_HUD_ACTION_BUTTON") != NULL) {
        static uint32_t last_x = UINT32_MAX;
        static uint32_t last_y = UINT32_MAX;
        static uint32_t last_screen = UINT32_MAX;
        static uint32_t last_brush = UINT32_MAX;
        const uint32_t x = guest_u32(0x0030E69Cu);
        const uint32_t y = guest_u32(0x0030E6A0u);
        const uint32_t screen = guest_u32(0x0035C904u);
        const uint32_t brush = guest_u32(0x0035C908u);
        if (x != last_x || y != last_y || screen != last_screen ||
            brush != last_brush) {
            fprintf(stderr,
                    "[HUD-ACTION-CONFIG] x=%g/%08X y=%g/%08X "
                    "screen=%u brush=%u\n",
                    guest_f32(0x0030E69Cu), x,
                    guest_f32(0x0030E6A0u), y, screen, brush);
            fflush(stderr);
            last_x = x;
            last_y = y;
            last_screen = screen;
            last_brush = brush;
        }
    }
    if (!input_poll_capture_done) {
        const char *capture_path =
            getenv("MERCENARIES_CAPTURE_INPUT_POLL_PATH");
        if (capture_path != NULL && capture_path[0] != '\0') {
            if (input_poll_capture_deadline_ms == 0u) {
                const char *delay_text =
                    getenv("MERCENARIES_CAPTURE_INPUT_POLL_DELAY_MS");
                const ULONGLONG delay_ms =
                    delay_text != NULL && delay_text[0] != '\0' ?
                        strtoull(delay_text, NULL, 0) : 90000u;
                input_poll_capture_deadline_ms = now + delay_ms;
            } else if (now >= input_poll_capture_deadline_ms) {
                d3d8_DebugCaptureFrameToPath(capture_path);
                input_poll_capture_done = 1;
            }
        }
    }
    if (g_test_auto_a_stop_deadline_ms != 0u &&
        now >= g_test_auto_a_stop_deadline_ms) {
        xbox_InputStopTestAutoA();
        g_test_auto_a_stop_deadline_ms = 0u;
    }
    if (g_test_standup_capture_deadline_ms != 0u &&
        now >= g_test_standup_capture_deadline_ms) {
        d3d8_PresentFrame();
        g_test_standup_capture_deadline_ms = 0u;
    }
    if (g_test_game_capture_deadline_ms != 0u &&
        now >= g_test_game_capture_deadline_ms) {
        const char *capture_path = getenv("MERCENARIES_CAPTURE_GAME_PATH");
        if (capture_path != NULL && capture_path[0] != '\0') {
            d3d8_DebugArmFlipCapture(capture_path);
            d3d8_PresentFrame();
        }
        g_test_game_capture_deadline_ms = 0u;
    }
    if (g_test_post_aircraft_capture_deadline_ms != 0u &&
        now >= g_test_post_aircraft_capture_deadline_ms) {
        const char *capture_path =
            getenv("MERCENARIES_CAPTURE_POST_AIRCRAFT_PATH");
        if (capture_path != NULL && capture_path[0] != '\0') {
            /* The aircraft exit cutscene can reach a stretch with no later
             * swapchain flip. Capture the current presented buffer directly
             * instead of arming a request that may never be consumed. */
            d3d8_DebugCaptureFrameToPath(capture_path);
        }
        g_test_post_aircraft_capture_deadline_ms = 0u;
    }
    recomp_trace_live_human_physics(now);
    recomp_trace_live_camera_transition(now);
    recomp_trace_aircraft_targets(now);
    recomp_service_scheduled_camera_captures(now);
    /* The retail aircraft-exit transition can spend tens of seconds in a
     * synchronous D3D wait before the Humvee route is active.  Arm only once
     * the route has a valid live vehicle, otherwise a healthy transition is
     * reported as a host/guest stall and obscures genuine late-route hangs. */
    if (g_recomp_post_aircraft_gate_route_started ||
        getenv("MERCENARIES_TRACE_GAME_STALL_MS") != NULL)
        recomp_arm_game_stall_watchdog();
    memset(&state, 0, sizeof(state));
    result = xbox_InputGetState(port, &state);
    if (port == 0u) {
        xbox_InputApplyTestExactLoadGame(&state);
        recomp_apply_test_survivor_flash();
    }
    if (port == 0u && recomp_apply_test_gamepad_command(&state, now))
        goto test_gamepad_ready;
    recomp_apply_test_pda_route(&state, now);
    recomp_apply_test_aircraft_pickup_route(&state, now);
    if (!recomp_apply_test_post_aircraft_gate_route(&state, now) &&
        !recomp_apply_test_allied_hq_entry(&state, now) &&
        g_recomp_aircraft_route_complete &&
        getenv("MERCENARIES_TEST_AUTO_DRIVE_AFTER_AIRCRAFT") != NULL) {
        static int drive_logged;
        static ULONGLONG last_vehicle_report_ms;
        const char *delay_text = getenv(
            "MERCENARIES_TEST_AUTO_DRIVE_AFTER_AIRCRAFT_DELAY_MS");
        const char *duration_text = getenv(
            "MERCENARIES_TEST_AUTO_DRIVE_AFTER_AIRCRAFT_DURATION_MS");
        const char *steer_text = getenv(
            "MERCENARIES_TEST_AUTO_DRIVE_STEER_X");
        const char *steer_duration_text = getenv(
            "MERCENARIES_TEST_AUTO_DRIVE_STEER_DURATION_MS");
        const ULONGLONG delay_ms =
            delay_text && delay_text[0] != '\0' ?
            strtoull(delay_text, NULL, 0) : 50000u;
        const ULONGLONG duration_ms =
            duration_text && duration_text[0] != '\0' ?
            strtoull(duration_text, NULL, 0) : 20000u;
        const ULONGLONG steer_duration_ms =
            steer_duration_text && steer_duration_text[0] != '\0' ?
            strtoull(steer_duration_text, NULL, 0) : duration_ms;
        const ULONGLONG elapsed = now - g_recomp_aircraft_route_complete_ms;
        if (elapsed >= delay_ms && elapsed < delay_ms + duration_ms) {
            long steer = steer_text && steer_text[0] != '\0' ?
                strtol(steer_text, NULL, 0) : 0;
            if (steer < -32768L)
                steer = -32768L;
            else if (steer > 32767L)
                steer = 32767L;
            state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
            if (elapsed - delay_ms < steer_duration_ms)
                state.Gamepad.sThumbLX = (SHORT)steer;
            if (!drive_logged) {
                fprintf(stderr,
                        "[AIRCRAFT-ROUTE] post-aircraft A/accelerate hold started "
                        "elapsed=%llu duration=%llu steer=%ld "
                        "steer_duration=%llu\n",
                        (unsigned long long)elapsed,
                        (unsigned long long)duration_ms, steer,
                        (unsigned long long)steer_duration_ms);
                if (getenv("MERCENARIES_TRACE_APU_ROUTE_STAGES") != NULL)
                    mcpx_apu_debug_dump_stream_voices();
                fflush(stderr);
                drive_logged = 1;
            }
            if (getenv("MERCENARIES_TRACE_POST_GATE_VEHICLE") != NULL &&
                g_recomp_live_vehicle_actor >= 0x00010000u &&
                g_recomp_live_vehicle_actor <= 0x03FFF000u &&
                now - last_vehicle_report_ms >= 1000u) {
                const uint32_t actor = g_recomp_live_vehicle_actor;
                fprintf(stderr,
                        "[POST-GATE-VEHICLE] actor=%08X pos=(%.7g,%.7g,%.7g) "
                        "forward=(%.7g,%.7g) right=(%.7g,%.7g) "
                        "elapsed=%llu\n",
                        actor, guest_f32(actor + 0xE0u),
                        guest_f32(actor + 0xE4u), guest_f32(actor + 0xE8u),
                        -guest_f32(actor + 0xD0u),
                        -guest_f32(actor + 0xD8u),
                        guest_f32(actor + 0xB0u),
                        guest_f32(actor + 0xB8u),
                        (unsigned long long)elapsed);
                fflush(stderr);
                last_vehicle_report_ms = now;
            }
        }
    }
    if (state.Gamepad.sThumbLY != 0 && !move_capture_started) {
        const char *start_path =
            getenv("MERCENARIES_CAPTURE_AUTO_MOVE_START_PATH");
        const char *delay_text =
            getenv("MERCENARIES_CAPTURE_AUTO_MOVE_DELAY_MS");
        if (start_path != NULL && start_path[0] != '\0')
            d3d8_DebugArmFlipCapture(start_path);
        if (getenv("MERCENARIES_TRACE_APU_VOICES") != NULL ||
            getenv("MERCENARIES_TRACE_APU_ROUTE_STAGES") != NULL)
            mcpx_apu_debug_dump_stream_voices();
        move_capture_deadline_ms = now +
            (delay_text != NULL && delay_text[0] != '\0' ?
                 strtoul(delay_text, NULL, 0) : 8000u);
        move_capture_started = 1;
    }
    if (move_capture_deadline_ms != 0u &&
        now >= move_capture_deadline_ms) {
        const char *path = getenv("MERCENARIES_CAPTURE_AUTO_MOVE_PATH");
        if (path != NULL && path[0] != '\0') {
            d3d8_DebugArmFlipCapture(path);
            d3d8_PresentFrame();
        }
        move_capture_deadline_ms = 0u;
    }
test_gamepad_ready:
    if(port==0u)recomp_controls_freecam_input(&state);
    ++call_count;
    if (getenv("MERCENARIES_TRACE_INPUT") != NULL &&
        (call_count <= 4u || state.Gamepad.wButtons != last_buttons ||
         state.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] != last_y)) {
        const uint32_t main_joystick = guest_u32(0x00413F90u);
        fprintf(stderr,
                "Retail XInput bridge: call=%u handle=%u port=%u state=%08X "
                "host_size=%u xbox_size=22 result=%lu packet=%lu buttons=%04X y=%u "
                "main=%08X vtable=%08X linked=%08X current=%d standby=%u "
                "down=%04X pressed=%04X\n",
                call_count, handle, port, state_address,
                (unsigned int)sizeof(state), (unsigned long)result,
                (unsigned long)state.dwPacketNumber,
                (unsigned int)state.Gamepad.wButtons,
                (unsigned int)state.Gamepad.bAnalogButtons[XBOX_BUTTON_Y],
                main_joystick,
                main_joystick ? guest_u32(main_joystick) : 0u,
                main_joystick ? guest_u32(main_joystick + 0x0Cu) : 0u,
                main_joystick ? (int32_t)guest_u32(main_joystick + 0x1Cu) : -2,
                main_joystick ? (unsigned int)guest_u8(main_joystick + 0xCEu) : 0u,
                main_joystick ? guest_u32(main_joystick + 0xC0u) : 0u,
                main_joystick ? guest_u32(main_joystick + 0xC4u) : 0u);
        fflush(stderr);
        last_buttons = state.Gamepad.wButtons;
        last_y = state.Gamepad.bAnalogButtons[XBOX_BUTTON_Y];
    }
    if (result == ERROR_SUCCESS && state_address >= 0x00010000u &&
        state_address <= 0x04000000u - 22u) {
        memcpy(guest_ptr(state_address), &state, 22u);
    }
    return result;
}

uint32_t recomp_xinput_set_state(uint32_t handle, uint32_t feedback_address)
{
    XBOX_VIBRATION vibration;
    const uint32_t port = handle - 1u;

    memset(&vibration, 0, sizeof(vibration));
    if (feedback_address >= 0x00010000u &&
        feedback_address <= 0x04000000u - 0x46u) {
        vibration.wLeftMotorSpeed = guest_u16(feedback_address + 0x42u);
        vibration.wRightMotorSpeed = guest_u16(feedback_address + 0x44u);
    }
    return xbox_InputSetState(port, &vibration);
}
void recomp_input_logical_checkpoint(uint32_t joystick_address)
{
    recomp_dev_missions_tick();
    static uint32_t samples;
    static uint32_t last_current = UINT32_MAX;
    static uint32_t last_down = UINT32_MAX;
    static uint32_t last_pressed = UINT32_MAX;
    static uint32_t use_trace_armed;
    static uint32_t use_trace_done;
    uint32_t current;
    uint32_t down;
    uint32_t pressed;

    if (getenv("MERCENARIES_TRACE_INPUT") == NULL ||
        joystick_address < 0x00010000u ||
        joystick_address > 0x04000000u - 0xD0u)
        return;
    current = guest_u32(joystick_address + 0x1Cu);
    down = guest_u32(joystick_address + 0xC0u);
    pressed = guest_u32(joystick_address + 0xC4u);
    if (use_trace_armed != 0u && use_trace_done == 0u) {
        const uint32_t end = g_recomp_recent_game_func_idx;
        const uint32_t count = end < 256u ? end : 256u;
        fprintf(stderr, "[USE-DISPATCH-AFTER] count=%u", count);
        for (uint32_t i = 0u; i < count; ++i) {
            if ((i & 7u) == 0u)
                fprintf(stderr, "\n  ");
            fprintf(stderr, "%08X ",
                    g_recomp_recent_game_funcs[(end - count + i) & 255u]);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
        use_trace_armed = 0u;
        use_trace_done = 1u;
    }
    if ((pressed & 0x10u) != 0u && use_trace_done == 0u)
        use_trace_armed = 1u;
    if (samples < 6u || current != last_current || down != last_down ||
        pressed != last_pressed) {
        fprintf(stderr,
                "Retail logical input: joystick=%08X current=%d standby=%u "
                "down=%04X pressed=%04X released=%04X "
                "pad0_data=%02X%02X pad0_old=%04X\n",
                joystick_address, (int32_t)current,
                (unsigned int)guest_u8(joystick_address + 0xCEu), down,
                pressed, guest_u32(joystick_address + 0xC8u),
                (unsigned int)guest_u8(0x007AC848u),
                (unsigned int)guest_u8(0x007AC849u),
                (unsigned int)guest_u16(0x007AC85Au));
        fflush(stderr);
        ++samples;
        last_current = current;
        last_down = down;
        last_pressed = pressed;
    }
}
void recomp_player_control_checkpoint(uint32_t stage, uint32_t player,
                                      uint32_t control, uint32_t value_bits,
                                      uint32_t actor)
{
    static uint32_t gather_samples;
    static uint32_t last_gather = UINT32_MAX;
    static uint32_t last_stage3_actor = UINT32_MAX;
    const uint32_t joystick_state = guest_u32(0x00323ACCu);
    float value = 0.0f;

    /* This checkpoint belongs to RsAiPlayer::ProcessControl. Preserve its
     * authoritative AI pointer even when verbose input tracing is disabled;
     * test routes use it to resolve Human -> RiderSeat -> owning vehicle. */
    if (player >= 0x00010000u && player <= 0x04000000u - 0xD50u)
        g_recomp_live_player_ai = player;

    if (getenv("MERCENARIES_TRACE_INPUT") == NULL || control != 0x2Bu)
        return;
    if (stage == 3u && actor == last_stage3_actor)
        return;
    if (stage == 3u)
        last_stage3_actor = actor;
    if ((stage == 4u || stage == 5u) && joystick_state != 14u)
        return;
    if ((stage == 4u || stage == 5u) && gather_samples >= 32u &&
        actor == last_gather)
        return;
    if (stage == 4u || stage == 5u) {
        ++gather_samples;
        last_gather = actor;
    }
    memcpy(&value, &value_bits, sizeof(value));
    fprintf(stderr,
            "Retail player use: stage=%u player=%08X state=%08X "
            "vehicle=%08X value=%g actor=%08X guid=%08X "
            "joy=%u/%u enabled=%u\n",
            stage, player,
            player >= 0x00010000u && player <= 0x04000000u - 0xD50u
                ? guest_u32(player + 0x18u) : 0u,
            player >= 0x00010000u && player <= 0x04000000u - 0xD50u
                ? guest_u32(player + 0x998u) : 0u,
            value, actor,
            player >= 0x00010000u && player <= 0x04000000u - 0xD50u
                ? guest_u32(player + 0xD10u) : 0u,
            joystick_state, guest_u32(0x00323AD0u),
            (unsigned int)guest_u8(0x0030EBA5u));
    if (stage == 4u && actor != 0u) {
        const uint32_t shown = actor < 8u ? actor : 8u;
        fputs("  obstacle actors:", stderr);
        for (uint32_t i = 0u; i < shown; ++i)
            fprintf(stderr, " %08X", guest_u32(0x00378EFCu + i * 0x28u));
        fputc('\n', stderr);
    }
    fflush(stderr);
}
void recomp_player_vehicle_exit_checkpoint(uint32_t stage, uint32_t player_ai)
{
    static int enabled = -1;
    uint32_t state;
    uint32_t human;
    uint32_t seat;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_PLAYER_VEHICLE_EXIT") != NULL;
    if (!enabled || player_ai < 0x00010000u ||
        player_ai > 0x04000000u - 0xD00u)
        return;

    g_recomp_live_player_ai = player_ai;
    state = guest_u32(player_ai + 0x18u);
    human = guest_u32(player_ai + 0x998u);
    seat = human >= 0x00010000u && human <= 0x04000000u - 0x76Cu
        ? guest_u32(human + 0x768u) : 0u;
    fprintf(stderr,
            "[PLAYER-VEHICLE-EXIT] %s ai=%08X state=%08X "
            "expected-exit=%08X in-progress=%u human=%08X seat=%08X "
            "rider-state=%u exit-vehicle=%08X driver=%u crawling=%u\n",
            stage == 0u ? "started" : "finished", player_ai, state,
            player_ai + 0xCD4u, (unsigned int)guest_u8(player_ai + 0x55Du),
            human, seat,
            seat >= 0x00010000u && seat <= 0x04000000u - 0x11Cu
                ? guest_u32(seat + 0x118u) : 0u,
            guest_u32(player_ai + 0xCE0u),
            (unsigned int)guest_u8(player_ai + 0xCF0u),
            (unsigned int)guest_u8(player_ai + 0xCF1u));
    fflush(stderr);
}

void recomp_human_collision_checkpoint(uint32_t stage, uint32_t actor,
                                       uint32_t enable,
                                       uint32_t obstacle)
{
    static uint32_t sequence;
    static uint32_t last_actor;
    static uint32_t last_enable = UINT32_MAX;
    static uint32_t last_obstacle;
    static int saw_disabled;
    static int standup_capture_armed;
    static int auto_y_armed_from_collision;
    const int valid = actor >= 0x00010000u &&
                      actor <= 0x04000000u - 0x800u;

    if (valid && guest_u32(actor) == 0x002E32B8u && stage == 1u &&
        !enable && !auto_y_armed_from_collision &&
        getenv("MERCENARIES_TEST_AUTO_Y_AFTER_COLLISION_DISABLED") != NULL) {
        auto_y_armed_from_collision = 1;
        xbox_InputArmTestAutoY();
    }

    /* This diagnostic pointer represents Jacobs, not the last Human whose
     * collision state changed.  The same checkpoint is shared by NPCs, so
     * accepting every valid Human made movement/camera observers silently
     * switch actors in populated scenes.  HumanPlayer's retail vtable is
     * already the authoritative predicate used by the stand-up arm above. */
    if (valid && guest_u32(actor) == 0x002E32B8u)
        g_recomp_live_human_actor = actor;
    if (stage == 1u && !enable)
        saw_disabled = 1;
    if (stage == 1u && !enable &&
        getenv("MERCENARIES_TRACE_CAMERA_TRANSITION") != NULL) {
        const ULONGLONG now = GetTickCount64();
        /* Pickup interactions occur after the authored stand-up animation and
         * a short walk through the C-17.  Keep the diagnostic window open
         * long enough to include the first interaction camera and its return. */
        g_test_camera_trace_until_ms = now + 60000u;
        g_test_camera_trace_next_ms = now;
        g_test_camera_trace_samples = 0u;
        if (g_mercenaries_standup_complete &&
            getenv("MERCENARIES_CAPTURE_INTERACTION_PATH") != NULL) {
            const char *delay_text =
                getenv("MERCENARIES_CAPTURE_INTERACTION_DELAY_MS");
            const unsigned long delay = delay_text && delay_text[0] != '\0' ?
                strtoul(delay_text, NULL, 0) : 0u;
            g_test_interaction_capture_at_ms = now + delay;
        }
    }
    if (stage == 2u && enable && saw_disabled) {
        g_mercenaries_standup_complete = 1u;
        if (getenv("MERCENARIES_TEST_AUTO_Y_STOP_AFTER_STANDUP") != NULL)
            xbox_InputDisarmTestAutoY();
        /* Keep diagnostics tied to the authoritative collision transition.
         * The script prompt checkpoint can be bypassed by fast movie skips,
         * while collision re-enable is the actual completed stand-up event. */
        if (getenv("MERCENARIES_CAPTURE_GAMEPLAY_AFTER_USE_ONLY") != NULL)
            g_mercenaries_gameplay_capture_active = 1u;
    }
    if (getenv("MERCENARIES_TRACE_HUMAN_COLLISION") == NULL)
        return;

    if (stage == 2u && enable && saw_disabled && !standup_capture_armed) {
        const char *capture_path = getenv("MERCENARIES_CAPTURE_STANDUP_PATH");
        if (capture_path && capture_path[0] != '\0') {
            d3d8_DebugArmFlipCapture(capture_path);
            g_test_standup_capture_deadline_ms = GetTickCount64() + 3000u;
            standup_capture_armed = 1;
        }
    }

    if (stage == 2u && enable && saw_disabled &&
        getenv("MERCENARIES_TEST_AUTO_MOVE_AFTER_STANDUP") != NULL)
        xbox_InputArmTestAutoMove();

    if (stage == 2u && enable && saw_disabled &&
        (getenv("MERCENARIES_TRACE_HUMAN_PHYSICS") != NULL ||
         getenv("MERCENARIES_TEST_AUTO_MOVE_AFTER_STANDUP") != NULL)) {
        const ULONGLONG now = GetTickCount64();
        g_test_standup_trace_until_ms = now + 5000u;
        g_test_standup_trace_next_ms = now;
        g_test_standup_trace_samples = 0u;
    }
    if (stage == 2u && enable && saw_disabled &&
        getenv("MERCENARIES_TRACE_CAMERA_TRANSITION") != NULL) {
        const ULONGLONG now = GetTickCount64();
        g_test_camera_trace_until_ms = now + 5000u;
        g_test_camera_trace_next_ms = now;
        g_test_camera_trace_samples = 0u;
    }
    if (stage == 1u && actor == last_actor && enable == last_enable &&
        obstacle == last_obstacle)
        return;

    last_actor = actor;
    last_enable = enable;
    last_obstacle = obstacle;
    fprintf(stderr,
            "[HUMAN-COLLISION] seq=%u stage=%u actor=%08X enable=%u "
            "obstacle=%08X rider-seat=%08X vtable=%08X "
            "stored-pos=(%.7g,%.7g,%.7g)\n",
            ++sequence, stage, actor, enable != 0u, obstacle,
            valid ? guest_u32(actor + 0x768u) : 0u,
            valid ? guest_u32(actor) : 0u,
            valid ? guest_f32(actor + 0xE0u) : 0.0f,
            valid ? guest_f32(actor + 0xE4u) : 0.0f,
            valid ? guest_f32(actor + 0xE8u) : 0.0f);
    fflush(stderr);
}

#include "winch_camera_preview.h"
#include "faction_preview.h"
#include "turret_camera_roll.h"
float recomp_turret_camera_roll(uint32_t state, float target, float old_pitch,
                               float dt)
{
    static int initialized, disabled;
    if (!initialized) {
        disabled = getenv("MERCENARIES_DISABLE_TURRET_ROLL_STABILITY") != NULL;
        initialized = 1;
    }
    if (disabled || !g_xbox_mem_offset || state < 0x10000u ||
        state > 0x4000000u - 0x30u || guest_u32(state) != 0x2E70E4u) {
        float factor = fminf(1.0f, fmaxf(0.0f, 12.5f * dt));
        return (float)((double)old_pitch + ((double)target-old_pitch)*factor);
    }
    return mercenaries_turret_roll(target, old_pitch,
        *(float*)guest_ptr(state+0x14u), *(float*)guest_ptr(state+0x24u), dt);
}


static uint32_t g_camera_collision_last_site;
static uint32_t g_camera_collision_last_state;
static float g_camera_collision_last_result[3];

void recomp_camera_collision_checkpoint(uint32_t site, uint32_t state,
                                        uint32_t result)
{
    static uint32_t matching_calls;
    static uint32_t inset_samples;
    const uint32_t camera = 0x004140E8u;
    const uint32_t current = guest_u32(camera + 0x24u);
    const uint32_t red_camera = guest_u32(camera + 0x1Cu);
    const uint32_t actor = g_recomp_live_human_actor;
    const int state_valid = state >= 0x00010000u &&
                            state <= 0x04000000u - 0x15Cu;
    const int result_valid = result >= 0x00010000u &&
                             result <= 0x04000000u - 12u;
    const int actor_valid = actor >= 0x00010000u &&
                            actor <= 0x04000000u - 0xECu;
    const int red_valid = red_camera >= 0x00010000u &&
                          red_camera <= 0x04000000u - 0x4Cu;
    uint32_t sample;

    preview_winch_hit(site, state, result);

    /* Diagnostic-only placement probe.  Move the post-PDA Mario64 collision
     * result a short distance toward the player's upper body.  If this makes
     * the otherwise black return frame visible, the failure follows which
     * side of nearby aircraft geometry the collision result occupies rather
     * than projection, render submission, or the scripted-camera handoff. */
    if (getenv("MERCENARIES_TEST_POST_PDA_CAMERA_INSET") != NULL &&
        g_test_post_pda_camera_return && state_valid && result_valid &&
        actor_valid && state == current &&
        guest_u32(state) == 0x002E6FD4u) {
        const float result_x = guest_f32(result);
        const float result_y = guest_f32(result + 4u);
        const float result_z = guest_f32(result + 8u);
        const float focus_x = guest_f32(actor + 0xE0u);
        const float focus_y = guest_f32(actor + 0xE4u) + 1.85f;
        const float focus_z = guest_f32(actor + 0xE8u);
        const float delta_x = focus_x - result_x;
        const float delta_y = focus_y - result_y;
        const float delta_z = focus_z - result_z;
        const float length_squared = delta_x * delta_x + delta_y * delta_y +
                                     delta_z * delta_z;
        const char *distance_env = getenv(
            "MERCENARIES_TEST_POST_PDA_CAMERA_INSET_DISTANCE");
        const float inset_distance = distance_env ?
            strtof(distance_env, NULL) : 0.45f;

        if (length_squared > 0.000001f && isfinite(length_squared) &&
            isfinite(inset_distance) && inset_distance > 0.0f) {
            const float scale = inset_distance / sqrtf(length_squared);
            const float inset_x = result_x + delta_x * scale;
            const float inset_y = result_y + delta_y * scale;
            const float inset_z = result_z + delta_z * scale;

            *(float *)guest_ptr(result) = inset_x;
            *(float *)guest_ptr(result + 4u) = inset_y;
            *(float *)guest_ptr(result + 8u) = inset_z;
            if (inset_samples++ < 12u) {
                fprintf(stderr,
                        "[CAMERA-INSET-EXPERIMENT] site=%08X "
                        "original=(%.7g,%.7g,%.7g) "
                        "inset=(%.7g,%.7g,%.7g)\n",
                        site, result_x, result_y, result_z,
                        inset_x, inset_y, inset_z);
                fflush(stderr);
            }
        }
    }

    if (getenv("MERCENARIES_TRACE_CAMERA_COLLISION") == NULL ||
        !state_valid || !result_valid || state != current ||
        guest_u32(state) != 0x002E6FD4u)
        return;

    sample = ++matching_calls;
    g_camera_collision_last_site = site;
    g_camera_collision_last_state = state;
    g_camera_collision_last_result[0] = guest_f32(result);
    g_camera_collision_last_result[1] = guest_f32(result + 4u);
    g_camera_collision_last_result[2] = guest_f32(result + 8u);
    if (sample > 300u || (sample > 32u && sample % 15u != 0u))
        return;

    fprintf(stderr,
            "[CAMERA-COLLISION] sample=%u site=%08X state=%08X "
            "result=(%.7g,%.7g,%.7g) desired=(%.7g,%.7g,%.7g) "
            "red=(%.7g,%.7g,%.7g) actor=%08X:(%.7g,%.7g,%.7g) "
            "stick=%.7g min=%.7g mode=%08X guid=%08X\n",
            sample, site, state,
            guest_f32(result), guest_f32(result + 4u),
            guest_f32(result + 8u),
            guest_f32(state + 0x40u), guest_f32(state + 0x44u),
            guest_f32(state + 0x48u),
            red_valid ? guest_f32(red_camera + 0x40u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x44u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x48u) : 0.0f,
            actor,
            actor_valid ? guest_f32(actor + 0xE0u) : 0.0f,
            actor_valid ? guest_f32(actor + 0xE4u) : 0.0f,
            actor_valid ? guest_f32(actor + 0xE8u) : 0.0f,
            guest_f32(state + 0xF8u), guest_f32(state + 0xFCu),
            guest_u32(state + 0x158u), guest_u32(state + 0x60u));
    fflush(stderr);
}

void recomp_camera_post_collision_checkpoint(uint32_t site, uint32_t state,
                                             uint32_t focus,
                                             uint32_t direction,
                                             uint32_t new_length_bits)
{
    static uint32_t samples;
    float new_length;
    float stick;
    uint32_t sample;

    preview_winch_post(site, state, focus, direction, new_length_bits);

    if (getenv("MERCENARIES_TRACE_CAMERA_COLLISION") == NULL ||
        site != g_camera_collision_last_site ||
        state != g_camera_collision_last_state ||
        focus < 0x00010000u || focus > 0x04000000u - 12u ||
        direction < 0x00010000u || direction > 0x04000000u - 12u)
        return;

    memcpy(&new_length, &new_length_bits, sizeof(new_length));
    stick = guest_f32(state + 0xF8u);
    sample = ++samples;
    if (sample > 300u || (sample > 32u && sample % 15u != 0u))
        return;

    fprintf(stderr,
            "[CAMERA-COLLISION-POST] sample=%u site=%08X "
            "focus=(%.7g,%.7g,%.7g) dir=(%.7g,%.7g,%.7g) "
            "hit=(%.7g,%.7g,%.7g) newlen=%.7g stick=%.7g "
            "output=(%.7g,%.7g,%.7g)\n",
            sample, site,
            guest_f32(focus), guest_f32(focus + 4u),
            guest_f32(focus + 8u),
            guest_f32(direction), guest_f32(direction + 4u),
            guest_f32(direction + 8u),
            g_camera_collision_last_result[0],
            g_camera_collision_last_result[1],
            g_camera_collision_last_result[2], new_length, stick,
            guest_f32(focus) + guest_f32(direction) * stick,
            guest_f32(focus + 4u) + guest_f32(direction + 4u) * stick,
            guest_f32(focus + 8u) + guest_f32(direction + 8u) * stick);
    fflush(stderr);
}

void recomp_camera_repair_direction(uint32_t state, uint32_t direction)
{
    static uint32_t trace_samples;
    float raw_x;
    float raw_y;
    float raw_z;
    float raw_length_squared;
    float raw_inverse_length;
    float raw_normal_x = 0.0f;
    float raw_normal_y = 0.0f;
    float raw_normal_z = 0.0f;
    float noise_free_x;
    float noise_free_y;
    float noise_free_z;
    float noise_free_length_squared;
    float noise_free_inverse_length;
    float base_x;
    float base_y;
    float base_z;
    float repaired_x;
    float repaired_y;
    float repaired_z;
    float repaired_length_squared;
    float repaired_inverse_length;
    float tilt;
    float yaw;
    float cos_tilt;
    float agreement = -2.0f;
    int raw_valid;
    int noise_free_valid;

    if (state < 0x00010000u || state > 0x04000000u - 4u ||
        direction < 0x00010100u || direction > 0x04000000u - 0x30u ||
        guest_u32(state) != 0x002E6FD4u)
        return;

    raw_x = guest_f32(direction);
    raw_y = guest_f32(direction + 4u);
    raw_z = guest_f32(direction + 8u);
    raw_length_squared = raw_x * raw_x + raw_y * raw_y + raw_z * raw_z;
    raw_valid = isfinite(raw_length_squared) &&
                raw_length_squared > 1.0e-12f;
    if (raw_valid) {
        raw_inverse_length = 1.0f / sqrtf(raw_length_squared);
        /* Correct FSINCOS lowering makes ApplyRotation agree with the source
         * XG matrix convention; retain its normalized direction and noise. */
        raw_normal_x = raw_x * raw_inverse_length;
        raw_normal_y = raw_y * raw_inverse_length;
        raw_normal_z = raw_z * raw_inverse_length;
    }

    noise_free_x = guest_f32(direction + 0x24u);
    noise_free_y = guest_f32(direction + 0x28u);
    noise_free_z = guest_f32(direction + 0x2Cu);
    noise_free_length_squared = noise_free_x * noise_free_x +
                                noise_free_y * noise_free_y +
                                noise_free_z * noise_free_z;
    noise_free_valid = isfinite(noise_free_length_squared) &&
                       noise_free_length_squared > 1.0e-12f;
    if (noise_free_valid) {
        noise_free_inverse_length = 1.0f / sqrtf(noise_free_length_squared);
        noise_free_x *= noise_free_inverse_length;
        noise_free_y *= noise_free_inverse_length;
        noise_free_z *= noise_free_inverse_length;
    }

    /* RsCameraStateMario64.cpp computes these immediately before the lifted
     * matrix calls: fLookTiltRad lives at esp+0x0C and fLookYawRad at
     * esp+0x28. `direction` is esp+0x40 at this patch point. */
    tilt = guest_f32(direction - 0x34u);
    yaw = guest_f32(direction - 0x18u);
    if (isfinite(tilt) && isfinite(yaw)) {
        cos_tilt = cosf(tilt);
        base_x = sinf(yaw) * cos_tilt;
        base_y = -sinf(tilt);
        base_z = cosf(yaw) * cos_tilt;
        repaired_x = base_x;
        repaired_y = base_y;
        repaired_z = base_z;

        /* Preserve the source noise/turbulence delta only when the lifted
         * pre-noise vector agrees with the analytic source result. */
        if (raw_valid && noise_free_valid) {
            agreement = base_x * noise_free_x + base_y * noise_free_y +
                        base_z * noise_free_z;
            if (agreement > 0.9f) {
                repaired_x += raw_normal_x - noise_free_x;
                repaired_y += raw_normal_y - noise_free_y;
                repaired_z += raw_normal_z - noise_free_z;
                repaired_length_squared = repaired_x * repaired_x +
                                          repaired_y * repaired_y +
                                          repaired_z * repaired_z;
                if (isfinite(repaired_length_squared) &&
                    repaired_length_squared > 1.0e-12f) {
                    repaired_inverse_length =
                        1.0f / sqrtf(repaired_length_squared);
                    repaired_x *= repaired_inverse_length;
                    repaired_y *= repaired_inverse_length;
                    repaired_z *= repaired_inverse_length;
                }
            }
        }
    } else if (raw_valid) {
        repaired_x = raw_normal_x;
        repaired_y = raw_normal_y;
        repaired_z = raw_normal_z;
    } else {
        return;
    }

    if (getenv("MERCENARIES_TRACE_CAMERA_COLLISION") != NULL &&
        g_mercenaries_standup_complete && trace_samples++ < 256u) {
        fprintf(stderr,
                "[CAMERA-DIRECTION] raw=(%.7g,%.7g,%.7g) "
                "noise-free=(%.7g,%.7g,%.7g) tilt=%.7g yaw=%.7g "
                "agree=%.7g repaired=(%.7g,%.7g,%.7g)\n",
                raw_x, raw_y, raw_z, noise_free_x, noise_free_y,
                noise_free_z, tilt, yaw, agreement,
                repaired_x, repaired_y, repaired_z);
        fflush(stderr);
    }

    *(float *)guest_ptr(direction) = repaired_x;
    *(float *)guest_ptr(direction + 4u) = repaired_y;
    *(float *)guest_ptr(direction + 8u) = repaired_z;
}
void recomp_camera_pre_collision_checkpoint(uint32_t state,
                                            uint32_t camera_position,
                                            uint32_t direction)
{
    static uint32_t samples;
    const uint32_t actor = g_recomp_live_human_actor;
    float dx;
    float dz;

    preview_winch_pre(state, camera_position);

    if (getenv("MERCENARIES_TRACE_CAMERA_COLLISION") == NULL ||
        state < 0x00010000u || state > 0x04000000u - 4u ||
        guest_u32(state) != 0x002E6FD4u ||
        actor < 0x00010000u || actor > 0x04000000u - 0xECu ||
        camera_position < 0x00010000u ||
        camera_position > 0x04000000u - 12u ||
        direction < 0x00010000u || direction > 0x04000000u - 12u)
        return;

    dx = guest_f32(camera_position) - guest_f32(actor + 0xE0u);
    dz = guest_f32(camera_position + 8u) - guest_f32(actor + 0xE8u);
    ++samples;
    if (samples > 300u || (samples > 32u && samples % 15u != 0u))
        return;
    fprintf(stderr,
            "[CAMERA-COLLISION-PRE] sample=%u camera=(%.7g,%.7g,%.7g) "
            "actor=(%.7g,%.7g,%.7g) dir=(%.7g,%.7g,%.7g) "
            "horizontal-dot=%.7g\n",
            samples,
            guest_f32(camera_position), guest_f32(camera_position + 4u),
            guest_f32(camera_position + 8u),
            guest_f32(actor + 0xE0u), guest_f32(actor + 0xE4u),
            guest_f32(actor + 0xE8u),
            guest_f32(direction), guest_f32(direction + 4u),
            guest_f32(direction + 8u),
            dx * guest_f32(direction) + dz * guest_f32(direction + 8u));
    fflush(stderr);
}

static int g_camera_ray_trace_active;
static uint32_t g_camera_ray_trace_sequence;
static uint32_t g_camera_ray_candidate_count;
static uint32_t g_camera_ray_callsite;

uint32_t recomp_ai_perception_ray_count_checkpoint(uint32_t site,
                                                   uint32_t candidate_count,
                                                   uint32_t stack)
{
    if (candidate_count > 15u) {
        fprintf(stderr,
                "[AI-RAY-COUNT-WATCH] site=%08X raw=%08X stack=%08X\n",
                site, candidate_count, stack);
        fflush(stderr);
    }
    return candidate_count;
}

uint32_t recomp_camera_ray_callsite_checkpoint(uint32_t site, uint32_t ray,
                                               uint32_t candidate_count,
                                               uint32_t stack)
{
    uint32_t sanitized_count = candidate_count;

    g_camera_ray_callsite = site;
    if (candidate_count > 1024u &&
        ray >= 0x00010004u && ray <= 0x04000000u - 0x88u) {
        uint32_t capacity = guest_u32(ray - 4u);
        uint32_t initialized = 0u;
        uint32_t i;

        if (site == 0x00069A44u)
            capacity = 15u;
        if (capacity <= 4096u) {
            for (i = 0u; i < capacity; ++i) {
                const uint32_t item = ray + i * 0x88u;
                const float min_x = guest_f32(item + 0x70u);
                const float min_y = guest_f32(item + 0x74u);
                const float min_z = guest_f32(item + 0x78u);
                const float max_x = guest_f32(item + 0x7Cu);
                const float max_y = guest_f32(item + 0x80u);
                const float max_z = guest_f32(item + 0x84u);
                if (!isfinite(min_x) || !isfinite(min_y) || !isfinite(min_z) ||
                    !isfinite(max_x) || !isfinite(max_y) || !isfinite(max_z) ||
                    min_x > max_x || min_y > max_y || min_z > max_z ||
                    (min_x == 0.0f && min_y == 0.0f && min_z == 0.0f &&
                     max_x == 0.0f && max_y == 0.0f && max_z == 0.0f))
                    break;
                ++initialized;
            }
        }
        if (site == 0x00069A44u)
            sanitized_count = initialized;
        else if (initialized > 0u)
            sanitized_count = initialized;
        fprintf(stderr,
                "[CAMERA-RAY-CALLSITE] site=%08X ray=%08X raw=%08X "
                "capacity=%u initialized=%u using=%u stack=%08X\n",
                site, ray, candidate_count, capacity, initialized,
                sanitized_count, stack);
        fflush(stderr);
    }
    return sanitized_count;
}

uint32_t recomp_camera_ray_begin_checkpoint(uint32_t ray, uint32_t flags,
                                            uint32_t include_flags,
                                            uint32_t candidate_count,
                                            uint32_t frame)
{
    const uint32_t camera = 0x004140E8u;
    const uint32_t state = guest_u32(camera + 0x24u);
    const uint32_t actor = g_recomp_live_human_actor;
    float sx;
    float sy;
    float sz;
    float ex;
    float ey;
    float ez;
    float ax;
    float ay;
    float az;
    float start_distance_squared;
    float end_distance_squared;
    uint32_t callsite = g_camera_ray_callsite;
    uint32_t sanitized_count = candidate_count;

    g_camera_ray_callsite = 0u;
    if (candidate_count > 1024u &&
        ray >= 0x00010004u && ray <= 0x04000000u - 0x88u) {
        const uint32_t capacity = guest_u32(ray - 4u);
        if (capacity > 0u && capacity <= 4096u) {
            uint32_t initialized = 0u;
            uint32_t i;
            for (i = 0u; i < capacity; ++i) {
                const uint32_t item = ray + i * 0x88u;
                const float min_x = guest_f32(item + 0x70u);
                const float min_y = guest_f32(item + 0x74u);
                const float min_z = guest_f32(item + 0x78u);
                const float max_x = guest_f32(item + 0x7Cu);
                const float max_y = guest_f32(item + 0x80u);
                const float max_z = guest_f32(item + 0x84u);
                if (!isfinite(min_x) || !isfinite(min_y) || !isfinite(min_z) ||
                    !isfinite(max_x) || !isfinite(max_y) || !isfinite(max_z) ||
                    min_x > max_x || min_y > max_y || min_z > max_z ||
                    (min_x == 0.0f && min_y == 0.0f && min_z == 0.0f &&
                     max_x == 0.0f && max_y == 0.0f && max_z == 0.0f))
                    break;
                ++initialized;
            }
            if (initialized > 0u)
                sanitized_count = initialized;
            fprintf(stderr,
                    "[CAMERA-RAY-GUARD] site=%08X ray=%08X raw=%08X "
                    "capacity=%u initialized=%u using=%u\n",
                    callsite, ray, candidate_count, capacity, initialized,
                    sanitized_count);
            fflush(stderr);
        }
    }

    g_camera_ray_trace_active = 0;
    g_camera_ray_candidate_count = 0u;
    if (candidate_count > 1024u ||
        getenv("MERCENARIES_TRACE_CAMERA_RAY_ALL") != NULL) {
        fprintf(stderr,
                "[CAMERA-RAY-ARGS] ray=%08X flags=%08X include=%08X "
                "count=%08X frame=%08X stack=%08X,%08X,%08X,%08X,"
                "%08X,%08X\n",
                ray, flags, include_flags, candidate_count, frame,
                guest_u32(frame), guest_u32(frame + 4u),
                guest_u32(frame + 8u), guest_u32(frame + 0xCu),
                guest_u32(frame + 0x10u), guest_u32(frame + 0x14u));
        fflush(stderr);
    }
    if (getenv("MERCENARIES_TRACE_CAMERA_COLLISION") == NULL ||
        state < 0x00010000u || state > 0x04000000u - 0x15Cu ||
        guest_u32(state) != 0x002E6FD4u ||
        actor < 0x00010000u || actor > 0x04000000u - 0xECu ||
        ray < 0x00010000u || ray > 0x04000000u - 0x88u)
        return sanitized_count;

    sx = guest_f32(ray + 0x58u);
    sy = guest_f32(ray + 0x5Cu);
    sz = guest_f32(ray + 0x60u);
    ex = guest_f32(ray + 0x64u);
    ey = guest_f32(ray + 0x68u);
    ez = guest_f32(ray + 0x6Cu);
    ax = guest_f32(actor + 0xE0u);
    ay = guest_f32(actor + 0xE4u);
    az = guest_f32(actor + 0xE8u);
    start_distance_squared = (sx - ax) * (sx - ax) +
                             (sy - ay) * (sy - ay) +
                             (sz - az) * (sz - az);
    end_distance_squared = (ex - ax) * (ex - ax) +
                           (ey - ay) * (ey - ay) +
                           (ez - az) * (ez - az);
    if (!isfinite(start_distance_squared) ||
        !isfinite(end_distance_squared) ||
        start_distance_squared > 100.0f || end_distance_squared > 100.0f)
        return sanitized_count;

    g_camera_ray_trace_active = 1;
    ++g_camera_ray_trace_sequence;
    if (g_camera_ray_trace_sequence <= 64u ||
        g_camera_ray_trace_sequence % 15u == 0u) {
        fprintf(stderr,
                "[CAMERA-RAY-BEGIN] seq=%u ray=%08X flags=%08X "
                "include=%08X start=(%.7g,%.7g,%.7g) "
                "end=(%.7g,%.7g,%.7g) aabb=(%.7g,%.7g,%.7g)-"
                "(%.7g,%.7g,%.7g)\n",
                g_camera_ray_trace_sequence, ray, flags, include_flags,
                sx, sy, sz, ex, ey, ez,
                guest_f32(ray + 0x70u), guest_f32(ray + 0x74u),
                guest_f32(ray + 0x78u), guest_f32(ray + 0x7Cu),
                guest_f32(ray + 0x80u), guest_f32(ray + 0x84u));
        fflush(stderr);
    }
    return sanitized_count;
}

void recomp_camera_ray_candidate_checkpoint(uint32_t broadphase_handle)
{
    uint32_t collidable;
    uint32_t obstacle;
    uint32_t obstacle_flags;
    uint32_t actor;
    uint32_t shape;
    uint32_t shape_vtable;

    if (!g_camera_ray_trace_active)
        return;
    ++g_camera_ray_candidate_count;
    if (g_camera_ray_trace_sequence > 64u &&
        g_camera_ray_trace_sequence % 15u != 0u)
        return;
    collidable = broadphase_handle >= 0x00010010u
        ? broadphase_handle - 0x10u : 0u;
    obstacle = collidable >= 0x00010000u &&
               collidable <= 0x04000000u - 0x24u
        ? guest_u32(collidable + 0x20u) : 0u;
    obstacle_flags = obstacle >= 0x00010000u &&
                     obstacle <= 0x04000000u - 0xB4u
        ? guest_u32(obstacle + 0xB0u) : 0u;
    actor = obstacle >= 0x00010000u &&
            obstacle <= 0x04000000u - 0xB4u
        ? guest_u32(obstacle + 0xA4u) : 0u;
    shape = collidable >= 0x00010000u &&
            collidable <= 0x04000000u - 0x24u
        ? guest_u32(collidable) : 0u;
    shape_vtable = shape >= 0x00010000u &&
                   shape <= 0x04000000u - 0x24u
        ? guest_u32(shape) : 0u;
    if (g_camera_ray_candidate_count <= 12u) {
        fprintf(stderr,
                "[CAMERA-RAY-CANDIDATE] seq=%u candidate=%u "
                "handle=%08X type=%u collidable=%08X obstacle=%08X "
                "flags=%08X layer=%u shape=%08X/%08X cast=%08X "
                "actor=%08X@(%.7g,%.7g,%.7g)\n",
                g_camera_ray_trace_sequence, g_camera_ray_candidate_count,
                broadphase_handle,
                broadphase_handle >= 0x00010000u &&
                        broadphase_handle <= 0x04000000u - 12u
                    ? guest_u32(broadphase_handle + 8u) : 0u,
                collidable, obstacle, obstacle_flags,
                collidable >= 0x00010000u &&
                        collidable <= 0x04000000u - 0x20u
                    ? guest_u32(collidable + 0x1Cu) & 0x1Fu : 0u,
                shape, shape_vtable,
                shape_vtable >= 0x00010000u &&
                        shape_vtable <= 0x04000000u - 0x24u
                    ? guest_u32(shape_vtable + 0x20u) : 0u,
                actor,
                actor >= 0x00010000u && actor <= 0x04000000u - 0xECu
                    ? guest_f32(actor + 0xE0u) : 0.0f,
                actor >= 0x00010000u && actor <= 0x04000000u - 0xECu
                    ? guest_f32(actor + 0xE4u) : 0.0f,
                actor >= 0x00010000u && actor <= 0x04000000u - 0xECu
                    ? guest_f32(actor + 0xE8u) : 0.0f);
        fflush(stderr);
    }
}

void recomp_camera_ray_shape_result_checkpoint(uint32_t collidable,
                                                uint32_t result_address,
                                                uint32_t output,
                                                uint32_t shape)
{
    if (!g_camera_ray_trace_active ||
        (g_camera_ray_trace_sequence > 64u &&
         g_camera_ray_trace_sequence % 15u != 0u))
        return;
    fprintf(stderr,
            "[CAMERA-RAY-SHAPE] seq=%u collidable=%08X shape=%08X "
            "result=%u fraction=%.7g root=%08X normal=(%.7g,%.7g,%.7g)\n",
            g_camera_ray_trace_sequence, collidable, shape,
            result_address >= 0x00010000u &&
                    result_address < 0x04000000u
                ? guest_u8(result_address) : 0u,
            output >= 0x00010000u && output <= 0x04000000u - 0x24u
                ? guest_f32(output + 0x14u) : -1.0f,
            output >= 0x00010000u && output <= 0x04000000u - 0x24u
                ? guest_u32(output + 0x20u) : 0u,
            output >= 0x00010000u && output <= 0x04000000u - 12u
                ? guest_f32(output) : 0.0f,
            output >= 0x00010000u && output <= 0x04000000u - 12u
                ? guest_f32(output + 4u) : 0.0f,
            output >= 0x00010000u && output <= 0x04000000u - 12u
                ? guest_f32(output + 8u) : 0.0f);
    fflush(stderr);
}
void recomp_camera_collision_query_checkpoint(uint32_t stage,
                                              uint32_t result,
                                              uint32_t camera_position)
{
    static uint32_t samples;
    static int trace_all = -1;
    const uint32_t camera = 0x004140E8u;
    const uint32_t state = guest_u32(camera + 0x24u);
    const uint32_t actor = g_recomp_live_human_actor;
    uint32_t sample;

    if (getenv("MERCENARIES_TRACE_CAMERA_COLLISION") == NULL ||
        state < 0x00010000u || state > 0x04000000u - 0x15Cu ||
        guest_u32(state) != 0x002E6FD4u ||
        actor < 0x00010000u || actor > 0x04000000u - 0xECu ||
        camera_position < 0x00010000u ||
        camera_position > 0x04000000u - 12u)
        return;

    if (trace_all < 0)
        trace_all = getenv("MERCENARIES_TRACE_CAMERA_QUERY_ALL") != NULL;
    sample = ++samples;
    if (!trace_all &&
        (sample > 900u || (sample > 96u && sample % 15u != 0u)))
        return;
    fprintf(stderr,
            "[CAMERA-COLLISION-QUERY] sample=%u stage=%u hits=%d "
            "camera=(%.7g,%.7g,%.7g) actor=(%.7g,%.7g,%.7g) "
            "near=%.7g half=(%.7g,%.7g,%.7g) "
            "look=(%.7g,%.7g,%.7g) "
            "collision-pos=(%.7g,%.7g,%.7g) "
            "collision-normal=(%.7g,%.7g,%.7g) obstacle=%08X "
            "hit-actor=%08X t=%.7g\n",
            sample, stage, (int32_t)result,
            guest_f32(camera_position), guest_f32(camera_position + 4u),
            guest_f32(camera_position + 8u),
            guest_f32(actor + 0xE0u), guest_f32(actor + 0xE4u),
            guest_f32(actor + 0xE8u),
            stage == 2u ? guest_f32(camera_position + 0x0Cu) : 0.0f,
            stage == 2u ? guest_f32(camera_position + 0x10u) : 0.0f,
            stage == 2u ? guest_f32(camera_position + 0x14u) : 0.0f,
            stage == 2u ? guest_f32(camera_position + 0x18u) : 0.0f,
            stage == 2u ? guest_f32(camera_position + 0x2Cu) : 0.0f,
            stage == 2u ? guest_f32(camera_position + 0x30u) : 0.0f,
            stage == 2u ? guest_f32(camera_position + 0x34u) : 0.0f,
            guest_f32(0x00378EE0u), guest_f32(0x00378EE4u),
            guest_f32(0x00378EE8u), guest_f32(0x00378EECu),
            guest_f32(0x00378EF0u), guest_f32(0x00378EF4u),
            guest_u32(0x00378EF8u), guest_u32(0x00378EFCu),
            guest_f32(0x00378F00u));
    fflush(stderr);
}

static uint32_t recomp_lookup_entity_by_hash(uint32_t hash,
                                             uint32_t *spore_out)
{
    uint32_t index = hash & 0x3FFFu;

    *spore_out = 0u;
    for (uint32_t probes = 0u; probes < 0x4000u; ++probes) {
        const uint32_t key = guest_u32(0x00624070u + index * 4u);
        if (key == 0u)
            return 0u;
        if (key == hash) {
            const uint32_t spore = guest_u32(0x00614070u + index * 4u);
            *spore_out = spore;
            return spore >= 0x00010000u && spore <= 0x04000000u - 0x28u
                ? guest_u32(spore + 0x24u) : 0u;
        }
        index = (index + 1u) & 0x3FFFu;
    }
    return 0u;
}

static void recomp_trace_aircraft_target(const char *name, uint32_t hash,
                                         uint32_t briefing_actor)
{
    uint32_t spore;
    uint32_t entity = recomp_lookup_entity_by_hash(hash, &spore);
    if (entity == 0u)
        entity = briefing_actor;
    const int valid = entity >= 0x00010000u &&
                      entity <= 0x04000000u - 0xECu;

    fprintf(stderr, " %s=%08X/%08X/%08X@(%.7g,%.7g,%.7g)",
            name, hash, spore, entity,
            valid ? guest_f32(entity + 0xE0u) : 0.0f,
            valid ? guest_f32(entity + 0xE4u) : 0.0f,
            valid ? guest_f32(entity + 0xE8u) : 0.0f);
}

static void recomp_trace_aircraft_targets(ULONGLONG now)
{
    const uint32_t actor = g_recomp_live_human_actor;
    const int actor_valid = actor >= 0x00010000u &&
                            actor <= 0x04000000u - 0xECu;
    const uint32_t camera = 0x004140E8u;
    const uint32_t red_camera = guest_u32(camera + 0x1Cu);
    const int red_valid = red_camera >= 0x00010000u &&
                          red_camera <= 0x04000000u - 0x4Cu;

    if (getenv("MERCENARIES_TRACE_AIRCRAFT_TARGETS") == NULL ||
        !g_mercenaries_standup_complete ||
        now < g_test_aircraft_target_trace_next_ms)
        return;

    fprintf(stderr,
            "[AIRCRAFT-TARGETS] player=%08X@(%.7g,%.7g,%.7g) "
            "camera=%08X/%08X@(%.7g,%.7g,%.7g)",
            actor,
            actor_valid ? guest_f32(actor + 0xE0u) : 0.0f,
            actor_valid ? guest_f32(actor + 0xE4u) : 0.0f,
            actor_valid ? guest_f32(actor + 0xE8u) : 0.0f,
            camera, red_camera,
            red_valid ? guest_f32(red_camera + 0x40u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x44u) : 0.0f,
            red_valid ? guest_f32(red_camera + 0x48u) : 0.0f);
    recomp_trace_aircraft_target("pda", 0xEC0F9DB6u,
                                 g_recomp_briefing_pda_actor);
    recomp_trace_aircraft_target("m4", 0x9756800Eu,
                                 g_recomp_briefing_m4_actor);
    recomp_trace_aircraft_target("grenades", 0xE316060Cu,
                                 g_recomp_briefing_grenades_actor);
    fputc('\n', stderr);
    fflush(stderr);
    g_test_aircraft_target_trace_next_ms = now + 500u;
}

void recomp_radius_collection_checkpoint(uint32_t center, uint32_t radius_bits,
                                         uint32_t include_flags,
                                         uint32_t excluded,
                                         uint32_t broadphase_count)
{
    static uint32_t samples;

    if (getenv("MERCENARIES_TRACE_INPUT") == NULL ||
        guest_u32(0x00323ACCu) != 14u || samples++ >= 32u)
        return;

    const uint32_t havok_world = guest_u32(0x0037A3F0u);
    const uint32_t broadphase =
        havok_world >= 0x00010000u && havok_world <= 0x04000000u - 0xC8u
            ? guest_u32(havok_world + 0xC4u) : 0u;
    const uint32_t broadphase_vtable =
        broadphase >= 0x00010000u && broadphase <= 0x04000000u - 4u
            ? guest_u32(broadphase) : 0u;
    const uint32_t query_method =
        broadphase_vtable >= 0x00010000u &&
                broadphase_vtable <= 0x04000000u - 0x28u
            ? guest_u32(broadphase_vtable + 0x24u) : 0u;
    uint32_t chair_spore;
    uint32_t getup_spore;
    const uint32_t chair_entity =
        recomp_lookup_entity_by_hash(0x5CB49D8Au, &chair_spore);
    const uint32_t getup_entity =
        recomp_lookup_entity_by_hash(0x729D95ACu, &getup_spore);
    float radius;

    memcpy(&radius, &radius_bits, sizeof(radius));
    fprintf(stderr,
            "[RADIUS-COLLECT] center=%08X xyz=(%g,%g,%g) radius=%g "
            "include=%08X excluded=%08X pairs=%u world=%08X "
            "broadphase=%08X query=%08X nodes=%u axes=%u/%u/%u "
            "chair=%08X/%08X getup=%08X/%08X\n",
            center,
            center >= 0x00010000u && center <= 0x04000000u - 12u
                ? guest_f32(center) : 0.0f,
            center >= 0x00010000u && center <= 0x04000000u - 12u
                ? guest_f32(center + 4u) : 0.0f,
            center >= 0x00010000u && center <= 0x04000000u - 12u
                ? guest_f32(center + 8u) : 0.0f,
            radius, include_flags, excluded, broadphase_count,
            havok_world, broadphase, query_method,
            broadphase ? guest_u32(broadphase + 0x44u) : 0u,
            broadphase ? guest_u32(broadphase + 0x50u) : 0u,
            broadphase ? guest_u32(broadphase + 0x5Cu) : 0u,
            broadphase ? guest_u32(broadphase + 0x68u) : 0u,
            chair_spore, chair_entity, getup_spore, getup_entity);
    fflush(stderr);
}
static void recomp_print_guest_string(uint32_t address)
{
    if (address < 0x00010000u || address >= 0x04000000u) {
        fputs("<invalid>", stderr);
        return;
    }
    for (uint32_t i = 0u; i < 96u; ++i) {
        const uint8_t ch = guest_u8(address + i);
        if (ch == 0u)
            return;
        fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
    }
    fputs("...", stderr);
}

static int recomp_guest_string_equals(uint32_t address, const char *text)
{
    uint32_t i;

    if (address < 0x00010000u || address >= 0x04000000u)
        return 0;
    for (i = 0u; text[i] != '\0'; ++i) {
        if (address + i >= 0x04000000u ||
            guest_u8(address + i) != (uint8_t)text[i])
            return 0;
    }
    return address + i < 0x04000000u && guest_u8(address + i) == 0u;
}

void recomp_lua_checkcode_checkpoint(void)
{
    static int enabled = -1;
    uint32_t proto;
    uint32_t source;
    uint32_t code;
    uint32_t sizecode;
    uint32_t sizelineinfo;
    uint32_t caller;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_CHECKCODE") != NULL;
    if (!enabled || g_esp < 0x00010000u || g_esp > 0x03FFFFF8u)
        return;

    caller = guest_u32(g_esp);
    proto = guest_u32(g_esp + 4u);
    if (proto < 0x00010000u || proto > 0x03FFFFB8u) {
        fprintf(stderr,
                "[LUA-CHECKCODE] caller=%08X proto=%08X invalid-proto\n",
                caller, proto);
        fflush(stderr);
        return;
    }

    code = guest_u32(proto + 0x0Cu);
    source = guest_u32(proto + 0x20u);
    sizecode = guest_u32(proto + 0x2Cu);
    sizelineinfo = guest_u32(proto + 0x30u);
    fprintf(stderr,
            "[LUA-CHECKCODE] caller=%08X proto=%08X code=%08X "
            "sizecode=%u lines=%u source=%08X name=",
            caller, proto, code, sizecode, sizelineinfo, source);
    if (source >= 0x00010000u && source <= 0x03FFFFF0u)
        recomp_print_guest_string(source + 0x10u);
    else
        fputs("<invalid>", stderr);
    fputc('\n', stderr);
    fflush(stderr);
}
void recomp_lua_symbexec_checkpoint(void)
{
    static int enabled = -1;
    uint32_t caller;
    uint32_t proto;
    uint32_t lastpc;
    uint32_t reg;
    uint32_t source;
    uint32_t code;
    uint32_t sizecode;
    uint32_t sizelineinfo;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_SYMBEXEC") != NULL;
    if (!enabled || g_esp < 0x00010000u || g_esp > 0x03FFFFF0u)
        return;

    caller = guest_u32(g_esp);
    proto = guest_u32(g_esp + 4u);
    lastpc = guest_u32(g_esp + 8u);
    reg = guest_u32(g_esp + 0xCu);
    if (proto < 0x00010000u || proto > 0x03FFFFB8u) {
        fprintf(stderr,
                "[LUA-SYMBEXEC] caller=%08X proto=%08X lastpc=%u "
                "reg=%08X invalid-proto\n",
                caller, proto, lastpc, reg);
        fflush(stderr);
        return;
    }

    code = guest_u32(proto + 0x0Cu);
    source = guest_u32(proto + 0x20u);
    sizecode = guest_u32(proto + 0x2Cu);
    sizelineinfo = guest_u32(proto + 0x30u);
    fprintf(stderr,
            "[LUA-SYMBEXEC] caller=%08X proto=%08X lastpc=%u reg=%08X "
            "code=%08X sizecode=%u lines=%u source=%08X name=",
            caller, proto, lastpc, reg, code, sizecode, sizelineinfo, source);
    if (source >= 0x00010000u && source <= 0x03FFFFF0u)
        recomp_print_guest_string(source + 0x10u);
    else
        fputs("<invalid>", stderr);
    fputc('\n', stderr);
    fflush(stderr);
}
uint32_t recomp_lua_symbexec_args_valid(uint32_t proto, uint32_t lastpc)
{
    uint32_t code;
    uint32_t sizecode;

    if (proto < 0x00010000u || proto > 0x03FFFFB8u)
        return 0u;
    code = guest_u32(proto + 0x0Cu);
    sizecode = guest_u32(proto + 0x2Cu);
    if (sizecode == 0u || sizecode > 0x00100000u || lastpc > sizecode)
        return 0u;
    if (code < 0x00010000u || code > 0x04000000u - sizecode * 4u)
        return 0u;
    return 1u;
}

void recomp_lua_vm_precall_checkpoint(uint32_t stage, uint32_t state,
                                      uint32_t function_object,
                                      uint32_t result_or_wanted,
                                      uint32_t stack, uint32_t saved_wanted)
{
    static int enabled = -1;
    uint32_t type = 0xFFFFFFFFu;
    uint32_t closure = 0u;
    uint32_t is_c = 0xFFFFFFFFu;
    uint32_t target = 0u;
    uint32_t top = 0u;
    uint32_t base = 0u;
    uint32_t ci = 0u;
    uint32_t ci_state = 0xFFFFFFFFu;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_VM_PRECALL") != NULL;
    if (!enabled || state != 0x008DECC0u)
        return;
    if (function_object >= 0x00010000u &&
        function_object <= 0x03FFFFF0u) {
        type = guest_u32(function_object);
        closure = guest_u32(function_object + 8u);
    }
    if (closure >= 0x00010000u && closure <= 0x03FFFFF0u) {
        is_c = guest_u8(closure + 6u);
        target = guest_u32(closure + 0x0Cu);
    }
    top = guest_u32(state + 8u);
    base = guest_u32(state + 0x0Cu);
    ci = guest_u32(state + 0x14u);
    if (ci >= 0x00010000u && ci <= 0x03FFFFE8u)
        ci_state = guest_u32(ci + 8u);
    fprintf(stderr,
            "[LUA-VM-PRECALL] stage=%u L=%08X function=%08X type=%u "
            "closure=%08X isC=%u target=%08X value=%08X "
            "stack=%08X saved=%08X top=%08X base=%08X "
            "ci=%08X ci_state=%08X\n",
            stage, state, function_object, type, closure, is_c, target,
            result_or_wanted, stack, saved_wanted, top, base, ci, ci_state);
    fflush(stderr);
}

void recomp_lua_callback_117830_checkpoint(uint32_t stage, uint32_t target,
                                           uint32_t saved_stack,
                                           uint32_t current_stack)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_CALLBACK_117830") != NULL;
    if (!enabled)
        return;
    if (stage >= 120u && !g_recomp_aircraft_route_complete)
        return;
    fprintf(stderr,
            "[LUA-CALLBACK-117830] stage=%u target=%08X saved=%08X "
            "current=%08X delta=%d eax=%08X ebx=%08X ecx=%08X "
            "edx=%08X esi=%08X edi=%08X ebp=%08X\n",
            stage, target, saved_stack, current_stack,
            (int32_t)(current_stack - saved_stack), g_eax, g_ebx,
            g_ecx, g_edx, g_esi, g_edi, g_seh_ebp);
    fflush(stderr);
}
void recomp_lua_cclosure_return_checkpoint(uint32_t target,
                                           uint32_t saved_stack,
                                           uint32_t current_stack,
                                           uint32_t state)
{
    static int enabled = -1;
    uint32_t expected_stack;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_CALLFRAMES") != NULL;
    if (!enabled)
        return;

    /* luaD_precall pushes one cdecl argument and a synthetic return word.
     * The callee consumes only the return word, leaving the argument for the
     * caller's subsequent add esp, 4. */
    expected_stack = saved_stack - 4u;
    if (current_stack == expected_stack)
        return;

    fprintf(stderr,
            "[LUA-CCLOSURE-RETURN] target=%08X saved=%08X current=%08X "
            "expected=%08X delta=%d state=%08X eax=%08X ebx=%08X "
            "ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X\n",
            target, saved_stack, current_stack, expected_stack,
            (int32_t)(current_stack - expected_stack), state, g_eax, g_ebx,
            g_ecx, g_edx, g_esi, g_edi, g_seh_ebp);
    fflush(stderr);
}
void recomp_lua_precall_return_checkpoint(uint32_t saved_stack,
                                          uint32_t current_stack,
                                          uint32_t state,
                                          uint32_t restored_state)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_CALLFRAMES") != NULL;
    /* This checkpoint is placed after the translated epilogue has restored
     * ESI, so that register no longer contains the entry Lua-state argument.
     * The invariant under test is the cdecl frame balance; reporting the
     * unrelated restored ESI value flooded diagnostic logs on valid calls. */
    (void)state;
    (void)restored_state;
    if (!enabled || current_stack == saved_stack)
        return;

    fprintf(stderr,
            "[LUA-PRECALL-RETURN] saved=%08X current=%08X delta=%d "
            "state=%08X restored=%08X eax=%08X ebx=%08X ecx=%08X "
            "edx=%08X esi=%08X edi=%08X ebp=%08X\n",
            saved_stack, current_stack,
            (int32_t)(current_stack - saved_stack), state, restored_state,
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_seh_ebp);
    fflush(stderr);
}
void recomp_lua_dafa0_stack_checkpoint(uint32_t stage, uint32_t stack,
                                       uint32_t value)
{
    if (getenv("MERCENARIES_TRACE_LUA_CALLFRAMES") == NULL ||
        !g_recomp_aircraft_route_complete)
        return;
    fprintf(stderr,
            "[LUA-DAFA0-STACK] stage=%u stack=%08X value=%08X "
            "eax=%08X esi=%08X\n",
            stage, stack, value, g_eax, g_esi);
    fflush(stderr);
}typedef struct recomp_lua_poscall_record {
    uint32_t site;
    uint32_t state;
    uint32_t wanted;
    uint32_t first_result;
    uint32_t top;
    uint32_t base;
    uint32_t ci;
    uint32_t ci_base;
    uint32_t ci_state;
} recomp_lua_poscall_record;

static recomp_lua_poscall_record g_recomp_lua_poscall_ring[128];
static uint32_t g_recomp_lua_poscall_index;

void recomp_lua_poscall_site_checkpoint(uint32_t site, uint32_t state,
                                        uint32_t wanted,
                                        uint32_t first_result)
{
    static int enabled = -1;
    recomp_lua_poscall_record *record;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_POSCALL_SITES") != NULL;
    if (!enabled)
        return;
    record = &g_recomp_lua_poscall_ring[
        g_recomp_lua_poscall_index++ %
        (sizeof(g_recomp_lua_poscall_ring) /
         sizeof(g_recomp_lua_poscall_ring[0]))];
    record->site = site;
    record->state = state;
    record->wanted = wanted;
    record->first_result = first_result;
    if (state >= 0x00010000u && state <= 0x03FFFFA0u) {
        record->top = guest_u32(state + 8u);
        record->base = guest_u32(state + 0x0Cu);
        record->ci = guest_u32(state + 0x14u);
        if (record->ci >= 0x00010000u && record->ci <= 0x03FFFFE8u) {
            record->ci_base = guest_u32(record->ci);
            record->ci_state = guest_u32(record->ci + 8u);
        } else {
            record->ci_base = 0u;
            record->ci_state = 0xFFFFFFFFu;
            fprintf(stderr,
                    "[LUA-POSCALL-CORRUPT] site=%u L=%08X wanted=%08X "
                    "first=%08X top=%08X base=%08X ci=%08X\n",
                    site, state, wanted, first_result, record->top,
                    record->base, record->ci);
            fflush(stderr);
        }
    } else {
        record->top = 0u;
        record->base = 0u;
        record->ci = 0u;
        record->ci_base = 0u;
        record->ci_state = 0xFFFFFFFFu;
        fprintf(stderr,
                "[LUA-POSCALL-INVALID] site=%u L=%08X wanted=%08X first=%08X\n",
                site, state, wanted, first_result);
        fflush(stderr);
    }
}

static void recomp_lua_dump_poscall_ring(void)
{
    const uint32_t capacity = (uint32_t)(
        sizeof(g_recomp_lua_poscall_ring) /
        sizeof(g_recomp_lua_poscall_ring[0]));
    const uint32_t count = g_recomp_lua_poscall_index < capacity ?
        g_recomp_lua_poscall_index : capacity;
    const uint32_t begin = g_recomp_lua_poscall_index - count;
    uint32_t i;

    if (getenv("MERCENARIES_TRACE_LUA_POSCALL_SITES") == NULL)
        return;
    fprintf(stderr, "[LUA-POSCALL-RING] count=%u\n", count);
    for (i = 0u; i < count; ++i) {
        const recomp_lua_poscall_record *record =
            &g_recomp_lua_poscall_ring[(begin + i) % capacity];
        fprintf(stderr,
                "  #%03u site=%u L=%08X wanted=%08X first=%08X "
                "top=%08X base=%08X ci=%08X ci_base=%08X ci_state=%08X\n",
                i, record->site, record->state, record->wanted,
                record->first_result, record->top, record->base,
                record->ci, record->ci_base, record->ci_state);
    }
    fflush(stderr);
}
void recomp_lua_dump_poscall_ring_on_crash(void)
{
    recomp_lua_dump_poscall_ring();
}
void recomp_lua_typeerror_checkpoint(void)
{
    static int enabled = -1;
    uint32_t state;
    uint32_t object;
    uint32_t operation;
    uint32_t type = 0xFFFFFFFFu;
    uint32_t ci = 0u;
    uint32_t ci_base = 0u;
    uint32_t ci_top = 0u;
    uint32_t ci_state = 0u;
    uint32_t function_object = 0u;
    uint32_t function_type = 0xFFFFFFFFu;
    uint32_t closure = 0u;
    uint32_t closure_kind = 0xFFFFFFFFu;
    uint32_t closure_is_c = 0xFFFFFFFFu;
    uint32_t closure_target = 0u;
    uint32_t state_top = 0u;
    uint32_t state_base = 0u;
    uint32_t base_ci = 0u;
    uint32_t end_ci = 0u;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_ERROR") != NULL;
    if (!enabled || g_esp < 0x00010000u || g_esp > 0x03FFFFF0u)
        return;
    state = guest_u32(g_esp + 4u);
    object = guest_u32(g_esp + 8u);
    operation = guest_u32(g_esp + 0xCu);
    if (object >= 0x00010000u && object <= 0x03FFFFF0u)
        type = guest_u32(object);
    if (state >= 0x00010000u && state <= 0x03FFFFA0u) {
        state_top = guest_u32(state + 8u);
        state_base = guest_u32(state + 0x0Cu);
        ci = guest_u32(state + 0x14u);
        end_ci = guest_u32(state + 0x24u);
        base_ci = guest_u32(state + 0x28u);
    }
    if (ci >= 0x00010000u && ci <= 0x03FFFFE8u) {
        ci_base = guest_u32(ci);
        ci_top = guest_u32(ci + 4u);
        ci_state = guest_u32(ci + 8u);
    }
    if (ci_base >= 0x00010010u && ci_base < 0x04000000u) {
        function_object = ci_base - 0x10u;
        function_type = guest_u32(function_object);
        closure = guest_u32(ci_base - 8u);
    }
    if (closure >= 0x00010000u && closure <= 0x03FFFFF0u) {
        closure_kind = guest_u8(closure + 4u);
        closure_is_c = guest_u8(closure + 6u);
        closure_target = guest_u32(closure + 0x0Cu);
    }
    fprintf(stderr,
            "[LUA-TYPEERROR] L=%08X object=%08X type=%u op=%08X "
            "top=%08X base=%08X ci=%08X ci_top=%08X ci_state=%08X "
            "ci_range=%08X-%08X function=%08X function_type=%u "
            "closure=%08X closure_kind=%u isC=%u target=%08X operation=",
            state, object, type, operation, state_top, state_base,
            ci, ci_top, ci_state, base_ci, end_ci, function_object,
            function_type, closure, closure_kind, closure_is_c,
            closure_target);
    recomp_print_guest_string(operation);
    fputc('\n', stderr);
    fflush(stderr);
    recomp_lua_dump_poscall_ring();
}
void recomp_lua_callframe_checkpoint(uint32_t stage, uint32_t state,
                                     uint32_t function_object,
                                     uint32_t aux0, uint32_t aux1)
{
    static int enabled = -1;
    static uint32_t tracked_state;
    uint32_t top;
    uint32_t base;
    uint32_t ci;
    uint32_t base_ci;
    uint32_t ci_base;
    uint32_t ci_top;
    uint32_t ci_state;
    uint32_t previous = 0u;
    uint32_t previous_base = 0u;
    uint32_t previous_top = 0u;
    uint32_t previous_state = 0u;
    uint32_t object;
    uint32_t object_type = 0xFFFFFFFFu;
    uint32_t closure = 0u;
    uint32_t closure_is_c = 0xFFFFFFFFu;
    uint32_t target = 0u;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_CALLFRAMES") != NULL;
    if (!enabled || state < 0x00010000u || state > 0x03FFFFA0u)
        return;

    top = guest_u32(state + 8u);
    base = guest_u32(state + 0x0Cu);
    ci = guest_u32(state + 0x14u);
    base_ci = guest_u32(state + 0x28u);
    if (ci < 0x00010000u || ci > 0x03FFFFE8u)
        return;
    ci_base = guest_u32(ci);
    ci_top = guest_u32(ci + 4u);
    ci_state = guest_u32(ci + 8u);
    object = function_object;
    if (object == 0u && ci_base >= 0x00010010u)
        object = ci_base - 0x10u;
    if (object >= 0x00010000u && object <= 0x03FFFFF0u) {
        object_type = guest_u32(object);
        closure = guest_u32(object + 8u);
    }
    if (closure >= 0x00010000u && closure <= 0x03FFFFF0u) {
        closure_is_c = guest_u8(closure + 6u);
        target = guest_u32(closure + 0x0Cu);
    }

    if (stage == 0u) {
        if (object_type != 6u || closure_is_c == 0u ||
            target != 0x0011AAD0u)
            return;
        tracked_state = state;
    } else if (tracked_state != state) {
        return;
    }

    if (ci >= base_ci + 0x18u && base_ci >= 0x00010000u) {
        previous = ci - 0x18u;
        previous_base = guest_u32(previous);
        previous_top = guest_u32(previous + 4u);
        previous_state = guest_u32(previous + 8u);
    }
    fprintf(stderr,
            "[LUA-CALLFRAME] stage=%u L=%08X top=%08X base=%08X "
            "ci=%08X {%08X,%08X,state=%08X} "
            "prev=%08X {%08X,%08X,state=%08X} "
            "function=%08X type=%u closure=%08X isC=%u "
            "target=%08X aux=%08X/%08X\n",
            stage, state, top, base, ci, ci_base, ci_top, ci_state,
            previous, previous_base, previous_top, previous_state,
            object, object_type, closure, closure_is_c, target, aux0, aux1);
    fflush(stderr);
    if (stage == 4u)
        tracked_state = 0u;
}
void recomp_briefing_actor_checkpoint(uint32_t stage,
                                      uint32_t template_name,
                                      uint32_t actor_name,
                                      uint32_t hardpoint,
                                      uint32_t actor)
{
    if (stage == 2u) {
        if (recomp_guest_string_equals(actor_name, "getdatapod"))
            g_recomp_briefing_pda_actor = actor;
        else if (recomp_guest_string_equals(actor_name, "getm4"))
            g_recomp_briefing_m4_actor = actor;
        else if (recomp_guest_string_equals(actor_name, "getgrenades"))
            g_recomp_briefing_grenades_actor = actor;
        else if (recomp_guest_string_equals(actor_name, "opencargodoor"))
            g_recomp_briefing_door_actor = actor;
        else if (recomp_guest_string_equals(actor_name, "usehumvee"))
            g_recomp_briefing_usehumvee_actor = actor;
    }
    if (getenv("MERCENARIES_TRACE_INPUT") == NULL &&
        getenv("MERCENARIES_TRACE_AIRCRAFT_TARGETS") == NULL)
        return;
    fprintf(stderr, "[BRIEFING-ACT] stage=%u template=", stage);
    recomp_print_guest_string(template_name);
    fputs(" name=", stderr);
    recomp_print_guest_string(actor_name);
    fprintf(stderr, " hardpoint=%08X actor=%08X scene_guid=%08X "
            "vtable=%08X pos=(%.7g,%.7g,%.7g)\n",
            hardpoint, actor, guest_u32(0x00403370u),
            actor >= 0x00010000u && actor <= 0x04000000u - 0xECu ? guest_u32(actor) : 0u,
            actor >= 0x00010000u && actor <= 0x04000000u - 0xECu ? guest_f32(actor + 0xE0u) : 0.0f,
            actor >= 0x00010000u && actor <= 0x04000000u - 0xECu ? guest_f32(actor + 0xE4u) : 0.0f,
            actor >= 0x00010000u && actor <= 0x04000000u - 0xECu ? guest_f32(actor + 0xE8u) : 0.0f);
    fflush(stderr);
}

void recomp_broadphase_query_checkpoint(uint32_t broadphase,
                                        uint32_t min_x, uint32_t min_y,
                                        uint32_t min_z, uint32_t max_x,
                                        uint32_t max_y, uint32_t max_z)
{
    static int shown;
    const uint32_t axis_offsets[3] = { 0x4Cu, 0x58u, 0x64u };
    if (shown || getenv("MERCENARIES_TRACE_INPUT") == NULL ||
        guest_u32(0x00323ACCu) != 14u || broadphase < 0x00010000u ||
        broadphase > 0x03FFFF94u)
        return;
    shown = 1;
    fprintf(stderr, "[BROADPHASE-QUERY] bp=%08X q=%u,%u,%u..%u,%u,%u "
            "offset=(%g,%g,%g) scale=(%g,%g,%g) nodes=%u\n",
            broadphase, min_x, min_y, min_z, max_x, max_y, max_z,
            guest_f32(broadphase + 0x10u), guest_f32(broadphase + 0x14u),
            guest_f32(broadphase + 0x18u), guest_f32(broadphase + 0x30u),
            guest_f32(broadphase + 0x34u), guest_f32(broadphase + 0x38u),
            guest_u32(broadphase + 0x44u));
    const uint32_t nodes = guest_u32(broadphase + 0x40u);
    const uint32_t node_count = guest_u32(broadphase + 0x44u);
    for (uint32_t i = 0u; i < node_count; ++i) {
        const uint32_t node = nodes + i * 16u;
        const uint16_t indices[6] = {
            guest_u16(node + 8u), guest_u16(node), guest_u16(node + 2u),
            guest_u16(node + 10u), guest_u16(node + 4u), guest_u16(node + 6u)
        };
        uint16_t values[6];
        for (uint32_t axis = 0u; axis < 3u; ++axis) {
            const uint32_t endpoints = guest_u32(broadphase + axis_offsets[axis]);
            values[axis] = guest_u16(endpoints + indices[axis] * 4u);
            values[axis + 3u] = guest_u16(endpoints + indices[axis + 3u] * 4u);
        }
        fprintf(stderr, "  [BP-NODE] i=%u idx=%u,%u,%u..%u,%u,%u "
                "q=%u,%u,%u..%u,%u,%u handle=%08X\n",
                i, indices[0], indices[1], indices[2], indices[3], indices[4],
                indices[5], values[0], values[1], values[2], values[3],
                values[4], values[5], guest_u32(node + 12u));
    }
    fflush(stderr);
}

void recomp_broadphase_bits_checkpoint(uint32_t broadphase, uint32_t bitfield,
                                       uint32_t packed_min_yz, uint32_t packed_max_yz)
{
    static int shown;
    if (shown || getenv("MERCENARIES_TRACE_INPUT") == NULL ||
        guest_u32(0x00323ACCu) != 14u)
        return;
    shown = 1;
    fprintf(stderr, "[BROADPHASE-BITS] bp=%08X bits=%08X/%08X min_yz=%08X max_yz=%08X\n",
            broadphase, guest_u32(bitfield), guest_u32(bitfield + 4u),
            packed_min_yz, packed_max_yz);
    fflush(stderr);
}

void recomp_frontend_menu_checkpoint(uint32_t owner_address, uint32_t new_menu_address)
{
    if (owner_address < 0x00010000u || owner_address > 0x04000000u - 0x3EBCu)
        return;
    recomp_options_menu_transition(owner_address, new_menu_address);
    if (getenv("MERCENARIES_TRACE_INPUT") == NULL)
        return;
    const uint32_t old_menu_address = guest_u32(owner_address + 0x3EB8u);
    const char *post_lua_text = getenv("MERCENARIES_TRACE_FRONTEND_POST_LUA_COUNT");
    if (post_lua_text != NULL && post_lua_text[0] != '\0') {
        const unsigned long parsed = strtoul(post_lua_text, NULL, 0);
        if (parsed > 0u && parsed <= UINT32_MAX &&
            g_frontend_post_lua_target == 0u) {
            g_frontend_post_lua_count = 0u;
            g_frontend_post_lua_target = (uint32_t)parsed;
            fprintf(stderr, "[FRONTEND-POST-LUA-ARM] target=%u\n",
                    (uint32_t)g_frontend_post_lua_target);
            fflush(stderr);
        }
    }
    fprintf(stderr, "Retail front-end menu: owner=%08X old=%08X old_vtable=%08X "
            "new=%08X new_vtable=%08X\n", owner_address, old_menu_address,
            old_menu_address ? guest_u32(old_menu_address) : 0u,
            new_menu_address, new_menu_address ? guest_u32(new_menu_address) : 0u);
    if (getenv("MERCENARIES_TRACE_FRONTEND_RECENT_EXIT") != NULL) {
        const uint32_t end = g_recomp_recent_game_func_idx;
        const uint32_t count = end < 256u ? end : 256u;
        fprintf(stderr, "[FRONTEND-RECENT] count=%u observed=%u\n", count, end);
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t index = (end - count + i) & 255u;
            fprintf(stderr, "  [F%03u] %08X\n", i,
                    (uint32_t)g_recomp_recent_game_funcs[index]);
        }
        fflush(stderr);
        ExitProcess(88u);
    }
    fflush(stderr);
}

void recomp_main_state_checkpoint(uint32_t state, uint32_t substate)
{
    static int game_capture_armed;
    static int shell_play_capture_armed;
    const char *capture_path = getenv("MERCENARIES_CAPTURE_GAME_PATH");
    const char *shell_play_capture_path =
        getenv("MERCENARIES_CAPTURE_SHELL_PLAY_PATH");
    const char *auto_a_stop_state = getenv("MERCENARIES_TEST_AUTO_A_STOP_MAIN_STATE");
    if (auto_a_stop_state && auto_a_stop_state[0] != '\0' &&
        state == (uint32_t)strtoul(auto_a_stop_state, NULL, 0))
        recomp_release_test_auto_a();
    if (state == 0x4249D707u && substate == 0xC2CBD863u &&
        getenv("MERCENARIES_CAPTURE_GAMEPLAY_AFTER_USE_ONLY") == NULL)
        g_mercenaries_gameplay_capture_active = 1u;
    if (!game_capture_armed && capture_path &&
        state == 0x4249D707u && substate == 0xC2CBD863u) {
        const char *delay_text = getenv("MERCENARIES_CAPTURE_GAME_DELAY_MS");
        const uint32_t delay_ms = delay_text && delay_text[0] != '\0' ?
            (uint32_t)strtoul(delay_text, NULL, 0) : 0u;
        if (delay_ms != 0u)
            g_test_game_capture_deadline_ms = GetTickCount64() + delay_ms;
        else
            d3d8_DebugArmFlipCapture(capture_path);
        game_capture_armed = 1;
    }
    if (!shell_play_capture_armed && shell_play_capture_path &&
        shell_play_capture_path[0] != '\0' &&
        state == 0x59C00449u && substate == 0xC2CBD863u) {
        /* Capture both the last presented image and the first flip after the
         * shell enters "play".  This remains diagnostic-only and avoids
         * relying on input polling while the front end is loading. */
        d3d8_DebugCaptureFrameToPath(shell_play_capture_path);
        d3d8_DebugArmFlipCapture(shell_play_capture_path);
        shell_play_capture_armed = 1;
    }
    if (getenv("MERCENARIES_TRACE_STATE") == NULL)
        return;
    fprintf(stderr, "Retail main state: tick=%lu old=%08X/%08X new=%08X/%08X\n",
            (unsigned long)GetTickCount(), guest_u32(0x00413F6Cu),
            guest_u32(0x00413F68u), state, substate);
    fflush(stderr);
}

void recomp_frontend_shell_loop_checkpoint(uint32_t stage, uint32_t owner,
                                           uint32_t channel)
{
    static uint32_t invocation;
    static ULONGLONG entered_ms;
    static ULONGLONG last_summary_ms;
    ULONGLONG now;
    int emit;

    if (getenv("MERCENARIES_TRACE_FRONTEND_SHELL") == NULL)
        return;
    now = GetTickCount64();
    if (stage == 0u) {
        ++invocation;
        entered_ms = now;
    }
    emit = invocation <= 4u;
    if (stage == 0u && now - last_summary_ms >= 1000u) {
        emit = 1;
        last_summary_ms = now;
    }
    if (!emit)
        return;
    fprintf(stderr,
            "[FRONTEND-SHELL] invocation=%u stage=%u owner=%08X "
            "channel=%u elapsed_ms=%llu\n",
            invocation, stage, owner, channel,
            (unsigned long long)(now - entered_ms));
    fflush(stderr);
}

void recomp_frontend_input_checkpoint(uint32_t menu_address, uint32_t input, uint32_t event)
{
    static uint32_t samples;
    if (input == 5u && event == 1u &&
        getenv("MERCENARIES_TEST_AUTO_A_STOP_FRONTEND_ACCEPT") != NULL &&
        g_test_auto_a_stop_deadline_ms == 0u)
        g_test_auto_a_stop_deadline_ms = GetTickCount64() + 250u;
    if (getenv("MERCENARIES_TRACE_INPUT") == NULL || samples >= 64u)
        return;
    fprintf(stderr, "Retail title input: tick=%lu menu=%08X vtable=%08X input=%u event=%u\n",
            (unsigned long)GetTickCount(), menu_address,
            menu_address >= 0x00010000u && menu_address <= 0x04000000u - 4u ?
                guest_u32(menu_address) : 0u, input, event);
    fflush(stderr);
    ++samples;
}

void recomp_loadsave_state_checkpoint(uint32_t stage, uint32_t owner_address,
                                      uint32_t state_address)
{
    static uint32_t samples;
    if (getenv("MERCENARIES_TRACE_INPUT") == NULL || samples >= 1024u ||
        owner_address < 0x00010000u || owner_address > 0x04000000u - 0x6C4u)
        return;
    const uint32_t depth = guest_u32(owner_address + 0x6C0u);
    if (stage == 5u && depth == 0u)
        return;
    if ((stage == 2u || stage == 5u) && state_address == 0u &&
        depth > 0u && depth <= 8u)
        state_address = guest_u32(owner_address + 0x6A0u + (depth - 1u) * 4u);
    const int valid = state_address >= 0x00010000u &&
                      state_address <= 0x04000000u - 12u;
    fprintf(stderr, "Retail load/save state: tick=%lu stage=%s depth=%u state=%08X "
            "vtable=%08X result=%u substate=%u last_result=%u\n",
            (unsigned long)GetTickCount(), stage == 1u ? "push" :
            (stage == 2u ? "pop" : (stage == 5u ? "manager-update" : "update")),
            depth, state_address,
            valid ? guest_u32(state_address) : 0u,
            valid ? guest_u32(state_address + 4u) : 0u,
            valid ? guest_u32(state_address + 8u) : 0u,
            guest_u32(owner_address + 0x6BCu));
    fflush(stderr);
    ++samples;
}

uint32_t recomp_loadsave_callback_sanitize(uint32_t owner_address,
                                           uint32_t callback_address)
{
    uint32_t vtable;
    uint32_t target;

    if (callback_address == 0u)
        return 0u;
    if (callback_address < 0x00010000u ||
        callback_address > 0x04000000u - 4u)
        goto invalid;
    vtable = guest_u32(callback_address);
    if (vtable < 0x00010000u || vtable > 0x04000000u - 12u)
        goto invalid;
    target = guest_u32(vtable + 8u);
    if (target < 0x00010000u || target >= 0x04000000u)
        goto invalid;
    return callback_address;

invalid:
    fprintf(stderr,
            "[LOADSAVE-CALLBACK-GUARD] owner=%08X callback=%08X\n",
            owner_address, callback_address);
    fflush(stderr);
    return 0u;
}

void recomp_redspace_bounds_checkpoint(uint32_t space_level, uint32_t circle,
    int32_t min_grid_x, int32_t min_grid_z, int32_t max_grid_x,
    int32_t max_grid_z, int32_t total_grid_squares, int32_t lower_bound,
    int32_t upper_bound, int32_t grid_width)
{
    static uint32_t samples;
    if (getenv("MERCENARIES_TRACE_REDSPACE") == NULL || samples++ >= 64u)
        return;
    fprintf(stderr, "[REDSPACE-BOUNDS] level=%08X circle=%08X x=%d..%d z=%d..%d "
            "total=%d bounds=%d..%d width=%d\n", space_level, circle,
            min_grid_x, max_grid_x, min_grid_z, max_grid_z, total_grid_squares,
            lower_bound, upper_bound, grid_width);
    fflush(stderr);
}

void recomp_redspace_probe_checkpoint(uint32_t space_level, int32_t grid_counter,
                                      int32_t grid_z, uint32_t index, uint32_t item)
{
    static uint32_t samples;
    if (getenv("MERCENARIES_TRACE_REDSPACE") == NULL || samples++ >= 256u)
        return;
    fprintf(stderr, "[REDSPACE-PROBE] level=%08X counter=%d z=%d index=%u item=%08X\n",
            space_level, grid_counter, grid_z, index, item);
    fflush(stderr);
}

void recomp_redspace_query_checkpoint(uint32_t stage, uint32_t space,
                                      uint32_t result, uint32_t count_or_max,
                                      uint32_t saved_count)
{
    static LONG active_queries;
    static DWORD active_thread;
    static uint32_t reports;
    static uint32_t consumer_reports;
    const DWORD thread_id = GetCurrentThreadId();
    uint32_t first_bad = UINT32_MAX;
    uint32_t first_bad_value = 0u;
    uint32_t i;

    if (getenv("MERCENARIES_TRACE_REDSPACE_QUERY") == NULL)
        return;

    if (stage == 0u) {
        const LONG depth = InterlockedIncrement(&active_queries);
        if (depth == 1)
            active_thread = thread_id;
        if (reports++ < 256u || depth != 1) {
            fprintf(stderr,
                    "[REDSPACE-QUERY] begin tid=%lu depth=%ld owner=%08X "
                    "result=%08X max=%u globals=(max=%u dist=%08X "
                    "dedup=%08X result=%08X)\n",
                    (unsigned long)thread_id, (long)depth, space, result,
                    count_or_max, guest_u32(0x85C57Cu),
                    guest_u32(0x85C580u), guest_u32(0x85C584u),
                    guest_u32(0x85C588u));
            fflush(stderr);
        }
        return;
    }

    if (result >= 0x00010000u && result <= 0x03FFF000u &&
        count_or_max <= 0x10000u) {
        for (i = 0; i < count_or_max; ++i) {
            const uint32_t item = guest_u32(result + i * 4u);
            if (item < 0x00010000u || item > 0x03FFF000u) {
                first_bad = i;
                first_bad_value = item;
                break;
            }
        }
    } else if (count_or_max != 0u) {
        first_bad = 0u;
        first_bad_value = result;
    }

    if (stage == 1u) {
        const LONG depth = InterlockedCompareExchange(&active_queries, 0, 0);
        if (reports++ < 512u || first_bad != UINT32_MAX ||
            count_or_max != saved_count || depth != 1 ||
            active_thread != thread_id) {
            fprintf(stderr,
                    "[REDSPACE-QUERY] end tid=%lu depth=%ld owner=%08X "
                    "result=%08X count=%u saved=%u first-bad=%s%u:%08X "
                    "globals=(max=%u dist=%08X dedup=%08X result=%08X)\n",
                    (unsigned long)thread_id, (long)depth, space, result,
                    count_or_max, saved_count,
                    first_bad == UINT32_MAX ? "none/" : "",
                    first_bad == UINT32_MAX ? 0u : first_bad,
                    first_bad_value, guest_u32(0x85C57Cu),
                    guest_u32(0x85C580u), guest_u32(0x85C584u),
                    guest_u32(0x85C588u));
            fflush(stderr);
        }
        InterlockedDecrement(&active_queries);
        return;
    }

    if (first_bad == UINT32_MAX && consumer_reports++ >= 64u)
        return;

    fprintf(stderr,
            "[REDSPACE-CONSUME] tid=%lu result=%08X count=%u "
            "first-bad=%s%u:%08X globals=(max=%u dist=%08X "
            "dedup=%08X result=%08X)\n",
            (unsigned long)thread_id, result, count_or_max,
            first_bad == UINT32_MAX ? "none/" : "",
            first_bad == UINT32_MAX ? 0u : first_bad, first_bad_value,
            guest_u32(0x85C57Cu), guest_u32(0x85C580u),
            guest_u32(0x85C584u), guest_u32(0x85C588u));
    fflush(stderr);
}

void recomp_actor_dispatch_checkpoint(uint32_t return_address,
                                      uint32_t manager,
                                      uint32_t object,
                                      uint32_t guest_stack)
{
    if (getenv("MERCENARIES_TRACE_ACTOR_DISPATCH") == NULL)
        return;
    if (object >= 0x00010000u && object <= 0x03FFFFFCu)
        return;

    fprintf(stderr,
            "[ACTOR-DISPATCH] invalid caller=%08X manager=%08X object=%08X "
            "stack=%08X stack_words=%08X,%08X,%08X,%08X\n",
            return_address, manager, object, guest_stack,
            guest_stack <= 0x03FFFFF0u ? guest_u32(guest_stack) : 0u,
            guest_stack <= 0x03FFFFECu ? guest_u32(guest_stack + 4u) : 0u,
            guest_stack <= 0x03FFFFE8u ? guest_u32(guest_stack + 8u) : 0u,
            guest_stack <= 0x03FFFFE4u ? guest_u32(guest_stack + 12u) : 0u);
    fflush(stderr);
}

/* Event-only history: no allocation, environment lookup or file output for
 * healthy removals. Preserve evidence before an invalid callback dereference;
 * do not suppress callbacks or attempt to repair corrupt ownership here. */
typedef struct RecompEntityRemovalSample {
    uint32_t stage, entity, world, stack, ticks, vtable, refs;
    uint32_t arrays[9];
    uint32_t invalid;
} RecompEntityRemovalSample;
static RecompEntityRemovalSample g_entity_removal_history[64];
static uint32_t g_entity_removal_history_count;
static uint32_t g_entity_removal_reports;

void recomp_entity_removal_checkpoint(uint32_t stage, uint32_t entity,
                                      uint32_t world, uint32_t stack)
{
    RecompEntityRemovalSample sample = {0};
    sample.stage = stage;
    sample.entity = entity;
    sample.world = world;
    sample.stack = stack;
    sample.ticks = guest_u32(0x4140CCu);
    if (entity < 0x10000u || entity > 0x04000000u - 0xA4u) {
        sample.invalid = 8u;
    } else {
        sample.vtable = guest_u32(entity);
        sample.refs = guest_u32(entity + 4u);
        for (uint32_t i = 0; i < 9u; ++i)
            sample.arrays[i] = guest_u32(entity + 0x80u + i * 4u);
        for (uint32_t i = 0; i < 3u; ++i) {
            uint32_t data = sample.arrays[i * 3u];
            uint32_t count = sample.arrays[i * 3u + 1u];
            /* The retail hkArray capacity uses bit 31 for non-owned memory. */
            uint32_t capacity = sample.arrays[i * 3u + 2u] & 0x7FFFFFFFu;
            if (count > capacity || (count &&
                (data < 0x10000u || data > 0x03FFFFFCu ||
                 count > (0x04000000u - data) / 4u)))
                sample.invalid |= 1u << i;
        }
    }
    g_entity_removal_history[g_entity_removal_history_count++ & 63u] = sample;
    if (!sample.invalid || g_entity_removal_reports >= 4u)
        return;
    ++g_entity_removal_reports;
    uint32_t available = g_entity_removal_history_count < 64u ?
                         g_entity_removal_history_count : 64u;
    xbox_preview_log_event("HAVOK-ENTITY-REMOVAL",
        "invalid=%X stage=%u entity=%08X world=%08X history=%u",
        sample.invalid,stage,entity,world,available);
    if (g_entity_removal_reports == 1u) {
        char context[16384];
        size_t used = 0;
        /* Minidumps may omit guest pages. Preserve the object (including the
         * allocation prefix) and immediate caller stack only on first failure.
         * Bound every read independently; corrupt pointers are evidence too. */
        const uint32_t bases[2] = {
            entity >= 0x10010u ? entity - 16u : entity, stack
        };
        const uint32_t lengths[2] = {208u, 64u};
        const char *labels[2] = {"OBJECT", "STACK"};
        for (uint32_t region = 0; region < 2u; ++region) {
            const uint32_t address = bases[region];
            const uint32_t length = lengths[region];
            if (address < 0x10000u || address > 0x04000000u - length)
                continue;
            for (uint32_t offset = 0; offset < length; offset += 16u) {
                int written = snprintf(context + used, sizeof(context) - used,
                    "[HAVOK-ENTITY-%s] %08X: %08X %08X %08X %08X\n",
                    labels[region], address + offset,
                    guest_u32(address + offset), guest_u32(address + offset + 4u),
                    guest_u32(address + offset + 8u), guest_u32(address + offset + 12u));
                if (written < 0 || (size_t)written >= sizeof(context) - used) {
                    context[used] = 0;
                    break;
                }
                used += (size_t)written;
            }
        }
        for (uint32_t i = 0; i < available; ++i) {
            const RecompEntityRemovalSample *h = &g_entity_removal_history[
                (g_entity_removal_history_count - available + i) & 63u];
            int written = snprintf(context + used, sizeof(context) - used,
                "[HAVOK-ENTITY-HISTORY] s=%u e=%08X w=%08X sp=%08X t=%u "
                "vt=%08X refs=%08X a=%08X,%08X,%08X;%08X,%08X,%08X;"
                "%08X,%08X,%08X bad=%X\n", h->stage,h->entity,h->world,
                h->stack,h->ticks,h->vtable,h->refs,h->arrays[0],h->arrays[1],
                h->arrays[2],h->arrays[3],h->arrays[4],h->arrays[5],
                h->arrays[6],h->arrays[7],h->arrays[8],h->invalid);
            if (written < 0 || (size_t)written >= sizeof(context) - used) {
                context[used] = 0;
                break;
            }
            used += (size_t)written;
        }
        xbox_preview_log_set_crash_context(context);
    }
}

uint32_t recomp_havok_callback_count_checkpoint(uint32_t owner,
                                                uint32_t array,
                                                uint32_t count,
                                                uint32_t context,
                                                uint32_t guest_stack)
{
    static uint32_t reports;
    const int owner_valid = owner >= 0x00010000u &&
                            owner <= 0x03FFFF30u;
    const int array_valid = count == 0u ||
        (array >= 0x00010000u && array <= 0x03FFFFF8u &&
         count <= (0x04000000u - array) / 8u);
    const int count_sane = count <= 0x1000u;
    const int trace = getenv("MERCENARIES_TRACE_HAVOK_CALLBACKS") != NULL;

    if (owner_valid && array_valid && count_sane && !trace)
        return count;

    if ((!owner_valid || !array_valid || !count_sane || reports++ < 64u)) {
        const uint32_t owner_vtable = owner_valid ? guest_u32(owner) : 0u;
        const uint32_t source = owner_valid ? guest_u32(owner + 8u) : 0u;
        fprintf(stderr,
                "[HAVOK-CALLBACK-LIST] owner=%08X vtable=%08X "
                "source=%08X array=%08X count=%u context=%08X "
                "stack=%08X valid=%u/%u/%u entries=",
                owner, owner_vtable, source, array, count, context,
                guest_stack, owner_valid != 0, array_valid != 0,
                count_sane != 0);
        for (uint32_t i = 0u; i < count && i < 4u && array_valid; ++i) {
            const uint32_t listener = guest_u32(array + i * 8u);
            fprintf(stderr, "%s%08X:%08X:%08X", i == 0u ? "" : ",",
                    listener,
                    listener >= 0x00010000u && listener <= 0x03FFFFFCu ?
                        guest_u32(listener) : 0u,
                    guest_u32(array + i * 8u + 4u));
        }
        fputc('\n', stderr);
        fflush(stderr);
    }

    /* Every source-equivalent Havok callback collection here is a small
     * listener array.  A corrupt count previously held one recompiled frame
     * inside 0x001C6B00 for minutes and then walked arbitrary guest memory.
     * Refuse only structurally impossible/absurd collections; healthy retail
     * behavior is unchanged and the diagnostic above retains provenance. */
    if (!owner_valid || !array_valid || !count_sane)
        return 0u;
    return count;
}

void recomp_actor_init_checkpoint(uint32_t stage, uint32_t object,
                                  uint32_t argument1,
                                  uint32_t argument2)
{
    static __declspec(thread) uint32_t expected_object;
    static uint32_t reports;
    const uint32_t vtable = object >= 0x00010000u && object <= 0x03FFF800u ?
                            guest_u32(object) : 0u;
    const uint32_t member_5a0 = object >= 0x00010000u && object <= 0x03FFF800u ?
                                guest_u32(object + 0x5A0u) : 0u;

    if (getenv("MERCENARIES_TRACE_ACTOR_INIT") == NULL)
        return;
    if (stage == 1u)
        expected_object = object;
    if (reports++ >= 128u && stage != 2u && vtable == 0x002E60E0u)
        return;

    fprintf(stderr,
            "[ACTOR-INIT] stage=%u object=%08X expected=%08X vtable=%08X "
            "member5A0=%08X arg1=%08X arg2=%08X\n",
            stage, object, expected_object, vtable, member_5a0,
            argument1, argument2);
    fflush(stderr);
}

void recomp_ai_init_stack_checkpoint(uint32_t stage, uint32_t owner,
                                     uint32_t guest_stack,
                                     uint32_t saved_esi,
                                     uint32_t saved_ebx)
{
    static __declspec(thread) uint32_t expected_owner;
    static __declspec(thread) uint32_t base_stack;

    if (getenv("MERCENARIES_TRACE_AI_INIT_STACK") == NULL)
        return;
    if (stage == 0u) {
        expected_owner = owner;
        base_stack = guest_stack;
    }
    if (owner != expected_owner)
        return;

    fprintf(stderr,
            "[AI-INIT-STACK] stage=%u owner=%08X stack=%08X delta=%d "
            "esi=%08X ebx=%08X words=%08X,%08X,%08X,%08X\n",
            stage, owner, guest_stack, (int32_t)(guest_stack - base_stack),
            saved_esi, saved_ebx,
            guest_stack <= 0x03FFFFF0u ? guest_u32(guest_stack) : 0u,
            guest_stack <= 0x03FFFFECu ? guest_u32(guest_stack + 4u) : 0u,
            guest_stack <= 0x03FFFFE8u ? guest_u32(guest_stack + 8u) : 0u,
            guest_stack <= 0x03FFFFE4u ? guest_u32(guest_stack + 12u) : 0u);
    fflush(stderr);
}

void recomp_ai_base_init_checkpoint(uint32_t stage, uint32_t owner,
                                    uint32_t value1, uint32_t value2,
                                    uint32_t guest_stack)
{
    if (getenv("MERCENARIES_TRACE_AI_BASE_INIT") == NULL)
        return;

    fprintf(stderr,
            "[AI-BASE-INIT] stage=%u owner=%08X value1=%08X value2=%08X "
            "stack=%08X words=%08X,%08X,%08X,%08X\n",
            stage, owner, value1, value2, guest_stack,
            guest_stack <= 0x03FFFFF0u ? guest_u32(guest_stack) : 0u,
            guest_stack <= 0x03FFFFECu ? guest_u32(guest_stack + 4u) : 0u,
            guest_stack <= 0x03FFFFE8u ? guest_u32(guest_stack + 8u) : 0u,
            guest_stack <= 0x03FFFFE4u ? guest_u32(guest_stack + 12u) : 0u);
    fflush(stderr);
}

uint32_t recomp_ai_process_stimuli_esi_checkpoint(uint32_t expected,
                                                  uint32_t actual)
{
    if (actual != expected &&
        getenv("MERCENARIES_TRACE_AI_VTABLE") != NULL) {
        fprintf(stderr,
                "[AI-CALLEE-SAVED] ProcessStimuli ESI expected=%08X "
                "actual=%08X restored=1\n",
                expected, actual);
        fflush(stderr);
    }
    return expected;
}

uint32_t recomp_ai_process_stimuli_esp_checkpoint(uint32_t expected,
                                                  uint32_t actual)
{
    if (actual != expected &&
        getenv("MERCENARIES_TRACE_AI_VTABLE") != NULL) {
        fprintf(stderr,
                "[AI-CALLEE-SAVED] ProcessStimuli ESP expected=%08X "
                "actual=%08X restored=1\n",
                expected, actual);
        fflush(stderr);
    }
    return expected;
}

uint32_t recomp_ai_process_stimuli_frame_checkpoint(uint32_t site,
                                                    uint32_t expected,
                                                    uint32_t actual)
{
    static LONG reported;

    if (actual != expected &&
        InterlockedCompareExchange(&reported, 1, 0) == 0) {
        fprintf(stderr,
                "[AI-PROCESS-STACK] site=%08X expected=%08X actual=%08X "
                "restored=1\n",
                site, expected, actual);
        fflush(stderr);
    }
    return expected;
}

uint32_t recomp_ai_update_vtable_checkpoint(uint32_t site, uint32_t object,
                                            uint32_t expected_vtable)
{
    const int trace = getenv("MERCENARIES_TRACE_AI_VTABLE") != NULL;
    const int repair = getenv("MERCENARIES_TEST_REPAIR_AI_VTABLE") != NULL;
    uint32_t current;

    if (object < 0x00010000u || object > 0x03FFFFFCu)
        return 0u;

    current = guest_u32(object);
    if (expected_vtable == 0u) {
        if (current >= 0x00200000u && current <= 0x0032FFF8u)
            return current;
        if (trace || repair) {
            fprintf(stderr,
                    "[AI-VTABLE] invalid-entry site=%08X object=%08X "
                    "current=%08X\n",
                    site, object, current);
            fflush(stderr);
        }
        return current;
    }

    if (current != expected_vtable) {
        fprintf(stderr,
                "[AI-VTABLE] changed site=%08X object=%08X entry=%08X "
                "current=%08X repair=%u\n",
                site, object, expected_vtable, current, repair ? 1u : 0u);
        if (repair && expected_vtable >= 0x00200000u &&
            expected_vtable <= 0x0032FFF8u) {
            *(volatile uint32_t *)guest_ptr(object) = expected_vtable;
            current = expected_vtable;
        }
        fflush(stderr);
    }
    return current;
}
typedef struct RecompAiStimulusSnapshot {
    uint32_t object;
    uint32_t active;
    uint32_t generation;
    uint32_t words[0x540u / 4u];
} RecompAiStimulusSnapshot;

static RecompAiStimulusSnapshot g_ai_stimulus_snapshots[256];
static uint32_t g_ai_stimulus_snapshot_generation;
static SRWLOCK g_ai_stimulus_snapshot_lock = SRWLOCK_INIT;

uint32_t recomp_ai_stimulus_boundary_checkpoint(uint32_t stage, uint32_t object,
                                                uint32_t expected_vtable,
                                                uint32_t expected_depth)
{
    const int trace = getenv("MERCENARIES_TRACE_AI_VTABLE") != NULL;
    const int repair = getenv("MERCENARIES_TEST_REPAIR_AI_VTABLE") != NULL;
    RecompAiStimulusSnapshot *snapshot = NULL;
    uint32_t current_vtable;
    uint32_t current_depth;
    uint32_t changed = 0u;
    uint32_t reported = 0u;
    uint32_t valid;

    if (object < 0x00010000u || object > 0x04000000u - 0x540u)
        return 0u;

    if (stage == 0u) {
        if (!trace && !repair)
            return 1u;
        AcquireSRWLockExclusive(&g_ai_stimulus_snapshot_lock);
        for (uint32_t i = 0u; i < 256u; ++i) {
            if (!g_ai_stimulus_snapshots[i].active) {
                snapshot = &g_ai_stimulus_snapshots[i];
                break;
            }
        }
        if (snapshot == NULL) {
            fprintf(stderr,
                    "[AI-STIMULUS-DIFF] snapshot-capacity object=%08X\n",
                    object);
            fflush(stderr);
            ReleaseSRWLockExclusive(&g_ai_stimulus_snapshot_lock);
            return 1u;
        }
        snapshot->object = object;
        snapshot->active = 1u;
        snapshot->generation = ++g_ai_stimulus_snapshot_generation;
        memcpy(snapshot->words, guest_ptr(object), sizeof(snapshot->words));
        ReleaseSRWLockExclusive(&g_ai_stimulus_snapshot_lock);
        return 1u;
    }

    current_vtable = guest_u32(object);
    current_depth = guest_u32(object + 0x4B8u);
    valid = current_vtable == expected_vtable &&
            expected_vtable >= 0x00200000u &&
            expected_vtable <= 0x0032FFF8u && current_depth <= 4u;
    if (!trace && !repair)
        return valid;

    AcquireSRWLockExclusive(&g_ai_stimulus_snapshot_lock);
    for (uint32_t i = 0u; i < 256u; ++i) {
        RecompAiStimulusSnapshot *candidate = &g_ai_stimulus_snapshots[i];
        if (candidate->active && candidate->object == object &&
            (snapshot == NULL || candidate->generation > snapshot->generation))
            snapshot = candidate;
    }

    if (snapshot == NULL) {
        fprintf(stderr,
                "[AI-STIMULUS-DIFF] snapshot-missing object=%08X "
                "entry-vtable=%08X current-vtable=%08X expected-depth=%u "
                "current-depth=%08X guard-exit=%u\n",
                object, expected_vtable, current_vtable, expected_depth,
                current_depth, valid ? 0u : 1u);
    } else {
        snapshot->active = 0u;
        if (valid) {
            ReleaseSRWLockExclusive(&g_ai_stimulus_snapshot_lock);
            return 1u;
        }
        for (uint32_t offset = 0u; offset < 0x540u; offset += 4u) {
            const uint32_t before = snapshot->words[offset / 4u];
            const uint32_t after = guest_u32(object + offset);
            if (before == after)
                continue;
            ++changed;
            if (reported++ < 96u)
                fprintf(stderr,
                        "  [AI-STIMULUS-WRITE] +%03X %08X -> %08X\n",
                        offset, before, after);
        }
        fprintf(stderr,
                "[AI-STIMULUS-DIFF] object=%08X changes=%u entry-vtable=%08X "
                "current-vtable=%08X expected-depth=%u current-depth=%08X "
                "guard-exit=%u\n",
                object, changed, expected_vtable, current_vtable,
                expected_depth, current_depth, valid ? 0u : 1u);
    }
    fflush(stderr);
    ReleaseSRWLockExclusive(&g_ai_stimulus_snapshot_lock);
    return valid;
}
int recomp_spore_ppd_is_valid(uint32_t site, uint32_t spore,
                              uint32_t ppd, uint32_t key)
{
    static LONG invalid_reports;
    uint32_t recent_index;
    uint32_t count;
    uint32_t i;

    if (ppd == 0u)
        return 0;
    if (ppd >= 0x00010000u && ppd <= 0x03FFFFF0u)
        return 1;

    if (InterlockedIncrement(&invalid_reports) > 16)
        return 0;

    fprintf(stderr,
            "[SPORE-PPD-INVALID] site=%08X spore=%08X ppd=%08X key=%08X "
            "flags=%04X guid=%08X esp=%08X\n",
            site, spore, ppd, key,
            (spore >= 0x00010000u && spore <= 0x03FFFFCEu)
                ? *(volatile uint16_t *)((uintptr_t)g_xbox_mem_offset + spore + 0x30u)
                : 0u,
            (spore >= 0x00010000u && spore <= 0x03FFFFD7u)
                ? *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + spore + 0x28u)
                : 0u,
            g_esp);
    recent_index = g_recomp_recent_game_func_idx;
    count = recent_index < 24u ? recent_index : 24u;
    fprintf(stderr, "[SPORE-PPD-INVALID-RECENT]");
    for (i = count; i != 0u; --i)
        fprintf(stderr, " %08X",
                g_recomp_recent_game_funcs[(recent_index - i) & 255u]);
    fputc('\n', stderr);
    fflush(stderr);
    return 0;
}
void recomp_spore_predicate_checkpoint(uint32_t stage, uint32_t spore,
                                       uint32_t camera, uint32_t position)
{
    static uint32_t samples;
    if (getenv("MERCENARIES_TRACE_SPORE_PREDICATE") == NULL || samples++ >= 256u)
        return;
    fprintf(stderr, "[SPORE-PREDICATE] stage=%u spore=%08X camera=%08X position=%08X\n",
            stage, spore, camera, position);
    fflush(stderr);
}

static int recomp_trace_xact_enabled(void)
{
    static volatile LONG cached = -1;
    LONG enabled = cached;
    if (enabled < 0) {
        enabled = GetEnvironmentVariableA(
                      "MERCENARIES_TRACE_XACT", NULL, 0) != 0;
        InterlockedCompareExchange(&cached, enabled, -1);
        enabled = cached;
    }
    return enabled != 0;
}

static int g_trace_weapon_fire_seen;

void recomp_weapon_fire_sound_checkpoint(
    uint32_t stage, uint32_t effect, uint32_t detail, uint32_t weapon,
    uint32_t cue_or_result, uint32_t handle, uint32_t stack)
{
    if (getenv("MERCENARIES_TRACE_WEAPON_FIRE_SOUND") == NULL)
        return;
    if (stage == 0u)
        g_trace_weapon_fire_seen = 1;
    fprintf(stderr,
            "[WEAPON-FIRE-SOUND] stage=%u effect=%08X detail=%08X "
            "weapon=%08X cue-result=%08X handle=%08X stack=%08X\n",
            stage, effect, detail, weapon, cue_or_result, handle, stack);
    fflush(stderr);
}

void recomp_xact_stream_checkpoint(uint32_t object, uint32_t descriptor,
                                   uint32_t format)
{
    if (recomp_trace_xact_enabled() ||
        getenv("MERCENARIES_TRACE_XACT_STREAM") != NULL)
        fprintf(stderr, "[XACT-STREAM] object=%08X descriptor=%08X format=%08X\n",
                object, descriptor, format);
}

void recomp_xact_list_checkpoint(uint32_t stage, uint32_t manager,
                                 uint32_t object)
{
    if (recomp_trace_xact_enabled())
        fprintf(stderr, "[XACT-LIST] stage=%u manager=%08X object=%08X\n",
                stage, manager, object);
}

static int g_trace_xact_cues_after_movie_active;
static int g_trace_xact_target_alloc_active;
static uint32_t g_trace_xact_target_alloc_counts[64];

#include "recomp_event_history.inc"

/* Observe the first damaged event call; do not repair or suppress callbacks. */
void recomp_event_abi_checkpoint(uint32_t caller, uint32_t target,
                                uint32_t expected_esp, uint32_t saved_esi,
                                uint32_t saved_edi)
{
    static int enabled = -1;
    static unsigned int mismatches;
    if (caller == 0x00111D90u && target == 0x001113A0u)
        recomp_event_record_return(saved_esi, expected_esp, saved_edi);
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_EVENT_ABI") != NULL;
    if (!enabled || mismatches >= 128u ||
        (g_esp == expected_esp && g_esi == saved_esi && g_edi == saved_edi))
        return;
    ++mismatches;
    fprintf(stderr, "[EVENT-ABI] caller=%08X target=%08X "
            "esp=%08X/%08X esi=%08X/%08X edi=%08X/%08X last=%08X\n",
            caller, target, g_esp, expected_esp, g_esi, saved_esi,
            g_edi, saved_edi, g_recomp_current_func);
    fflush(stderr);
}

static int recomp_trace_hq_audio_enabled(void)
{
    return g_mercenaries_hq_briefing_active != 0u &&
        getenv("MERCENARIES_TRACE_HQ_AUDIO") != NULL;
}

/* Prepared wrappers do not subscribe to STOP until Play. XACT can destroy a
 * primed cue before its distance delay/manual trigger expires; keeping _pCue
 * then sends recycled memory to XACT_FLAG_SOUNDCUE_PREPARED. Invalidate that
 * reference at the actual destructor, before the allocator can reuse it.
 * Keep playing/paused identities intact for normal STOP notification delivery.
 * The wrapper, source, delay, and manual-trigger state remain owned by RedXact;
 * its next Play creates a fresh cue through the ordinary non-prepared path. */
void recomp_xact_retire_prepared_cue(uint32_t cue)
{
    const uint32_t manager = 0x0085C600u;
    const uint32_t first = manager + 0x7568u + 0x84u;
    if (!cue) return;
    for (unsigned i = 0; i < 128u; ++i) {
        const uint32_t entry = manager + 0x91ECu + i * 8u;
        const uint32_t handle = guest_u32(entry);
        if (!handle) continue;
        const uint32_t wrapper = guest_u32(entry + 4u);
        if (wrapper < first || wrapper >= first + 128u * 0x38u ||
            (wrapper - first) % 0x38u ||
            guest_u32(wrapper + 4u) != handle ||
            guest_u32(wrapper + 8u) != cue) continue;
        const uint32_t state = guest_u32(wrapper);
        if (state < 1u || state > 3u) continue;
        *(uint32_t *)guest_ptr(wrapper + 8u) = 0u;
        xbox_preview_log_event("audio-cue-lifetime",
            "retired prepared cue=%08X wrapper=%08X handle=%08X state=%u hash=%08X",
            cue, wrapper, handle, state, guest_u32(wrapper + 0x1Cu));
    }
}

/* Opt-in lifetime evidence for prepared cues. Never changes guest ownership. */
void recomp_xact_cue_lifetime_checkpoint(uint32_t stage, uint32_t object,
                                         uint32_t result)
{
    static int enabled = -1;
    unsigned i;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_PREPARED_CUE_LIFETIME") != NULL;
    if (!enabled || object < 0x10000u || object > 0x03FFFFC0u)
        return;
    for (i = 0; i < (stage == 2u ? 128u : 1u); ++i) {
        uint32_t wrapper = object;
        uint32_t cue;
        if (stage == 2u) {
            const uint32_t slot = 0x0085C600u + 0x91ECu + i * 8u;
            if (!guest_u32(slot))
                continue;
            wrapper = guest_u32(slot + 4u);
            if (wrapper < 0x10000u || wrapper > 0x03FFFFC0u ||
                guest_u32(wrapper + 8u) != object)
                continue;
        }
        cue = guest_u32(wrapper + 8u);
        fprintf(stderr, "[XACT-LIFETIME] stage=%u wrapper=%08X state=%u "
                "handle=%08X hash=%08X cue=%08X result=%08X",
                stage, wrapper, guest_u32(wrapper), guest_u32(wrapper + 4u),
                guest_u32(wrapper + 0x1Cu), cue, result);
        if (cue >= 0x10000u && cue <= 0x03FFFFC0u)
            fprintf(stderr, " refs=%u flags=%04X track=%08X",
                    guest_u32(cue + 0x34u), guest_u32(cue + 0x38u) & 0xFFFFu,
                    guest_u32(cue + 0x28u));
        if (stage == 2u) {
            void *frames[24];
            USHORT count = CaptureStackBackTrace(0, 24, frames, NULL);
            USHORT n;
            fprintf(stderr, " native=");
            for (n = 0; n < count; ++n)
                fprintf(stderr, "%s%p", n ? "," : "", frames[n]);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    }
}

/* A successful Play is not enough: XACT allocates notification descriptors
 * separately. Keep the exact failed STOP subscription without guest allocation
 * so terminal recovery can deliver it even under memory pressure. Slots follow
 * the fixed wrapper pool, and are cleared before every wrapper release/reuse. */
typedef struct recomp_xact_failed_subscription {
    uint32_t handle, cue, descriptor[6];
} recomp_xact_failed_subscription;
static recomp_xact_failed_subscription xact_failed_subscriptions[128];
static int recomp_xact_wrapper_slot(uint32_t wrapper)
{
    const uint32_t first = 0x0085C600u + 0x7568u + 0x84u;
    if (wrapper < first || wrapper >= first + 128u * 0x38u ||
        (wrapper - first) % 0x38u) return -1;
    return (int)((wrapper - first) / 0x38u);
}
void recomp_xact_forget_subscription(uint32_t wrapper)
{
    int slot = recomp_xact_wrapper_slot(wrapper);
    if (slot >= 0) memset(&xact_failed_subscriptions[slot], 0,
                          sizeof(xact_failed_subscriptions[slot]));
}
void recomp_xact_stop_subscription(uint32_t wrapper, uint32_t descriptor, uint32_t result)
{
    int slot = recomp_xact_wrapper_slot(wrapper);
    if (slot < 0) return;
    recomp_xact_forget_subscription(wrapper);
    if ((int32_t)result >= 0 || descriptor < 0x10000u ||
        descriptor > 0x04000000u - 24u || guest_u32(descriptor) != 0x40001u ||
        !guest_u32(wrapper + 4u) || !guest_u32(wrapper + 8u) ||
        guest_u32(descriptor + 12u) != guest_u32(wrapper + 8u)) return;
    recomp_xact_failed_subscription *pending = &xact_failed_subscriptions[slot];
    pending->handle = guest_u32(wrapper + 4u);
    pending->cue = guest_u32(wrapper + 8u);
    memcpy(pending->descriptor, guest_ptr(descriptor), 24u);
    xbox_preview_log_event("audio-stop-registration-failure",
        "handle=%08X cue=%08X hash=%08X result=%08X",
        pending->handle, pending->cue, guest_u32(wrapper + 0x1cu), result);
}
int recomp_test_fail_guard_notification(uint32_t cue)
{
    const char *fault = getenv("MERCENARIES_TEST_HQ_GUARD_FAILURE");
    if (!fault || strcmp(fault, "stop-registration") ||
        !getenv("MERCENARIES_TEST_GAMEPAD_FILE") || !guard_wait.handle) return 0;
    uint32_t managed = recomp_guard_managed_cue(guard_wait.handle);
    if (!managed || managed == UINT32_MAX) return 0;
    for (unsigned i=0; i<128u; ++i) {
        uint32_t wrapper=0x0085C600u+0x7568u+0x84u+i*0x38u;
        if (guest_u32(wrapper+4u)==guest_u32(managed+8u) &&
            guest_u32(wrapper+8u)==cue && guest_u32(wrapper)) {
            xbox_preview_log_event("hq-guard", "private-test fail XACT notification allocation cue=%08X", cue);
            return 1;
        }
    }
    return 0;
}

void recomp_xact_cue_checkpoint(uint32_t stage, uint32_t manager,
                                uint32_t cue, uint32_t result)
{
    if (stage == 1u)
        recomp_xact_cue_lifetime_checkpoint(3u, cue, result);
    if (stage == 2u && xbox_preview_log_enabled()) {
        const uint32_t name = guest_u32(cue + 0x1Cu);
        if (name == 0x23F08077u || name == 0xD24E4223u)
            xbox_preview_log_event("extraction-voice-result",
                "cue=%08X result=%08X handle=%08X instance=%08X "
                "source=%d category=%u bank=%08X bank_cue=%u",
                name, result, guest_u32(cue + 4u), guest_u32(cue + 8u),
                (int32_t)guest_u32(cue + 0xCu), guest_u32(cue + 0x24u),
                guest_u32(cue + 0x14u), guest_u32(cue + 0x18u));
    }
    const int failure_only =
        getenv("MERCENARIES_TRACE_XACT_FAILURES") != NULL &&
        stage == 2u && (int32_t)result < 0;
    if (getenv("MERCENARIES_TRACE_XACT_ALLOC_SUMMARY") != NULL &&
        guest_u32(cue + 0x1Cu) == 0x03E4CB2Au) {
        if (stage == 1u) {
            memset(g_trace_xact_target_alloc_counts, 0,
                   sizeof(g_trace_xact_target_alloc_counts));
            g_trace_xact_target_alloc_active = 1;
        } else if (stage == 2u) {
            unsigned int i;
            fprintf(stderr, "[XACT-ALLOC-SUMMARY]");
            for (i = 0u; i < 64u; ++i)
                if (g_trace_xact_target_alloc_counts[i] != 0u)
                    fprintf(stderr, " %u:%u", i,
                            g_trace_xact_target_alloc_counts[i]);
            fprintf(stderr, "\n");
            fflush(stderr);
            g_trace_xact_target_alloc_active = 0;
        }
    }
    if (recomp_trace_xact_enabled() ||
        getenv("MERCENARIES_TRACE_XACT_CUES") != NULL ||
        recomp_trace_hq_audio_enabled() ||
        (stage >= 3u && stage <= 5u &&
         getenv("MERCENARIES_TRACE_XACT_RETIRE") != NULL) ||
        (g_trace_xact_cues_after_movie_active &&
         getenv("MERCENARIES_TRACE_XACT_CUES_AFTER_MOVIE") != NULL) ||
        failure_only) {
        fprintf(stderr,
                "[XACT-CUE] stage=%u manager=%08X cue=%08X result=%08X "
                "state=%u handle=%08X pCue=%08X source=%u spatial=%08X "
                "soundbank=%08X bank-cue=%u hash=%08X category=%u "
                "length=%.7g delay=%.7g manual=%u retries=%u "
                "started=%u peak=%u\n",
                stage, manager, cue, result,
                guest_u32(cue + 0x00u), guest_u32(cue + 0x04u),
                guest_u32(cue + 0x08u), guest_u32(cue + 0x0Cu),
                guest_u32(cue + 0x10u),
                guest_u32(cue + 0x14u), guest_u32(cue + 0x18u),
                guest_u32(cue + 0x1Cu), guest_u32(cue + 0x24u),
                guest_f32(cue + 0x28u), guest_f32(cue + 0x2Cu),
                guest_u32(cue + 0x30u), guest_u32(cue + 0x34u),
                manager ? guest_u32(manager + 0x9A00u) : 0u,
                manager ? guest_u32(manager + 0x9A04u) : 0u);
        fflush(stderr);
    }
}

void recomp_xact_alloc_checkpoint(uint32_t stage, uint32_t owner,
                                  uint32_t object, uint32_t result)
{
    static uint64_t diagnostic_stage_counts[64];
    uint64_t diagnostic_count = 0u;
    if (stage >= 60u && stage < 64u) {
        diagnostic_count = ++diagnostic_stage_counts[stage];
        if (diagnostic_count > 16u &&
            (diagnostic_count & (diagnostic_count - 1u)) != 0u)
            return;
    }
    const int internal_failure_only =
        getenv("MERCENARIES_TRACE_XACT_INTERNAL_FAILURES") != NULL &&
        ((stage == 1u && object == 0u) ||
         ((stage == 31u || stage == 34u) && result == 0u) ||
         ((stage == 2u || stage == 20u || stage == 21u) &&
          (int32_t)result < 0) ||
         (((stage >= 30u && stage <= 36u) ||
           (stage >= 40u && stage <= 42u)) && (int32_t)result < 0));
    if (g_trace_xact_target_alloc_active && stage < 64u)
        ++g_trace_xact_target_alloc_counts[stage];
    if (getenv("MERCENARIES_TRACE_XACT_ALLOCS") != NULL ||
        (g_trace_xact_cues_after_movie_active &&
         getenv("MERCENARIES_TRACE_XACT_CUES_AFTER_MOVIE") != NULL &&
         stage != 30u) ||
        internal_failure_only) {
        fprintf(stderr,
                "[XACT-ALLOC] stage=%u count=%llu owner=%08X object=%08X result=%08X\n",
                stage, (unsigned long long)diagnostic_count, owner, object, result);
        if ((stage == 50u || stage == 51u) &&
            object >= 0x10000u && object <= 0x04000000u - 0x18u &&
            owner >= 0x10000u && owner <= 0x04000000u - 0x28u) {
            const uint32_t completed = guest_u32(object + 8u);
            const uint32_t status = guest_u32(object + 12u);
            const uint32_t voice = guest_u32(owner + 0x24u);
            fprintf(stderr, "[XACT-PACKET] stage=%u stream=%08X vtable=%08X "
                    "packet=%08X data=%08X bytes=%u completed=%08X/%08X "
                    "status=%08X/%08X voice=%08X",
                    stage, owner, guest_u32(owner), object,
                    guest_u32(object), guest_u32(object + 4u), completed,
                    completed >= 0x10000u && completed <= 0x03FFFFFCu ?
                        guest_u32(completed) : 0xFFFFFFFFu,
                    status, status >= 0x10000u && status <= 0x03FFFFFCu ?
                        guest_u32(status) : 0xFFFFFFFFu, voice);
            if (voice >= 0x10000u && voice <= 0x04000000u - 0x88u)
                fprintf(stderr, " voice_flags=%04X head=%08X/%08X "
                        "pending=%08X/%08X",
                        guest_u16(voice + 0x12u),
                        guest_u32(voice + 0x18u), guest_u32(voice + 0x1Cu),
                        guest_u32(voice + 0x20u), guest_u32(voice + 0x24u));
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
}

void recomp_xact_setup_checkpoint(uint32_t stage, uint32_t manager,
                                  uint32_t cue_name, uint32_t value0,
                                  uint32_t value1)
{
    if (recomp_trace_xact_enabled() ||
        ((stage == 17u || stage == 18u) &&
         getenv("MERCENARIES_TRACE_HASH_TABLE") != NULL &&
         cue_name == guest_u32(0x00371DC8u)))
        fprintf(stderr, "[XACT-SETUP] stage=%u manager=%08X cue=%08X values=%08X/%08X\n",
                stage, manager, cue_name, value0, value1);
}

void recomp_xact_play_checkpoint(uint32_t stage, uint32_t cue,
                                  uint32_t config, uint32_t result)
{
    /* Simulate missing banks before any low-level voice is created. Only
     * the welcome cue armed by the private HQ harness is affected. */
    if (stage == 2u && recomp_test_hq_managed_failure_hash &&
        config >= 0x10000u && config <= 0x03fffff8u &&
        guest_u32(config + 4u) == recomp_test_hq_managed_failure_hash) {
        recomp_test_hq_managed_failure_hash = 0;
        g_eax = 0;
        xbox_preview_log_event("hq-entry", "private-test managed welcome cue bank failure handle=%08X", guest_u32(cue + 4u));
    }
    /* These are the Allied pilot's airborne and smoke/LZ acknowledgements.
     * Record request, bank readiness, and returned low-level handle separately;
     * a valid handle alone does not prove that a line was audible. */
    if (config >= 0x10000u && config <= 0x03fffff8u &&
        xbox_preview_log_enabled()) {
        const uint32_t name = guest_u32(config + 4u);
        if (name == 0x23F08077u || name == 0xD24E4223u)
            xbox_preview_log_event("extraction-voice",
                "stage=%u cue=%08X managed=%08X result=%08X low_level=%u",
                stage, name, cue, result,
                stage == 3u ? recomp_xact_low_level_handle_exists(result) : 0u);
    }
    if (recomp_trace_xact_enabled() ||
        getenv("MERCENARIES_TRACE_XACT_PLAY") != NULL ||
        recomp_trace_hq_audio_enabled())
        fprintf(stderr,
                "[XACT-PLAY] stage=%u cue=%08X config=%08X result=%08X "
                "low-level=%u\n",
                stage, cue, config, result,
                stage == 3u ? recomp_xact_low_level_handle_exists(result) : 0);
}

void recomp_xact_properties_checkpoint(uint32_t wrapper, uint32_t properties,
                                       uint32_t result)
{
    if (getenv("MERCENARIES_TRACE_XACT_PROPERTIES") == NULL &&
        !recomp_trace_hq_audio_enabled())
        return;
    fprintf(stderr,
            "[XACT-PROPERTIES] wrapper=%08X props=%08X result=%08X "
            "flags=%08X category=%08X length=%u maxdist=%g cue-index=%u\n",
            wrapper, properties, result,
            guest_u32(properties + 0x00u), guest_u32(properties + 0x18u),
            guest_u32(properties + 0x34u),
            (double)guest_f32(properties + 0x58u),
            guest_u32(wrapper + 0x18u));
    fflush(stderr);
}

void recomp_xact_managed_checkpoint(uint32_t stage, uint32_t handle,
                                    uint32_t cue_name, uint32_t value)
{
    static int dumped_full_weapon_pool;
    static int dumped_requested_pool;
    const int dump_requested =
        getenv("MERCENARIES_TRACE_XACT_MANAGED_SLOTS") != NULL;
    const int weapon_cue =
        getenv("MERCENARIES_TRACE_WEAPON_FIRE_SOUND") != NULL &&
        (cue_name == 0x6E3A9CDAu || cue_name == 0x7240BE8Au);
    if (recomp_trace_xact_enabled() ||
        getenv("MERCENARIES_TRACE_MUSIC") != NULL || weapon_cue) {
        const uint32_t manager = guest_u32(0x00690AC0u);
        unsigned int active = 0u, due = 0u, loops = 0u;
        unsigned int states[5] = {0u, 0u, 0u, 0u, 0u};
        unsigned int i;
        if (manager >= 0x00010000u && manager < 0x04000000u - 0x1680u) {
            for (i = 0u; i < 64u; ++i) {
                if (guest_u8(manager + 4u + i) != 0u) {
                    const uint32_t object = manager + 0x44u + i * 0x50u;
                    const uint32_t state = guest_u32(object + 0x38u);
                    const float length = guest_f32(object + 0x40u);
                    const float remaining = guest_f32(object + 0x44u);
                    ++active;
                    if (state < 5u)
                        ++states[state];
                    if (length < 0.0f)
                        ++loops;
                    else if (state == 2u && remaining < -0.25f)
                        ++due;
                }
            }
        }
        fprintf(stderr,
                "[XACT-MANAGED] stage=%u handle=%08X cue=%08X value=%08X "
                "map=%u/%08X/%08X default=%08X manager=%08X "
                "pool=%u/%u next=%u active=%u states=%u/%u/%u/%u/%u "
                "due=%u loops=%u\n",
                stage, handle, cue_name, value,
                guest_u32(0x00690B3Cu), guest_u32(0x00690B48u),
                guest_u32(0x00690B44u), guest_u32(0x00690B4Cu), manager,
                manager ? guest_u16(manager) : 0u,
                manager ? guest_u16(manager + 2u) : 0u,
                manager ? guest_u32(manager + 0x1644u) : 0u,
                active, states[0], states[1], states[2], states[3], states[4],
                due, loops);
        if (((!dumped_full_weapon_pool && weapon_cue && stage == 2u &&
              active == 64u) ||
             (!dumped_requested_pool && dump_requested && stage == 3u &&
              cue_name == 0x03E4CB2Au)) &&
            manager >= 0x00010000u && manager < 0x04000000u - 0x1680u) {
            if (weapon_cue && active == 64u)
                dumped_full_weapon_pool = 1;
            if (dump_requested && cue_name == 0x03E4CB2Au)
                dumped_requested_pool = 1;
            for (i = 0u; i < 64u; ++i) {
                if (guest_u8(manager + 4u + i) != 0u) {
                    const uint32_t object = manager + 0x44u + i * 0x50u;
                    const uint32_t config = guest_u32(object + 0x34u);
                    fprintf(stderr,
                            "  [XACT-MANAGED-SLOT] slot=%u object=%08X "
                            "cue-handle=%08X sound-handle=%08X config=%08X "
                            "name=%08X state=%u length=%g remaining=%g "
                            "called=%u stream=%u\n",
                            i, object, guest_u32(object + 4u),
                            guest_u32(object + 8u), config,
                            config ? guest_u32(config + 4u) : 0u,
                            guest_u32(object + 0x38u),
                            guest_f32(object + 0x40u),
                            guest_f32(object + 0x44u),
                            guest_u8(object + 0x3Cu),
                            guest_u8(object + 0x3Du));
                }
            }
        }
        fflush(stderr);
    }
}

int recomp_xact_low_level_handle_exists(uint32_t handle)
{
    const uint32_t pool = 0x0085C600u + 0x7568u;
    const uint32_t count = guest_u16(pool);
    uint32_t i;

    if (handle == 0u || count > 1024u)
        return 0;
    for (i = 0u; i < count; ++i) {
        if (guest_u32(pool + 0x1C84u + i * 8u) == handle)
            return 1;
    }
    return 0;
}

void recomp_xact_managed_update_checkpoint(uint32_t cue)
{
    static unsigned int samples;
    const uint32_t state = guest_u32(cue + 0x38u);
    const uint32_t sound_handle = guest_u32(cue + 8u);
    const float length = guest_f32(cue + 0x40u);
    if (getenv("MERCENARIES_TRACE_XACT_FAILURES") == NULL ||
        samples >= 256u || state != 2u || sound_handle == 0u ||
        length != 0.0f)
        return;
    ++samples;
    fprintf(stderr,
            "[XACT-MANAGED-STALE] n=%u cue=%08X cue-handle=%08X "
            "sound-handle=%08X config=%08X name=%08X state=%u "
            "length=%g remaining=%g called=%u stream=%u low-level=%u\n",
            samples, cue, guest_u32(cue + 4u), sound_handle,
            guest_u32(cue + 0x34u),
            guest_u32(guest_u32(cue + 0x34u) + 4u), state,
            length, guest_f32(cue + 0x44u), guest_u8(cue + 0x3Cu),
            guest_u8(cue + 0x3Du),
            recomp_xact_low_level_handle_exists(sound_handle));
    fflush(stderr);
}
void recomp_xact_prepare_failure(uint32_t manager, uint32_t wrapper,
                                  uint32_t result)
{
    /* Keep failure evidence throughout long sessions. The former lifetime
     * limit discarded every failure after the first 16, including later radio
     * dialogue. Bound bursts per ten seconds; always retain the two AN
     * extraction cues that prompted this diagnostic. Playback is unchanged. */
    static uint64_t window_start;
    static unsigned samples, suppressed;
    if (!xbox_preview_log_enabled()) return;
    const uint64_t now = GetTickCount64();
    const uint32_t name = guest_u32(wrapper + 0x1Cu);
    if (now - window_start >= 10000u) {
        window_start = now;
        samples = 0u;
    }
    if (samples < 16u || name == 0x23F08077u || name == 0xD24E4223u) {
        ++samples;
        xbox_preview_log_event("audio-prepare-failure",
            "result=%08X cue=%08X handle=%08X source=%d first_free=%u "
            "bank=%08X bank_cue=%u suppressed=%u",
            result, name, guest_u32(wrapper + 4u),
            (int32_t)guest_u32(wrapper + 0xCu), guest_u32(manager + 0x7564u),
            guest_u32(wrapper + 0x14u), guest_u32(wrapper + 0x18u), suppressed);
        suppressed = 0u;
    } else if (suppressed != UINT32_MAX) {
        ++suppressed;
    }
}

/* Called only after GetNotification drains its queue, under XACT's existing
 * critical section. A cue can finish before the game registers its stop event,
 * or its event can be lost when XACT's fixed notification pool fills. Reconcile
 * only registered, terminal cues; never infer completion from elapsed time.
 * The caller unregisters the recovered event and the game's ordinary consumer
 * destroys the cue, releases its source, and delivers the managed callback. */
int recomp_xact_recover_stop_notification(uint32_t engine, uint32_t filter,
                                          uint32_t output)
{
    const uint32_t manager = 0x0085C600u, pool = manager + 0x7568u;
    if (filter || !engine || guest_u32(manager + 4u) != engine + 8u ||
        output < 0x10000u || output > 0x04000000u - 40u ||
        guest_u32(manager + 0x99F0u) >= 128u)
        return 0;
    const unsigned count = guest_u16(pool);
    if (count > 128u) return 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t entry = pool + 0x1C84u + i * 8u;
        const uint32_t wrapper = guest_u32(entry + 4u);
        if (wrapper < pool + 0x84u || wrapper > pool + 0x1C84u - 0x38u ||
            (wrapper - pool - 0x84u) % 0x38u ||
            guest_u32(wrapper) != 4u || !guest_u32(entry) ||
            guest_u32(wrapper + 4u) != guest_u32(entry))
            continue;
        const uint32_t cue = guest_u32(wrapper + 8u);
        if (cue < 0x10000u || cue > 0x04000000u - 0x40u ||
            !(guest_u8(cue + 0x20u) & 4u) ||
            guest_u32(cue + 0x28u) || guest_u32(cue + 0x2Cu) ||
            guest_u32(cue + 0x30u))
            continue;
        const uint32_t descriptors = guest_u32(cue + 0x24u);
        const uint32_t bank = guest_u32(wrapper + 0x14u);
        if (bank < 0x10000u || bank > 0x04000000u - 12u ||
            guest_u32(cue + 0x14u) != guest_u32(bank + 8u)) continue;
        const uint32_t *stop = NULL;
        /* Prefer the actual registered event; an explicit failed registration
         * is the only alternative. Absence alone does not establish intent. */
        if (descriptors >= 0x10000u && descriptors <= 0x04000000u - 48u)
            stop = (const uint32_t *)guest_ptr(descriptors + 24u);
        if (!stop || stop[0] != 0x00040001u || stop[3] != cue ||
            stop[1] != guest_u32(bank + 8u)) {
            const int slot = recomp_xact_wrapper_slot(wrapper);
            if (slot < 0) continue;
            const recomp_xact_failed_subscription *pending = &xact_failed_subscriptions[slot];
            if (pending->handle != guest_u32(entry) || pending->cue != cue ||
                pending->descriptor[1] != guest_u32(bank + 8u)) continue;
            if (getenv("MERCENARIES_TEST_GAMEPAD_FILE") &&
                getenv("MERCENARIES_TEST_NO_STOP_SUBSCRIPTION_RECOVERY")) continue;
            stop = pending->descriptor;
        }
        memset(guest_ptr(output), 0, 40u);
        memcpy(guest_ptr(output), stop, 24u);
        *(uint32_t *)guest_ptr(output + 8u) = guest_u16(cue + 0x18u);
        static unsigned reports;
        if (reports++ < 32u && xbox_preview_log_enabled())
            xbox_preview_log_event("audio-stop-recovered",
                "handle=%08X cue=%08X hash=%08X source=%d flags=%02X",
                guest_u32(entry), cue, guest_u32(wrapper + 0x1Cu),
                (int32_t)guest_u32(wrapper + 0xCu), guest_u8(cue + 0x20u));
        return 1;
    }
    return 0;
}

static void recomp_audio_source_preview_snapshot(void)
{
    const uint32_t manager = 0x0085C600u, pool = manager + 0x7568u;
    unsigned count = guest_u16(pool), used = 0, invalid = 0, stopped = 0;
    if (count > 128u) return;
    for (unsigned i = 0; i < 64u; ++i)
        used += guest_u8(manager + 0x7524u + i) != 0u;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t wrapper = guest_u32(pool + 0x1C88u + i * 8u);
        if (wrapper < pool + 0x84u || wrapper > pool + 0x1C84u - 0x38u)
            continue;
        uint32_t state = guest_u32(wrapper), cue = guest_u32(wrapper + 8u);
        invalid += state == 0u && cue == 0u;
        if (state == 4u && cue >= 0x10000u && cue <= 0x04000000u - 0x40u &&
            (guest_u8(cue + 0x20u) & 4u) &&
            !guest_u32(cue + 0x28u) && !guest_u32(cue + 0x2Cu) &&
            !guest_u32(cue + 0x30u)) ++stopped;
    }
    xbox_preview_log_event("audio-sources",
        "used=%u/64 wrappers=%u invalid=%u stopped_pending=%u first_free=%u",
        used, count, invalid, stopped, guest_u32(manager + 0x7564u));
}

/* Preview timeline only: bounded, read-only music state every five seconds.
 * A healthy host output queue does not prove that a managed music cue exists.
 * No guest calls, heap allocation, audio locks or playback recovery here. */
void recomp_music_preview_snapshot(void)
{
    static ULONGLONG last;
    static int sampled;
    const uint32_t dj=0x0037B9B4u;
    uint32_t cue=0, sound=0, cue_state=0xffffffffu, count=0xffffffffu;
    uint32_t prepared=0, called=0, low_level=0;
    float length=0, remaining=0;
    if(!g_xbox_mem_offset || !xbox_preview_log_enabled())return;
    ULONGLONG now=GetTickCount64();
    if(sampled && now-last<5000)return;
    sampled=1;last=now;
    recomp_audio_source_preview_snapshot();
    const uint32_t handle=guest_u32(dj),pool=guest_u32(0x00690AC0u);
    /* RedXactCue pool: at most 64 handle/pointer entries, retail Find at
     * 001FE410. Validate the complete table and cue before following them. */
    if(pool>=0x10000u && pool<=0x04000000u-0x1644u){
        count=guest_u16(pool);
        if(handle && count<=64u)for(uint32_t i=0;i<count;++i){
            if(guest_u32(pool+0x1444u+i*8u)!=handle)continue;
            uint32_t candidate=guest_u32(pool+0x1448u+i*8u);
            if(candidate>=0x10000u && candidate<=0x04000000u-0x48u &&
               guest_u32(candidate+4u)==handle)cue=candidate;
            break;
        }
    }
    if(cue){
        sound=guest_u32(cue+8u);cue_state=guest_u32(cue+0x38u);
        called=guest_u8(cue+0x3Cu);prepared=guest_u8(cue+0x3Du);
        length=guest_f32(cue+0x40u);remaining=guest_f32(cue+0x44u);
        low_level=recomp_xact_low_level_handle_exists(sound);
    }
    xbox_preview_log_event("music-state",
        "handle=%08X track=%08X faction=%08X mute=%u duck=%u explore=%u "
        "tension_enabled=%u lockout=%.6g cue=%08X cue_state=%u "
        "sound=%08X low_level=%u prepared=%u called=%u length=%.6g "
        "remaining=%.6g managed_count=%u",
        handle,guest_u32(dj+4u),guest_u32(dj+8u),guest_u8(dj+0x2Cu),
        guest_u8(dj+0x2Du),guest_u8(dj+0xCu),guest_u8(dj+0xDu),
        guest_f32(dj+0x28u),cue,cue_state,sound,low_level,prepared,called,
        length,remaining,count);
}

void recomp_music_checkpoint(uint32_t stage, uint32_t state, uint32_t value)
{
    /* Existing rare StartMusic entry/exit hooks are enough to correlate a
     * request with the periodic state, including failed/stale handles. */
    if((stage==10u || stage==11u) && g_xbox_mem_offset &&
       state>=0x10000u && state<=0x04000000u-0x30u && xbox_preview_log_enabled())
        xbox_preview_log_sample("music-request",
            "phase=%s requested=%08X handle=%08X track=%08X faction=%08X mute=%u duck=%u",
            stage==10u?"begin":"end",value,guest_u32(state),guest_u32(state+4u),
            guest_u32(state+8u),guest_u8(state+0x2Cu),guest_u8(state+0x2Du));
    if (getenv("MERCENARIES_TRACE_MUSIC") == NULL)
        return;
    fprintf(stderr,
            "[MUSIC] stage=%u state=%08X value=%08X "
            "handle=%08X name=%08X faction=%08X avg=%g flags=%02X/%02X\n",
            stage, state, value,
            guest_u32(state), guest_u32(state + 4u), guest_u32(state + 8u),
            guest_f32(state + 0x10u), guest_u8(state + 0x0Cu),
            guest_u8(state + 0x0Du));
    fflush(stderr);
}

void recomp_xact_bank_checkpoint(uint32_t stage, uint32_t manager)
{
    if (getenv("MERCENARIES_TRACE_XACT_BANKS") == NULL)
        return;
    fprintf(stderr,
            "[XACT-BANKS] stage=%u manager=%08X engine=%08X waves=%u sounds=%u\n",
            stage, manager, guest_u32(manager + 4u),
            guest_u32(manager + 0x741Cu), guest_u32(manager + 0x7420u));
    fflush(stderr);
}
void recomp_xact_voice_start_checkpoint(uint32_t stage, uint32_t voice,
                                        uint32_t value0, uint32_t value1)
{
    static uint32_t samples;
    if ((getenv("MERCENARIES_TRACE_XACT_VOICE_START") == NULL &&
         !recomp_trace_hq_audio_enabled()) ||
        samples++ >= 512u)
        return;
    fprintf(stderr,
            "[XACT-VOICE-START] stage=%u voice=%08X values=%08X/%08X "
            "flags=%04X count=%u handles=%04X,%04X,%04X,%04X\n",
            stage, voice, value0, value1, guest_u16(voice + 0x12u),
            guest_u8(voice + 0x64u), guest_u16(voice + 0x0Cu),
            guest_u16(voice + 0x0Eu), guest_u16(voice + 0x10u),
            guest_u16(voice + 0x12u));
    fflush(stderr);
}
int recomp_xact_stream_has_unsubmitted_read(uint32_t stream)
{
    unsigned int i;
    if (stream < 0x00010000u || stream > 0x04000000u - 0x50u)
        return 0;
    for (i = 0u; i < 2u; ++i) {
        const uint32_t slot = stream + 0x10u + i * 0x20u;
        /* AAAA belongs to the file-read side; XMediaObject::Process changes
         * it to FFFF. EOF must not discard a successful read just because
         * the PC completed it before the second buffer was requested.
         * This predicate is used only at stream EOF, not stop/free teardown. */
        if (guest_u32(slot + 8u) == 0xAAAAu &&
            guest_u32(slot + 4u) != 0u &&
            (int32_t)guest_u32(slot + 0xCu) >= 0)
            return 1;
    }
    return 0;
}

void recomp_xact_sound_update_checkpoint(uint32_t stage, uint32_t sound,
                                         uint32_t event, uint32_t value)
{
    static int initialized;
    static int enabled;
    static uint32_t target;
    static uint32_t samples;
    static uint32_t wave_filter;
    static int filter_wave;
    static int after_fire;
    static uint32_t sample_limit = 8192u;
    int sound_valid;
    int event_valid;

    if (!initialized) {
        const char *target_text = getenv("MERCENARIES_TRACE_XACT_SOUND_TARGET");
        const char *wave_text = getenv("MERCENARIES_TRACE_XACT_WAVE_ID");
        const char *limit_text = getenv("MERCENARIES_TRACE_XACT_SOUND_LIMIT");
        enabled = getenv("MERCENARIES_TRACE_XACT_SOUND_UPDATE") != NULL;
        after_fire = getenv("MERCENARIES_TRACE_XACT_SOUND_AFTER_FIRE") != NULL;
        target = target_text ? (uint32_t)strtoul(target_text, NULL, 0) : 0u;
        filter_wave = wave_text != NULL;
        wave_filter = wave_text ? (uint32_t)strtoul(wave_text, NULL, 0) : 0u;
        /* Longer opt-in recordings must still have a finite event budget.
         * Keep the existing default; reject malformed/zero/negative input. */
        if (limit_text && limit_text[0] >= '0' && limit_text[0] <= '9') {
            char *limit_end;
            const unsigned long requested = strtoul(limit_text, &limit_end, 10);
            if (*limit_end == '\0' && requested != 0ul)
                sample_limit = requested > 65536ul ? 65536u : (uint32_t)requested;
        }
        initialized = 1;
    }
    if (!enabled || samples >= sample_limit || (target != 0u && sound != target))
        return;
    /* Optional late-game diagnostic: do not exhaust the bounded trace on
     * boot music before the first actual weapon sound request. No guest
     * reads, gameplay changes or budget consumption occur before arming. */
    if (after_fire && !g_trace_weapon_fire_seen)
        return;

    sound_valid = sound >= 0x00010000u && sound <= 0x04000000u - 0x50u;
    event_valid = event >= 0x00010000u && event <= 0x04000000u - 0x88u;
    if (filter_wave && (!event_valid || guest_u16(event + 0x56u) != wave_filter))
        return;
    ++samples;
    fprintf(stderr,
            "[XACT-SOUND-UPDATE] n=%u stage=%u sound=%08X event=%08X value=%08X ms=%llu",
            samples, stage, sound, event, value,
            (unsigned long long)GetTickCount64());
    if (sound_valid)
        fprintf(stderr,
                " state=%04X config=%08X events=%08X flags=%08X owner=%08X"
                " cursor=%08X active=%08X",
                guest_u16(sound + 0x30u), guest_u32(sound + 0x10u),
                guest_u32(sound + 0x34u), guest_u32(sound + 0x38u),
                guest_u32(sound + 0x44u), guest_u32(sound + 0x4Cu),
                guest_u32(sound + 0x40u));
    if (event_valid)
        fprintf(stderr,
                " event_flags=%04X object=%08X list=%08X/%08X wave=%08X"
                " stream=%08X slots=%08X/%08X/%08X/%08X"
                " timing=%08X/%08X/%08X/%08X tail=%08X/%08X/%08X",
                guest_u16(event), guest_u32(event + 4u),
                guest_u32(event + 0x0Cu), guest_u32(event + 0x10u),
                guest_u32(event + 0x38u), guest_u32(event + 0x40u),
                guest_u32(event + 0x44u), guest_u32(event + 0x48u),
                guest_u32(event + 0x4Cu), guest_u32(event + 0x50u),
                guest_u32(event + 0x54u), guest_u32(event + 0x58u),
                guest_u32(event + 0x5Cu), guest_u32(event + 0x60u),
                guest_u32(event + 0x68u), guest_u32(event + 0x7Cu),
                guest_u32(event + 0x84u));
    if (event_valid) {
        const uint32_t stream = guest_u32(event + 0x40u);
        const uint32_t object = guest_u32(event + 4u);
        if (stream >= 0x00010000u && stream <= 0x04000000u - 0x50u)
            fprintf(stderr,
                    " io0=%08X/%08X/%08X/%08X io1=%08X/%08X/%08X/%08X irql=%u",
                    guest_u32(stream + 0x10u), guest_u32(stream + 0x14u),
                    guest_u32(stream + 0x18u), guest_u32(stream + 0x1Cu),
                    guest_u32(stream + 0x30u), guest_u32(stream + 0x34u),
                    guest_u32(stream + 0x38u), guest_u32(stream + 0x3Cu),
                    (unsigned int)*(volatile uint8_t *)guest_ptr(0x24u));
        if (object >= 0x00010000u && object <= 0x04000000u - 0x28u)
            fprintf(stderr,
                    " object_data=%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X/%08X",
                    guest_u32(object), guest_u32(object + 4u),
                    guest_u32(object + 8u), guest_u32(object + 0xCu),
                    guest_u32(object + 0x10u), guest_u32(object + 0x14u),
                    guest_u32(object + 0x18u), guest_u32(object + 0x1Cu),
                    guest_u32(object + 0x20u), guest_u32(object + 0x24u));
    }
    /* Stage 100 receives the actual queued packet, not a guessed wave ID. */
    if (stage == 100u && value >= 0x00010000u &&
        value <= 0x04000000u - 0x20u) {
        const uint32_t packet = guest_u32(value + 0x14u);
        fprintf(stderr, " due=%08X:%08X packet=%08X",
                guest_u32(value + 0x1Cu), guest_u32(value + 0x18u), packet);
        if (packet >= 0x00010000u && packet <= 0x04000000u - 0x10u)
            fprintf(stderr, " packet_data=%08X/%08X/%08X/%08X",
                    guest_u32(packet), guest_u32(packet + 4u),
                    guest_u32(packet + 8u), guest_u32(packet + 0xCu));
    }
    fputc('\n', stderr);
    fflush(stderr);
}
static int recomp_spatial_object_valid(uint32_t object)
{
    uint32_t vtable;
    if (object < 0x00010000u || object > 0x04000000u - 4u)
        return 0;
    vtable = guest_u32(object);
    return vtable >= 0x00200000u && vtable < 0x00400000u;
}

static volatile DWORD g_actor_query_game_thread_id;

static volatile LONG g_game_stall_watchdog_started;
static volatile DWORD g_game_stall_watchdog_thread_id;
static volatile LONG g_game_progress_snapshot_enabled;

#if defined(_M_X64)
static void recomp_dump_suspended_native_stack(const char *tag,
                                                CONTEXT context)
{
    unsigned int frame;

    fprintf(stderr, "[%s-STACK]", tag);
    for (frame = 0u; frame < 24u && context.Rip != 0u; ++frame) {
        DWORD64 image_base = 0u;
        PRUNTIME_FUNCTION runtime_function = RtlLookupFunctionEntry(
            context.Rip, &image_base, NULL);
        HMODULE frame_module = NULL;
        char module_name[MAX_PATH] = "unknown";
        DWORD64 module_base = 0u;

        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)(uintptr_t)context.Rip, &frame_module)) {
            char module_path[MAX_PATH];
            const char *base_name;
            module_base = (DWORD64)(uintptr_t)frame_module;
            if (GetModuleFileNameA(frame_module, module_path,
                                   sizeof(module_path)) != 0u) {
                base_name = strrchr(module_path, '\\');
                strncpy_s(module_name, sizeof(module_name),
                          base_name ? base_name + 1 : module_path, _TRUNCATE);
            }
        }
        fprintf(stderr, "\n  #%02u %s+0x%llX rip=%p rsp=%p", frame,
                module_name,
                (unsigned long long)(module_base != 0u ?
                    context.Rip - module_base : context.Rip),
                (void *)(uintptr_t)context.Rip,
                (void *)(uintptr_t)context.Rsp);

        if (runtime_function != NULL) {
            PVOID handler_data = NULL;
            DWORD64 establisher_frame = 0u;
            const DWORD64 previous_rip = context.Rip;
            const DWORD64 previous_rsp = context.Rsp;
            __try {
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip,
                                 runtime_function, &context, &handler_data,
                                 &establisher_frame, NULL);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                fprintf(stderr, " [unwind-fault=%08lX]",
                        (unsigned long)GetExceptionCode());
                context.Rip = 0u;
            }
            if (context.Rip != 0u && context.Rip == previous_rip &&
                context.Rsp == previous_rsp) {
                fprintf(stderr, " [unwind-no-progress]");
                break;
            }
        } else {
            DWORD64 return_address = 0u;
            SIZE_T bytes_read = 0u;
            if (context.Rsp == 0u ||
                !ReadProcessMemory(GetCurrentProcess(),
                                   (const void *)(uintptr_t)context.Rsp,
                                   &return_address, sizeof(return_address),
                                   &bytes_read) ||
                bytes_read != sizeof(return_address))
                break;
            context.Rip = return_address;
            context.Rsp += sizeof(return_address);
        }
    }
    fputc('\n', stderr);
}
#endif

static void recomp_dump_game_stall_snapshot(const char *tag,
                                            uint32_t stalled_ms)
{
    HANDLE game_thread = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE, g_game_stall_watchdog_thread_id);

    if (game_thread != NULL) {
        CONTEXT context;
        HMODULE module = GetModuleHandleW(NULL);
        int have_context = 0;
        int thread_suspended = 0;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(game_thread) != (DWORD)-1) {
            thread_suspended = 1;
            have_context = GetThreadContext(game_thread, &context) != 0;
        }
        if (have_context) {
#if defined(_M_X64)
            fprintf(stderr,
                    "[%s-HOST] stalled_ms=%u tid=%lu rip=%p rva=%08llX "
                    "rsp=%p rax=%p rbx=%p rcx=%p rdx=%p rsi=%p rdi=%p\n",
                    tag, stalled_ms,
                    (unsigned long)g_game_stall_watchdog_thread_id,
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
            recomp_dump_suspended_native_stack(tag, context);
#endif
        }
        if (thread_suspended)
            ResumeThread(game_thread);
        CloseHandle(game_thread);
    }

    fprintf(stderr,
            "[%s-GUEST] stalled_ms=%u current=%08X "
            "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "ebp=%08X esp=%08X recent_index=%u\n",
            tag, stalled_ms, (uint32_t)g_recomp_current_func,
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_seh_ebp, g_esp,
            (uint32_t)g_recomp_recent_game_func_idx);
    {
        const uint32_t end = g_recomp_recent_game_func_idx;
        const uint32_t count = end < 96u ? end : 96u;
        fprintf(stderr, "[%s-RECENT] count=%u", tag, count);
        for (uint32_t i = 0u; i < count; ++i) {
            if ((i & 7u) == 0u)
                fprintf(stderr, "\n  ");
            fprintf(stderr, "%08X ",
                    (uint32_t)g_recomp_recent_game_funcs[
                        (end - count + i) & 255u]);
        }
        fputc('\n', stderr);
    }
    fflush(stderr);
}

static DWORD WINAPI recomp_game_stall_watchdog_thread(LPVOID parameter)
{
    const uint32_t threshold_ms = (uint32_t)(uintptr_t)parameter;
    const uint32_t sample_ms = threshold_ms <= 100u ? 2u : 500u;
    uint32_t last_heartbeat = g_recomp_watchdog_heartbeat;
    uint32_t unchanged_ms = 0u;
    uint32_t progress_ms = 0u;
    int reported = 0;

    for (;;) {
        uint32_t heartbeat;
        Sleep(sample_ms);
        progress_ms += sample_ms;
        if (g_game_progress_snapshot_enabled && progress_ms >= threshold_ms) {
            recomp_dump_game_stall_snapshot("GAME-PROGRESS", progress_ms);
            progress_ms = 0u;
        }
        heartbeat = g_recomp_watchdog_heartbeat;
        if (heartbeat != last_heartbeat) {
            last_heartbeat = heartbeat;
            unchanged_ms = 0u;
            reported = 0;
            continue;
        }
        unchanged_ms += sample_ms;
        if (!reported && unchanged_ms >= threshold_ms) {
            recomp_dump_game_stall_snapshot("GAME-STALL", unchanged_ms);
            reported = 1;
        }
    }
}

static void recomp_arm_game_stall_watchdog(void)
{
    char value[32];
    char *end = NULL;
    unsigned long threshold_ms;
    HANDLE thread;
    DWORD value_length;

    if (InterlockedCompareExchange(&g_game_stall_watchdog_started, 1, 0) != 0)
        return;
    value_length = GetEnvironmentVariableA(
        "MERCENARIES_TRACE_GAME_STALL_MS", value, sizeof(value));
    if (value_length == 0u) {
        /* Deterministic long-route tests must always preserve evidence if the
         * title enters a hot guest loop and therefore stops polling input. */
        if (getenv("MERCENARIES_TEST_AUTO_DRIVE_GATE_ROUTE") == NULL) {
            InterlockedExchange(&g_game_stall_watchdog_started, 0);
            return;
        }
        /* Aircraft/Humvee scene transitions can legitimately spend over
         * five seconds in a synchronous D3D wait before input polling
         * resumes.  Fifteen seconds still catches the observed indefinite
         * post-roadblock freeze while avoiding that known false positive. */
        threshold_ms = 15000u;
    } else {
        threshold_ms = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || threshold_ms < 20u ||
            threshold_ms > 60000u) {
            InterlockedExchange(&g_game_stall_watchdog_started, 0);
            return;
        }
    }
    g_game_stall_watchdog_thread_id = GetCurrentThreadId();
    g_game_progress_snapshot_enabled =
        getenv("MERCENARIES_TRACE_GAME_PROGRESS") != NULL;
    g_recomp_watchdog_heartbeat_enabled = 1u;
    thread = CreateThread(NULL, 0, recomp_game_stall_watchdog_thread,
                          (LPVOID)(uintptr_t)(uint32_t)threshold_ms, 0, NULL);
    if (thread != NULL) {
        CloseHandle(thread);
        fprintf(stderr, "[GAME-STALL] watchdog armed threshold_ms=%lu tid=%lu\n",
                threshold_ms,
                (unsigned long)g_game_stall_watchdog_thread_id);
        fflush(stderr);
    } else {
        g_recomp_watchdog_heartbeat_enabled = 0u;
        InterlockedExchange(&g_game_stall_watchdog_started, 0);
    }
}
static DWORD WINAPI recomp_actor_query_stall_probe_thread(LPVOID parameter)
{
    uint32_t packed = (uint32_t)(uintptr_t)parameter;
    uint32_t event = packed >> 24;
    uint32_t delay_ms = packed & 0x00FFFFFFu;
    uint32_t end, count, i;

    Sleep(delay_ms);
    {
        HANDLE game_thread = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                THREAD_QUERY_INFORMATION,
            FALSE, g_actor_query_game_thread_id);
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
            if (have_context) {
#if defined(_M_X64)
                fprintf(stderr,
                        "[ACTOR-QUERY-HOST-PROBE] event=%u tid=%lu "
                        "rip=%p rva=%08llX rsp=%p rax=%p rbx=%p "
                        "rcx=%p rdx=%p rsi=%p rdi=%p\n",
                        event, (unsigned long)g_actor_query_game_thread_id,
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
#endif
            }
            CloseHandle(game_thread);
        }
    }
    fprintf(stderr,
            "[ACTOR-QUERY-STALL-PROBE] event=%u after=%u current=%08X "
            "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "ebp=%08X esp=%08X\n",
            event, delay_ms, (uint32_t)g_recomp_current_func,
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_seh_ebp, g_esp);
    end = g_recomp_recent_func_idx;
    count = end < 32u ? end : 32u;
    fprintf(stderr, "[ACTOR-QUERY-STALL-RECENT] count=%u\n", count);
    for (i = 0u; i < count; ++i) {
        uint32_t index = (end - count + i) & 63u;
        fprintf(stderr, "  [%02u] %08X\n", i,
                (uint32_t)g_recomp_recent_funcs[index]);
    }
    end = g_recomp_recent_game_func_idx;
    count = end < 64u ? end : 64u;
    fprintf(stderr, "[ACTOR-QUERY-STALL-GAME] count=%u\n", count);
    for (i = 0u; i < count; ++i) {
        uint32_t index = (end - count + i) & 255u;
        fprintf(stderr, "  [%03u] %08X\n", i,
                (uint32_t)g_recomp_recent_game_funcs[index]);
    }
    fflush(stderr);
    return 0;
}

static void recomp_actor_query_arm_stall_probe(uint32_t filtered)
{
    char value[32];
    char *end = NULL;
    unsigned long delay_ms;
    HANDLE thread;

    if (filtered == 0u || filtered > 16u)
        return;
    g_actor_query_game_thread_id = GetCurrentThreadId();
    if (GetEnvironmentVariableA("MERCENARIES_TRACE_ACTOR_QUERY_STALL_MS",
                                value, sizeof(value)) == 0u)
        return;
    delay_ms = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || delay_ms == 0u ||
        delay_ms > 60000u)
        return;
    thread = CreateThread(NULL, 0, recomp_actor_query_stall_probe_thread,
                          (LPVOID)(uintptr_t)
                              ((filtered << 24) | (uint32_t)delay_ms),
                          0, NULL);
    if (thread)
        CloseHandle(thread);
}

uint32_t recomp_redscene_collected_item_checkpoint(uint32_t level,
                                                   uint32_t source_index,
                                                   uint32_t item,
                                                   uint32_t result_index)
{
    static uint32_t filtered;
    if (item == 0u || recomp_spatial_object_valid(item))
        return item;
    ++filtered;
    if (filtered <= 32u || (filtered & (filtered - 1u)) == 0u) {
        fprintf(stderr,
                "[REDSCENE-ITEM-FILTER] n=%u level=%08X source_index=%u "
                "result_index=%u item=%08X\n",
                filtered, level, source_index, result_index, item);
        fflush(stderr);
    }
    return 0u;
}

uint32_t recomp_actor_query_entry_checkpoint(uint32_t result_index,
                                             uint32_t spatial_item,
                                             uint32_t actor)
{
    static uint32_t filtered;
    if (actor == 0u || recomp_spatial_object_valid(actor))
        return actor;
    ++filtered;
    recomp_actor_query_arm_stall_probe(filtered);
    if (filtered <= 32u || (filtered & (filtered - 1u)) == 0u) {
        fprintf(stderr,
                "[ACTOR-QUERY-FILTER] n=%u result_index=%u item=%08X "
                "item_vtable=%08X actor=%08X\n",
                filtered, result_index, spatial_item,
                spatial_item >= 0x00010000u &&
                        spatial_item <= 0x04000000u - 4u
                    ? guest_u32(spatial_item) : 0u,
                actor);
        fflush(stderr);
    }
    return 0u;
}

void recomp_nonvolatile_icall_checkpoint(
    uint32_t site, uint32_t expected_ebx, uint32_t current_ebx,
    uint32_t expected_esi, uint32_t current_esi,
    uint32_t expected_edi, uint32_t current_edi,
    uint32_t expected_ebp, uint32_t current_ebp,
    uint32_t expected_esp, uint32_t current_esp)
{
    static uint32_t reports;
    static int trace_enabled = -1;

    if (trace_enabled < 0)
        trace_enabled = getenv("MERCENARIES_TRACE_ICALL_NONVOLATILE") != NULL;
    if (!trace_enabled ||
        (expected_ebx == current_ebx && expected_esi == current_esi &&
         expected_edi == current_edi && expected_ebp == current_ebp &&
         expected_esp == current_esp) || reports++ >= 64u)
        return;
    fprintf(stderr,
            "[ICALL-NONVOLATILE] site=%08X ebx=%08X/%08X "
            "esi=%08X/%08X edi=%08X/%08X ebp=%08X/%08X "
            "esp=%08X/%08X eax=%08X ecx=%08X edx=%08X\n",
            site, current_ebx, expected_ebx, current_esi, expected_esi,
            current_edi, expected_edi, current_ebp, expected_ebp,
            current_esp, expected_esp, g_eax, g_ecx, g_edx);
    fflush(stderr);
}

void recomp_human_fire_anim_checkpoint(uint32_t human, uint32_t anim,
                                       float pitch, uint32_t shoot_state,
                                       uint32_t shoot_anim)
{
    static uint32_t reports;

    if (getenv("MERCENARIES_TRACE_PLAYER_FIRE_ANIM") == NULL ||
        shoot_state == 0u || reports++ >= 256u)
        return;
    fprintf(stderr,
            "[PLAYER-FIRE-ANIM] human=%08X anim=%08X pitch=%.9g "
            "shoot-state=%u shoot-anim=%u finite=%u\n",
            human, anim, pitch, shoot_state, shoot_anim,
            isfinite(pitch) != 0);
    fflush(stderr);
}

static uint32_t g_bullet_damage_actor;
static uint8_t g_bullet_damage_before[0x1000];
static int g_bullet_damage_snapshot_valid;

void recomp_bullet_hit_checkpoint(uint32_t stage, uint32_t projectile,
                                  uint32_t actor, uint32_t value)
{
    static unsigned int samples;
    if (getenv("MERCENARIES_TRACE_BULLET_HITS") == NULL || samples++ >= 512u)
        return;
    if (stage == 1u && actor >= 0x00010000u &&
        actor <= 0x04000000u - sizeof(g_bullet_damage_before)) {
        memcpy(g_bullet_damage_before, guest_ptr(actor),
               sizeof(g_bullet_damage_before));
        g_bullet_damage_actor = actor;
        g_bullet_damage_snapshot_valid = 1;
    }
    fprintf(stderr,
            "[BULLET-HIT] stage=%u projectile=%08X actor=%08X value=%08X "
            "projectile-pos=(%.7g,%.7g,%.7g) actor-vtable=%08X "
            "actor-pos=(%.7g,%.7g,%.7g) damage=%g/%g/%g type=%08X\n",
            stage, projectile, actor, value,
            guest_f32(projectile + 0xE0u), guest_f32(projectile + 0xE4u),
            guest_f32(projectile + 0xE8u), actor ? guest_u32(actor) : 0u,
            actor ? guest_f32(actor + 0xE0u) : 0.0f,
            actor ? guest_f32(actor + 0xE4u) : 0.0f,
            actor ? guest_f32(actor + 0xE8u) : 0.0f,
            guest_f32(projectile + 0x178u),
            guest_f32(projectile + 0x17Cu),
            guest_f32(projectile + 0x180u),
            guest_u32(projectile + 0x1B4u));
    fflush(stderr);
}

void recomp_bullet_damage_result_checkpoint(uint32_t projectile,
                                            uint32_t actor, float result)
{
    unsigned int changed = 0u;
    if (getenv("MERCENARIES_TRACE_BULLET_HITS") == NULL)
        return;
    fprintf(stderr,
            "[BULLET-DAMAGE] projectile=%08X actor=%08X "
            "actor-vtable=%08X result=%.7g\n",
            projectile, actor, actor ? guest_u32(actor) : 0u, result);
    if (g_bullet_damage_snapshot_valid && actor == g_bullet_damage_actor) {
        for (uint32_t offset = 0u;
             offset + sizeof(uint32_t) <= sizeof(g_bullet_damage_before);
             offset += sizeof(uint32_t)) {
            uint32_t before;
            uint32_t after = guest_u32(actor + offset);
            float before_f, after_f;
            memcpy(&before, g_bullet_damage_before + offset, sizeof(before));
            if (before == after)
                continue;
            memcpy(&before_f, &before, sizeof(before_f));
            memcpy(&after_f, &after, sizeof(after_f));
            fprintf(stderr,
                    "[BULLET-ACTOR-DELTA] actor=%08X offset=%04X "
                    "before=%08X after=%08X before-f=%.7g after-f=%.7g\n",
                    actor, offset, before, after, before_f, after_f);
            if (++changed >= 64u)
                break;
        }
        g_bullet_damage_snapshot_valid = 0;
    }
    fflush(stderr);
}
void recomp_human_fire_play_checkpoint(uint32_t stage, uint32_t human,
                                       uint32_t anim, uint32_t handle,
                                       uint32_t params, uint32_t shoot_state,
                                       uint32_t shoot_anim, uint32_t result)
{
    static uint32_t reports;

    if (getenv("MERCENARIES_TRACE_PLAYER_FIRE_ANIM") == NULL ||
        shoot_state == 0u || guest_u32(human) != 0x002E32B8u ||
        reports++ >= 256u)
        return;
    fprintf(stderr,
            "[PLAYER-FIRE-PLAY] stage=%u human=%08X anim=%08X "
            "handle=%08X params=%08X flags=%u/%u/%u/%u "
            "shoot-state=%u shoot-anim=%u result=%u\n",
            stage, human, anim, handle, params,
            guest_u8(params + 0x13u), guest_u8(params + 0x14u),
            guest_u8(params + 0x15u), guest_u8(params + 0x16u),
            shoot_state, shoot_anim, result != 0u);
    fflush(stderr);
}

void recomp_human_weapon_update_checkpoint(uint32_t stage, uint32_t human,
                                           uint32_t weapon,
                                           uint32_t value0,
                                           uint32_t value1)
{
    static uint32_t reports;

    if (getenv("MERCENARIES_TRACE_PLAYER_FIRE_ANIM") == NULL ||
        guest_u32(human) != 0x002E32B8u || reports++ >= 512u)
        return;
    fprintf(stderr,
            "[PLAYER-WEAPON-UPDATE] stage=%u human=%08X weapon=%08X "
            "values=%08X/%08X shoot-state=%u shoot-anim=%u\n",
            stage, human, weapon, value0, value1,
            guest_u32(human + 0x6C4u), guest_u32(human + 0x6C8u));
    fflush(stderr);
}
uint32_t recomp_actor_query_count_checkpoint(uint32_t site, uint32_t count)
{
    static uint32_t corrected;
    if (count <= 0x80u)
        return count;
    ++corrected;
    if (corrected <= 32u || (corrected & (corrected - 1u)) == 0u) {
        fprintf(stderr,
                "[ACTOR-QUERY-COUNT] n=%u site=%08X count=%08X "
                "clamped=00000080 eax=%08X ecx=%08X edx=%08X esp=%08X\n",
                corrected, site, count, g_eax, g_ecx, g_edx, g_esp);
        fflush(stderr);
    }
    return 0x80u;
}

void recomp_redscene_checkpoint(uint32_t stage, uint32_t scene,
                                uint32_t subject, uint32_t result_buffer,
                                uint32_t count)
{
    static int enabled = -1;
    static int trace_enabled;
    static int terrain_trace_enabled;
    static uint64_t terrain_transitions;
    static uint64_t activations, deactivations, queries;
    static uint32_t last_count = UINT32_MAX;
    static uint32_t write_result, write_count;
    static uint32_t write_shadow[2000];
    static int source_watch_armed;
    static int allocator_watch_armed;
    static uint32_t source_watch_index = 1891u;
    const uint32_t space = scene + 0x14u;
    uint32_t levels, entries = 0u;
    if (enabled < 0) {
        const char *watch_index;
        trace_enabled = getenv("MERCENARIES_TRACE_REDSCENE") != NULL;
        terrain_trace_enabled =
            getenv("MERCENARIES_TRACE_TERRAIN_STATE") != NULL;
        enabled = trace_enabled || terrain_trace_enabled ||
            getenv("MERCENARIES_TRACE_REDSCENE_WRITE_WATCH") != NULL ||
            getenv("MERCENARIES_TRACE_HAVOK_ALLOCATOR_UNDERFLOW") != NULL;
        watch_index = getenv("MERCENARIES_TRACE_REDSCENE_WRITE_INDEX");
        if (watch_index != NULL) {
            char *end = NULL;
            const unsigned long parsed = strtoul(watch_index, &end, 10);
            if (end != watch_index && *end == '\0' && parsed <= 4095u)
                source_watch_index = (uint32_t)parsed;
        }
    }
    if (!enabled)
        return;
    levels = guest_u32(space);
    if (levels > 8u)
        levels = 8u;
    for (uint32_t level = 0u; level < levels; ++level)
        entries += guest_u32(space + 4u + level * 0x1Cu + 8u);
    if ((stage == 1u || stage == 3u) && subject == 0x00643370u &&
        terrain_trace_enabled) {
        fprintf(stderr,
                "[TERRAIN-%s] n=%llu item=%08X flags=%08X active=%u "
                "scene=%08X levels=%u entries=%u esp=%08X\n",
                stage == 1u ? "ACTIVATE" : "DEACTIVATE",
                (unsigned long long)++terrain_transitions, subject,
                guest_u32(subject + 0x14u),
                guest_u32(subject + 0x14u) & 1u, scene, levels, entries,
                g_esp);
        fflush(stderr);
    }
    if (stage == 8u) {
        if (!allocator_watch_armed &&
            getenv("MERCENARIES_TRACE_HAVOK_ALLOCATOR_UNDERFLOW") != NULL) {
            const uint32_t memory = guest_u32(0x004409ACu);
            recomp_arm_havok_stack_watchpoint(memory);
            allocator_watch_armed = 1;
        }
        const uint32_t source_index = subject;
        const uint32_t item = result_buffer;
        const uint32_t result_index = count;
        const uint32_t item_buffer = guest_u32(scene + 4u);
        const uint32_t sphere_buffer = guest_u32(scene);
        const uint32_t current_result = guest_u32(0x0085C5A4u);
        if (!source_watch_armed && source_index == source_watch_index &&
            item >= 0x00010000u && item < 0x04000000u &&
            getenv("MERCENARIES_TRACE_REDSCENE_WRITE_WATCH") != NULL) {
            recomp_arm_redscene_watchpoint(item_buffer + source_index * 4u);
            source_watch_armed = 1;
        }
        if (current_result != write_result || result_index == 0u) {
            write_result = current_result;
            write_count = 0u;
        }
        if (result_index < 2000u) {
            write_shadow[result_index] = item;
            if (write_count <= result_index)
                write_count = result_index + 1u;
        }
        if (item < 0x00010000u || item >= 0x04000000u) {
            fprintf(stderr,
                    "[REDSCENE-WRITE] result=%08X result_index=%u "
                    "item=%08X level=%08X source_index=%u source=%08X "
                    "items=%08X spheres=%08X sphere=(%08X,%08X,%08X,%08X) "
                    "invalid=1\n",
                    current_result, result_index, item, scene, source_index,
                    item_buffer + source_index * 4u, item_buffer, sphere_buffer,
                    guest_u32(sphere_buffer + source_index * 16u),
                    guest_u32(sphere_buffer + source_index * 16u + 4u),
                    guest_u32(sphere_buffer + source_index * 16u + 8u),
                    guest_u32(sphere_buffer + source_index * 16u + 12u));
            fflush(stderr);
        }
        return;
    }
    if (!trace_enabled)
        return;
    if (stage == 1u || stage == 3u) {
        uint64_t *counter = stage == 1u ? &activations : &deactivations;
        const uint64_t sample = ++*counter;
        if (sample <= 32u || (sample & (sample - 1u)) == 0u) {
            fprintf(stderr, "[REDSCENE-%s] n=%llu scene=%08X item=%08X "
                    "vtable=%08X sphere=(%.6g,%.6g,%.6g,%.6g) flags=%08X "
                    "levels=%u entries=%u\n", stage == 1u ? "ACTIVATE" : "DEACTIVATE",
                    (unsigned long long)sample, scene, subject, guest_u32(subject),
                    guest_f32(subject + 4u), guest_f32(subject + 8u),
                    guest_f32(subject + 12u), guest_f32(subject + 16u),
                    guest_u32(subject + 0x14u), levels, entries);
            fflush(stderr);
        }
        return;
    }
    if (stage == 7u) {
        static uint64_t presort_queries;
        const uint32_t distances = subject;
        uint32_t first_invalid = UINT32_MAX;
        uint32_t first_mismatch = UINT32_MAX;
        const uint64_t sample = ++presort_queries;
        if (sample == 1u &&
            getenv("MERCENARIES_TRACE_HAVOK_ALLOCATOR_UNDERFLOW") != NULL) {
            const uint32_t memory = guest_u32(0x004409ACu);
            recomp_arm_havok_stack_watchpoint(memory);
        }
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t item = guest_u32(result_buffer + i * 4u);
            if (item < 0x00010000u || item >= 0x04000000u) {
                first_invalid = i;
                break;
            }
        }
        if (write_result == result_buffer && write_count >= count) {
            for (uint32_t i = 0u; i < count; ++i) {
                if (guest_u32(result_buffer + i * 4u) != write_shadow[i]) {
                    first_mismatch = i;
                    break;
                }
            }
        }
        const int should_log = first_invalid != UINT32_MAX || sample <= 32u ||
            first_mismatch != UINT32_MAX ||
            (sample & (sample - 1u)) == 0u;
        if (should_log)
            fprintf(stderr,
                    "[REDSCENE-PRESORT] n=%llu result=%08X distances=%08X "
                    "count=%u invalid=%u mismatch=%u shadow=%08X/%u\n",
                    (unsigned long long)sample, result_buffer, distances, count,
                    first_invalid, first_mismatch, write_result, write_count);
        if (first_invalid != UINT32_MAX) {
            const uint32_t begin = first_invalid > 3u ? first_invalid - 3u : 0u;
            const uint32_t finish = first_invalid + 4u < count ?
                first_invalid + 4u : count;
            for (uint32_t i = begin; i < finish; ++i)
                fprintf(stderr, "  presort[%u]=%08X distance=%08X\n", i,
                        guest_u32(result_buffer + i * 4u),
                        distances ? guest_u32(distances + i * 4u) : 0u);
        }
        if (should_log)
            fflush(stderr);
        return;
    }
    if (stage >= 4u) {
        fprintf(stderr, "[REDSCENE-PHASE] stage=%u scene=%08X levels=%u entries=%u\n",
                stage, scene, levels, entries);
        fflush(stderr);
        return;
    }
    if (stage == 2u) {
        const uint64_t sample = ++queries;
        const uint32_t distances = guest_u32(0x0085C59Cu);
        uint32_t first_invalid = UINT32_MAX;
        for (uint32_t i = 0u; i < count; ++i) {
            const uint32_t item = guest_u32(result_buffer + i * 4u);
            if (item < 0x00010000u || item >= 0x04000000u) {
                first_invalid = i;
                break;
            }
        }
        if (sample <= 32u || count != last_count ||
            (sample & (sample - 1u)) == 0u || first_invalid != UINT32_MAX) {
            fprintf(stderr, "[REDSCENE-COLLECT] n=%llu scene=%08X "
                    "sphere=(%.6g,%.6g,%.6g,%.6g) result=%08X count=%u "
                    "distances=%08X alias=%u invalid=%u levels=%u entries=%u",
                    (unsigned long long)sample, scene, guest_f32(subject),
                    guest_f32(subject + 4u), guest_f32(subject + 8u),
                    guest_f32(subject + 12u), result_buffer, count, distances,
                    distances == result_buffer, first_invalid, levels, entries);
            for (uint32_t level = 0u; level < levels; ++level) {
                const uint32_t level_address = space + 4u + level * 0x1Cu;
                fprintf(stderr, " L%u=%u/%u/r%.6g", level,
                        guest_u32(level_address + 8u),
                        guest_u32(level_address + 0x0Cu),
                        guest_f32(level_address + 0x14u));
            }
            fputc('\n', stderr);
            for (uint32_t i = 0u; i < count && i < 8u; ++i) {
                const uint32_t item = guest_u32(result_buffer + i * 4u);
                const int valid = item >= 0x00010000u && item < 0x04000000u;
                fprintf(stderr, "  result[%u]=%08X vtable=%08X flags=%08X\n",
                        i, item, valid ? guest_u32(item) : 0u,
                        valid ? guest_u32(item + 0x14u) : 0u);
            }
            if (first_invalid != UINT32_MAX && first_invalid >= 8u) {
                const uint32_t begin = first_invalid > 3u ? first_invalid - 3u : 0u;
                const uint32_t finish = first_invalid + 4u < count ?
                    first_invalid + 4u : count;
                for (uint32_t i = begin; i < finish; ++i)
                    fprintf(stderr, "  suspect[%u]=%08X distance=%08X\n", i,
                            guest_u32(result_buffer + i * 4u), distances != 0u ?
                            guest_u32(distances + i * 4u) : 0u);
            }
            fflush(stderr);
        }
        last_count = count;
    }
}

void recomp_terrain_rendering_request(uint32_t requested_enable,
                                      uint32_t renderable_flags)
{
    static int enabled = -1;
    static uint64_t requests;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_TERRAIN_STATE") != NULL;
    if (!enabled)
        return;
    fprintf(stderr,
            "[TERRAIN-REQUEST] n=%llu enable=%u flags=%08X active=%u "
            "eax=%08X ecx=%08X esp=%08X caller=%08X\n",
            (unsigned long long)++requests, requested_enable != 0u,
            renderable_flags, renderable_flags & 1u, g_eax, g_ecx, g_esp,
            guest_u32(g_esp));
    fflush(stderr);
}

static int g_asset_dispatch_trace_enabled = -1;
static uint32_t g_world_asset_name;
static uint64_t g_world_asset_xfrm_count;
static float g_world_asset_min_x, g_world_asset_min_y, g_world_asset_min_z;
static float g_world_asset_max_x, g_world_asset_max_y, g_world_asset_max_z;
static float g_world_asset_nearest_dist2;
static float g_world_asset_nearest_x, g_world_asset_nearest_y, g_world_asset_nearest_z;
static float g_world_asset_rotated_nearest_dist2;
static float g_world_asset_rotated_nearest_x, g_world_asset_rotated_nearest_y;
static float g_world_asset_rotated_nearest_z;
static uint64_t g_world_asset_composed_count;
static uint64_t g_world_asset_composed_property_count;
static uint64_t g_world_asset_composed_spore_count;
static float g_world_asset_composed_min_x, g_world_asset_composed_min_y;
static float g_world_asset_composed_min_z;
static float g_world_asset_composed_max_x, g_world_asset_composed_max_y;
static float g_world_asset_composed_max_z;
static float g_world_asset_composed_nearest_dist2;
static float g_world_asset_composed_nearest_x, g_world_asset_composed_nearest_y;
static float g_world_asset_composed_nearest_z;

void recomp_world_xfrm_checkpoint(uint32_t matrix_address,
                                   uint32_t chunk_address)
{
    static int enabled = -1;
    static uint64_t samples;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_WORLD_XFRM") != NULL;
    if (!enabled && g_asset_dispatch_trace_enabled != 1)
        return;

    const uint64_t sample = enabled ? ++samples : 0u;
    const float tx = guest_f32(matrix_address + 48u);
    const float ty = guest_f32(matrix_address + 52u);
    const float tz = guest_f32(matrix_address + 56u);
    if (g_asset_dispatch_trace_enabled == 1 && g_world_asset_name != 0u) {
        const float dx = tx - 1236.8f;
        const float dz = tz + 1356.0f;
        const float dist2 = dx * dx + dz * dz;
        const float rotated_dx = -tx - 1236.8f;
        const float rotated_dz = -tz + 1356.0f;
        const float rotated_dist2 =
            rotated_dx * rotated_dx + rotated_dz * rotated_dz;
        ++g_world_asset_xfrm_count;
        if (tx < g_world_asset_min_x) g_world_asset_min_x = tx;
        if (ty < g_world_asset_min_y) g_world_asset_min_y = ty;
        if (tz < g_world_asset_min_z) g_world_asset_min_z = tz;
        if (tx > g_world_asset_max_x) g_world_asset_max_x = tx;
        if (ty > g_world_asset_max_y) g_world_asset_max_y = ty;
        if (tz > g_world_asset_max_z) g_world_asset_max_z = tz;
        if (dist2 < g_world_asset_nearest_dist2) {
            g_world_asset_nearest_dist2 = dist2;
            g_world_asset_nearest_x = tx;
            g_world_asset_nearest_y = ty;
            g_world_asset_nearest_z = tz;
        }
        if (rotated_dist2 < g_world_asset_rotated_nearest_dist2) {
            g_world_asset_rotated_nearest_dist2 = rotated_dist2;
            g_world_asset_rotated_nearest_x = -tx;
            g_world_asset_rotated_nearest_y = ty;
            g_world_asset_rotated_nearest_z = -tz;
        }
    }
    if (!enabled || (sample > 32u && (sample & (sample - 1u)) != 0u))
        return;
    fprintf(stderr,
            "[WORLD-XFRM] n=%llu matrix=%08X chunk=%08X "
            "x=(%.6g,%.6g,%.6g,%.6g) y=(%.6g,%.6g,%.6g,%.6g) "
            "z=(%.6g,%.6g,%.6g,%.6g) t=(%.6g,%.6g,%.6g,%.6g)\n",
            (unsigned long long)sample, matrix_address, chunk_address,
            guest_f32(matrix_address), guest_f32(matrix_address + 4u),
            guest_f32(matrix_address + 8u), guest_f32(matrix_address + 12u),
            guest_f32(matrix_address + 16u), guest_f32(matrix_address + 20u),
            guest_f32(matrix_address + 24u), guest_f32(matrix_address + 28u),
            guest_f32(matrix_address + 32u), guest_f32(matrix_address + 36u),
            guest_f32(matrix_address + 40u), guest_f32(matrix_address + 44u),
            guest_f32(matrix_address + 48u), guest_f32(matrix_address + 52u),
            guest_f32(matrix_address + 56u), guest_f32(matrix_address + 60u));
    fflush(stderr);
}

void recomp_asset_dispatch_checkpoint(uint32_t asset_name,
                                      uint32_t asset_type,
                                      uint32_t chunk_address)
{
    static uint32_t samples;
    if (g_asset_dispatch_trace_enabled < 0)
        g_asset_dispatch_trace_enabled =
            getenv("MERCENARIES_TRACE_ASSET_DISPATCH") != NULL;
    if (!g_asset_dispatch_trace_enabled)
        return;
    {
        const uint32_t file_address = guest_u32(chunk_address + 0x18u);
        const uint32_t data_address = file_address != 0u
                                          ? guest_u32(file_address + 4u)
                                          : 0u;
        const uint32_t file_size = file_address != 0u
                                       ? guest_u32(file_address + 8u)
                                       : 0u;
        const uint32_t file_position = file_address != 0u
                                           ? guest_u32(file_address + 0x10u)
                                           : 0u;
        uint32_t fnv1a = 2166136261u;
        if (data_address >= 0x00010000u && file_size <= 64u * 1024u * 1024u) {
            const uint8_t *bytes = (const uint8_t *)guest_ptr(data_address);
            for (uint32_t i = 0; i < file_size; ++i)
                fnv1a = (fnv1a ^ bytes[i]) * 16777619u;
            if (asset_name == 0xE02DEBB6u) {
                const char *dump_path = getenv("MERCENARIES_DUMP_SHL_PAYLOAD");
                if (dump_path != NULL && dump_path[0] != '\0') {
                    FILE *dump = fopen(dump_path, "wb");
                    if (dump != NULL) {
                        fwrite(bytes, 1u, file_size, dump);
                        fclose(dump);
                    }
                }
            }
        }
        fprintf(stderr,
                "[ASSET-PAYLOAD] name=%08X file=%08X data=%08X size=%u "
                "position=%u fnv1a=%08X\n",
                asset_name, file_address, data_address, file_size,
                file_position, fnv1a);
    }
    if (g_world_asset_name != 0u && g_world_asset_xfrm_count != 0u) {
        fprintf(stderr,
                "[WORLD-ASSET-SUMMARY] name=%08X xfrms=%llu "
                "min=(%.6g,%.6g,%.6g) max=(%.6g,%.6g,%.6g) "
                "nearest-shell=(%.6g,%.6g,%.6g) dist2=%.6g "
                "rot180-nearest=(%.6g,%.6g,%.6g) rot180-dist2=%.6g\n",
                g_world_asset_name,
                (unsigned long long)g_world_asset_xfrm_count,
                g_world_asset_min_x, g_world_asset_min_y, g_world_asset_min_z,
                g_world_asset_max_x, g_world_asset_max_y, g_world_asset_max_z,
                g_world_asset_nearest_x, g_world_asset_nearest_y,
                g_world_asset_nearest_z, g_world_asset_nearest_dist2,
                g_world_asset_rotated_nearest_x,
                g_world_asset_rotated_nearest_y,
                g_world_asset_rotated_nearest_z,
                g_world_asset_rotated_nearest_dist2);
    }
    if (g_world_asset_name != 0u && g_world_asset_composed_count != 0u) {
        fprintf(stderr,
                "[WORLD-COMPOSED-SUMMARY] name=%08X total=%llu properties=%llu "
                "spores=%llu min=(%.6g,%.6g,%.6g) max=(%.6g,%.6g,%.6g) "
                "nearest-shell=(%.6g,%.6g,%.6g) dist2=%.6g\n",
                g_world_asset_name,
                (unsigned long long)g_world_asset_composed_count,
                (unsigned long long)g_world_asset_composed_property_count,
                (unsigned long long)g_world_asset_composed_spore_count,
                g_world_asset_composed_min_x, g_world_asset_composed_min_y,
                g_world_asset_composed_min_z, g_world_asset_composed_max_x,
                g_world_asset_composed_max_y, g_world_asset_composed_max_z,
                g_world_asset_composed_nearest_x,
                g_world_asset_composed_nearest_y,
                g_world_asset_composed_nearest_z,
                g_world_asset_composed_nearest_dist2);
    }
    fprintf(stderr,
            "[ASSET-DISPATCH] n=%u name=%08X type=%08X chunk=%08X "
            "chunk0=%08X chunk4=%08X chunk8=%08X chunkC=%08X\n",
            ++samples, asset_name, asset_type, chunk_address,
            guest_u32(chunk_address), guest_u32(chunk_address + 4u),
            guest_u32(chunk_address + 8u), guest_u32(chunk_address + 12u));
    g_world_asset_name = asset_name;
    g_world_asset_xfrm_count = 0u;
    g_world_asset_min_x = g_world_asset_min_y = g_world_asset_min_z = FLT_MAX;
    g_world_asset_max_x = g_world_asset_max_y = g_world_asset_max_z = -FLT_MAX;
    g_world_asset_nearest_dist2 = FLT_MAX;
    g_world_asset_rotated_nearest_dist2 = FLT_MAX;
    g_world_asset_nearest_x = g_world_asset_nearest_y =
        g_world_asset_nearest_z = 0.0f;
    g_world_asset_composed_count = 0u;
    g_world_asset_composed_property_count = 0u;
    g_world_asset_composed_spore_count = 0u;
    g_world_asset_composed_min_x = g_world_asset_composed_min_y =
        g_world_asset_composed_min_z = FLT_MAX;
    g_world_asset_composed_max_x = g_world_asset_composed_max_y =
        g_world_asset_composed_max_z = -FLT_MAX;
    g_world_asset_composed_nearest_dist2 = FLT_MAX;
    g_world_asset_composed_nearest_x = g_world_asset_composed_nearest_y =
        g_world_asset_composed_nearest_z = 0.0f;
    fflush(stderr);
}

void recomp_world_composed_checkpoint(uint32_t kind, uint32_t matrix_address,
                                      uint32_t transform_address)
{
    static int trace_enabled = -1;
    static uint64_t samples;

    if (trace_enabled < 0)
        trace_enabled = getenv("MERCENARIES_TRACE_WORLD_COMPOSED") != NULL;
    if (!trace_enabled && g_asset_dispatch_trace_enabled != 1)
        return;

    const float mx = guest_f32(matrix_address + 48u);
    const float my = guest_f32(matrix_address + 52u);
    const float mz = guest_f32(matrix_address + 56u);
    const float tx = guest_f32(transform_address + 48u);
    const float ty = guest_f32(transform_address + 52u);
    const float tz = guest_f32(transform_address + 56u);
    const float dx = tx - 1236.8f;
    const float dz = tz + 1356.0f;
    const float dist2 = dx * dx + dz * dz;
    const uint64_t sample = trace_enabled ? ++samples : 0u;
    if (g_asset_dispatch_trace_enabled == 1 && g_world_asset_name != 0u &&
        isfinite(tx) && isfinite(ty) && isfinite(tz)) {
        ++g_world_asset_composed_count;
        if (kind == 1u)
            ++g_world_asset_composed_property_count;
        else if (kind == 2u)
            ++g_world_asset_composed_spore_count;
        if (tx < g_world_asset_composed_min_x) g_world_asset_composed_min_x = tx;
        if (ty < g_world_asset_composed_min_y) g_world_asset_composed_min_y = ty;
        if (tz < g_world_asset_composed_min_z) g_world_asset_composed_min_z = tz;
        if (tx > g_world_asset_composed_max_x) g_world_asset_composed_max_x = tx;
        if (ty > g_world_asset_composed_max_y) g_world_asset_composed_max_y = ty;
        if (tz > g_world_asset_composed_max_z) g_world_asset_composed_max_z = tz;
        if (dist2 < g_world_asset_composed_nearest_dist2) {
            g_world_asset_composed_nearest_dist2 = dist2;
            g_world_asset_composed_nearest_x = tx;
            g_world_asset_composed_nearest_y = ty;
            g_world_asset_composed_nearest_z = tz;
        }
    }
    if (!trace_enabled ||
        (sample > 32u && (sample & (sample - 1u)) != 0u))
        return;
    fprintf(stderr,
            "[WORLD-COMPOSED] n=%llu asset=%08X kind=%u matrix=%08X "
            "copy=%08X matrix-t=(%.6g,%.6g,%.6g) "
            "copy-t=(%.6g,%.6g,%.6g)\n",
            (unsigned long long)sample, g_world_asset_name, kind,
            matrix_address, transform_address, mx, my, mz, tx, ty, tz);
    fflush(stderr);
}

void recomp_world_transform_checkpoint(uint32_t matrix_address,
                                        uint32_t transform_address)
{
    static int enabled = -1;
    static uint64_t samples;
    const uint64_t sample = ++samples;
    const float tx = guest_f32(transform_address);
    const float ty = guest_f32(transform_address + 4u);
    const float tz = guest_f32(transform_address + 8u);
    const float qx = guest_f32(transform_address + 12u);
    const float qy = guest_f32(transform_address + 16u);
    const float qz = guest_f32(transform_address + 20u);
    const float qs = guest_f32(transform_address + 24u);
    const int bad = !isfinite(tx) || !isfinite(ty) || !isfinite(tz) ||
                    !isfinite(qx) || !isfinite(qy) || !isfinite(qz) ||
                    !isfinite(qs);
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_WORLD_TRANSFORM") != NULL;
    if (!enabled || (!bad && sample > 32u && (sample & (sample - 1u)) != 0u))
        return;
    fprintf(stderr,
            "[WORLD-TRANSFORM] n=%llu matrix=%08X transform=%08X "
            "matrix-t=(%.6g,%.6g,%.6g) transform-t=(%.6g,%.6g,%.6g) "
            "q=(%.6g,%.6g,%.6g,%.6g)%s\n",
            (unsigned long long)sample, matrix_address, transform_address,
            guest_f32(matrix_address + 48u),
            guest_f32(matrix_address + 52u),
            guest_f32(matrix_address + 56u), tx, ty, tz, qx, qy, qz, qs,
            bad ? " BAD" : "");
    fflush(stderr);
}
void recomp_shell_prewarm_checkpoint(uint32_t iteration,
                                     uint32_t position_address,
                                     uint32_t direction_x_bits,
                                     uint32_t direction_y_bits,
                                     uint32_t direction_z_bits)
{
    static int enabled = -1;
    float dx, dy, dz;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_SHELL_PREWARM") != NULL;
    if (!enabled)
        return;
    memcpy(&dx, &direction_x_bits, sizeof(dx));
    memcpy(&dy, &direction_y_bits, sizeof(dy));
    memcpy(&dz, &direction_z_bits, sizeof(dz));
    fprintf(stderr,
            "[SHELL-PREWARM] iteration=%u camera=%08X "
            "position=(%.6g,%.6g,%.6g) direction=(%.6g,%.6g,%.6g) "
            "scene_entries=%u\n",
            iteration, guest_u32(0x0041410Cu),
            guest_f32(position_address), guest_f32(position_address + 4u),
            guest_f32(position_address + 8u), dx, dy, dz,
            guest_u32(0x00692238u) + guest_u32(0x00692254u));
    fflush(stderr);
}

#include "menu_layout_guest.h"
#include "carry_pickup_guest.h"

void recomp_menu_paint_checkpoint(uint32_t stage, uint32_t menu,
                                  uint32_t index, uint32_t item_hash,
                                  uint32_t fade_bits, uint32_t text_buffer)
{
    if (stage == 1u) recomp_options_prepare_menu_paint(text_buffer, menu);
    recomp_options_replace_label(item_hash, text_buffer);
    static int enabled = -1;
    static uint64_t paints;
    static uint64_t labels;
    uint64_t sample;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_MENU_PAINT") != NULL;
    if (!enabled)
        return;

    sample = stage == 1u ? ++paints : ++labels;
    if (sample > 32u && (sample & (sample - 1u)) != 0u)
        return;

    if (stage == 1u) {
        const uint32_t item_count = guest_u32(menu + 0x3Cu);
        fprintf(stderr,
                "[MENU-PAINT] n=%llu menu=%08X vtable=%08X owner=%08X "
                "selected=%u items=%u timer=%.6g per=%.6g enabled=%u",
                (unsigned long long)sample, menu, guest_u32(menu),
                text_buffer, guest_u32(menu + 0x38u), item_count,
                guest_f32(menu + 0xC4u), guest_f32(menu + 0xC8u),
                guest_u32(menu + 0xCCu) & 0xFFu);
        for (uint32_t i = 0u; i < item_count && i < 16u; ++i) {
            fprintf(stderr, " i%u=%08X/%.6g", i,
                    guest_u32(menu + 0x40u + i * 4u),
                    guest_f32(menu + 0x80u + i * 4u));
        }
        fputc('\n', stderr);
    } else {
        float fade;
        char text[121];
        uint32_t i = 0u;
        memcpy(&fade, &fade_bits, sizeof(fade));
        if (text_buffer >= 0x00010000u && text_buffer < 0x04000000u) {
            for (; i + 1u < sizeof(text) && text_buffer + i < 0x04000000u; ++i) {
                const uint8_t ch = guest_u8(text_buffer + i);
                if (ch == 0u)
                    break;
                text[i] = ch >= 0x20u && ch < 0x7Fu ? (char)ch : '.';
            }
        }
        text[i] = '\0';
        fprintf(stderr,
                "[MENU-LABEL] n=%llu menu=%08X index=%u hash=%08X "
                "fade=%.6g buffer=%08X text=%s\n",
                (unsigned long long)sample, menu, index, item_hash,
                fade, text_buffer, text);
        g_menu_text_trace_armed = 1u;
    }
    fflush(stderr);
}

void recomp_temp_array_checkpoint(uint32_t stage, uint32_t object,
                                  uint32_t allocator, uint32_t value0,
                                  uint32_t value1)
{
    static int enabled = -1;
    static uint64_t samples;
    static uint64_t eight_byte_samples;
    static uint64_t callback_samples;
    static uint64_t release_samples;
    static uint32_t merge_left;
    static uint32_t merge_right;
    static uint32_t merge_left_count;
    static uint32_t merge_right_count;
    uint64_t sample;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_TEMP_ARRAY") != NULL;
    if (!enabled)
        return;

    if (stage == 59u) {
        merge_left = object;
        merge_right = allocator;
        merge_left_count = value0;
        merge_right_count = value1;
        return;
    }

    /* These merge callbacks are ordinary Xbox cdecl calls. EBX/ESI/EDI and
     * the caller stack must survive them. The merge can recurse, so pair each
     * pre/post checkpoint on a small per-site stack before reporting a real
     * ABI violation. */
    if (stage >= 42u && stage <= 53u) {
        static uint32_t saved_ebx[6][64];
        static uint32_t saved_esi[6][64];
        static uint32_t saved_edi[6][64];
        static uint32_t saved_esp[6][64];
        static uint32_t saved_callback[6][64];
        static uint32_t depth[6];
        const uint32_t slot = (stage - 42u) >> 1u;
        if ((stage & 1u) == 0u) {
            const uint32_t level = depth[slot] < 64u ? depth[slot]++ : 63u;
            saved_ebx[slot][level] = g_ebx;
            saved_esi[slot][level] = g_esi;
            saved_edi[slot][level] = g_edi;
            saved_esp[slot][level] = g_esp;
            saved_callback[slot][level] = allocator;
        } else if (depth[slot] != 0u) {
            const uint32_t level = --depth[slot];
            if (g_ebx != saved_ebx[slot][level] ||
                g_esi != saved_esi[slot][level] ||
                g_edi != saved_edi[slot][level] ||
                g_esp != saved_esp[slot][level]) {
                fprintf(stderr,
                        "[TEMP-NONVOLATILE-MISMATCH] stage=%u target=%08X "
                        "depth=%u ebx=%08X->%08X esi=%08X->%08X "
                        "edi=%08X->%08X esp=%08X->%08X current=%08X\n",
                        stage, saved_callback[slot][level], level,
                        saved_ebx[slot][level], g_ebx,
                        saved_esi[slot][level], g_esi,
                        saved_edi[slot][level], g_edi,
                        saved_esp[slot][level], g_esp,
                        g_recomp_current_func);
                fflush(stderr);
            }
        }
    }
    if (stage == 56u &&
        (object == 0xBF800000u || allocator == 0xBF800000u ||
         value0 == 0xBF800000u || value1 == 0xBF800000u)) {
        fprintf(stderr,
                "[TEMP-ARRAY-MALFORMED-COMPARE] left=%08X right=%08X "
                "left_second=%08X right_second=%08X eax=%08X ebx=%08X "
                "ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X "
                "entry=%08X/%u,%08X/%u\n",
                object, allocator, value0, value1, g_eax, g_ebx, g_ecx,
                g_edx, g_esi, g_edi, g_esp, merge_left, merge_left_count,
                merge_right, merge_right_count);
        fflush(stderr);
    }
    if (stage >= 100u && stage < 700u) {
        fprintf(stderr,
                "[DCALL-STACK] stage=%u target=%08X sampledEsp=%08X "
                "sampledEbx=%08X sampledEsi=%08X eax=%08X ebx=%08X "
                "ecx=%08X edx=%08X esi=%08X edi=%08X current=%08X esp=%08X\n",
                stage, allocator, object, value0, value1, g_eax, g_ebx,
                g_ecx, g_edx, g_esi, g_edi, g_recomp_current_func, g_esp);
        fflush(stderr);
        return;
    }

    /* sub_00168030's pool lives below the global scratch arena. Keep every
     * diagnostic-only lookup sample; the generic temp-array counters may
     * already be throttled by thousands of unrelated calls before Load Game. */
    if (stage >= 24u && stage <= 33u) {
        fprintf(stderr,
                "[PROPERTY-SLOT] stage=%u pool=%08X source=%08X "
                "value=%08X targetOrCount=%08X count=%02X "
                "eax=%08X ebx=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X current=%08X esp=%08X\n",
                stage, object, allocator, value0, value1,
                guest_u8(object + 0x8C9u), g_eax, g_ebx, g_ecx, g_edx,
                g_esi, g_edi, g_recomp_current_func, g_esp);
        fflush(stderr);
        return;
    }

    if (stage >= 6u && stage <= 9u && object < 0x008BF800u)
        return;
    if (stage >= 42u && stage <= 53u && value1 < 0x008BF800u)
        return;

    sample = stage == 20u
                 ? ++release_samples
                 : (stage >= 6u && stage <= 9u)
                 ? ++eight_byte_samples
                 : (stage >= 42u && stage <= 53u)
                       ? ++callback_samples
                       : ++samples;
    if (stage != 20u && !(stage >= 30u && stage <= 39u) &&
        sample > ((stage >= 6u && stage <= 9u) ||
                          (stage >= 42u && stage <= 53u)
                      ? 4096u
                      : 512u) &&
        (sample & (sample - 1u)) != 0u)
        return;

    if (stage == 20u) {
        fprintf(stderr,
                "[SCENE-RELEASE-MISMATCH] n=%llu owner=%08X object=%08X "
                "vtable=%08X fields=%08X,%08X,%08X,%08X paired=%08X "
                "pairedVtable=%08X target=%08X esp=%08X\n",
                (unsigned long long)sample, g_edi, object, allocator,
                guest_u32(object + 4u), guest_u32(object + 8u),
                guest_u32(object + 0xCu), guest_u32(object + 0x10u),
                value0, guest_u32(value0), value1, g_esp);
        fflush(stderr);
        return;
    }

    if (stage >= 40u) {
        static uint32_t saved_stack[16];
        static uint32_t saved_target[16];
        const uint32_t slot = ((stage - 40u) >> 1u) & 15u;
        if ((stage & 1u) == 0u) {
            saved_stack[slot] = value0;
            saved_target[slot] = allocator;
        }
        fprintf(stderr,
                "[TEMP-NESTED-CALL] n=%llu stage=%u slot=%u "
                "target=%08X saved=%08X current=%08X delta=%d\n",
                (unsigned long long)sample, stage, slot,
                (stage & 1u) ? saved_target[slot] : allocator,
                (stage & 1u) ? saved_stack[slot] : value0, value1,
                (int32_t)(value1 - ((stage & 1u) ? saved_stack[slot] : value0)));
        fflush(stderr);
        return;
    }

    if (stage >= 10u) {
        fprintf(stderr,
                "[TEMP-ARRAY-CALL] n=%llu stage=%u local=%08X "
                "target=%08X saved=%08X current=%08X delta=%d\n",
                (unsigned long long)sample, stage, object, allocator,
                value0, value1, (int32_t)(value1 - value0));
        fflush(stderr);
        return;
    }

    fprintf(stderr,
            "[TEMP-ARRAY] n=%llu stage=%u object=%08X "
            "data=%08X count=%08X capacity=%08X end=%08X size=%08X "
            "allocator=%08X current=%08X remaining=%08X owner=%08X "
            "value0=%08X value1=%08X\n",
            (unsigned long long)sample, stage, object,
            guest_u32(object), guest_u32(object + 4u),
            guest_u32(object + 8u), guest_u32(object + 0xCu),
            guest_u32(object + 0x10u), allocator,
            guest_u32(allocator + 8u), guest_u32(allocator + 0xCu),
            guest_u32(allocator + 0x14u), value0, value1);
    fflush(stderr);
}

void recomp_allocator_alias_checkpoint(uint32_t site, uint32_t target)
{
    const uint32_t allocator = guest_u32(0x4409ACu);
    if (target + 0x10u <= 0x00FA67C0u || target >= 0x00FA67E0u)
        return;

    fprintf(stderr,
            "[TEMP-ALLOCATOR-ALIAS] site=%08X caller=%08X target=%08X allocator=%08X "
            "current=%08X remaining=%08X frame=%08X end=%08X "
            "source=%08X,%08X,%08X,%08X eax=%08X ebx=%08X ecx=%08X "
            "edx=%08X esi=%08X edi=%08X esp=%08X seh_ebp=%08X "
            "stack=%08X,%08X,%08X,%08X\n",
            site,
            g_recomp_recent_funcs[(g_recomp_recent_func_idx - 2u) & 63u],
            target, allocator, guest_u32(allocator + 8u),
            guest_u32(allocator + 0xCu), guest_u32(allocator + 0x10u),
            guest_u32(allocator + 0x14u), guest_u32(g_eax),
            guest_u32(g_eax + 4u), guest_u32(g_eax + 8u),
            guest_u32(g_eax + 0xCu), g_eax, g_ebx, g_ecx, g_edx,
            g_esi, g_edi, g_esp, g_seh_ebp, guest_u32(g_esp),
            guest_u32(g_esp + 4u), guest_u32(g_esp + 8u),
            guest_u32(g_esp + 0xCu));
    fflush(stderr);
}
void recomp_xmm_store_checkpoint(uint32_t addr, const float src[4])
{
    uint32_t bits[4];
    if ((addr + 0x10u <= 0x00FA67C0u || addr >= 0x00FA67E0u) &&
        (addr + 0x10u <= 0x01254050u || addr >= 0x01254060u))
        return;
    memcpy(bits, src, sizeof(bits));
    fprintf(stderr,
            "[XMM-ALLOCATOR-ALIAS] func=%08X addr=%08X "
            "bits=%08X,%08X,%08X,%08X eax=%08X ebx=%08X ecx=%08X "
            "edx=%08X esi=%08X edi=%08X esp=%08X seh_ebp=%08X\n",
            (uint32_t)g_recomp_current_func, addr, bits[0], bits[1],
            bits[2], bits[3], g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi,
            g_esp, g_seh_ebp);
    fflush(stderr);
}
void recomp_float_mem_checkpoint(uint32_t addr)
{
    fprintf(stderr,
            "[FLOAT-ALLOCATOR-ACCESS] func=%08X addr=%08X value=%08X "
            "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "esp=%08X seh_ebp=%08X\n",
            (uint32_t)g_recomp_current_func, addr, guest_u32(addr),
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp, g_seh_ebp);
    fflush(stderr);
}
void recomp_allocator_call_checkpoint(uint32_t site, uint32_t phase,
                                      uint32_t allocator,
                                      uint32_t requested,
                                      uint32_t result)
{
    static uint32_t saved_allocator[4];
    static uint32_t saved_requested[4];
    static uint32_t saved_target[4];
    const uint32_t slot = site & 3u;
    if (phase == 0u) {
        saved_allocator[slot] = allocator;
        saved_requested[slot] = requested;
        saved_target[slot] = guest_u32(guest_u32(allocator) + 0x28u);
        return;
    }
    fprintf(stderr,
            "[TEMP-ALLOC-CALL] site=%u allocator=%08X target=%08X "
            "requested=%08X result=%08X current=%08X remaining=%08X "
            "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "esp=%08X\n",
            site, saved_allocator[slot], saved_target[slot],
            saved_requested[slot], result,
            guest_u32(saved_allocator[slot] + 8u),
            guest_u32(saved_allocator[slot] + 0xCu),
            g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
    fflush(stderr);
}
void recomp_lua_settable_checkpoint(uint32_t state, uint32_t iteration,
                                    uint32_t target, uint32_t metamethod,
                                    uint32_t key, uint32_t value)
{
    static int enabled = -1;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_CORRUPTION") != NULL;
    if (!enabled)
        return;

    const uint32_t target_type = guest_u32(target);
    const uint32_t target_object = guest_u32(target + 8u);
    const uint32_t metamethod_type = guest_u32(metamethod);
    const uint32_t metamethod_object = guest_u32(metamethod + 8u);
    const uint32_t key_type = guest_u32(key);
    const uint32_t key_object = guest_u32(key + 8u);
    const uint32_t value_type = guest_u32(value);
    const uint32_t value_object = guest_u32(value + 8u);
    uint32_t metatable = 0u;

    if (target_type == 5u && target_object >= 0x00010000u &&
        target_object < 0x03FFFFF4u)
        metatable = guest_u32(target_object + 8u);

    fprintf(stderr,
            "[LUA-SETTABLE] i=%u L=%08X target=%08X type=%u obj=%08X "
            "metatable=%08X tm=%08X type=%u obj=%08X "
            "key=%08X type=%u obj=%08X value=%08X type=%u obj=%08X",
            iteration, state, target, target_type, target_object, metatable,
            metamethod, metamethod_type, metamethod_object,
            key, key_type, key_object, value, value_type, value_object);
    if (key_type == 4u && key_object >= 0x00010000u &&
        key_object < 0x03FFFFF0u) {
        uint32_t length = guest_u32(key_object + 0x0Cu);
        uint32_t shown = length < 80u ? length : 80u;
        fputs(" keyText=", stderr);
        if (shown <= 0x04000000u - key_object - 0x10u) {
            for (uint32_t i = 0u; i < shown; ++i) {
                const uint8_t ch = guest_u8(key_object + 0x10u + i);
                fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
            }
        }
    }
    fputc('\n', stderr);
    fflush(stderr);
}
void recomp_shl_vm_checkpoint(uint32_t stage, uint32_t state,
                              uint32_t value0, uint32_t value1)
{
    static int enabled = -1;
    static int active;
    static uint32_t samples;
    uint32_t type = 0xFFFFFFFFu;
    uint32_t object = 0u;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_SHL_LUA") != NULL;
    if (!enabled)
        return;
    if (stage == 0u)
        active = (value1 == 2331u || value1 == 2332u);
    if (!active || samples++ >= 256u)
        return;

    if (value0 >= 0x00010000u && value0 < 0x03FFFFF0u) {
        type = guest_u32(value0);
        object = guest_u32(value0 + 8u);
    }
    fprintf(stderr,
            "[SHL-VM] #%u stage=%u L=%08X value0=%08X value1=%08X "
            "type=%u object=%08X",
            samples - 1u, stage, state, value0, value1, type, object);
    if ((stage == 2u || stage == 42u) && type == 4u && object >= 0x00010000u &&
        object < 0x03FFFFF0u) {
        const uint32_t length = guest_u32(object + 0x0Cu);
        const uint32_t shown = length < 96u ? length : 96u;
        fputs(" keyText=", stderr);
        if (shown <= 0x04000000u - object - 0x10u) {
            for (uint32_t i = 0u; i < shown; ++i) {
                const uint8_t ch = guest_u8(object + 0x10u + i);
                fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
            }
        }
        if (stage == 42u && length == 5u &&
            memcmp(guest_ptr(object + 0x10u), "table", 5u) == 0) {
            g_shl_followup_index = 0u;
            g_shl_followup_budget = 384u;
        }
    }
    fputc('\n', stderr);
    fflush(stderr);
}
static void print_lua_string_value(uint32_t value)
{
    const uint32_t type = guest_u32(value);
    const uint32_t object = guest_u32(value + 8u);

    fprintf(stderr, " value=%08X type=%u object=%08X", value, type, object);
    if (type == 4u && object >= 0x00010000u && object < 0x03FFFFF0u) {
        const uint32_t length = guest_u32(object + 0x0Cu);
        const uint32_t shown = length < 64u ? length : 64u;
        fputs(" text=\"", stderr);
        if (shown <= 0x04000000u - object - 0x10u) {
            for (uint32_t i = 0u; i < shown; ++i) {
                const uint8_t ch = guest_u8(object + 0x10u + i);
                fputc(ch >= 0x20u && ch < 0x7Fu ? ch : '.', stderr);
            }
        }
        fputc('"', stderr);
    }
}

void recomp_lua_equal_checkpoint(uint32_t left, uint32_t right,
                                 uint32_t result, uint32_t expected,
                                 uint32_t instruction, uint32_t next_pc)
{
    static int enabled = -1;
    static uint32_t samples;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_SHL_LUA") != NULL;
    if (!enabled || samples++ >= 96u)
        return;

    fputs("[LUA-EQ] left", stderr);
    print_lua_string_value(left);
    fputs(" right", stderr);
    print_lua_string_value(right);
    fprintf(stderr,
            " result=%u expected=%u rawA=%u instruction=%08X "
            "nextPc=%08X next=%08X after=%08X\n",
            result, expected, instruction >> 24, instruction, next_pc,
            guest_u32(next_pc), guest_u32(next_pc + 4u));
    fflush(stderr);
}
void recomp_update_caller_checkpoint(uint32_t caller, uint32_t object)
{
    static int enabled = -1;
    static uint32_t samples;
    uint32_t vtable;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_UPDATE_CALLER") != NULL;
    if (!enabled || object < 0x00010000u || object > 0x03FFF9FFu)
        return;

    vtable = guest_u32(object);
    if (vtable != 0x002E0668u && samples >= 64u)
        return;
    ++samples;
    fprintf(stderr,
            "[UPDATE-CALL] caller=%08X object=%08X vtable=%08X "
            "ring=%08X/%08X fallback=%08X\n",
            caller, object, vtable, guest_u32(object + 0x254u),
            guest_u32(object + 0x258u), guest_u32(object + 0x260u));
    fflush(stderr);
}
void recomp_update_esi_checkpoint(uint32_t site, uint32_t expected,
                                  uint32_t current)
{
    static uint32_t reports;

    if (current == expected || reports++ >= 16u)
        return;
    fprintf(stderr,
            "[UPDATE-ESI-CLOBBER] site=%08X expected=%08X current=%08X "
            "eax=%08X ecx=%08X edx=%08X esp=%08X\n",
            site, expected, current, g_eax, g_ecx, g_edx, g_esp);
    fflush(stderr);
}
void recomp_object_getter_checkpoint(uint32_t site, uint32_t object,
                                     uint32_t return_address)
{
    if ((object & 3u) == 0u)
        return;
    fprintf(stderr,
            "[OBJECT-GETTER-INVALID] site=%08X object=%08X return=%08X "
            "eax=%08X esi=%08X edi=%08X esp=%08X\n",
            site, object, return_address, g_eax, g_esi, g_edi, g_esp);
    fflush(stderr);
}

uint32_t recomp_validate_texture_bind(uint32_t caller, uint32_t stage,
                                      uint32_t texture)
{
    static uint32_t reports;

    if (texture == 0u ||
        (texture >= 0x00010000u && texture <= 0x03FFFFE0u))
        return texture;
    if (reports++ < 64u) {
        fprintf(stderr,
                "[TEXTURE-BIND-INVALID] caller=%08X stage=%u "
                "texture=%08X; binding NULL\n",
                caller, stage, texture);
        fflush(stderr);
    }
    /* The retail SetTexture helper dereferences non-NULL resources. A value
       outside mapped guest RAM is necessarily corrupted state, not a valid
       Xbox resource. Preserve execution and make the missing bind explicit
       rather than allowing a host access violation. */
    return 0u;
}
void recomp_transient_update_abi_checkpoint(
    uint32_t site, uint32_t expected_esi, uint32_t current_esi,
    uint32_t expected_edi, uint32_t current_edi,
    uint32_t expected_esp, uint32_t current_esp)
{
    static uint32_t reports;

    if ((expected_esi == current_esi && expected_edi == current_edi &&
         expected_esp == current_esp) || reports++ >= 16u)
        return;
    fprintf(stderr,
            "[TRANSIENT-UPDATE-ABI] site=%08X esi=%08X/%08X "
            "edi=%08X/%08X esp=%08X/%08X eax=%08X ecx=%08X edx=%08X\n",
            site, current_esi, expected_esi, current_edi, expected_edi,
            current_esp, expected_esp, g_eax, g_ecx, g_edx);
    fflush(stderr);
}
void recomp_human_animation_pointer_checkpoint(uint32_t stage,
                                               uint32_t actor)
{
    typedef struct { uint32_t actor, value; } slot_t;
    static slot_t slots[64];
    static uint32_t reports;
    uint32_t value;
    uint32_t i;

    if (getenv("MERCENARIES_TRACE_HUMAN_ANIMATION_POINTER") == NULL ||
        actor < 0x00010000u || actor > 0x03FFF000u)
        return;
    value = guest_u32(actor + 0x79Cu);
    for (i = 0; i < 64u && slots[i].actor != actor && slots[i].actor != 0u;
         ++i) {}
    if (i == 64u)
        return;
    if (slots[i].actor == 0u) {
        slots[i].actor = actor;
        slots[i].value = value;
        return;
    }
    if (slots[i].value != value && reports++ < 128u) {
        fprintf(stderr,
                "[HUMAN-ANIM-POINTER] stage=%u actor=%08X old=%08X "
                "value=%08X current=%08X eax=%08X ebx=%08X ecx=%08X "
                "edx=%08X esi=%08X edi=%08X esp=%08X\n",
                stage, actor, slots[i].value, value, g_recomp_current_func,
                g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp);
        fflush(stderr);
    }
    slots[i].value = value;
}
void recomp_zephyr_anim_checkpoint(uint32_t stage, uint32_t instance,
                                   uint32_t source_handle,
                                   uint32_t animation)
{
    typedef struct {
        uint32_t stage;
        uint32_t instance;
        uint32_t animation;
    } invalid_slot_t;
    static invalid_slot_t invalid_slots[256];
    static uint32_t reports;
    static int enabled = -1;
    uint32_t frames = 0u, joints = 0u, joint_data = 0u;
    uint32_t i;
    int valid = 0;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_ZEPHYR_ANIM") != NULL;
    if (!enabled || instance < 0x00010000u ||
        instance > 0x03FFF4F0u || reports >= 256u)
        return;

    if (stage != 0u)
        animation = guest_u32(instance + 0xB00u);

    if (animation >= 0x00010000u && animation <= 0x03FFFFD8u &&
        (animation & 3u) == 0u) {
        joint_data = guest_u32(animation + 0x18u);
        frames = guest_u16(animation + 0x20u);
        joints = guest_u16(animation + 0x22u);
        valid = joint_data >= 0x00010000u &&
                joint_data <= 0x03FFFFE8u &&
                (joint_data & 3u) == 0u &&
                frames > 0u && joints > 0u && joints <= 64u;
    }

    /* A null animation is the ordinary SetAnim clear path. Valid handles are
     * overwhelmingly common and would exhaust the bounded trace before the
     * first corrupt sampler use. Record only distinct invalid non-null tuples. */
    if (animation == 0u || valid)
        return;
    for (i = 0u; i < 256u; ++i) {
        if (invalid_slots[i].instance == instance &&
            invalid_slots[i].animation == animation &&
            invalid_slots[i].stage == stage)
            return;
        if (invalid_slots[i].instance == 0u)
            break;
    }
    if (i == 256u)
        return;
    invalid_slots[i].stage = stage;
    invalid_slots[i].instance = instance;
    invalid_slots[i].animation = animation;
    ++reports;
    fprintf(stderr,
            "[ZEPHYR-ANIM] stage=%u instance=%08X source=%08X "
            "anim=%08X valid=%u frames=%u joints=%u joint_data=%08X "
            "maps=%08X/%08X/%08X/%08X current=%08X esp=%08X\n",
            stage, instance, source_handle, animation, valid, frames,
            joints, joint_data, guest_u32(instance + 0xAC0u),
            guest_u32(instance + 0xAC4u),
            guest_u32(instance + 0xAFCu),
            guest_u32(instance + 0xB00u),
            g_recomp_current_func, g_esp);
    fflush(stderr);
}
#define TARGET_MANAGER_WATCH_CAPACITY 2048u
static uint32_t g_target_manager_watch_targets[TARGET_MANAGER_WATCH_CAPACITY];
static uint32_t g_target_manager_watch_previous[TARGET_MANAGER_WATCH_CAPACITY];
static uint32_t g_target_manager_watch_count;
static uint32_t g_target_manager_watch_reported;
static uint32_t g_target_manager_watch_armed_reports;
volatile uint32_t g_recomp_target_manager_watch_enabled;
static int g_target_manager_watch_enabled = -1;

static int recomp_target_manager_trace_enabled(void)
{
    if (g_target_manager_watch_enabled < 0) {
        g_target_manager_watch_enabled =
            getenv("MERCENARIES_TRACE_TARGET_MANAGER") != NULL;
        g_recomp_target_manager_watch_enabled =
            g_target_manager_watch_enabled != 0;
    }
    return g_target_manager_watch_enabled;
}

static int recomp_target_manager_node_valid(uint32_t target,
                                            uint32_t manager)
{
    uint32_t node;
    uint32_t next;
    uint32_t previous;

    if (target < 0x00010000u || target > 0x03FFFFE8u ||
        manager < 0x00010000u || manager > 0x03FFFFF0u)
        return 0;
    node = target + 4u;
    next = guest_u32(node);
    previous = guest_u32(node + 4u);
    return next >= 0x00010000u && next <= 0x03FFFFF0u &&
           previous >= 0x00010000u && previous <= 0x03FFFFF0u &&
           (next & 3u) == 0u && (previous & 3u) == 0u &&
           guest_u32(node + 8u) == target &&
           guest_u32(target + 0x10u) == manager &&
           guest_u32(next + 4u) == node && guest_u32(previous) == node;
}

static void recomp_target_manager_dump_corruption(const char *reason,
                                                   uint32_t stage,
                                                   uint32_t manager,
                                                   uint32_t target,
                                                   uint32_t previous)
{
    uint32_t end;
    uint32_t count;
    uint32_t i;

    if (g_target_manager_watch_reported++)
        return;
    fprintf(stderr,
            "[TARGET-LIST-CORRUPT] reason=%s stage=%u manager=%08X "
            "target=%08X vtable=%08X next=%08X prev=%08X object=%08X "
            "list=%08X owner=%08X previous=%08X current=%08X "
            "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "ebp=%08X esp=%08X\n",
            reason, stage, manager, target,
            target >= 0x00010000u && target <= 0x03FFFFE8u ?
                guest_u32(target) : 0u,
            target >= 0x00010000u && target <= 0x03FFFFE8u ?
                guest_u32(target + 4u) : 0u,
            target >= 0x00010000u && target <= 0x03FFFFE8u ?
                guest_u32(target + 8u) : 0u,
            target >= 0x00010000u && target <= 0x03FFFFE8u ?
                guest_u32(target + 0xCu) : 0u,
            target >= 0x00010000u && target <= 0x03FFFFE8u ?
                guest_u32(target + 0x10u) : 0u,
            target >= 0x00010000u && target <= 0x03FFFFA0u ?
                guest_u32(target + 0x5Cu) : 0u,
            previous, g_recomp_current_func, g_eax, g_ebx, g_ecx, g_edx,
            g_esi, g_edi, g_seh_ebp, g_esp);
    end = g_recomp_recent_game_func_idx;
    count = end < 128u ? end : 128u;
    fprintf(stderr, "[TARGET-LIST-CORRUPT-RECENT] count=%u\n", count);
    for (i = 0u; i < count; ++i) {
        const uint32_t index = (end - count + i) & 255u;
        fprintf(stderr, "  [%03u] %08X\n", i,
                g_recomp_recent_game_funcs[index]);
    }
    fflush(stderr);
    g_recomp_entry_trace_enabled = 1u;
}

static void recomp_target_manager_watch_validate(void)
{
    uint32_t i;

    if (!recomp_target_manager_trace_enabled() ||
        g_target_manager_watch_reported)
        return;
    for (i = 0u; i < g_target_manager_watch_count; ++i) {
        const uint32_t target = g_target_manager_watch_targets[i];
        uint32_t manager;
        if (target == 0u)
            continue;
        manager = guest_u32(target + 0x10u);
        if (manager == 0u)
            continue;
        if (!recomp_target_manager_node_valid(target, manager)) {
            recomp_target_manager_dump_corruption(
                "watch", 3u, manager, target,
                g_target_manager_watch_previous[i]);
            return;
        }
        g_target_manager_watch_previous[i] = g_recomp_current_func;
    }
}

void recomp_target_manager_watch_checkpoint(uint32_t xbox_va)
{
    static uint32_t cursor;
    uint32_t attempts;

    if (!g_recomp_target_manager_watch_enabled ||
        g_target_manager_watch_reported ||
        g_target_manager_watch_count == 0u)
        return;
    for (attempts = 0u; attempts < g_target_manager_watch_count;
         ++attempts) {
        const uint32_t i = cursor++ % g_target_manager_watch_count;
        const uint32_t target = g_target_manager_watch_targets[i];
        uint32_t manager;
        if (target == 0u)
            continue;
        manager = guest_u32(target + 0x10u);
        if (manager != 0u &&
            !recomp_target_manager_node_valid(target, manager)) {
            recomp_target_manager_dump_corruption(
                "function-entry", 4u, manager, target,
                g_target_manager_watch_previous[i]);
            return;
        }
        g_target_manager_watch_previous[i] = xbox_va;
        return;
    }
}

void recomp_target_manager_checkpoint(uint32_t stage, uint32_t manager,
                                      uint32_t target)
{
    uint32_t owner;
    uint32_t i;
    uint32_t free_slot = TARGET_MANAGER_WATCH_CAPACITY;

    if (!recomp_target_manager_trace_enabled() ||
        target < 0x00010000u || target > 0x03FFFFA0u)
        return;
    owner = guest_u32(target + 0x5Cu);

    for (i = 0u; i < g_target_manager_watch_count; ++i) {
        if (g_target_manager_watch_targets[i] == target)
            break;
        if (free_slot == TARGET_MANAGER_WATCH_CAPACITY &&
            g_target_manager_watch_targets[i] == 0u)
            free_slot = i;
    }
    if (stage == 1u) {
        if (!recomp_target_manager_node_valid(target, manager)) {
            recomp_target_manager_dump_corruption(
                "post-insert", stage, manager, target,
                g_recomp_current_func);
            return;
        }
        if (i == g_target_manager_watch_count) {
            if (free_slot < TARGET_MANAGER_WATCH_CAPACITY) {
                i = free_slot;
            } else if (g_target_manager_watch_count <
                       TARGET_MANAGER_WATCH_CAPACITY) {
                i = g_target_manager_watch_count++;
            }
        }
        if (i < TARGET_MANAGER_WATCH_CAPACITY) {
            g_target_manager_watch_targets[i] = target;
            g_target_manager_watch_previous[i] = g_recomp_current_func;
            if (g_target_manager_watch_armed_reports < 16u) {
                const uint32_t owner_vtable =
                    owner >= 0x00010000u && owner <= 0x03FFFFFCu ?
                        guest_u32(owner) : 0u;
                fprintf(stderr,
                        "[TARGET-LIST-ARM] slot=%u target=%08X "
                        "vtable=%08X manager=%08X owner=%08X "
                        "owner-vtable=%08X current=%08X\n",
                        i, target, guest_u32(target), manager, owner,
                        owner_vtable, g_recomp_current_func);
                fflush(stderr);
                ++g_target_manager_watch_armed_reports;
            }
        }
    } else if (stage == 2u) {
        if (guest_u32(target + 0x10u) != 0u ||
            guest_u32(target + 0xCu) != 0u) {
            recomp_target_manager_dump_corruption(
                "post-remove", stage, manager, target,
                g_recomp_current_func);
        }
        if (i < g_target_manager_watch_count) {
            g_target_manager_watch_targets[i] = 0u;
            g_target_manager_watch_previous[i] = 0u;
        }
    }
}

uint32_t recomp_traffic_random_path_target(uint32_t manager,
                                           uint32_t path_record,
                                           uint32_t target)
{
    static int initialized;
    static uint32_t target_cap;
    static uint32_t reports;
    const uint32_t path = guest_u32(path_record);
    const uint32_t zone = guest_u32(path_record + 4u);
    const uint32_t network = guest_u32(path_record + 8u);
    uint32_t result = target;

    if (!initialized) {
        const char *value = getenv("MERCENARIES_TEST_TRAFFIC_RANDOM_PATH_TARGET_CAP");
        target_cap = value != NULL ? (uint32_t)strtoul(value, NULL, 0) : 0u;
        initialized = 1;
    }
    if (target_cap != 0u && result > target_cap)
        result = target_cap;
    if ((getenv("MERCENARIES_TRACE_TRAFFIC_SPAWN") != NULL ||
         result != target) && reports++ < 512u) {
        fprintf(stderr,
                "[TRAFFIC-RANDOM-PATH] manager=%08X paths=%u record=%08X "
                "path=%08X zone=%08X network=%08X length=%.9g density=%.9g "
                "attached=%u destroyed=%u target=%u effective=%u\n",
                manager, guest_u32(manager + 0x18B54u), path_record, path,
                zone, network,
                path >= 0x00010000u && path <= 0x03FFFFF8u ?
                    guest_f32(path + 0x24u) : 0.0f,
                zone >= 0x00010000u && zone <= 0x03FFFFF8u ?
                    guest_f32(zone + 4u) : 0.0f,
                guest_u32(path_record + 0xCu),
                guest_u32(path_record + 0x1Cu), target, result);
        fflush(stderr);
    }
    return result;
}
/* Keep unrelated traffic from exhausting a focused lifetime capture. */
static int recomp_traffic_trace_accept(uint32_t path_record)
{
    static int enabled = -1;
    static int filtered;
    static uint32_t zone_filter;
    if (enabled < 0) {
        const char *filter = getenv("MERCENARIES_TRACE_TRAFFIC_ZONE");
        enabled = getenv("MERCENARIES_TRACE_TRAFFIC_SPAWN") != NULL;
        if (enabled && filter && *filter) {
            if (strlen(filter) != 8u || strspn(filter, "0123456789abcdefABCDEF") != 8u) {
                fprintf(stderr, "[TRAFFIC-TRACE] invalid zone filter; capture disabled\n");
                enabled = 0;
            } else {
                zone_filter = (uint32_t)strtoul(filter, NULL, 16);
                filtered = 1;
            }
        }
    }
    if (!enabled || path_record < 0x10000u || path_record > 0x04000000u - 0x28u)
        return 0;
    if (filtered) {
        const uint32_t zone = guest_u32(path_record + 4u);
        return zone >= 0x10000u && zone <= 0x04000000u - 0x280u &&
               guest_u32(zone) == zone_filter;
    }
    return 1;
}

void recomp_traffic_spawn_checkpoint(uint32_t stage, uint32_t manager,
                                     uint32_t path_record,
                                     uint32_t spawn_type,
                                     uint32_t point_index,
                                     uint32_t matrix, uint32_t result)
{
    static uint32_t reports;
    const int trace = recomp_traffic_trace_accept(path_record);
    const int preview = (stage == 1u || stage == 3u) && (result & 0xFFu) != 0u &&
        (spawn_type == 0x51C5EF2Bu || spawn_type == 0xFEEF1EC9u) &&
        xbox_preview_log_enabled();
    if ((!trace && !preview) || reports >= 4096u)
        return;
    ++reports;
    const uint32_t path = guest_u32(path_record);
    const uint32_t zone = guest_u32(path_record + 4u);
    const uint32_t network = guest_u32(path_record + 8u);
    const uint32_t path_name =
        path >= 0x00010000u && path <= 0x03FFFFF8u ?
            guest_u32(path + 4u) : 0u;
    const uint32_t zone_name =
        zone >= 0x00010000u && zone <= 0x03FFFD80u ?
            guest_u32(zone) : 0u;
    uint32_t zone_type_index = UINT32_MAX;
    uint32_t zone_type_cap = 0u;
    uint32_t zone_type_count = 0u;
    uint32_t zone_type_total = 0u;

    if (zone >= 0x00010000u && zone <= 0x03FFFD80u) {
        zone_type_total = guest_u32(zone + 8u);
        const uint32_t bounded_type_total =
            zone_type_total < 15u ? zone_type_total : 15u;
        for (uint32_t index = 0u; index < bounded_type_total; ++index) {
            if (guest_u32(zone + 0x0Cu + index * 4u) == spawn_type) {
                zone_type_index = index;
                zone_type_cap = guest_u32(zone + 0x48u + index * 4u);
                zone_type_count = guest_u32(zone + 0x84u + index * 4u);
                break;
            }
        }
    }

    if (preview)
        xbox_preview_log_event("helicopter-spawn",
            "template=%08X zone=%08X path=%08X path_attached=%u "
            "zone_attached=%u type_count=%u type_cap=%u network_attached=%u",
            spawn_type, zone_name, path_name, guest_u32(path_record + 0xCu),
            zone >= 0x10000u && zone <= 0x03FFFD80u ? guest_u32(zone + 0x26Cu) : 0u,
            zone_type_count, zone_type_cap,
            network >= 0x10000u && network <= 0x03FFFFF0u ? guest_u32(network + 4u) : 0u);
    if (!trace) return;

    fprintf(stderr,
            "[TRAFFIC-SPAWN] stage=%u tick=%llu manager=%08X record=%08X "
            "path=%08X path_name=%08X zone=%08X zone_name=%08X network=%08X "
            "type=%08X zone_type_index=%08X zone_type_cap=%u "
            "zone_type_count=%u zone_type_total=%u point=%u matrix=%08X "
            "position=(%.9g,%.9g,%.9g) result=%u "
            "path_attached=%d path_destroyed=%d network_attached=%d "
            "network_max=%d network_desired=%.9g zone_attached=%d "
            "zone_destroyed=%d zone_desired=%.9g path_desired=%.9g\n",
            stage, (unsigned long long)GetTickCount64(), manager, path_record, path, path_name, zone, zone_name,
            network, spawn_type, zone_type_index, zone_type_cap,
            zone_type_count, zone_type_total, point_index, matrix,
            matrix >= 0x00010000u && matrix <= 0x03FFFFC0u ?
                guest_f32(matrix + 0x30u) : 0.0f,
            matrix >= 0x00010000u && matrix <= 0x03FFFFC0u ?
                guest_f32(matrix + 0x34u) : 0.0f,
            matrix >= 0x00010000u && matrix <= 0x03FFFFC0u ?
                guest_f32(matrix + 0x38u) : 0.0f,
            result,
            (int32_t)guest_u32(path_record + 0xCu),
            (int32_t)guest_u32(path_record + 0x1Cu),
            network >= 0x00010000u && network <= 0x03FFFFF0u ?
                (int32_t)guest_u32(network + 4u) : INT32_MIN,
            network >= 0x00010000u && network <= 0x03FFFFF0u ?
                (int32_t)guest_u32(network) : INT32_MIN,
            network >= 0x00010000u && network <= 0x03FFFFF0u ?
                guest_f32(network + 0xCu) : -1.0f,
            zone >= 0x00010000u && zone <= 0x03FFFD80u ?
                (int32_t)guest_u32(zone + 0x26Cu) : INT32_MIN,
            zone >= 0x00010000u && zone <= 0x03FFFD80u ?
                (int32_t)guest_u32(zone + 0x270u) : INT32_MIN,
            zone >= 0x00010000u && zone <= 0x03FFFD80u ?
                guest_f32(zone + 0x268u) : -1.0f,
            guest_f32(path_record + 0x10u));
    fflush(stderr);
}

/* Bounded opt-in provenance; does not change lifecycle or guest registers. */



/* Identity-aware helicopter attachments, including arrivals outside the zone filter. */
void recomp_traffic_attach_checkpoint(uint32_t site, uint32_t path, uint32_t ai)
{
    static int enabled = -1;
    static uint32_t reports;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_TRAFFIC_SPAWN") != NULL;
    if (!enabled || reports >= 2048u || ai < 0x10000u || ai > 0x03FFFFC0u)
        return;
    const uint32_t actor = guest_u32(ai + 0x10u);
    if (actor < 0x10000u || actor > 0x03FFFF10u || guest_u32(actor) != 0x002E21A8u)
        return;
    const uint32_t manager = 0x003DC970u;
    const uint32_t count = guest_u32(manager + 0x18B54u);
    if (count > 4096u)
        return;
    uint32_t record = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t candidate = manager + 0x18B58u + i * 0x28u;
        if (guest_u32(candidate) == path) { record = candidate; break; }
    }
    if (!record)
        return;
    const uint32_t zone = guest_u32(record + 4u);
    const uint32_t spore = guest_u32(actor + 8u);
    if (zone < 0x10000u || zone > 0x03FFFD80u ||
        spore < 0x10000u || spore > 0x03FFFFCCu)
        return;
    ++reports;
    fprintf(stderr,
        "[TRAFFIC-ATTACH] tick=%llu site=%08X ai=%08X actor=%08X guid=%08X "
        "old_path=%08X path=%08X record=%08X zone=%08X zone_name=%08X "
        "path_attached_before=%d zone_attached_before=%d destroyed=%d "
        "hp=%.9g position=(%.9g,%.9g,%.9g)\n",
        (unsigned long long)GetTickCount64(), site, ai, actor, guest_u32(spore + 0x28u),
        guest_u32(ai + 0x3Cu), path, record, zone, guest_u32(zone),
        (int32_t)guest_u32(record + 0xCu), (int32_t)guest_u32(zone + 0x26Cu),
        (int32_t)guest_u32(zone + 0x270u), guest_f32(actor + 0x98u),
        guest_f32(actor + 0xE0u), guest_f32(actor + 0xE4u), guest_f32(actor + 0xE8u));
    fflush(stderr);
}

void recomp_traffic_release_checkpoint(uint32_t site, uint32_t ai, uint32_t destroyed)
{
    static int enabled = -1;
    static uint32_t reports;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_TRAFFIC_SPAWN") != NULL;
    if (!enabled || reports >= 2048u || ai < 0x10000u || ai > 0x03FFFFC0u)
        return;
    const uint32_t path = guest_u32(ai + 0x3Cu);
    const uint32_t count = guest_u32(0x003DC970u + 0x18B54u);
    if (!path || count > 4096u)
        return;
    uint32_t record = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t candidate = 0x003DC970u + 0x18B58u + i * 0x28u;
        if (guest_u32(candidate) == path) {
            record = candidate;
            break;
        }
    }
    if (!record || !recomp_traffic_trace_accept(record))
        return;
    ++reports;
    const uint32_t actor = guest_u32(ai + 0x10u);
    const int actor_valid = actor >= 0x10000u && actor <= 0x03FFFF10u;
    const uint32_t spore = actor_valid ? guest_u32(actor + 8u) : 0u;
    const int spore_valid = spore >= 0x10000u && spore <= 0x03FFFFCCu;
    fprintf(stderr,
        "[TRAFFIC-RELEASE] tick=%llu site=%08X ai=%08X actor=%08X path=%08X record=%08X "
        "destroyed=%u hp=%.9g spore=%08X guid=%08X spore_flags=%08X "
        "position=(%.9g,%.9g,%.9g)\n",
        (unsigned long long)GetTickCount64(), site, ai, actor, path, record,
        destroyed, actor_valid ? guest_f32(actor + 0x98u) : 0.0f,
        spore, spore_valid ? guest_u32(spore + 0x28u) : 0u,
        spore_valid ? guest_u32(spore + 0x30u) : 0u,
        actor_valid ? guest_f32(actor + 0xE0u) : 0.0f,
        actor_valid ? guest_f32(actor + 0xE4u) : 0.0f,
        actor_valid ? guest_f32(actor + 0xE8u) : 0.0f);
    fflush(stderr);
}

void recomp_traffic_detach_checkpoint(uint32_t stage, uint32_t path_record,
                                      uint32_t ai, uint32_t destroyed)
{
    static uint32_t reports;
    if (!recomp_traffic_trace_accept(path_record) || reports >= 4096u)
        return;
    ++reports;
    const uint32_t zone = guest_u32(path_record + 4u);
    const uint32_t network = guest_u32(path_record + 8u);
    const uint32_t actor = ai >= 0x10000u && ai <= 0x04000000u - 0x14u ?
                           guest_u32(ai + 0x10u) : 0u;
    const int actor_valid = actor >= 0x10000u && actor <= 0x04000000u - 0xF0u;

    fprintf(stderr,
            "[TRAFFIC-DETACH] stage=%u tick=%llu record=%08X ai=%08X actor=%08X destroyed=%u "
            "zone=%08X zone_name=%08X network=%08X path_attached=%d path_destroyed=%d "
            "network_attached=%d zone_attached=%d zone_destroyed=%d "
            "actor_pos=(%.9g,%.9g,%.9g)\n",
            stage, (unsigned long long)GetTickCount64(), path_record, ai, actor, destroyed, zone,
            zone >= 0x10000u && zone <= 0x03FFFD80u ? guest_u32(zone) : 0u, network,
            (int32_t)guest_u32(path_record + 0xCu),
            (int32_t)guest_u32(path_record + 0x1Cu),
            network >= 0x00010000u && network <= 0x03FFFFF0u ?
                (int32_t)guest_u32(network + 4u) : INT32_MIN,
            zone >= 0x00010000u && zone <= 0x03FFFD80u ?
                (int32_t)guest_u32(zone + 0x26Cu) : INT32_MIN,
            zone >= 0x00010000u && zone <= 0x03FFFD80u ?
                (int32_t)guest_u32(zone + 0x270u) : INT32_MIN,
            actor_valid ? guest_f32(actor + 0xE0u) : 0.0f,
            actor_valid ? guest_f32(actor + 0xE4u) : 0.0f,
            actor_valid ? guest_f32(actor + 0xE8u) : 0.0f);
    fflush(stderr);
}

/* StopSys discards all registered paths. Their accumulated desired traffic
 * must not survive into the next world, whose new path records start at zero.
 * This invariant is independent of OG Bugs. Attached counts belong to actor
 * teardown and must not be reset here. */
void recomp_traffic_stop_network_totals(uint32_t manager)
{
    if (xbox_preview_log_enabled())
        xbox_preview_log_event("traffic-world-stop",
            "paths=%u vehicle_desired=%g pedestrian_desired=%g reset=1",
            guest_u32(manager + 0x18B54u), guest_f32(manager + 0x22D40u),
            guest_f32(manager + 0x22D50u));
    *(float *)guest_ptr(manager + 0x22D40u) = 0.0f;
    *(float *)guest_ptr(manager + 0x22D50u) = 0.0f;
}

void recomp_traffic_update_checkpoint(uint32_t manager, float distance_sq,
                                      float camera_x, float camera_y,
                                      float camera_z, float previous_x,
                                      float previous_y, float previous_z)
{
    static int enabled = -1;
    static uint32_t updates;
    static uint32_t random_refills;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_TRAFFIC_SPAWN") != NULL;
    if (!enabled)
        return;
    updates++;
    if (distance_sq > 10000.0f) {
        random_refills++;
        fprintf(stderr,
                "[TRAFFIC-UPDATE] update=%u refill=%u manager=%08X "
                "distance_sq=%.9g camera=(%.9g,%.9g,%.9g) "
                "previous=(%.9g,%.9g,%.9g) paths=%u\n",
                updates, random_refills, manager, distance_sq,
                camera_x, camera_y, camera_z,
                previous_x, previous_y, previous_z,
                guest_u32(manager + 0x18B54u));
        fflush(stderr);
    } else if ((updates % 1800u) == 0u) {
        fprintf(stderr,
                "[TRAFFIC-UPDATE] update=%u refill=%u manager=%08X "
                "distance_sq=%.9g heartbeat=1 paths=%u\n",
                updates, random_refills, manager, distance_sq,
                guest_u32(manager + 0x18B54u));
        fflush(stderr);
    }
}
volatile uint32_t g_recomp_particle_list_watch_enabled;

static void recomp_particle_list_report(const char *reason,
                                        uint32_t node,
                                        uint32_t next,
                                        uint32_t payload,
                                        uint32_t previous,
                                        uint32_t current)
{
    static LONG reported;
    uint32_t end;
    uint32_t count;
    uint32_t i;

    if (InterlockedCompareExchange(&reported, 1, 0) != 0)
        return;
    fprintf(stderr,
            "[PARTICLE-LIST-CORRUPT] reason=%s node=%08X next=%08X "
            "payload=%08X previous=%08X current=%08X eax=%08X "
            "ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
            "ebp=%08X esp=%08X\n",
            reason, node, next, payload, previous, current, g_eax,
            g_ebx, g_ecx, g_edx, g_esi, g_edi, g_seh_ebp, g_esp);
    end = g_recomp_recent_game_func_idx;
    count = end < 128u ? end : 128u;
    fprintf(stderr, "[PARTICLE-LIST-RECENT] count=%u\n", count);
    for (i = 0u; i < count; ++i) {
        const uint32_t index = (end - count + i) & 255u;
        fprintf(stderr, "  [%03u] %08X\n", i,
                g_recomp_recent_game_funcs[index]);
    }
    fflush(stderr);
}

void recomp_particle_list_watch_checkpoint(uint32_t xbox_va)
{
    static uint32_t samples;
    static uint32_t previous;
    uint32_t node;
    uint32_t i;

    /* The effect registry is an intrusive list rooted at
     * 0x30DC94. Sample it often enough to identify the first translated
     * routine after corruption while keeping this opt-in diagnostic usable. */
    if ((samples++ & 0xFu) != 0u)
        return;
    node = guest_u32(0x0030DC94u);
    if (node == 0u) {
        previous = xbox_va;
        return;
    }
    for (i = 0u; i < 4096u; ++i) {
        uint32_t next;
        uint32_t payload;
        if (node < 0x00010000u || node > 0x03FFFFF0u ||
            (node & 3u) != 0u) {
            recomp_particle_list_report("node", node, 0u, 0u,
                                        previous, xbox_va);
            return;
        }
        next = guest_u32(node);
        payload = guest_u32(node + 8u);
        if (payload == 0u) {
            previous = xbox_va;
            return;
        }
        if (payload < 0x00010000u || payload > 0x03FFFFD0u ||
            (payload & 3u) != 0u) {
            recomp_particle_list_report("payload", node, next, payload,
                                        previous, xbox_va);
            return;
        }
        if (next < 0x00010000u || next > 0x03FFFFF0u ||
            (next & 3u) != 0u) {
            recomp_particle_list_report("next", node, next, payload,
                                        previous, xbox_va);
            return;
        }
        node = next;
    }
    recomp_particle_list_report("cycle", node, 0u, 0u,
                                previous, xbox_va);
}
#define COLLISION_AGENT_WATCH_CAPACITY 4096u
static uint32_t g_collision_agent_watch_objects[COLLISION_AGENT_WATCH_CAPACITY];
static uint32_t g_collision_agent_watch_dispatchers[COLLISION_AGENT_WATCH_CAPACITY];
static uint32_t g_collision_agent_watch_previous[COLLISION_AGENT_WATCH_CAPACITY];
static uint32_t g_collision_agent_watch_count;
static uint32_t g_collision_agent_watch_reported;
volatile uint32_t g_recomp_collision_agent_watch_enabled;
static int g_collision_agent_trace_enabled = -1;

/* Event-only PDA evidence: no per-frame scan, guest calls, or state changes. */
void recomp_datapod_intel_checkpoint(uint32_t status, uint32_t stage)
{
    uint32_t vm, suit, captured = 0u, killed = 0u, displayed = 0u;
    uint32_t pending = 0u, earned = 0u;
    if (status < 0x10000u || status > 0x03FFFC30u)
        return;
    vm = guest_u32(status + 0x24u);
    suit = guest_u32(status + 0x198u);
    if (vm < 0x10000u || vm > 0x03FF1C80u || suit > 3u)
        return;
    for (uint32_t i = 0u; i < 13u; ++i) {
        const uint32_t card = vm + 0xDB40u + (suit * 13u + i) * 40u;
        const uint32_t state = guest_u32(card + 12u);
        const uint32_t bit = 1u << i;
        if (guest_u8(card + 8u)) displayed |= bit;
        if (state == 3u) captured |= bit;
        if (state == 2u) killed |= bit;
        if (state == 2u || state == 3u) {
            earned += i < 9u ? i + 2u : i < 12u ? 30u : 0u;
            if (!(displayed & bit)) pending |= bit;
        }
    }
    xbox_preview_log_event("pda-intel",
        "stage=%s suit=%u displayed=%.6g target=%.6g earned=%u "
        "captured=%04X killed=%04X shown=%04X pending=%04X "
        "timer=%.6g animating=%u rank=%u",
        stage == 1u ? "enter" : stage == 2u ? "award" : "exit",
        suit, guest_f32(status + 0x3B8u), guest_f32(status + 0x3BCu),
        earned, captured, killed, displayed, pending,
        guest_f32(status + 0x3B0u), guest_u8(status + 0x3B4u),
        guest_u32(status + 0x19Cu));
}

void recomp_datapod_timer_checkpoint(uint32_t vm, float delta_time,
                                     float input_timer)
{
    static int enabled = -1;
    static uint32_t active_vm;
    static uint32_t samples;
    static ULONGLONG interval_start_ms;
    static float interval_start_timer;
    const ULONGLONG now = GetTickCount64();

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_DATAPOD_FLASH") != NULL;
    if (!enabled || vm < 0x00010000u || vm > 0x03FF0000u ||
        guest_u32(vm + 0x9694u) == UINT32_MAX)
        return;
    if (active_vm != vm || input_timer < interval_start_timer) {
        active_vm = vm;
        samples = 0u;
        interval_start_ms = now;
        interval_start_timer = input_timer;
    }
    samples += 1u;
    if ((samples % 15u) != 0u)
        return;
    fprintf(stderr,
            "[DATAPOD-FLASH-TIMER] vm=%08X samples=%u dt=%.9g timer=%.9g "
            "guest_elapsed=%.9g host_elapsed_ms=%llu mode=%u current=%08X\n",
            vm, samples, delta_time, input_timer,
            input_timer - interval_start_timer,
            (unsigned long long)(now - interval_start_ms),
            guest_u32(vm + 0x9694u), g_recomp_current_func);
    fflush(stderr);
}
void recomp_secondary_ammo_checkpoint(uint32_t stage, uint32_t hud,
                                      uint32_t value)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_SECONDARY_AMMO") != NULL;
    if (!enabled || reports >= 256u || hud < 0x00010000u ||
        hud > 0x03FFFF80u)
        return;
    reports += 1u;
    fprintf(stderr,
            "[SECONDARY-AMMO] stage=%u hud=%08X ammo=%d max=%d "
            "texture=%08X inactive=%.9g fade=%.9g scale=%.9g "
            "flash_time=%.9g flash_alpha=%.9g flash_speed=%.9g "
            "pre=%08X flash_color=%08X flap=%.9g value=%08X current=%08X\n",
            stage, hud, (int32_t)guest_u32(hud + 0x2Cu),
            (int32_t)guest_u32(hud + 0x30u), guest_u32(hud + 0x38u),
            guest_f32(hud + 0x3Cu), guest_f32(hud + 0x40u),
            guest_f32(hud + 0x48u), guest_f32(hud + 0x50u),
            guest_f32(hud + 0x54u), guest_f32(hud + 0x58u),
            guest_u32(hud + 0x5Cu), guest_u32(hud + 0x60u),
            guest_f32(hud + 0x70u), value, g_recomp_current_func);
    fflush(stderr);
}
void recomp_human_move_selection_checkpoint(uint32_t physics,
                                            float stick_magnitude)
{
    static int enabled = -1;
    static uint32_t reports;
    static uint32_t last_physics;
    static uint32_t last_speed = UINT32_MAX;
    static uint32_t last_mode = UINT32_MAX;
    static uint32_t last_run_enabled = UINT32_MAX;
    uint32_t speed;
    uint32_t mode;
    uint32_t run_enabled;
    int full_stick_walk;
    int state_changed;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_FORCED_WALK") != NULL;
    if (!enabled || physics < 0x00010000u || physics > 0x03FFFF50u ||
        guest_u8(physics + 0xA4u) == 0u)
        return;
    speed = guest_u32(physics + 0xCu);
    mode = guest_u32(physics + 8u);
    run_enabled = guest_u8(physics + 0x65u) != 0u;
    full_stick_walk = stick_magnitude >= 0.95f && speed != 2u;
    state_changed = physics != last_physics || speed != last_speed ||
                    mode != last_mode || run_enabled != last_run_enabled;
    if (!full_stick_walk && !state_changed)
        return;
    last_physics = physics;
    last_speed = speed;
    last_mode = mode;
    last_run_enabled = run_enabled;
    if (reports++ >= 256u)
        return;
    fprintf(stderr,
            "[HUMAN-MOVE-STATE] physics=%08X stick=%.7g selected=%u "
            "run_enabled=%u mode=%u walk=%.7g run=%.7g current=%08X\n",
            physics, stick_magnitude, speed, run_enabled, mode,
            guest_f32(physics + 0x44u), guest_f32(physics + 0x48u),
            g_recomp_current_func);
    fflush(stderr);
}
void recomp_human_jump_checkpoint(uint32_t stage, uint32_t actor,
                                  uint32_t physics, uint32_t result)
{
    static int enabled = -1;
    static uint32_t last_physics;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_FORCED_WALK") != NULL;
    if (!enabled || actor < 0x00010000u || actor > 0x03FFF000u ||
        guest_u32(actor) != 0x002E32B8u || reports++ >= 64u)
        return;
    if (stage == 0u && physics >= 0x00010000u && physics <= 0x03FFFF50u)
        last_physics = physics;
    physics = last_physics;
    fprintf(stderr,
            "[HUMAN-JUMP] stage=%u actor=%08X physics=%08X result=%u "
            "offground=%u selected=%u run_enabled=%u runxy=(%.7g,%.7g) "
            "current=%08X\n",
            stage, actor, physics, result,
            guest_u32(actor + 0x6CCu),
            physics ? guest_u32(physics + 0xCu) : UINT32_MAX,
            physics ? guest_u8(physics + 0x65u) : 0u,
            physics ? guest_f32(physics + 0x30u) : 0.0f,
            physics ? guest_f32(physics + 0x34u) : 0.0f,
            g_recomp_current_func);
    fflush(stderr);
}
void recomp_human_run_enabled_checkpoint(uint32_t site, uint32_t actor,
                                         uint32_t physics, uint32_t enabled_value)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_FORCED_WALK") != NULL;
    if (!enabled || actor != g_recomp_live_human_actor || reports++ >= 128u)
        return;
    fprintf(stderr,
            "[HUMAN-RUN-STATE] site=%08X actor=%08X physics=%08X enabled=%u "
            "can_run=%u submerged=%u current=%08X\n",
            site, actor, physics, enabled_value != 0u,
            guest_u8(actor + 0x817u), guest_u32(actor + 0x818u),
            g_recomp_current_func);
    fflush(stderr);
}

uint32_t recomp_collision_dispatch_state(uint32_t object, uint32_t state)
{
    static uint32_t rejected;

    if (state == 0u || state == object || state == object + 8u ||
        state == object + 0x10u || state == object + 0x18u)
        return state;
    if (object >= 0x00010000u && object <= 0x03FFFFD0u &&
        guest_u32(object) == 0x002F30F4u && rejected++ < 16u) {
        fprintf(stderr,
                "[COLLISION-DISPATCH-REJECT] object=%08X state=%08X "
                "current=%08X\n",
                object, state, g_recomp_current_func);
        fflush(stderr);
    }
    return 0u;
}

static int recomp_collision_agent_trace_enabled(void)
{
    if (g_collision_agent_trace_enabled < 0) {
        g_collision_agent_trace_enabled =
            getenv("MERCENARIES_TRACE_COLLISION_AGENT") != NULL;
        g_recomp_collision_agent_watch_enabled =
            g_collision_agent_trace_enabled != 0;
    }
    return g_collision_agent_trace_enabled;
}

static void recomp_collision_agent_dump_corruption(
    const char *reason, uint32_t object, uint32_t expected,
    uint32_t actual, uint32_t previous, uint32_t current)
{
    uint32_t end;
    uint32_t count;
    uint32_t i;
    uint32_t slot = UINT32_MAX;

    if (g_collision_agent_watch_reported++)
        return;
    for (i = 0u; i < g_collision_agent_watch_count; ++i) {
        if (g_collision_agent_watch_objects[i] == object) {
            slot = i;
            break;
        }
    }
    fprintf(stderr,
            "[COLLISION-DISPATCHER-CORRUPT] reason=%s object=%08X slot=%u "
            "vtable=%08X expected=%08X actual=%08X previous=%08X "
            "current=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X "
            "esi=%08X edi=%08X ebp=%08X esp=%08X\n",
            reason, object, slot, guest_u32(object), expected, actual,
            previous, current, g_eax, g_ebx, g_ecx, g_edx,
            g_esi, g_edi, g_seh_ebp, g_esp);
    end = g_recomp_recent_game_func_idx;
    count = end < 256u ? end : 256u;
    fprintf(stderr, "[COLLISION-DISPATCHER-RECENT] count=%u\n", count);
    for (i = 0u; i < count; ++i) {
        const uint32_t index = (end - count + i) & 255u;
        fprintf(stderr, "  [%03u] %08X\n", i,
                g_recomp_recent_game_funcs[index]);
    }
    fflush(stderr);
    g_recomp_entry_trace_enabled = 1u;
    /* Diagnostic-only: restore the self-dispatch pointer after the polling
     * watcher catches corruption, then let the existing DR0 write watcher
     * stop on the next native instruction that modifies this exact field. */
    *(volatile uint32_t *)((uintptr_t)g_xbox_mem_offset + object + 0x20u) = expected;
    recomp_arm_redscene_watchpoint(object + 0x20u);
    fprintf(stderr,
            "[COLLISION-DISPATCHER-WRITE-WATCH] armed=%08X restored=%08X count=%u\n",
            object + 0x20u, expected, g_collision_agent_watch_count);
    fflush(stderr);
}

static void recomp_collision_agent_watch_track(uint32_t stage,
                                                uint32_t object,
                                                uint32_t dispatcher)
{
    uint32_t i;
    uint32_t vacant = COLLISION_AGENT_WATCH_CAPACITY;

    if ((stage != 9u && stage != 10u && stage != 11u) ||
        object < 0x00010000u || object > 0x03FFFFD0u ||
        guest_u32(object) != 0x002F30F4u)
        return;
    for (i = 0u; i < g_collision_agent_watch_count; ++i) {
        if (g_collision_agent_watch_objects[i] == object)
            break;
        if (g_collision_agent_watch_objects[i] == 0u &&
            vacant == COLLISION_AGENT_WATCH_CAPACITY)
            vacant = i;
    }
    if (stage == 10u) {
        const uint32_t actual = guest_u32(object + 0x20u);
        if (i < g_collision_agent_watch_count && actual == 0u) {
            /* A null callback is a valid teardown state. Stop watching this
             * lifetime so its storage can be reused without a false report. */
            g_collision_agent_watch_objects[i] = 0u;
        } else if (i < g_collision_agent_watch_count &&
                   actual != g_collision_agent_watch_dispatchers[i]) {
            recomp_collision_agent_dump_corruption(
                "pre-setter", object,
                g_collision_agent_watch_dispatchers[i], actual,
                g_collision_agent_watch_previous[i],
                g_recomp_current_func);
        }
        return;
    }
    if (stage == 11u && dispatcher != 0u &&
        (dispatcher < 0x00010000u || dispatcher > 0x03FFFFFCu ||
         (dispatcher & 3u) != 0u)) {
        const uint32_t expected =
            i < g_collision_agent_watch_count ?
                g_collision_agent_watch_dispatchers[i] : object + 8u;
        recomp_collision_agent_dump_corruption(
            "setter-argument", object, expected, dispatcher,
            i < g_collision_agent_watch_count ?
                g_collision_agent_watch_previous[i] : 0u,
            g_recomp_current_func);
        return;
    }
    if (i == g_collision_agent_watch_count) {
        if (vacant != COLLISION_AGENT_WATCH_CAPACITY) {
            i = vacant;
        } else {
            if (g_collision_agent_watch_count >= COLLISION_AGENT_WATCH_CAPACITY)
                return;
            ++g_collision_agent_watch_count;
        }
        g_collision_agent_watch_objects[i] = object;
    }
    if (dispatcher == 0u) {
        g_collision_agent_watch_objects[i] = 0u;
        return;
    }
    g_collision_agent_watch_dispatchers[i] = dispatcher;
    g_collision_agent_watch_previous[i] = g_recomp_current_func;
    if (stage == 9u) {
        const char *slot_text = getenv("MERCENARIES_COLLISION_WRITE_SLOT");
        if (slot_text != NULL && slot_text[0] != '\0' &&
            i == (uint32_t)strtoul(slot_text, NULL, 0)) {
            recomp_arm_redscene_watchpoint(object + 0x20u);
            fprintf(stderr,
                    "[COLLISION-DISPATCHER-EARLY-WATCH] slot=%u object=%08X field=%08X expected=%08X\n",
                    i, object, object + 0x20u, dispatcher);
            fflush(stderr);
        }
    }
}

void recomp_collision_agent_recycle_range(uint32_t base, uint32_t size,
                                          uint32_t site)
{
    uint32_t i;
    uint32_t end;

    if (!g_recomp_collision_agent_watch_enabled || size == 0u ||
        base > UINT32_MAX - size)
        return;
    end = base + size;
    for (i = 0u; i < g_collision_agent_watch_count; ++i) {
        const uint32_t object = g_collision_agent_watch_objects[i];
        if (object >= base && object < end) {
            fprintf(stderr,
                    "[COLLISION-DISPATCHER-RECYCLE] slot=%u object=%08X "
                    "base=%08X size=%08X site=%08X\n",
                    i, object, base, size, site);
            g_collision_agent_watch_objects[i] = 0u;
        }
    }
}

void recomp_collision_manager_checkpoint(uint32_t manager)
{
    uint32_t count;
    uint32_t i;

    if (!recomp_collision_agent_trace_enabled() ||
        manager < 0x00010000u || manager > 0x03FFFFE0u)
        return;
    count = guest_u32(manager);
    if (count > 64u)
        return;
    for (i = 0u; i < count; ++i) {
        const uint32_t object = manager + 0x10u + i * 0xB0u;
        const uint32_t callback = guest_u32(object + 0x20u);
        uint32_t callback_vtable = 0u;
        uint32_t callback_method = 0u;
        int invalid = 0;
        if (guest_u32(object) != 0x002F30F4u || callback == 0u)
            continue;
        if (callback < 0x00010000u || callback > 0x03FFFFFCu ||
            (callback & 3u) != 0u) {
            invalid = 1;
        } else {
            callback_vtable = guest_u32(callback);
            if (callback_vtable < 0x002D0000u ||
                callback_vtable > 0x00320000u ||
                (callback_vtable & 3u) != 0u) {
                invalid = 1;
            } else {
                callback_method = guest_u32(callback_vtable + 4u);
                if (callback_method < 0x00010000u ||
                    callback_method >= 0x002D0000u)
                    invalid = 1;
            }
        }
        if (invalid) {
            fprintf(stderr,
                    "[COLLISION-MANAGER-INVALID] manager=%08X count=%u "
                    "index=%u object=%08X callback=%08X vtable=%08X "
                    "method=%08X owner=%08X\n",
                    manager, count, i, object, callback,
                    callback_vtable, callback_method,
                    guest_u32(manager + 4u));
            recomp_collision_agent_dump_corruption(
                "manager-entry", object, object + 8u, callback,
                g_recomp_current_func, 0x0016D2F0u);
            return;
        }
    }
}
void recomp_collision_agent_watch_checkpoint(uint32_t xbox_va)
{
    static int poll_watch_enabled = -1;
    static uint32_t cursor;
    uint32_t attempts;
    uint32_t checked = 0u;

    /* Construction reuses storage whose old vtable can remain temporarily
     * visible. Keep eager polling behind a separate opt-in; manager-entry
     * validation above is the authoritative live-dispatch check. */
    if (poll_watch_enabled < 0)
        poll_watch_enabled =
            getenv("MERCENARIES_COLLISION_POLL_WATCH") != NULL;
    if (!g_recomp_collision_agent_watch_enabled || !poll_watch_enabled ||
        g_collision_agent_watch_reported ||
        g_collision_agent_watch_count == 0u)
        return;
    for (attempts = 0u; attempts < g_collision_agent_watch_count;
         ++attempts) {
        const uint32_t i = cursor++ % g_collision_agent_watch_count;
        const uint32_t object = g_collision_agent_watch_objects[i];
        uint32_t actual;
        if (object == 0u)
            continue;
        if (guest_u32(object) != 0x002F30F4u) {
            /* The owning actor destroyed this embedded agent. */
            g_collision_agent_watch_objects[i] = 0u;
            continue;
        }
        actual = guest_u32(object + 0x20u);
        if (actual == 0u) {
            /* Null is the normal post-release state, not corruption. */
            g_collision_agent_watch_objects[i] = 0u;
            continue;
        }
        if (actual != g_collision_agent_watch_dispatchers[i]) {
            recomp_collision_agent_dump_corruption(
                "function-entry", object,
                g_collision_agent_watch_dispatchers[i], actual,
                g_collision_agent_watch_previous[i], xbox_va);
            return;
        }
        g_collision_agent_watch_previous[i] = xbox_va;
        if (++checked >= 8u)
            return;
    }
}
void recomp_collision_agent_checkpoint(uint32_t stage, uint32_t object,
                                       uint32_t context, uint32_t detail)
{
    static uint32_t reports;
    uint32_t vtable = 0u;
    uint32_t dispatcher = 0u;

    if (!recomp_collision_agent_trace_enabled())
        return;
    recomp_collision_agent_watch_track(stage, object, context);
    if (reports >= 128u)
        return;
    if (object >= 0x00010000u && object <= 0x03FFFFDCu) {
        vtable = guest_u32(object);
        dispatcher = guest_u32(object + 0x20u);
    }
    /* Retain a short baseline, then only anomalous object/dispatcher values. */
    if (reports >= 12u && (object & 3u) == 0u &&
        dispatcher >= 0x00010000u && dispatcher <= 0x03FFFDC8u &&
        (dispatcher & 3u) == 0u &&
        (stage < 10u || context == 0u ||
         (context >= 0x00010000u && context <= 0x03FFFFFCu &&
          (context & 3u) == 0u)))
        return;
    ++reports;
    fprintf(stderr,
            "[COLLISION-AGENT] stage=%u object=%08X vtable=%08X "
            "dispatcher=%08X context=%08X detail=%08X current=%08X "
            "esp=%08X\n",
            stage, object, vtable, dispatcher, context, detail,
            g_recomp_current_func, g_esp);
    fflush(stderr);
}

void recomp_vehicle_door_checkpoint(uint32_t stage, uint32_t object,
                                    uint32_t context, uint32_t detail)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_VEHICLE_DOOR") != NULL;
    if (!enabled || reports++ >= 2048u)
        return;

    if (stage == 2u && object >= 0x00010000u &&
        object <= 0x03FFFF70u) {
        union { uint32_t u; float f; } dt;
        dt.u = detail;
        fprintf(stderr,
                "[VEHICLE-DOOR] stage=update entrance=%08X state=%08X "
                "dt=%.9g angle=%.9g open_speed=%.9g close_speed=%.9g "
                "max_angle=%.9g current=%08X\n",
                object, context, dt.f, guest_f32(object + 0x74u),
                guest_f32(object + 0x7Cu), guest_f32(object + 0x80u),
                guest_f32(object + 0x78u), g_recomp_current_func);
    } else {
        fprintf(stderr,
                "[VEHICLE-DOOR] stage=%u object=%08X context=%08X "
                "detail=%08X current=%08X esp=%08X\n",
                stage, object, context, detail,
                g_recomp_current_func, g_esp);
    }
    fflush(stderr);
}

void recomp_human_head_checkpoint(uint32_t stage, uint32_t animation,
                                  uint32_t dt_bits)
{
    typedef struct {
        uint32_t animation;
        uint32_t heading_bits;
        uint32_t target_x_bits;
        uint32_t target_z_bits;
        uint8_t touched;
    } slot_t;
    union { uint32_t u; float f; } dt, heading, target_x, target_z;
    static slot_t slots[64];
    static uint32_t reports;
    uint32_t i;

    if (getenv("MERCENARIES_TRACE_HUMAN_HEAD") == NULL ||
        animation < 0x00010000u || animation > 0x03FFD000u ||
        stage != 1u)
        return;
    dt.u = dt_bits;
    heading.u = guest_u32(animation + 0x2938u);
    target_x.u = guest_u32(animation + 0x2908u);
    target_z.u = guest_u32(animation + 0x2910u);
    for (i = 0; i < 64u && slots[i].animation != animation &&
         slots[i].animation != 0u; ++i) {}
    if (i == 64u || reports >= 4096u)
        return;
    if (slots[i].animation == 0u) {
        slots[i].animation = animation;
    } else if (slots[i].heading_bits == heading.u &&
               slots[i].target_x_bits == target_x.u &&
               slots[i].target_z_bits == target_z.u &&
               slots[i].touched == guest_u8(animation + 0x2981u)) {
        return;
    }
    slots[i].heading_bits = heading.u;
    slots[i].target_x_bits = target_x.u;
    slots[i].target_z_bits = target_z.u;
    slots[i].touched = guest_u8(animation + 0x2981u);
    ++reports;
    fprintf(stderr,
            "[HUMAN-HEAD] anim=%08X dt=%g heading=%g target=%g,%g "
            "limits=%g/%g touched=%u hold=%u stamp=%08X/%08X "
            "rate=%g timeout=%g\n",
            animation, dt.f, heading.f, target_x.f, target_z.f,
            guest_f32(animation + 0x2924u),
            guest_f32(animation + 0x2928u),
            guest_u8(animation + 0x2981u),
            guest_u8(animation + 0x2982u),
            guest_u32(animation + 0x2914u),
            guest_u32(animation + 0x2918u),
            guest_f32(0x0030D8C8u), guest_f32(0x0030D8CCu));
    fflush(stderr);
}
static uint32_t roadblock_trace_model;
static uint32_t roadblock_trace_hash = 0x89D23BAEu;
static int recomp_roadblock_model_trace_enabled(void)
{
    /* Launch-only diagnostic, owned by the serialized guest/render thread.
     * Presence (including an empty value) enables it. Avoid CRT environment
     * scans/locking at every model, queue item and primitive when disabled. */
    static int enabled = -1;
    if (enabled < 0) {
        const char *option = getenv("MERCENARIES_TRACE_ROADBLOCK_MODEL");
        enabled = option != NULL;
        /* Preserve the old empty/1 switch. An explicit hexadecimal asset
         * hash targets another model without adding another hot-path gate. */
        if (option && option[0] == '0' &&
            (option[1] == 'x' || option[1] == 'X')) {
            char *end = NULL;
            const unsigned long hash = strtoul(option + 2, &end, 16);
            if (strlen(option + 2) <= 8u && end != option + 2 &&
                *end == '\0' && hash != 0 &&
                hash <= 0xFFFFFFFFul)
                roadblock_trace_hash = (uint32_t)hash;
            else
                enabled = 0;
        }
    }
    return enabled;
}
void recomp_redmodel_render_checkpoint(uint32_t stage, uint32_t object,
                                       uint32_t value0, uint32_t value1)
{

    static uint32_t samples;

    if (!recomp_roadblock_model_trace_enabled() ||
        samples >= 1024u)
        return;
    if (stage <= 1u) {
        if (value0 != roadblock_trace_hash)
            return;
        if (stage == 1u)
            roadblock_trace_model = value1;
    } else if (object != roadblock_trace_model ||
               roadblock_trace_model < 0x00010000u ||
               roadblock_trace_model > 0x03FFFF00u) {
        return;
    }
    if (stage >= 6u && stage <= 10u) {
        const uint32_t segment = value0;
        fprintf(stderr,
                "[ROADBLOCK-MODEL] sample=%u stage=%u model=%08X "
                "segment=%08X arg=%08X lod=%02X texture=%08X "
                "state=%08X flags=%04X bone=%u attr-flags=%02X "
                "attr-color=%08X\n",
                samples++, stage, object, segment, value1,
                guest_u8(segment + 0x54u), guest_u32(segment + 0x30u),
                guest_u32(segment + 0x20u), guest_u16(segment + 0x46u),
                guest_u16(segment + 0x44u),
                stage == 8u ? guest_u8(value1 + 0xCu) : 0u,
                stage == 8u ? guest_u32(value1 + 8u) : 0u);
    } else {
        fprintf(stderr,
                "[ROADBLOCK-MODEL] sample=%u stage=%u object=%08X "
                "value0=%08X value1=%08X\n",
                samples++, stage, object, value0, value1);
    }
    fflush(stderr);
}
/* The retail Jennifer body has a separate 30-vertex alpha-blended fringe.
 * Its skinned shader group runs after vehicle glass, unlike the separate hair
 * mesh. Draw this verified primitive before transparent glass, keeping its
 * shader, alpha, depth and lighting state. Do not reorder the shader schedule:
 * in particular the bump shader's EndAlwaysCalled must retain its position. */
static int recomp_is_jennifer_fringe(uint32_t item)
{
    if (item<0x10000u || item>0x04000000u-0x94u) return 0;
    uint32_t primitive=guest_u32(item+0x60u), texture=guest_u32(item+0x70u);
    if (primitive<0x10000u || primitive>0x04000000u-0x58u ||
        texture<0x10000u || texture>0x04000000u-0x28u) return 0;
    return guest_u32(texture+0x24u)==0xEF72CE17u && /* prokat_british_head */
        guest_u32(primitive+0x30u)==0xEF72CE17u &&
        guest_u32(primitive+0x14u)==30u &&
        (guest_u32(item+0x6Cu)&0xFFu)==1u;
}
void recomp_render_hair_before_glass(void)
{
    if (getenv("MERCENARIES_TEST_ISOLATE_INPUT") &&
        getenv("MERCENARIES_TEST_HAIR_ORIGINAL_ORDER")) return;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    unsigned rendered=0;
    for(unsigned shader_id=20;shader_id<=23;++shader_id) {
        uint32_t shader=guest_u32(0x8429B0u+shader_id*4u);
        if(shader<0x10000u || shader>0x03fffef4u || (guest_u32(shader+0x108u)&4u))continue;
        uint32_t vt=guest_u32(shader);
        if(vt<0x10000u || vt>0x03ffffe8u)continue;
        /* Only the retail skinned lit family has these no-op end methods. */
        if(guest_u32(vt+0x10u)!=0x209040u || guest_u32(vt+0x14u)!=0x209040u)continue;
        recomp_func_t begin=recomp_lookup(guest_u32(vt+8u));
        recomp_func_t draw=recomp_lookup(guest_u32(vt+12u));
        recomp_func_t end=recomp_lookup(guest_u32(vt+16u));
        if(!begin || !draw || !end)continue;
        uint32_t link=0x7ACDD8u+(2u*38u+shader_id)*4u;
        unsigned begun=0,visited=0;
        while(visited++<40000u) {
            uint32_t item=guest_u32(link);
            if(!item || item<0x10000u || item>0x04000000u-0x94u)break;
            uint32_t next=guest_u32(item+0x90u);
            if(next==item)break;
            if(!recomp_is_jennifer_fringe(item)){link=item+0x90u;continue;}
            if(!begun){g_ecx=shader;recomp_guest_push_u32(0);begin();begun=1;}
            *(uint32_t*)guest_ptr(link)=next;
            g_ecx=shader;recomp_guest_push_u32(item);recomp_guest_push_u32(0);draw();
            ++rendered;
        }
        if(begun){g_ecx=shader;recomp_guest_push_u32(0);end();}
    }
    recomp_restore_guest_cpu_context(&saved);
    if(rendered && getenv("MERCENARIES_TRACE_HAIR_ORDER")) {
        static unsigned reports;
        if(reports++<8u)fprintf(stderr,"[HAIR-ORDER] drew %u fringe item(s) before vehicle glass\n",rendered);
    }
}

void recomp_redmodel_queue_checkpoint(
    uint32_t stage, uint32_t phase, uint32_t shader_id,
    uint32_t item, uint32_t shader_object, uint32_t target,
    uint32_t expected_esi, uint32_t current_esi,
    uint32_t expected_edi, uint32_t current_edi,
    uint32_t expected_esp, uint32_t current_esp)
{
    static uint32_t samples;
    static uint32_t abi_reports;
    uint32_t segments;
    uint32_t count;
    uint32_t primitive;

    if (stage == 1u &&
        (expected_esi != current_esi || expected_edi != current_edi ||
         expected_esp != current_esp) && abi_reports++ < 16u) {
        fprintf(stderr,
                "[RENDER-QUEUE-ABI] phase=%u shader=%u target=%08X "
                "esi=%08X/%08X edi=%08X/%08X esp=%08X/%08X\n",
                phase, shader_id, target, current_esi, expected_esi,
                current_edi, expected_edi, current_esp, expected_esp);
    }
    if (!recomp_roadblock_model_trace_enabled() ||
        samples >= 512u || roadblock_trace_model < 0x00010000u ||
        roadblock_trace_model > 0x03FFFF00u ||
        item < 0x00010000u || item > 0x03FFFF00u)
        return;
    segments = guest_u32(roadblock_trace_model + 0x80u);
    count = guest_u16(roadblock_trace_model + 0x78u);
    primitive = guest_u32(item + 0x60u);
    if (segments < 0x00010000u || count == 0u || count > 0x1000u ||
        primitive < segments || primitive >= segments + count * 0x58u ||
        (primitive - segments) % 0x58u != 0u)
        return;
    fprintf(stderr,
            "[ROADBLOCK-QUEUE] sample=%u stage=%u phase=%u shader=%u "
            "item=%08X shader-object=%08X target=%08X primitive=%08X "
            "flags=%04X bones=%u matrices=%08X state=%08X "
            "texture=%08X color=%08X next=%08X\n",
            samples++, stage, phase, shader_id, item, shader_object, target,
            primitive, guest_u16(item + 0x64u), guest_u16(item + 0x66u),
            guest_u32(item + 0x68u), guest_u32(item + 0x6Cu),
            guest_u32(item + 0x70u), guest_u32(item + 0x78u),
            guest_u32(item + 0x90u));
    fflush(stderr);
}
void recomp_redprimitive_draw_checkpoint(uint32_t stage,
                                         uint32_t primitive)
{
    static uint32_t samples;
    uint32_t segments;
    uint32_t count;

    if (!recomp_roadblock_model_trace_enabled() ||
        samples >= 1024u || roadblock_trace_model < 0x00010000u ||
        roadblock_trace_model > 0x03FFFF00u ||
        primitive < 0x00010000u || primitive > 0x03FFFF00u)
        return;
    segments = guest_u32(roadblock_trace_model + 0x80u);
    count = guest_u16(roadblock_trace_model + 0x78u);
    if (segments < 0x00010000u || count == 0u || count > 0x1000u ||
        primitive < segments || primitive >= segments + count * 0x58u ||
        (primitive - segments) % 0x58u != 0u)
        return;
    g_mercenaries_roadblock_model_draw_active = 1u;
    fprintf(stderr,
            "[ROADBLOCK-PRIMITIVE] sample=%u stage=%u primitive=%08X "
            "indices=%08X stream=%08X type=%08X start=%u count=%u\n",
            samples++, stage, primitive, guest_u32(primitive),
            guest_u32(primitive + 4u), guest_u32(primitive + 8u),
            guest_u32(primitive + 0xCu), guest_u32(primitive + 0x10u));
    fflush(stderr);
}
void recomp_player_update_lifetime_checkpoint(uint32_t stage,
                                              uint32_t owner)
{
    static uint32_t reports;
    static uint32_t tracked_owner;
    static uint32_t previous_vtable;
    static uint32_t previous_actor;
    uint32_t vtable;
    uint32_t actor;
    uint32_t actor_vtable = 0u;

    if (getenv("MERCENARIES_TRACE_PLAYER_UPDATE_LIFETIME") == NULL ||
        reports >= 512u || owner < 0x00010000u || owner > 0x03FFF000u)
        return;
    vtable = guest_u32(owner);
    actor = guest_u32(owner + 0x998u);
    if (actor >= 0x00010000u && actor <= 0x03FFF000u)
        actor_vtable = guest_u32(actor);
    if (tracked_owner != owner || previous_vtable != vtable ||
        previous_actor != actor) {
        fprintf(stderr,
                "[PLAYER-UPDATE-LIFETIME] report=%u stage=%u owner=%08X "
                "vtable=%08X actor=%08X actor-vtable=%08X current=%08X "
                "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
                "esp=%08X\n",
                reports++, stage, owner, vtable, actor, actor_vtable,
                g_recomp_current_func, g_eax, g_ebx, g_ecx, g_edx,
                g_esi, g_edi, g_esp);
        fflush(stderr);
    }
    tracked_owner = owner;
    previous_vtable = vtable;
    previous_actor = actor;
}
void recomp_notification_owner_watch_checkpoint(uint32_t xbox_va)
{
    uint32_t value;
    uint32_t i;

    if (!g_recomp_notification_owner_watch_enabled)
        return;
    if (g_notification_owner_auto_enabled) {
        for (i = 0u; i < g_notification_owner_auto_count; ++i) {
            uint32_t address = g_notification_owner_auto_addresses[i];
            uint32_t expected = g_notification_owner_auto_expected[i];
            uint32_t entry;
            uint32_t parent;
            if (address == 0u || expected == 0u)
                continue;
            entry = address - 0x1Cu;
            parent = entry - 0x18u;
            if (guest_u32(entry) != 0x002FC5A0u ||
                guest_u32(parent) != 0x002E2CE0u) {
                g_notification_owner_auto_expected[i] = 0u;
                continue;
            }
            value = guest_u32(address);
            if (value != expected) {
                fprintf(stderr,
                        "[NOTIFICATION-OWNER-WATCH] changed-auto slot=%u "
                        "address=%08X expected=%08X value=%08X "
                        "previous=%08X current=%08X eax=%08X ebx=%08X "
                        "ecx=%08X edx=%08X esi=%08X edi=%08X "
                        "ebp=%08X esp=%08X\n",
                        i, address, expected, value,
                        g_notification_owner_auto_previous_func[i], xbox_va,
                        g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi,
                        g_seh_ebp, g_esp);
                fflush(stderr);
                g_recomp_notification_owner_watch_enabled = 0u;
                return;
            }
            g_notification_owner_auto_previous_func[i] = xbox_va;
        }
        return;
    }
    if (g_notification_owner_watch_address < 0x00010000u ||
        g_notification_owner_watch_address > 0x03FFFFFCu)
        return;
    value = guest_u32(g_notification_owner_watch_address);
    if (!g_notification_owner_watch_armed) {
        if (value == g_notification_owner_watch_expected) {
            g_notification_owner_watch_armed = 1;
            g_notification_owner_watch_previous_func = xbox_va;
            fprintf(stderr,
                    "[NOTIFICATION-OWNER-WATCH] armed address=%08X "
                    "expected=%08X current=%08X\n",
                    g_notification_owner_watch_address,
                    g_notification_owner_watch_expected, xbox_va);
            fflush(stderr);
        }
        return;
    }
    if (value != g_notification_owner_watch_expected) {
        fprintf(stderr,
                "[NOTIFICATION-OWNER-WATCH] changed address=%08X "
                "expected=%08X value=%08X previous=%08X current=%08X "
                "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X "
                "edi=%08X ebp=%08X esp=%08X\n",
                g_notification_owner_watch_address,
                g_notification_owner_watch_expected, value,
                g_notification_owner_watch_previous_func, xbox_va,
                g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi,
                g_seh_ebp, g_esp);
        fflush(stderr);
        g_recomp_notification_owner_watch_enabled = 0u;
        return;
    }
    g_notification_owner_watch_previous_func = xbox_va;
}
void recomp_notification_list_checkpoint(uint32_t stage, uint32_t cursor,
                                         uint32_t entry, uint32_t target)
{
    typedef struct {
        uint32_t sequence, stage, cursor, next, previous;
        uint32_t next_previous, previous_next, cursor_entry, entry;
        uint32_t flags, target, current, eax, ecx, edx, esi, edi, esp;
    } event_t;
    static event_t history[128];
    static uint32_t sequence;
    static uint32_t dumped;
    event_t *event;
    uint32_t next = 0u;
    uint32_t previous = 0u;
    uint32_t next_previous = 0u;
    uint32_t previous_next = 0u;
    uint32_t cursor_entry = 0u;
    uint32_t flags = 0u;
    uint32_t cursor_valid;
    uint32_t entry_valid;
    uint32_t cursor_entry_valid;
    uint32_t links_valid = 0u;
    uint32_t first;
    uint32_t i;
    static uint32_t observed_targets[256];
    static uint32_t observed_count;
    static int entries_enabled = -1;
    static int owner_watch_initialized;

    recomp_target_manager_watch_validate();

    if (!owner_watch_initialized) {
        const char *owner_watch =
            getenv("MERCENARIES_WATCH_NOTIFICATION_OWNER");
        unsigned address = 0u, expected = 0u;
        owner_watch_initialized = 1;
        if (owner_watch != NULL && strcmp(owner_watch, "dispatch") == 0) {
            g_notification_owner_dispatch_watch_enabled = 1;
            g_recomp_entry_trace_enabled = 1u;
        } else if (owner_watch != NULL && strcmp(owner_watch, "auto") == 0) {
            g_notification_owner_auto_enabled = 1;
            g_recomp_notification_owner_watch_enabled = 1u;
        } else if (owner_watch != NULL &&
            sscanf(owner_watch, "%x:%x", &address, &expected) == 2 &&
            address >= 0x00010000u && address <= 0x03FFFFFCu &&
            expected >= 0x00010000u && expected <= 0x03FFFFFCu) {
            g_notification_owner_watch_address = address;
            g_notification_owner_watch_expected = expected;
            g_recomp_notification_owner_watch_enabled = 1u;
        }
    }
    if (g_notification_owner_dispatch_watch_enabled && stage == 1u &&
        entry >= 0x00010018u && entry <= 0x03FFFFC0u &&
        guest_u32(entry) == 0x002FC5A0u &&
        guest_u32(entry - 0x18u) == 0x002E2CE0u &&
        guest_u32(entry + 0x1Cu) != entry - 0x18u) {
        const uint32_t parent = entry - 0x18u;
        const uint32_t owner = guest_u32(entry + 0x1Cu);
        const uint32_t end = g_recomp_recent_game_func_idx;
        const uint32_t count = end < 128u ? end : 128u;
        fprintf(stderr,
                "[NOTIFICATION-OWNER-CORRUPT] entry=%08X parent=%08X "
                "owner=%08X delta=%d target=%08X current=%08X "
                "eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X "
                "edi=%08X ebp=%08X esp=%08X\n",
                entry, parent, owner, (int32_t)(owner - parent), target,
                g_recomp_current_func, g_eax, g_ebx, g_ecx, g_edx,
                g_esi, g_edi, g_seh_ebp, g_esp);
        fprintf(stderr, "[NOTIFICATION-OWNER-CORRUPT-RECENT] count=%u\n",
                count);
        for (i = 0u; i < count; ++i) {
            const uint32_t index = (end - count + i) & 255u;
            fprintf(stderr, "  [%03u] %08X\n", i,
                    g_recomp_recent_game_funcs[index]);
        }
        fflush(stderr);
        g_notification_owner_dispatch_watch_enabled = 0;
    }
    if (g_notification_owner_auto_enabled && stage == 1u &&
        entry >= 0x00010018u && entry <= 0x03FFFFC0u &&
        guest_u32(entry) == 0x002FC5A0u &&
        guest_u32(entry - 0x18u) == 0x002E2CE0u &&
        guest_u32(entry + 0x1Cu) == entry - 0x18u) {
        uint32_t address = entry + 0x1Cu;
        uint32_t observed;
        uint32_t free_slot = NOTIFICATION_OWNER_AUTO_CAPACITY;
        for (observed = 0u; observed < g_notification_owner_auto_count;
             ++observed) {
            if (g_notification_owner_auto_addresses[observed] == address)
                break;
            if (free_slot == NOTIFICATION_OWNER_AUTO_CAPACITY &&
                g_notification_owner_auto_expected[observed] == 0u)
                free_slot = observed;
        }
        if (observed == g_notification_owner_auto_count) {
            if (free_slot < NOTIFICATION_OWNER_AUTO_CAPACITY) {
                observed = free_slot;
            } else if (g_notification_owner_auto_count <
                       NOTIFICATION_OWNER_AUTO_CAPACITY) {
                observed = g_notification_owner_auto_count++;
            }
        }
        if (observed < NOTIFICATION_OWNER_AUTO_CAPACITY &&
            g_notification_owner_auto_expected[observed] == 0u) {
            g_notification_owner_auto_addresses[observed] = address;
            g_notification_owner_auto_expected[observed] = entry - 0x18u;
            g_notification_owner_auto_previous_func[observed] =
                g_recomp_current_func;
            fprintf(stderr,
                    "[NOTIFICATION-OWNER-WATCH] auto-armed slot=%u "
                    "address=%08X expected=%08X current=%08X\n",
                    observed, address, entry - 0x18u,
                    g_recomp_current_func);
            fflush(stderr);
        }
    }
    if (entries_enabled < 0)
        entries_enabled = getenv("MERCENARIES_TRACE_NOTIFICATION_ENTRIES") != NULL;
    if (entries_enabled && stage == 1u &&
        entry >= 0x00010000u && entry <= 0x03FFFFD0u &&
        target >= 0x00010000u && target <= 0x002AFFFFu) {
        uint32_t observed;
        for (observed = 0u; observed < observed_count; ++observed) {
            if (observed_targets[observed] == target)
                break;
        }
        if (observed == observed_count && observed_count < 256u) {
            observed_targets[observed_count] = target;
            fprintf(stderr,
                    "[NOTIFICATION-ENTRY] index=%u entry=%08X vtable=%08X "
                    "target=%08X flags=%02X words=%08X/%08X/%08X\n",
                    observed_count, entry, guest_u32(entry), target,
                    guest_u8(entry + 0x18u), guest_u32(entry + 0x1Cu),
                    guest_u32(entry + 0x20u), guest_u32(entry + 0x24u));
            observed_count++;
            fflush(stderr);
        }
    }

    if (getenv("MERCENARIES_TRACE_NOTIFICATION_LIST") == NULL || dumped)
        return;
    cursor_valid = cursor >= 0x00010000u && cursor <= 0x03FFFFF0u;
    if (cursor_valid) {
        next = guest_u32(cursor);
        previous = guest_u32(cursor + 4u);
        cursor_entry = guest_u32(cursor + 8u);
        if (next >= 0x00010000u && next <= 0x03FFFFF0u &&
            previous >= 0x00010000u && previous <= 0x03FFFFF0u &&
            (next & 3u) == 0u && (previous & 3u) == 0u) {
            next_previous = guest_u32(next + 4u);
            previous_next = guest_u32(previous);
            links_valid = next_previous == cursor &&
                          previous_next == cursor;
        }
    }
    entry_valid = entry >= 0x00010000u && entry <= 0x03FFFFE0u;
    cursor_entry_valid =
        cursor_entry >= 0x00010000u && cursor_entry <= 0x03FFFFE0u;
    if (entry_valid)
        flags = guest_u8(entry + 0x18u);
    event = &history[sequence & 127u];
    event->sequence = sequence++;
    event->stage = stage;
    event->cursor = cursor;
    event->next = next;
    event->previous = previous;
    event->next_previous = next_previous;
    event->previous_next = previous_next;
    event->cursor_entry = cursor_entry;
    event->entry = entry;
    event->flags = flags;
    event->target = target;
    event->current = g_recomp_current_func;
    event->eax = g_eax;
    event->ecx = g_ecx;
    event->edx = g_edx;
    event->esi = g_esi;
    event->edi = g_edi;
    event->esp = g_esp;
    if (cursor_valid && links_valid && (entry == 0u || entry_valid) &&
        (cursor_entry == 0u || cursor_entry_valid) &&
        target != 0x002376FCu)
        return;

    dumped = 1u;
    first = sequence > 128u ? sequence - 128u : 0u;
    fprintf(stderr,
            "[NOTIFICATION-LIST-FAULT] sequence=%u stage=%u cursor=%08X "
            "entry=%08X cursor-entry=%08X target=%08X reason=%s\n",
            sequence - 1u, stage, cursor, entry, cursor_entry, target,
            target == 0x002376FCu ? "purecall" :
            !cursor_valid ? "invalid-pointer" :
            !links_valid ? "broken-links" : "invalid-entry");
    for (i = first; i < sequence; ++i) {
        event = &history[i & 127u];
        fprintf(stderr,
                "[NOTIFICATION-LIST] sequence=%u stage=%u cursor=%08X "
                "next=%08X prev=%08X next-prev=%08X prev-next=%08X "
                "cursor-entry=%08X entry=%08X flags=%02X "
                "target=%08X current=%08X eax=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X esp=%08X\n",
                event->sequence, event->stage, event->cursor, event->next,
                event->previous, event->next_previous,
                event->previous_next, event->cursor_entry, event->entry,
                event->flags, event->target, event->current, event->eax,
                event->ecx, event->edx, event->esi, event->edi, event->esp);
    }
    fflush(stderr);
}
uint32_t recomp_notification_cursor_sanitize(uint32_t cursor)
{
    static uint32_t reports;
    const uint32_t range_valid =
        cursor >= 0x00010000u && cursor <= 0x03FFFFF0u;
    const uint32_t aligned = (cursor & 3u) == 0u;
    uint32_t next = 0u;
    uint32_t previous = 0u;
    uint32_t entry = 0u;
    uint32_t next_previous = 0u;
    uint32_t previous_next = 0u;
    uint32_t links_valid = 0u;
    uint32_t entry_valid = 0u;

    if (range_valid && aligned) {
        next = guest_u32(cursor);
        previous = guest_u32(cursor + 4u);
        entry = guest_u32(cursor + 8u);
        if (next >= 0x00010000u && next <= 0x03FFFFF0u &&
            previous >= 0x00010000u && previous <= 0x03FFFFF0u &&
            (next & 3u) == 0u && (previous & 3u) == 0u) {
            next_previous = guest_u32(next + 4u);
            previous_next = guest_u32(previous);
        }
        links_valid =
            next >= 0x00010000u && next <= 0x03FFFFF0u &&
            previous >= 0x00010000u && previous <= 0x03FFFFF0u &&
            (next & 3u) == 0u && (previous & 3u) == 0u &&
            next_previous == cursor && previous_next == cursor;
        entry_valid =
            entry == 0u ||
            (entry >= 0x00010000u && entry <= 0x03FFFFE0u &&
             (entry & 3u) == 0u);
        if (links_valid && entry_valid)
            return cursor;
    }

    if (reports++ < 16u) {
        fprintf(stderr,
                "[NOTIFICATION-CURSOR-GUARD] cursor=%08X next=%08X "
                "prev=%08X next-prev=%08X prev-next=%08X entry=%08X "
                "current=%08X esp=%08X\n",
                cursor, next, previous, next_previous, previous_next, entry,
                g_recomp_current_func, g_esp);
        fflush(stderr);
    }
    return 0u;
}
uint32_t recomp_pbl_thread_next_after_update(uint32_t cursor,
                                             uint32_t saved_next)
{
    const uint32_t linked_cursor =
        recomp_notification_cursor_sanitize(cursor);

    /* Retail 0x1FA10D calls the update callback; 0x1FA11B then reads the
     * live successor before the deferred removal at 0x1FA127..0x1FA14C.
     * Nine mutation cases are compared with direct x86 execution by
     * test_notification_retail_oracle.py; see the dated evidence document.
     * Reading the live link here is essential: Update() may deactivate some
     * other thread, including the successor that existed before dispatch.
     * Retain the saved successor only as a defensive fallback for a callback
     * that unexpectedly unlinked the current thread itself. */
    if (linked_cursor != 0u)
        return guest_u32(linked_cursor);
    return recomp_notification_cursor_sanitize(saved_next);
}
void recomp_vehicle_reward_checkpoint(uint32_t stage, uint32_t actor,
                                      uint32_t source, uint32_t credited,
                                      uint32_t cash_bits,
                                      uint32_t money_bits)
{
    union { uint32_t bits; float value; } cash, money, before, delta;
    static uint32_t samples;
    static uint32_t pending_player;
    static uint32_t pending_cash_bits;
    static uint32_t pending_money_bits;
    uint32_t vtable = 0u;
    int payout_matches = 0;

    if (getenv("MERCENARIES_TRACE_VEHICLE_REWARD") == NULL ||
        samples >= 2048u)
        return;
    if (actor >= 0x00010000u && actor <= 0x03FFFFF0u)
        vtable = guest_u32(actor);
    if (stage == 3u) {
        pending_player = source;
        pending_cash_bits = cash_bits;
        pending_money_bits = money_bits;
    } else if (stage == 4u) {
        /* Kill can bypass every reward branch, leaving EBP as unrelated
         * scratch state at the common exit. Only report a player/balance when
         * stage 3 actually reached the retail SetMoney call. */
        source = pending_player;
        cash_bits = pending_cash_bits;
        if (pending_player == 0u)
            money_bits = 0u;
    }
    if (stage >= 2u && source >= 0x00010000u &&
        source <= 0x03FFF000u && money_bits == 0u)
        money_bits = guest_u32(source + 0xAC4u);
    cash.bits = cash_bits;
    money.bits = money_bits;
    before.bits = stage == 4u ? pending_money_bits : money_bits;
    delta.value = stage == 4u ? money.value - before.value : 0.0f;
    if (stage == 4u && pending_player != 0u && isfinite(delta.value) &&
        isfinite(cash.value))
        payout_matches = fabsf(delta.value - cash.value) <= 0.01f;
    fprintf(stderr,
            "[VEHICLE-REWARD] sample=%u stage=%u actor=%08X "
            "vtable=%08X source=%08X player-credit=%u cash=%.9g "
            "cash-bits=%08X money-before=%.9g money=%.9g "
            "money-bits=%08X delta=%.9g payout-matches=%u current=%08X\n",
            samples++, stage, actor, vtable, source, credited != 0u,
            cash.value, cash.bits, before.value, money.value, money.bits,
            delta.value, payout_matches,
            g_recomp_current_func);
    if (stage == 4u) {
        pending_player = 0u;
        pending_cash_bits = 0u;
        pending_money_bits = 0u;
    }
    fflush(stderr);
}
void recomp_vehicle_audio_update_checkpoint(uint32_t effect)
{
    typedef struct VehicleAudioTraceSlot {
        uint32_t effect;
        uint32_t detail_set;
        uint32_t lod;
        uint32_t ignition;
        uint32_t engine_handle;
        uint32_t brake_handle;
        uint32_t road_handle;
        uint32_t accel_handle;
        uint32_t reverse_handle;
        uint32_t skid_handle;
        uint32_t slide_handle;
        ULONGLONG last_report_ms;
    } VehicleAudioTraceSlot;
    static int enabled = -1;
    static VehicleAudioTraceSlot slots[32];
    static uint32_t next_slot;
    VehicleAudioTraceSlot *slot = NULL;
    ULONGLONG now;
    uint32_t detail_set, lod, ignition, engine_handle, brake_handle;
    uint32_t road_handle, accel_handle, reverse_handle, skid_handle;
    uint32_t slide_handle;
    uint32_t i;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_VEHICLE_AUDIO_STATE") != NULL;
    if (!enabled || effect < 0x00010000u || effect > 0x03FFFE00u)
        return;

    for (i = 0; i < 32u; ++i) {
        if (slots[i].effect == effect) {
            slot = &slots[i];
            break;
        }
    }
    if (slot == NULL) {
        slot = &slots[next_slot++ % 32u];
        memset(slot, 0, sizeof(*slot));
        slot->effect = effect;
    }

    detail_set = guest_u32(effect + 0x08u);
    lod = guest_u32(effect + 0x10u);
    ignition = guest_u8(effect + 0x140u);
    /* Retail RsSoundEffectCar2 layout, cross-checked against
     * RsSoundEffectVehicle.h and the accesses in
     * sub_0014F720. Keep this observer exact: the old labels accidentally
     * reported EngineAccel as Engine and Skid as Brake, which made valid
     * high-detail vehicle audio look absent. */
    engine_handle = guest_u32(effect + 0xE8u);
    brake_handle = guest_u32(effect + 0xECu);
    road_handle = guest_u32(effect + 0xE0u);
    accel_handle = guest_u32(effect + 0xF0u);
    reverse_handle = guest_u32(effect + 0xF4u);
    skid_handle = guest_u32(effect + 0xFCu);
    slide_handle = guest_u32(effect + 0x100u);
    now = GetTickCount64();
    if (slot->detail_set == detail_set && slot->lod == lod &&
        slot->ignition == ignition && slot->engine_handle == engine_handle &&
        slot->brake_handle == brake_handle &&
        slot->road_handle == road_handle &&
        slot->accel_handle == accel_handle &&
        slot->reverse_handle == reverse_handle &&
        slot->skid_handle == skid_handle &&
        slot->slide_handle == slide_handle &&
        now - slot->last_report_ms < 1000u)
        return;

    fprintf(stderr,
            "[VEHICLE-AUDIO-STATE] effect=%08X effect-set=%08X "
            "detail-set=%08X lod=%u ignition=%u car=%08X "
            "wheel-speed=%.7g throttle=%.7g prior-throttle=%.7g "
            "rpm=%.7g throttle-param=%.7g reverse-param=%.7g gear=%d "
            "engine=%08X brake=%08X road=%08X accel=%08X "
            "reverse=%08X skid=%08X slide=%08X current=%08X\n",
            effect, guest_u32(effect + 0x04u), detail_set, lod, ignition,
            guest_u32(effect + 0x144u), guest_f32(effect + 0xB4u),
            guest_f32(effect + 0xBCu), guest_f32(effect + 0xC0u),
            guest_f32(effect + 0xCCu), guest_f32(effect + 0xD0u),
            guest_f32(effect + 0xD4u), (int32_t)guest_u32(effect + 0xD8u),
            engine_handle, brake_handle, road_handle, accel_handle,
            reverse_handle, skid_handle, slide_handle,
            g_recomp_current_func);
    fflush(stderr);
    slot->detail_set = detail_set;
    slot->lod = lod;
    slot->ignition = ignition;
    slot->engine_handle = engine_handle;
    slot->brake_handle = brake_handle;
    slot->road_handle = road_handle;
    slot->accel_handle = accel_handle;
    slot->reverse_handle = reverse_handle;
    slot->skid_handle = skid_handle;
    slot->slide_handle = slide_handle;
    slot->last_report_ms = now;
}
void recomp_find_culprit_checkpoint(uint32_t victim, uint32_t damage_type,
                                    uint32_t suspect_mask, uint32_t culprit,
                                    uint32_t best_value_bits)
{
    union { uint32_t bits; float value; } best;
    static uint32_t samples;

    if (getenv("MERCENARIES_TRACE_VEHICLE_REWARD") == NULL ||
        samples >= 2048u)
        return;
    best.bits = best_value_bits;
    fprintf(stderr,
            "[FIND-CULPRIT] sample=%u victim=%08X vtable=%08X "
            "pos=(%.7g,%.7g,%.7g) damage-type=%u suspect-mask=%08X "
            "culprit=%08X culprit-vtable=%08X best=%.9g\n",
            samples++, victim,
            victim >= 0x00010000u && victim <= 0x03FFFFF0u ?
                guest_u32(victim) : 0u,
            victim >= 0x00010000u && victim <= 0x03FFFF10u ?
                guest_f32(victim + 0xE0u) : 0.0f,
            victim >= 0x00010000u && victim <= 0x03FFFF10u ?
                guest_f32(victim + 0xE4u) : 0.0f,
            victim >= 0x00010000u && victim <= 0x03FFFF10u ?
                guest_f32(victim + 0xE8u) : 0.0f,
            damage_type, suspect_mask, culprit,
            culprit >= 0x00010000u && culprit <= 0x03FFFFF0u ?
                guest_u32(culprit) : 0u,
            best.value);
    fflush(stderr);
}
void recomp_camera_tilt_loop_checkpoint(uint32_t site,
                                        uint32_t expected_edi,
                                        uint32_t current_edi,
                                        uint32_t expected_esp,
                                        uint32_t current_esp)
{
    static uint32_t reports;

    if ((current_edi == expected_edi && current_esp == expected_esp) ||
        reports++ >= 16u)
        return;
    fprintf(stderr,
            "[CAMERA-TILT-LOOP-ABI] site=%08X expected_edi=%08X "
            "current_edi=%08X expected_esp=%08X current_esp=%08X\n",
            site, expected_edi, current_edi, expected_esp, current_esp);
    fflush(stderr);
}
void recomp_havok_constraint_esi_checkpoint(uint32_t constraint,
                                            uint32_t target,
                                            uint32_t expected,
                                            uint32_t current)
{
    static uint32_t reports;

    if (current == expected || reports++ >= 16u)
        return;
    fprintf(stderr,
            "[HAVOK-CONSTRAINT-ESI-CLOBBER] constraint=%08X target=%08X "
            "expected=%08X current=%08X eax=%08X ecx=%08X edx=%08X "
            "esp=%08X\n",
            constraint, target, expected, current, g_eax, g_ecx, g_edx,
            g_esp);
    fflush(stderr);
}
void recomp_havok_nested_abi_checkpoint(uint32_t stage, uint32_t target,
                                        uint32_t expected_esi,
                                        uint32_t current_esi,
                                        uint32_t saved_slot,
                                        uint32_t expected_slot,
                                        uint32_t current_slot,
                                        uint32_t expected_esp,
                                        uint32_t current_esp)
{
    static uint32_t reports;

    if ((current_esi == expected_esi && current_slot == expected_slot &&
         current_esp == expected_esp) ||
        reports++ >= 32u)
        return;
    fprintf(stderr,
            "[HAVOK-NESTED-ABI-CLOBBER] stage=%u target=%08X "
            "esi=%08X/%08X slot=%08X value=%08X/%08X esp=%08X/%08X\n",
            stage, target, current_esi, expected_esi, saved_slot,
            current_slot, expected_slot, current_esp, expected_esp);
    fflush(stderr);
}
void recomp_gate_ai_checkpoint(uint32_t stage, uint32_t gate_ai,
                               uint32_t gate_actor, uint32_t stimulus,
                               uint32_t stimulus_actor, float value0,
                               float value1)
{
    static ULONGLONG last_stage_ms[4];
    static uint32_t reports;
    const ULONGLONG now = GetTickCount64();
    float gate_x = 0.0f;
    float gate_z = 0.0f;

    if (getenv("MERCENARIES_TRACE_GATE_AI") == NULL || reports >= 2048u ||
        g_recomp_live_vehicle_actor < 0x00010000u ||
        g_recomp_live_vehicle_actor > 0x03FFF000u)
        return;
    if (gate_actor >= 0x00010000u && gate_actor <= 0x03FFF000u) {
        const char *max_x_text =
            getenv("MERCENARIES_TRACE_GATE_AI_MAX_X");
        const float vehicle_x = guest_f32(g_recomp_live_vehicle_actor + 0xE0u);
        const float vehicle_z = guest_f32(g_recomp_live_vehicle_actor + 0xE8u);
        float dx;
        float dz;
        gate_x = guest_f32(gate_actor + 0xE0u);
        gate_z = guest_f32(gate_actor + 0xE8u);
        if (max_x_text != NULL && max_x_text[0] != '\0' &&
            gate_x > strtof(max_x_text, NULL))
            return;
        dx = gate_x - vehicle_x;
        dz = gate_z - vehicle_z;
        if (dx * dx + dz * dz > 10000.0f)
            return;
    }
    if (stimulus_actor != 0u && g_recomp_live_vehicle_actor != 0u &&
        stimulus_actor != g_recomp_live_vehicle_actor)
        return;
    if (stage < 3u) {
        if (now - last_stage_ms[stage] < 500u)
            return;
        last_stage_ms[stage] = now;
    }
    ++reports;
    fprintf(stderr,
            "[GATE-AI] stage=%u ai=%08X gate=%08X gate-pos=(%.7g,%.7g) "
            "stim=%08X actor=%08X live-vehicle=%08X value=(%.7g,%.7g) "
            "delay=(%.7g,%.7g) timer=(%.7g,%.7g) radius=%.7g "
            "vehicles-only=%u\n",
            stage, gate_ai, gate_actor, gate_x, gate_z, stimulus,
            stimulus_actor, g_recomp_live_vehicle_actor, value0, value1,
            guest_f32(gate_ai + 0x580u), guest_f32(gate_ai + 0x584u),
            guest_f32(gate_ai + 0x588u), guest_f32(gate_ai + 0x58Cu),
            guest_f32(gate_ai + 0x590u), guest_u8(gate_ai + 0x594u));
    fflush(stderr);
}

void recomp_gate_motion_checkpoint(uint32_t stage, uint32_t gate_actor,
                                   uint32_t gate_index, uint32_t gate_prop,
                                   uint32_t physics, float value0,
                                   float value1, float value2)
{
    static ULONGLONG last_stage_ms[5];
    static uint32_t reports;
    const ULONGLONG now = GetTickCount64();
    uint32_t target;

    if (getenv("MERCENARIES_TRACE_GATE_MOTION") == NULL || stage >= 5u ||
        gate_actor < 0x00010000u || gate_actor > 0x03FFF000u ||
        gate_index >= 8u || reports >= 512u)
        return;
    if (g_recomp_live_vehicle_actor >= 0x00010000u &&
        g_recomp_live_vehicle_actor <= 0x03FFF000u) {
        const float dx = guest_f32(gate_actor + 0xE0u) -
                         guest_f32(g_recomp_live_vehicle_actor + 0xE0u);
        const float dz = guest_f32(gate_actor + 0xE8u) -
                         guest_f32(g_recomp_live_vehicle_actor + 0xE8u);
        if (dx * dx + dz * dz > 10000.0f)
            return;
    }
    if (now - last_stage_ms[stage] < 250u)
        return;
    last_stage_ms[stage] = now;
    ++reports;
    target = gate_actor + 0x770u + gate_index * 12u;
    fprintf(stderr,
            "[GATE-MOTION] stage=%u gate=%08X state=%u railed=%u index=%u "
            "guid=%08X locked=%u prop=%08X physics=%08X rigid=%08X "
            "current=(%.7g,%.7g,%.7g) target=(%.7g,%.7g,%.7g) "
            "open-dir=(%.7g,%.7g,%.7g) value=(%.7g,%.7g,%.7g)\n",
            stage, gate_actor, guest_u8(gate_actor + 0x6E0u),
            guest_u8(gate_actor + 0x7D8u), gate_index,
            guest_u32(gate_actor + 0x6F0u + gate_index * 4u),
            guest_u8(gate_actor + 0x7D0u + gate_index), gate_prop, physics,
            physics >= 0x00010000u && physics <= 0x03FFF000u ?
                guest_u32(physics + 0x1Cu) : 0u,
            gate_prop >= 0x00010000u && gate_prop <= 0x03FFF000u ?
                guest_f32(gate_prop + 0xE0u) : 0.0f,
            gate_prop >= 0x00010000u && gate_prop <= 0x03FFF000u ?
                guest_f32(gate_prop + 0xE4u) : 0.0f,
            gate_prop >= 0x00010000u && gate_prop <= 0x03FFF000u ?
                guest_f32(gate_prop + 0xE8u) : 0.0f,
            guest_f32(target), guest_f32(target + 4u),
            guest_f32(target + 8u),
            gate_prop >= 0x00010000u && gate_prop <= 0x03FFF000u ?
                guest_f32(gate_prop + 0x560u) : 0.0f,
            gate_prop >= 0x00010000u && gate_prop <= 0x03FFF000u ?
                guest_f32(gate_prop + 0x564u) : 0.0f,
            gate_prop >= 0x00010000u && gate_prop <= 0x03FFF000u ?
                guest_f32(gate_prop + 0x568u) : 0.0f,
            value0, value1, value2);
    fflush(stderr);
}

void recomp_tree_predicate_checkpoint(uint32_t stage, uint32_t object,
                                      uint32_t out, uint32_t left,
                                      uint32_t right)
{
    static uint32_t reports;

    if (object == 0x00C80B58u && reports++ >= 32u)
        return;
    fprintf(stderr,
            "[TREE-PREDICATE] stage=%u object=%08X out=%08X "
            "left=%08X right=%08X esp=%08X\n",
            stage, object, out, left, right, g_esp);
    fflush(stderr);
}
void recomp_tree_merge_checkpoint(uint32_t stage, uint32_t entry,
                                  uint32_t table, uint32_t index,
                                  uint32_t left, uint32_t right)
{
    typedef struct TreePairHistory {
        uint32_t address, owner, left, right;
    } TreePairHistory;
    static TreePairHistory history[4096];
    static uint32_t history_index;

    if (stage == 10u) {
        TreePairHistory *slot = &history[history_index++ & 4095u];
        slot->address = entry;
        slot->owner = table;
        slot->left = left;
        slot->right = right;
        return;
    }
    if (stage == 11u) {
        for (uint32_t i = 0; i < 4096u && i < history_index; ++i) {
            const TreePairHistory *slot =
                &history[(history_index - 1u - i) & 4095u];
            if (slot->address == entry) {
                fprintf(stderr,
                        "[TREE-PAIR-HISTORY] address=%08X owner=%08X "
                        "was=%08X/%08X now=%08X/%08X age=%u\n",
                        entry, slot->owner, slot->left, slot->right,
                        left, right, i);
                fflush(stderr);
                return;
            }
        }
        fprintf(stderr,
                "[TREE-PAIR-HISTORY] address=%08X owner=UNKNOWN "
                "now=%08X/%08X\n", entry, left, right);
        fflush(stderr);
        return;
    }
    fprintf(stderr,
            "[TREE-MERGE] stage=%u entry=%08X table=%08X index=%08X "
            "left=%08X fields=%08X,%08X,%08X "
            "right=%08X fields=%08X,%08X,%08X esp=%08X\n",
            stage, entry, table, index, left,
            guest_u32(left + 4u), guest_u32(left + 8u),
            guest_u32(left + 0x1Cu), right,
            guest_u32(right + 4u), guest_u32(right + 8u),
            guest_u32(right + 0x1Cu), g_esp);
    fflush(stderr);
}
void recomp_pair_sort_checkpoint(uint32_t base, uint32_t count,
                                 uint32_t stack)
{
    static int enabled = -1;
    static uint32_t reports;
    uint32_t i;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_BROADPHASE_INVARIANTS") != NULL;
    if (!enabled)
        return;
    if (base < 0x00010000u || base >= 0x04000000u || count > 0x10000u)
        return;
    for (i = 0; i < count; ++i) {
        const uint32_t left = guest_u32(base + i * 8u);
        const uint32_t right = guest_u32(base + i * 8u + 4u);
        const int left_bad = left < 0x00010000u || left >= 0x04000000u;
        const int right_bad = right < 0x00010000u || right >= 0x04000000u;
        if ((left_bad || right_bad) && reports++ < 16u) {
            const uint32_t first = i > 3u ? i - 3u : 0u;
            const uint32_t last = i + 1u < count ? i + 1u : count - 1u;
            fprintf(stderr,
                    "[PAIR-SORT-CORRUPTION] base=%08X count=%u index=%u "
                    "left=%08X right=%08X stack=%08X esp=%08X\n",
                    base, count, i, left, right, stack, g_esp);
            for (uint32_t j = first; j <= last; ++j) {
                const uint32_t a = guest_u32(base + j * 8u);
                const uint32_t b = guest_u32(base + j * 8u + 4u);
                fprintf(stderr,
                        "  pair[%u]=%08X(key=%08X)/%08X(key=%08X)\n",
                        j, a,
                        a >= 0x00010000u && a < 0x04000000u
                            ? guest_u32(a + 4u) : 0xFFFFFFFFu,
                        b,
                        b >= 0x00010000u && b < 0x04000000u
                            ? guest_u32(b + 4u) : 0xFFFFFFFFu);
            }
            {
                const uint32_t end = g_recomp_recent_func_idx;
                const uint32_t n = end < 24u ? end : 24u;
                for (uint32_t j = 0; j < n; ++j) {
                    const uint32_t ring = (end - n + j) & 63u;
                    fprintf(stderr, "  pair-source[%u]=%08X\n", j,
                            g_recomp_recent_funcs[ring]);
                }
            }
            fflush(stderr);
            return;
        }
    }
}
void recomp_pair_append_checkpoint(uint32_t site, uint32_t first,
                                   uint32_t second, uint32_t array,
                                   uint32_t stack, uint32_t frame)
{
    static int enabled = -1;
    static uint32_t reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_BROADPHASE_INVARIANTS") != NULL;
    if (!enabled)
        return;

    const int first_valid = first >= 0x00010000u && first < 0x04000000u;
    const int second_valid = second >= 0x00010000u && second < 0x04000000u;
    const uint32_t left = first_valid ? guest_u32(first + 0xCu) : 0xFFFFFFFFu;
    const uint32_t right = second_valid ? guest_u32(second + 0xCu) : 0xFFFFFFFFu;
    const int left_bad = left < 0x00010000u || left >= 0x04000000u;
    const int right_bad = right < 0x00010000u || right >= 0x04000000u;

    if (!(first_valid && second_valid) || left_bad || right_bad) {
        if (reports++ >= 16u)
            return;
        fprintf(stderr,
                "[PAIR-APPEND-CORRUPTION] site=%08X first=%08X->%08X "
                "second=%08X->%08X array=%08X data=%08X count=%u "
                "capacity=%08X stack=%08X frame=%08X esp=%08X\n",
                site, first, left, second, right, array,
                array >= 0x00010000u && array < 0x04000000u
                    ? guest_u32(array) : 0xFFFFFFFFu,
                array >= 0x00010000u && array < 0x04000000u
                    ? guest_u32(array + 4u) : 0xFFFFFFFFu,
                array >= 0x00010000u && array < 0x04000000u
                    ? guest_u32(array + 8u) : 0xFFFFFFFFu,
                stack, frame, g_esp);
        if (first_valid) {
            fprintf(stderr,
                    "  first-fields=%08X,%08X,%08X,%08X\n",
                    guest_u32(first), guest_u32(first + 4u),
                    guest_u32(first + 8u), guest_u32(first + 0xCu));
        }
        if (second_valid) {
            fprintf(stderr,
                    "  second-fields=%08X,%08X,%08X,%08X\n",
                    guest_u32(second), guest_u32(second + 4u),
                    guest_u32(second + 8u), guest_u32(second + 0xCu));
        }
        {
            const uint32_t context = guest_u32(stack + 0xCu);
            const uint32_t nodes = guest_u32(stack + 0x14u);
            const uint32_t z_endpoints = guest_u32(context + 0x64u);
            fprintf(stderr,
                    "  broadphase context=%08X nodes=%08X current-index=%u "
                    "other-index=%u z-endpoints=%08X size=%u capacity=%08X "
                    "new-max-z=%08X regs ebx=%08X ecx=%08X edx=%08X "
                    "edi=%08X\n",
                    context, nodes, guest_u32(stack + 0x10u),
                    nodes && second >= nodes ? (second - nodes) >> 4 : 0xFFFFFFFFu,
                    z_endpoints, guest_u32(context + 0x68u),
                    guest_u32(context + 0x6Cu), guest_u32(stack + 0x68u),
                    g_ebx, g_ecx, g_edx, g_edi);
            if (g_edi >= z_endpoints + 8u &&
                g_edi < z_endpoints + guest_u32(context + 0x68u) * 4u + 8u) {
                fprintf(stderr,
                        "  endpoints[-2..2]=%08X,%08X,%08X,%08X,%08X\n",
                        guest_u32(g_edi - 8u), guest_u32(g_edi - 4u),
                        guest_u32(g_edi), guest_u32(g_edi + 4u),
                        guest_u32(g_edi + 8u));
            }
        }
        fflush(stderr);
    }
}
void recomp_broadphase_invariant_checkpoint(uint32_t context,
                                            uint32_t object_cursor,
                                            uint32_t object_end,
                                            uint32_t aabb_cursor)
{
    static int enabled = -1;
    static int player_trace_enabled = -1;
    static int player_layout_dumped;
    static uint32_t iteration;
    static uint32_t reports;
    static uint32_t player_iteration;
    static uint32_t player_reports;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_BROADPHASE_INVARIANTS") != NULL;
    if (player_trace_enabled < 0)
        player_trace_enabled =
            getenv("MERCENARIES_TRACE_PLAYER_TRANSFORM") != NULL;
    if (!enabled && !player_trace_enabled)
        return;

    if (player_trace_enabled && !player_layout_dumped) {
        const uint32_t actor = g_recomp_live_human_actor;
        const int actor_valid =
            actor >= 0x00010000u && actor <= 0x03FFF800u;
        if (!actor_valid)
            return;
        player_layout_dumped = 1;
        fprintf(stderr,
                "[PLAYER-LAYOUT] actor=%08X vtable=%08X obstacle=%08X "
                "stored-matrix=%08X\n",
                actor, guest_u32(actor), guest_u32(actor + 0x76Cu),
                actor + 0xB0u);
        fflush(stderr);
    }

    if (player_trace_enabled && g_recomp_live_human_actor >= 0x00010000u &&
        g_recomp_live_human_actor <= 0x03FFF800u &&
        (player_iteration++ % 120u) == 0u && player_reports < 128u) {
        const uint32_t actor = g_recomp_live_human_actor;
        fprintf(stderr,
                "[PLAYER-TRANSFORM] sample=%u actor=%08X obstacle=%08X "
                "pos=(%.7g,%.7g,%.7g) basis0=(%.7g,%.7g,%.7g)\n",
                player_reports++, actor, guest_u32(actor + 0x76Cu),
                guest_f32(actor + 0xE0u), guest_f32(actor + 0xE4u),
                guest_f32(actor + 0xE8u), guest_f32(actor + 0xB0u),
                guest_f32(actor + 0xB4u), guest_f32(actor + 0xB8u));
        fflush(stderr);
    }    if (!enabled)
        return;

    const uint32_t nodes = guest_u32(context + 0x40u);
    const uint32_t node_count = guest_u32(context + 0x44u);
    const uint32_t axis[3] = {
        guest_u32(context + 0x4Cu),
        guest_u32(context + 0x58u),
        guest_u32(context + 0x64u)
    };
    const uint32_t axis_count[3] = {
        guest_u32(context + 0x50u),
        guest_u32(context + 0x5Cu),
        guest_u32(context + 0x68u)
    };
    const uint32_t min_offset[3] = { 8u, 0u, 2u };
    const uint32_t max_offset[3] = { 10u, 4u, 6u };
    const uint32_t this_iteration = iteration++;

    if (nodes < 0x00010000u || nodes >= 0x04000000u ||
        node_count > 0x10000u)
        return;
    /* Node zero is Havok's world guard and intentionally shares marker slots. */
    for (uint32_t i = 1u; i < node_count; ++i) {
        const uint32_t node = nodes + i * 16u;
        const uint32_t handle = guest_u32(node + 0xCu);
        if ((handle & 1u) != 0u)
            continue;
        for (uint32_t a = 0; a < 3u; ++a) {
            const uint32_t min_index = guest_u16(node + min_offset[a]);
            const uint32_t max_index = guest_u16(node + max_offset[a]);
            int invalid = axis[a] < 0x00010000u || axis[a] >= 0x04000000u ||
                          min_index >= axis_count[a] || max_index >= axis_count[a];
            uint32_t min_endpoint = 0xFFFFFFFFu;
            uint32_t max_endpoint = 0xFFFFFFFFu;
            if (!invalid) {
                min_endpoint = guest_u32(axis[a] + min_index * 4u);
                max_endpoint = guest_u32(axis[a] + max_index * 4u);
                invalid = (min_endpoint >> 16) != i ||
                          (max_endpoint >> 16) != i ||
                          (min_endpoint & 1u) != 0u ||
                          (max_endpoint & 1u) == 0u;
            }
            if (invalid && reports++ < 32u) {
                const uint32_t object = object_cursor < object_end
                    ? guest_u32(object_cursor) : 0xFFFFFFFFu;
                const uint32_t update_index = object >= 0x00010000u &&
                    object < 0x04000000u ? guest_u32(object) : 0xFFFFFFFFu;
                fprintf(stderr,
                        "[BROADPHASE-INVARIANT] iteration=%u context=%08X "
                        "cursor=%08X/%08X aabb=%08X object=%08X update-node=%u "
                        "bad-node=%u axis=%u nodes=%08X count=%u "
                        "axis-base=%08X axis-count=%u min=%u(%08X) "
                        "max=%u(%08X) fields=%08X,%08X,%08X,%08X\n",
                        this_iteration, context, object_cursor, object_end,
                        aabb_cursor, object, update_index, i, a, nodes,
                        node_count, axis[a], axis_count[a], min_index,
                        min_endpoint, max_index, max_endpoint,
                        guest_u32(node), guest_u32(node + 4u),
                        guest_u32(node + 8u), handle);
                fflush(stderr);
                return;
            }
        }
    }
}void recomp_render_list_checkpoint(uint32_t phase, uint32_t root,
                                   uint32_t node)
{
    static uint32_t active_root;
    static uint32_t previous_node;
    static uint32_t reports;

    if (phase == 0u) {
        active_root = root;
        previous_node = node;
        if (node == 0x00209040u) {
            fprintf(stderr,
                    "[RENDER-LIST-BAD-HEAD] root=%08X head=%08X "
                    "eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                    root, node, g_eax, g_ecx, g_edx, g_esi, g_edi, g_esp);
            fflush(stderr);
        }
        return;
    }
    if (phase == 2u && node != previous_node) {
        fprintf(stderr,
                "[RENDER-LIST-ESI-CLOBBER] root=%08X expected=%08X actual=%08X "
                "eax=%08X ecx=%08X edx=%08X edi=%08X esp=%08X\n",
                active_root, previous_node, node, g_eax, g_ecx, g_edx,
                g_edi, g_esp);
        fflush(stderr);
        previous_node = node;
        return;
    }
    if ((node == 0x00209040u || node < 0x00010000u ||
         node >= 0x04000000u) && reports++ < 32u) {
        fprintf(stderr,
                "[RENDER-LIST-CORRUPTION] root=%08X previous=%08X next=%08X "
                "eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                active_root, previous_node, node, g_eax, g_ecx, g_edx,
                g_esi, g_edi, g_esp);
        fflush(stderr);
    }
    previous_node = node;
}
void recomp_render_stack_checkpoint(uint32_t phase, uint32_t saved_esp,
                                    uint32_t current_esp,
                                    uint32_t saved_esi,
                                    uint32_t stack_value)
{
    if (current_esp != saved_esp || stack_value != saved_esi) {
        fprintf(stderr,
                "[RENDER-WALKER-STACK] phase=%u savedEsp=%08X currentEsp=%08X "
                "savedEsi=%08X stackValue=%08X eax=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X\n",
                phase, saved_esp, current_esp, saved_esi, stack_value,
                g_eax, g_ecx, g_edx, g_esi, g_edi);
        fflush(stderr);
    }
}
void recomp_render_icall_checkpoint(uint32_t phase, uint32_t target,
                                    uint32_t saved_esp,
                                    uint32_t current_esp)
{
    if (current_esp != saved_esp) {
        fprintf(stderr,
                "[RENDER-WALKER-ICALL] phase=%u target=%08X savedEsp=%08X "
                "currentEsp=%08X delta=%d eax=%08X ecx=%08X edx=%08X "
                "esi=%08X edi=%08X\n",
                phase, target, saved_esp, current_esp,
                (int32_t)(current_esp - saved_esp), g_eax, g_ecx, g_edx,
                g_esi, g_edi);
        fflush(stderr);
    }
}

void recomp_pda_store_render_checkpoint(uint32_t phase, uint32_t view,
                                        uint32_t category, uint32_t item,
                                        uint32_t row, uint32_t value,
                                        uint32_t stack)
{
    static uint32_t reports;
    if (getenv("MERCENARIES_TRACE_PDA_STORE") == NULL || reports++ >= 512u)
        return;
    fprintf(stderr,
            "[PDA-STORE-RENDER] phase=%u view=%08X category=%u item=%08X "
            "row=%u value=%08X esp=%08X eax=%08X ecx=%08X edx=%08X "
            "esi=%08X edi=%08X\n",
            phase, view, category, item, row, value, stack,
            g_eax, g_ecx, g_edx, g_esi, g_edi);
    fflush(stderr);
}

void recomp_render_owner_checkpoint(uint32_t phase, uint32_t object)
{
    static uint32_t expected;
    static uint32_t reports;

    if (phase == 0u)
        expected = object;
    if ((object != expected || object < 0x00300000u) && reports++ < 32u) {
        fprintf(stderr,
                "[RENDER-OWNER-CORRUPTION] phase=%u expected=%08X actual=%08X "
                "eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X esp=%08X\n",
                phase, expected, object, g_eax, g_ecx, g_edx, g_esi, g_edi,
                g_esp);
        fflush(stderr);
    }
}
void recomp_d3d_device_checkpoint(uint32_t phase, uint32_t expected,
                                  uint32_t actual, uint32_t stack)
{
    static uint32_t reports;
    static uint32_t path_reports;
    if (phase >= 90u && phase <= 103u && path_reports++ < 32u) {
        const uint32_t before_sleep = g_esi;
        uint32_t after_sleep = before_sleep;
        if (phase == 103u) {
            Sleep(1u);
            after_sleep = g_esi;
        }
        fprintf(stderr,
                "[D3D-DEVICE-PATH] tid=%lu phase=%u expected=%08X "
                "argument=%08X live=%08X afterSleep=%08X esp=%08X\n",
                (unsigned long)GetCurrentThreadId(), phase, expected, actual,
                before_sleep, after_sleep, stack);
        fflush(stderr);
    }    if (actual != expected && reports++ < 128u) {
        fprintf(stderr,
                "[D3D-DEVICE-ESI] phase=%u expected=%08X actual=%08X "
                "esp=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X edi=%08X\n",
                phase, expected, actual, stack, g_eax, g_ebx, g_ecx,
                g_edx, g_edi);
        fflush(stderr);
    }
}
void recomp_lua_rehash_checkpoint(uint32_t result, uint32_t table,
                                  uint32_t key)
{
    static uint32_t reports;
    uint32_t lsize;
    uint32_t node_count;

    if (result != 0x002FBD78u || reports++ >= 8u)
        return;

    lsize = guest_u8(table + 7u);
    node_count = lsize < 31u ? (1u << lsize) : 0u;
    fprintf(stderr,
            "[LUA-REHASH-MISS] result=%08X table=%08X key=%08X "
            "keyType=%u keyLo=%08X keyHi=%08X array=%08X sizeArray=%u "
            "node=%08X lsize=%u nodes=%u firstFree=%08X flags=%02X\n",
            result, table, key, guest_u32(key), guest_u32(key + 8u),
            guest_u32(key + 12u), guest_u32(table + 0x0Cu),
            guest_u32(table + 0x1Cu), guest_u32(table + 0x10u), lsize,
            node_count, guest_u32(table + 0x14u), guest_u8(table + 6u));
    if (node_count > 64u)
        node_count = 64u;
    for (uint32_t i = 0u; i < node_count; ++i) {
        const uint32_t node = guest_u32(table + 0x10u) + i * 40u;
        const uint32_t key_type = guest_u32(node);
        const uint32_t value_type = guest_u32(node + 0x10u);
        if (key_type != 0u || value_type != 0u) {
            fprintf(stderr,
                    "  node[%u]=%08X keyType=%u keyLo=%08X keyHi=%08X "
                    "valueType=%u valueLo=%08X next=%08X\n",
                    i, node, key_type, guest_u32(node + 8u),
                    guest_u32(node + 12u), value_type,
                    guest_u32(node + 0x18u), guest_u32(node + 0x20u));
        }
    }
    fflush(stderr);
}
void recomp_lua_buffer_grow_checkpoint(uint32_t buffer, uint32_t desired,
                                       uint32_t current)
{
    static int enabled = -1;
    uint32_t recent_index;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_LUA_CORRUPTION") != NULL;
    if (!enabled)
        return;
    if (desired < 0x00100000u)
        return;

    fprintf(stderr,
            "[LUA-BUFFER-GROW] buffer=%08X data=%08X capacity=%u (%08X) "
            "desired=%u (%08X) current=%u (%08X)\n",
            buffer, guest_u32(buffer), guest_u32(buffer + 4u),
            guest_u32(buffer + 4u), desired, desired, current, current);
    recent_index = g_recomp_recent_func_idx;
    for (uint32_t i = 0u; i < 20u && i < recent_index; ++i) {
        const uint32_t index = (recent_index - 1u - i) & 63u;
        fprintf(stderr, "  lua-grow-recent[-%u]=%08X\n", i,
                g_recomp_recent_funcs[index]);
    }
    fflush(stderr);
}

/*
 * The retail Xbox CRT heap is initialized over the first 1 MiB allocation at
 * XBOX_HEAP_BASE. Its lifted RtlAllocateHeap implementation is exceptionally
 * dependent on x86 flags and packed heap metadata, and eventually rejects a
 * valid 12-byte allocation during audio startup. Keep the retail heap object
 * and its first page intact, but manage the remainder as deterministic guest
 * address space derived from the shared runtime memory layout.
 */
#define MERC_HEAP_ARENA_START (XBOX_HEAP_BASE + 0x00001000u)
#define MERC_HEAP_ARENA_END   (XBOX_HEAP_BASE + 0x00100000u)
#define MERC_HEAP_INITIAL_BLOCKS 16384u
/* No more live 16-byte blocks can fit in the guest address space. */
#define MERC_HEAP_MAX_BLOCKS (0x04000000u / 16u)
#define MERC_HEAP_ZERO_MEMORY 0x00000008u
#define MERC_HEAP_REALLOC_IN_PLACE_ONLY 0x00000010u

typedef struct merc_heap_block {
    uint32_t address;
    uint32_t requested;
    uint32_t capacity;
    uint8_t in_use;
} merc_heap_block_t;

static merc_heap_block_t g_merc_heap_initial_blocks[MERC_HEAP_INITIAL_BLOCKS];
static merc_heap_block_t *g_merc_heap_blocks = g_merc_heap_initial_blocks;
static uint32_t g_merc_heap_block_capacity = MERC_HEAP_INITIAL_BLOCKS;
static uint32_t g_merc_heap_block_count;
static uint32_t g_merc_heap_handle;
static uint8_t g_merc_heap_initialized;
static uint8_t g_merc_heap_oom_reported;
static SRWLOCK g_merc_heap_lock = SRWLOCK_INIT;

/* Called under g_merc_heap_lock. The table is host bookkeeping, not guest
 * RAM. A full table must not turn available guest memory into a Lua OOM.
 * Keep the small static table until growth is needed, then double on demand.
 * Callers must reacquire any table pointers after this function succeeds. */
static int merc_heap_reserve_block(void)
{
    uint32_t capacity;
    merc_heap_block_t *blocks;
    if (g_merc_heap_block_count < g_merc_heap_block_capacity)
        return 1;
    if (g_merc_heap_block_capacity >= MERC_HEAP_MAX_BLOCKS)
        return 0;
    capacity = g_merc_heap_block_capacity * 2u;
    if (capacity > MERC_HEAP_MAX_BLOCKS)
        capacity = MERC_HEAP_MAX_BLOCKS;
    blocks = (merc_heap_block_t *)malloc((size_t)capacity * sizeof(*blocks));
    if (blocks == NULL)
        return 0;
    memcpy(blocks, g_merc_heap_blocks,
           (size_t)g_merc_heap_block_count * sizeof(*blocks));
    if (g_merc_heap_blocks != g_merc_heap_initial_blocks)
        free(g_merc_heap_blocks);
    g_merc_heap_blocks = blocks;
    g_merc_heap_block_capacity = capacity;
    return 1;
}

static uint32_t merc_heap_aligned_size(uint32_t requested)
{
    uint32_t size = requested == 0u ? 1u : requested;

    if (size > UINT32_MAX - 15u)
        return 0u;
    return (size + 15u) & ~15u;
}

static void merc_heap_initialize(uint32_t heap)
{
    if (g_merc_heap_initialized)
        return;

    g_merc_heap_blocks[0].address = MERC_HEAP_ARENA_START;
    g_merc_heap_blocks[0].requested = 0u;
    g_merc_heap_blocks[0].capacity =
        MERC_HEAP_ARENA_END - MERC_HEAP_ARENA_START;
    g_merc_heap_blocks[0].in_use = 0u;
    g_merc_heap_block_count = 1u;
    g_merc_heap_handle = heap;
    g_merc_heap_initialized = 1u;

    fprintf(stderr,
            "[RECOMP HEAP] retail heap=%08X manual arena=%08X..%08X (%u bytes)\n",
            heap, MERC_HEAP_ARENA_START, MERC_HEAP_ARENA_END,
            MERC_HEAP_ARENA_END - MERC_HEAP_ARENA_START);
}

static void merc_heap_remove_block(uint32_t index)
{
    if (index + 1u < g_merc_heap_block_count) {
        memmove(&g_merc_heap_blocks[index],
                &g_merc_heap_blocks[index + 1u],
                (g_merc_heap_block_count - index - 1u) *
                    sizeof(g_merc_heap_blocks[0]));
    }
    --g_merc_heap_block_count;
}

static void merc_heap_coalesce(uint32_t index)
{
    merc_heap_block_t *block;

    if (index >= g_merc_heap_block_count)
        return;

    if (index != 0u) {
        merc_heap_block_t *previous = &g_merc_heap_blocks[index - 1u];
        block = &g_merc_heap_blocks[index];
        if (!previous->in_use && !block->in_use &&
            previous->address + previous->capacity == block->address) {
            previous->capacity += block->capacity;
            merc_heap_remove_block(index);
            --index;
        }
    }

    if (index + 1u < g_merc_heap_block_count) {
        block = &g_merc_heap_blocks[index];
        merc_heap_block_t *next = &g_merc_heap_blocks[index + 1u];
        if (!block->in_use && !next->in_use &&
            block->address + block->capacity == next->address) {
            block->capacity += next->capacity;
            merc_heap_remove_block(index + 1u);
        }
    }
}

static int32_t merc_heap_find(uint32_t address)
{
    uint32_t index;

    for (index = 0u; index < g_merc_heap_block_count; ++index) {
        if (g_merc_heap_blocks[index].in_use &&
            g_merc_heap_blocks[index].address == address)
            return (int32_t)index;
    }
    return -1;
}

static uint32_t merc_heap_allocate_locked(
    uint32_t heap, uint32_t flags, uint32_t requested)
{
    const uint32_t capacity = merc_heap_aligned_size(requested);
    uint32_t index;

    merc_heap_initialize(heap);
    if (capacity == 0u)
        return 0u;

    for (index = 0u; index < g_merc_heap_block_count; ++index) {
        merc_heap_block_t *block = &g_merc_heap_blocks[index];

        if (block->in_use || block->capacity < capacity)
            continue;

        if (block->capacity > capacity &&
            merc_heap_reserve_block()) {
            block = &g_merc_heap_blocks[index];
            memmove(&g_merc_heap_blocks[index + 2u],
                    &g_merc_heap_blocks[index + 1u],
                    (g_merc_heap_block_count - index - 1u) *
                        sizeof(g_merc_heap_blocks[0]));
            g_merc_heap_blocks[index + 1u].address =
                block->address + capacity;
            g_merc_heap_blocks[index + 1u].requested = 0u;
            g_merc_heap_blocks[index + 1u].capacity =
                block->capacity - capacity;
            g_merc_heap_blocks[index + 1u].in_use = 0u;
            ++g_merc_heap_block_count;
            block = &g_merc_heap_blocks[index];
            block->capacity = capacity;
        }

        block->requested = requested;
        block->in_use = 1u;
        if ((flags & MERC_HEAP_ZERO_MEMORY) != 0u)
            memset(guest_ptr(block->address), 0, block->capacity);
        return block->address;
    }

    /*
     * Preserve the ordinary heap's 16-byte alignment when its initial arena
     * fills. Page-aligning every fallback (including 16/48-byte Lua objects)
     * strands sub-page holes and can exhaust guest RAM while megabytes remain
     * free. Contiguous/page allocations use their separate kernel path.
     * Retain each backing record so free, realloc, and _msize stay coherent.
     */
    if (merc_heap_reserve_block()) {
        const uint32_t address = xbox_HeapAlloc(capacity, 16u);

        if (address != 0u) {
            merc_heap_block_t *block =
                &g_merc_heap_blocks[g_merc_heap_block_count++];
            block->address = address;
            block->requested = requested;
            block->capacity = capacity;
            block->in_use = 1u;
            return address;
        }
    }

    if (!g_merc_heap_oom_reported) {
        fprintf(stderr,
                "[RECOMP HEAP] allocation failed for %u bytes "
                "(heap=%08X, initial heap=%08X, records=%u/%u)\n",
                requested, heap, g_merc_heap_handle,
                g_merc_heap_block_count, g_merc_heap_block_capacity);
        g_merc_heap_oom_reported = 1u;
    }
    return 0u;
}

static uint32_t merc_heap_allocate(
    uint32_t heap, uint32_t flags, uint32_t requested)
{
    uint32_t result;

    AcquireSRWLockExclusive(&g_merc_heap_lock);
    result = merc_heap_allocate_locked(heap, flags, requested);
    ReleaseSRWLockExclusive(&g_merc_heap_lock);
    return result;
}

static uint32_t merc_heap_free_locked(uint32_t address)
{
    const int32_t found = merc_heap_find(address);

    if (address == 0u)
        return 1u;
    if (found < 0)
        return 0u;

    /* Blocks outside the process heap's fixed 1 MiB arena were obtained from
     * xbox_HeapAlloc.  Remove their private record and return the backing
     * allocation to the shared 64 MiB Xbox heap; retaining a reusable private
     * block here would alias any later shared-heap reuse of the same pages. */
    if (address < MERC_HEAP_ARENA_START || address >= MERC_HEAP_ARENA_END) {
        xbox_HeapFree(address);
        merc_heap_remove_block((uint32_t)found);
        return 1u;
    }

    g_merc_heap_blocks[found].requested = 0u;
    g_merc_heap_blocks[found].in_use = 0u;
    merc_heap_coalesce((uint32_t)found);
    return 1u;
}

static uint32_t merc_heap_free(uint32_t address)
{
    uint32_t result;

    AcquireSRWLockExclusive(&g_merc_heap_lock);
    result = merc_heap_free_locked(address);
    ReleaseSRWLockExclusive(&g_merc_heap_lock);
    return result;
}

uint32_t recomp_title_heap_allocate(uint32_t size)
{
    return merc_heap_allocate(guest_u32(0x004409ACu), 0u, size);
}

void recomp_title_heap_free(uint32_t address)
{
    (void)merc_heap_free(address);
}
/* Prepare the optional mission correction without changing guest registers.
 * The Lua parser consumes this temporary buffer before the script executes. */
uint32_t recomp_prepare_mission_script(uint32_t source, uint32_t length, uint32_t *out_length)
{
    char *recomp_patch_mission_script(const char *, size_t, size_t *);
    size_t patched_length;
    char *patched;
    uint32_t buffer;
    *out_length = length;
    if (source < 0x10000u || source >= 0x04000000u ||
        length > 0x04000000u - source)
        return 0u;
    patched = recomp_patch_mission_script((const char *)guest_ptr(source), length, &patched_length);
    if (!patched)
        return 0u;
    buffer = recomp_title_heap_allocate((uint32_t)patched_length + 1u);
    if (buffer) {
        memcpy(guest_ptr(buffer), patched, patched_length + 1u);
        *out_length = (uint32_t)patched_length;
    }
    free(patched);
    return buffer;
}

static uint32_t merc_heap_reallocate(
    uint32_t heap, uint32_t flags, uint32_t address, uint32_t requested)
{
    const uint32_t capacity = merc_heap_aligned_size(requested);
    uint32_t result = 0u;
    int32_t found;

    AcquireSRWLockExclusive(&g_merc_heap_lock);
    merc_heap_initialize(heap);

    if (address == 0u) {
        result = merc_heap_allocate_locked(heap, flags, requested);
        goto done;
    }

    found = merc_heap_find(address);
    if (found < 0 || capacity == 0u)
        goto done;

    {
        merc_heap_block_t *block = &g_merc_heap_blocks[found];
        const uint32_t old_requested = block->requested;

        if (capacity <= block->capacity) {
            if ((flags & MERC_HEAP_ZERO_MEMORY) != 0u &&
                requested > old_requested) {
                memset((uint8_t *)guest_ptr(address) + old_requested, 0,
                       requested - old_requested);
            }
            block->requested = requested;
            result = address;
            goto done;
        }

        if ((uint32_t)found + 1u < g_merc_heap_block_count) {
            merc_heap_block_t *next = &g_merc_heap_blocks[found + 1];
            if (!next->in_use &&
                block->address + block->capacity == next->address &&
                block->capacity + next->capacity >= capacity) {
                const uint32_t needed = capacity - block->capacity;
                if (next->capacity == needed) {
                    block->capacity = capacity;
                    merc_heap_remove_block((uint32_t)found + 1u);
                } else {
                    block->capacity = capacity;
                    next = &g_merc_heap_blocks[found + 1];
                    next->address += needed;
                    next->capacity -= needed;
                }
                if ((flags & MERC_HEAP_ZERO_MEMORY) != 0u &&
                    requested > old_requested) {
                    memset((uint8_t *)guest_ptr(address) + old_requested, 0,
                           requested - old_requested);
                }
                block = &g_merc_heap_blocks[found];
                block->requested = requested;
                result = address;
                goto done;
            }
        }

        if ((flags & MERC_HEAP_REALLOC_IN_PLACE_ONLY) == 0u) {
            result = merc_heap_allocate_locked(heap, flags, requested);
            if (result != 0u) {
                const uint32_t copy_size =
                    old_requested < requested ? old_requested : requested;
                memcpy(guest_ptr(result), guest_ptr(address), copy_size);

                merc_heap_free_locked(address);
            }
        }
    }

done:
    ReleaseSRWLockExclusive(&g_merc_heap_lock);
    return result;
}

static uint32_t merc_heap_size(uint32_t address, uint32_t *size)
{
    int32_t found;
    uint32_t result = 0u;

    AcquireSRWLockShared(&g_merc_heap_lock);
    found = merc_heap_find(address);
    if (found >= 0) {
        *size = g_merc_heap_blocks[found].capacity;
        result = 1u;
    }
    ReleaseSRWLockShared(&g_merc_heap_lock);
    return result;
}

/* Lua normally uses the isolated title heap to avoid the retail small-block
 * free lists. That must not hide the engine's reserved main-pool memory when
 * the outer Xbox heap is fragmented. Borrow through Pool::Alloc directly:
 * it does not use small pools, run memory-helper callbacks, or retry forever.
 * Keep explicit ownership so realloc/free return each block to its allocator.
 * No additional guest RAM, graphics-pool changes, or mod setting is needed. */
typedef struct merc_lua_pool_block {
    uint32_t address;
    struct merc_lua_pool_block *next;
} merc_lua_pool_block;
static merc_lua_pool_block *g_merc_lua_pool_blocks;

static uint32_t merc_main_pool_allocate(uint32_t size, merc_lua_pool_block **blocks, const char *event)
{
    uint32_t result;
    const uint32_t pool = 0x00645984u;
    uint32_t start = guest_u32(pool), bytes = guest_u32(pool + 4u);
    uint32_t capacity = guest_u32(pool + 0x14u);
    uint32_t free_end = guest_u32(pool + 0x18u);
    uint32_t alloc_begin = guest_u32(pool + 0x1cu);
    /* Lua can run before RedMemory initialization. Leave room for both the
     * allocated record and a split free record; avoid its metadata OOM path. */
    if (start < 0x10000u || start >= 0x04000000u ||
        !bytes || bytes > 0x04000000u - start || size > bytes ||
        size > guest_u32(pool + 8u) || capacity > 7000u ||
        alloc_begin > capacity || free_end >= alloc_begin ||
        alloc_begin - free_end < 3u ||
        g_esp < 0x20000u || g_esp >= 0x04000000u) return 0u;
    merc_lua_pool_block *record = (merc_lua_pool_block *)malloc(sizeof(*record));
    if (!record) return 0u;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    extern void sub_001F6D80(void);
    recomp_guest_push_u32(0u); /* not a temporary allocation */
    recomp_guest_push_u32(1u); /* array */
    recomp_guest_push_u32(1u); /* allocate from the high end */
    recomp_guest_push_u32(0u); /* optional debug description */
    recomp_guest_push_u32(size);
    recomp_guest_push_u32(0u);
    g_ecx = pool;
    sub_001F6D80(); /* RedMemory::Pool::Alloc; thiscall, ret 20 */
    result = g_eax;
    recomp_restore_guest_cpu_context(&saved);
    if (!result) { free(record); return 0u; }
    record->address = result;
    record->next = *blocks;
    *blocks = record;
    xbox_preview_log_event(event, "main-pool fallback bytes=%u address=%08X", size, result);
    return result;
}

static int merc_main_pool_free(uint32_t address, merc_lua_pool_block **blocks)
{
    merc_lua_pool_block **link = blocks;
    while (*link && (*link)->address != address) link = &(*link)->next;
    if (!*link) return 0;
    merc_lua_pool_block *record = *link;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    extern void sub_001F6970(void);
    recomp_guest_push_u32(1u); /* array */
    recomp_guest_push_u32(address);
    recomp_guest_push_u32(0u);
    g_ecx = 0x00645984u;
    sub_001F6970(); /* RedMemory::Pool::Free; thiscall, ret 8 */
    recomp_restore_guest_cpu_context(&saved);
    *link = record->next;
    free(record);
    return 1;
}

static uint32_t merc_lua_allocate(uint32_t heap, uint32_t size)
{
    /* Reproduce the captured 512->1024-global-table allocation failure only
     * in an explicitly isolated private integration test. */
    static int force_hq_table_oom = -1;
    if (force_hq_table_oom < 0)
        force_hq_table_oom = getenv("MERCENARIES_TEST_GAMEPAD_FILE") &&
                            getenv("MERCENARIES_TEST_LUA_TITLE_HEAP_OOM");
    int force_failure = force_hq_table_oom && size == 40960u &&
                        guest_u8(0x403384u) && !guest_u8(0x403385u);
    force_failure |= recomp_test_guard_heap_failure;
    uint32_t result = force_failure ? 0u : merc_heap_allocate(heap, 0u, size);
    if (result || !size) return result;
    /* Private negative control: reproduce the old guard OOM without fallback. */
    if (recomp_test_guard_heap_failure && getenv("MERCENARIES_TEST_GUARD_NO_POOL")) return 0u;
    return merc_main_pool_allocate(size, &g_merc_lua_pool_blocks, "lua-memory");
}

static void merc_lua_free(uint32_t address)
{
    if (!merc_main_pool_free(address, &g_merc_lua_pool_blocks))
        (void)merc_heap_free(address);
}

/*
 * Retail 0x00178870 is MercRealloc from RsMain.cpp, Lua's allocator
 * callback. Keep its allocate-copy-free semantics, but use the tracked guest
 * heap directly so Lua strings cannot inherit a corrupted RedMemory
 * SmallBlockPool free list while the rest of the retail allocator remains in
 * use. This is cdecl: the caller removes the three arguments.
 */
void sub_00178870(void)
{
    const uint32_t block = guest_u32(g_esp + 4u);
    const uint32_t size = guest_u32(g_esp + 8u);
    const uint32_t old_size = guest_u32(g_esp + 12u);
    const uint32_t heap = g_merc_heap_initialized ? g_merc_heap_handle : 0u;
    uint32_t allocation = 0u;

    if (block != 0u && size == 0u) {
        if (block != 1u)
            merc_lua_free(block);
    } else if (block != 0u) {
        allocation = merc_lua_allocate(heap, size);
        if (allocation != 0u) {
            const uint32_t copy_size = size < old_size ? size : old_size;
            memcpy(guest_ptr(allocation), guest_ptr(block), copy_size);
            if (block != 1u)
                merc_lua_free(block);
        }
    } else if (size == 0u) {
        allocation = 1u;
    } else {
        allocation = merc_lua_allocate(heap, size);
    }

    if (size != 0u && allocation == 0u) {
        fprintf(stderr,
                "[LUA-REALLOC-FAIL] block=%08X requested=%u (%08X) "
                "old=%u (%08X) tracked=%u\n",
                block, size, size, old_size, old_size,
                g_merc_heap_block_count);
        fflush(stderr);
    }

    g_eax = allocation;
    g_esp += 4u;
}

/* XMem's ordinary audio objects share the fragmented title heap. On failure,
 * borrow a main-pool block using the same bounded allocator as Lua. Physical
 * allocations retain their original alignment/cache API. Ownership is tracked
 * independently, so XMemFree never sends a borrowed block to RtlFreeHeap. */
static merc_lua_pool_block *g_merc_audio_pool_blocks;
static int recomp_audio_pool_attributes(uint32_t attributes)
{
    /* Audited nonphysical DirectSound/XACT allocation attributes in retail. */
    switch (attributes) {
    case 0x6484800Bu: case 0x6484800Du: case 0x6484800Eu:
    case 0x6484800Fu: case 0x64848010u: case 0x64848011u:
    case 0x64848015u: case 0x6484A000u: case 0x6484A002u:
    case 0x6484A001u: case 0x6484A003u: case 0x6484A004u:
    case 0x6484A005u: case 0x6484A006u: case 0x6482A000u:
        return 1;
    default: return 0;
    }
}
uint32_t recomp_audio_pool_fallback(uint32_t size, uint32_t attributes)
{
    if (!size || !recomp_audio_pool_attributes(attributes)) return 0;
    if (getenv("MERCENARIES_TEST_ISOLATE_INPUT") && getenv("MERCENARIES_TEST_AUDIO_NO_POOL")) return 0;
    uint32_t p = merc_main_pool_allocate(size, &g_merc_audio_pool_blocks, "audio-memory");
    if (p && (attributes & 0x40000000u)) memset(guest_ptr(p), 0, size);
    return p;
}
int recomp_audio_pool_free(uint32_t address)
{
    return merc_main_pool_free(address, &g_merc_audio_pool_blocks);
}
int recomp_test_audio_alloc_failure(uint32_t attributes)
{
    return attributes == 0x6484A003u &&
        getenv("MERCENARIES_TEST_ISOLATE_INPUT") &&
        getenv("MERCENARIES_TEST_GAMEPAD_FILE") &&
        getenv("MERCENARIES_TEST_AUDIO_HEAP_OOM") &&
        guest_u32(0x413F6Cu) == 0x4249D707u;
}

/*
 * Lua 5 luaS_newlstr, following the supplied Pandemic lstring.c.  A stale
 * pointer in one retail hash bucket currently appears during asynchronous
 * startup loading.  Keep all valid entries and sever only an impossible
 * low-RAM chain link before delegating allocation/insertion to newlstr.
 */
void sub_001E26E0(void)
{
    const uint32_t saved_ebx = g_ebx;
    const uint32_t saved_esi = g_esi;
    const uint32_t saved_edi = g_edi;
    const uint32_t state = guest_u32(g_esp + 4u);
    const uint32_t string = guest_u32(g_esp + 8u);
    const uint32_t length = guest_u32(g_esp + 12u);
    uint32_t hash = length;
    const uint32_t step = (length >> 5) + 1u;
    uint32_t remaining;
    uint32_t global;
    uint32_t table;
    uint32_t table_size;
    uint32_t link;
    uint32_t node;

    for (remaining = length; remaining >= step; remaining -= step) {
        hash ^= (hash << 5) + (hash >> 2) +
                (uint32_t)guest_u8(string + remaining - 1u);
    }

    global = guest_u32(state + 0x10u);
    table = guest_u32(global);
    table_size = guest_u32(global + 8u);
    link = table + (hash & (table_size - 1u)) * 4u;
    node = guest_u32(link);

    while (node != 0u) {
        if (node < 0x00010000u || node > 0x03FFFFEFu) {
            static uint32_t corrupt_chain_reports;
            if (corrupt_chain_reports++ < 16u) {
                fprintf(stderr,
                        "[LUA] discarded corrupt string-chain node=%08X "
                        "link=%08X hash=%08X len=%u state=%08X str=%08X "
                        "global=%08X table=%08X size=%u\n",
                        node, link, hash, length, state, string,
                        global, table, table_size);
            }
            *(volatile uint32_t *)guest_ptr(link) = 0u;
            break;
        }
        if (guest_u32(node + 0x0Cu) == length &&
            memcmp(guest_ptr(string), guest_ptr(node + 0x10u), length) == 0) {
            g_eax = node;
            g_ebx = saved_ebx;
            g_esi = saved_esi;
            g_edi = saved_edi;
            g_esp += 4u;
            return;
        }
        link = node;
        node = guest_u32(node);
    }

    g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = length;
    g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = string;
    g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = state;
    g_eax = hash;
    g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
    sub_001E2640();
    g_esp += 12u;
    g_ebx = saved_ebx;
    g_esi = saved_esi;
    g_edi = saved_edi;
    g_esp += 4u;
}
/*
 * Retail GetLastError/SetLastError use the Xbox FS-backed TIB.  The static
 * recompiler flattens FS accesses into guest low memory, which is not stable
 * once the title starts its asynchronous movie-loading path.  Preserve the
 * observable value directly instead of dereferencing a stale TIB.
 */
static uint32_t g_xbox_last_error;

void sub_0022B150(void)
{
    g_eax = g_xbox_last_error;
    g_esp += 4u;
}

void sub_0022B178(void)
{
    g_xbox_last_error = guest_u32(g_esp + 4u);
    g_esp += 8u;
}
/* Retail RtlAllocateHeap: stdcall (heap, flags, size). */
void sub_0022DDA4(void)
{
    const uint32_t heap = guest_u32(g_esp + 4u);
    const uint32_t flags = guest_u32(g_esp + 8u);
    const uint32_t size = guest_u32(g_esp + 12u);

    g_eax = merc_heap_allocate(heap, flags, size);
    g_esp += 16u;
}

/* Retail RtlFreeHeap: stdcall (heap, flags, allocation). */
void sub_0022E51F(void)
{
    const uint32_t allocation = guest_u32(g_esp + 12u);

    g_eax = merc_heap_free(allocation);
    g_esp += 16u;
}

/* Retail RtlReAllocateHeap: stdcall (heap, flags, allocation, size). */
void sub_0022E713(void)
{
    const uint32_t heap = guest_u32(g_esp + 4u);
    const uint32_t flags = guest_u32(g_esp + 8u);
    const uint32_t allocation = guest_u32(g_esp + 12u);
    const uint32_t size = guest_u32(g_esp + 16u);

    g_eax = merc_heap_reallocate(heap, flags, allocation, size);
    g_esp += 20u;
}
/*
 * Retail 0x0023BE3C is the Xbox CRT _msize wrapper. Its internal heap helper
 * reads allocation metadata immediately before the supplied pointer, but the
 * CRT _onexit table invokes it once with a null base before allocating its
 * first block. Normalize that empty-table query to zero capacity, then retain
 * the retail metadata calculation for every real allocation. The caller will
 * consequently follow its original realloc path.
 *
 * Recompile.ps1 excludes this symbol from generated bodies, so both direct
 * calls and dispatch-table calls resolve here.
 */
void sub_0023BE3C(void)
{
    const uint32_t allocation = guest_u32(g_esp + 4);

    if (allocation == 0) {
        g_eax = 0;
    } else {
        uint32_t manual_size;

        if (merc_heap_size(allocation, &manual_size)) {
            g_eax = manual_size;
        } else {
            const uint8_t flags = guest_u8(allocation - 11);

            if ((flags & 1u) == 0) {
                g_eax = UINT32_MAX;
            } else if ((flags & 8u) != 0) {
                g_edx = guest_u16(allocation - 16);
                g_eax = guest_u32(allocation - 24) - g_edx;
            } else {
                g_eax = ((uint32_t)guest_u16(allocation - 16) << 4) -
                        guest_u8(allocation - 10);
            }
        }
    }

    g_esp += 4;
}


/* Execute the CRT copy once, before the lifted prefix can modify an
 * overlapping source. The cdecl entry consumes only its return address;
 * EBP, EBX, ESI and EDI remain the caller's values. */
void recomp_crt_memmove(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const uint32_t source = guest_u32(g_esp + 8u);
    const uint32_t length = guest_u32(g_esp + 12u);
    if (length != 0u)
        memmove(guest_ptr(destination), guest_ptr(source), length);
    g_eax = destination;
    g_esp += 4u;
}

/*
 * Mercenaries links an Xbox CRT memcpy/memmove implementation at 0x00238C00.
 * Its forward and backward copy paths use embedded jump tables whose targets
 * are basic-block entries rather than functions. Until the generic lifter can
 * represent those entries as labels, finish the operation from the original
 * arguments and reproduce the function epilogue here.
 *
 * Kept for legacy interior entry resolution. The maintained 0x00238C00 entry
 * now calls recomp_crt_memmove before any copy: restarting the original range
 * after a prefix copy is NOT safe when destination precedes an overlapping
 * source. Ordinary CRT calls no longer reach this continuation.
 */
static void mercenaries_memmove_finish(void)
{
    const uint32_t frame = g_seh_ebp;
    const uint32_t destination = guest_u32(frame + 8);
    const uint32_t source = guest_u32(frame + 12);
    const uint32_t length = guest_u32(frame + 16);
    uint32_t caller_ebp;

    if (length != 0) {
        memmove(guest_ptr(destination), guest_ptr(source), length);
    }
    g_eax = destination;

    /* pop esi; pop edi; leave; ret */
    g_esi = guest_u32(g_esp);
    g_esp += 4;
    g_edi = guest_u32(g_esp);
    caller_ebp = guest_u32(frame);
    g_esp = frame + 8;
    g_seh_ebp = caller_ebp;
}

/*
 * The retail XDK DirectSound image loader decrypts dsstdfx.bin through three
 * tiny routines at 0x002A666D..0x002A67E2. The lifted implementation is
 * correct, but it performs one translated call per PRNG byte plus 80 calls to
 * initialize each eight-byte key. A 384 KiB effects image consequently takes
 * tens of millions of native C calls before title startup can continue.
 *
 * Keep the original algorithm and its two guest-visible PRNG state dwords,
 * but execute the byte loop natively. Recompile.ps1 excludes 0x002A6713 from
 * generated bodies so direct and indirect calls both resolve here.
 */
static void merc_dsound_rng_step(uint32_t low, uint32_t high,
                                  uint32_t *next_low,
                                  uint32_t *next_high)
{
    uint32_t feedback = low;

    feedback = (feedback << 2) ^ low;
    feedback = (feedback << 1) ^ low;
    feedback = (feedback << 28) ^ high;

    *next_low = (low >> 1) | (high << 31);
    *next_high = (high >> 1) | (feedback & 0x80000000u);
}

static void merc_dsound_rng_store(uint32_t low, uint32_t high)
{
    *(volatile uint32_t *)guest_ptr(0x002BB168u) = low;
    *(volatile uint32_t *)guest_ptr(0x002BB16Cu) = high;
}

static void merc_dsound_rng_advance(uint32_t *low, uint32_t *high)
{
    uint32_t next_low;
    uint32_t next_high;

    merc_dsound_rng_step(*low, *high, &next_low, &next_high);
    *low = next_low;
    *high = next_high;
    merc_dsound_rng_store(next_low, next_high);
}

void sub_002A6713(void)
{
    const uint32_t state_address = guest_u32(g_esp + 4u);
    uint32_t input_address = guest_u32(g_esp + 8u);
    uint32_t length = guest_u32(g_esp + 12u);
    const uint32_t output_address = guest_u32(g_esp + 16u);
    const uint32_t decode_header = guest_u32(g_esp + 20u);
    uint8_t initial_state[8];
    uint8_t decoded_state[8];
    uint32_t rng_low = 0x49DE12BAu;
    uint32_t rng_high = 0x7FA49BCAu;
    uint32_t index;

    /* sub_002A66E6 advances the constants once, then advances state 80 times. */
    for (index = 0u; index < 81u; ++index)
        merc_dsound_rng_advance(&rng_low, &rng_high);
    memcpy(initial_state, &rng_low, sizeof(rng_low));
    memcpy(initial_state + sizeof(rng_low), &rng_high, sizeof(rng_high));

    if (decode_header != 0u) {
        const uint8_t *input = (const uint8_t *)guest_ptr(input_address);

        for (index = 0u; index < 8u; ++index)
            decoded_state[index] = input[index] ^ initial_state[index];
        if (state_address != 0u)
            memcpy(guest_ptr(state_address), decoded_state,
                   sizeof(decoded_state));
    } else {
        if (state_address == 0u) {
            g_eax = UINT32_MAX;
            g_esp += 24u;
            return;
        }
        memcpy(decoded_state, guest_ptr(state_address), sizeof(decoded_state));
    }

    memcpy(&rng_low, initial_state, sizeof(rng_low));
    memcpy(&rng_high, initial_state + sizeof(rng_low), sizeof(rng_high));
    merc_dsound_rng_advance(&rng_low, &rng_high);

    if (decode_header != 0u) {
        input_address += 8u;
        length -= 8u;
    }

    for (index = 0u; index < length; ++index) {
        const uint32_t key_index = index & 7u;
        const uint8_t input = guest_u8(input_address + index);
        const uint8_t product = (uint8_t)(
            decoded_state[key_index] * initial_state[key_index]);
        const uint8_t mixed = (uint8_t)(input - product);

        merc_dsound_rng_advance(&rng_low, &rng_high);
        *(volatile uint8_t *)guest_ptr(output_address + index) =
            (uint8_t)(initial_state[key_index] ^ (uint8_t)rng_low ^ mixed);
    }

    g_eax = 0u;
    g_esp += 24u;
}

/*
 * Lua 5 luaK_patchlistaux, following the supplied lcode.c.  The optimized
 * retail routine keeps the jump-list index in EDX and uses CDQ while fixing
 * offsets.  The generic lift can consequently lose the live list index and
 * follow a bogus negative code entry.  Keep the retail register calling
 * convention but express the jump-chain operations directly.
 */
void sub_001E89E0(void)
{
    static uint32_t trace_calls;
    const uint32_t fs = g_ebx;
    int32_t list = (int32_t)g_edx;
    const int32_t ttarget = (int32_t)guest_u32(g_esp + 4u);
    const int32_t treg = (int32_t)guest_u32(g_esp + 8u);
    const int32_t ftarget = (int32_t)guest_u32(g_esp + 12u);
    const int32_t freg = (int32_t)guest_u32(g_esp + 16u);
    const int32_t dtarget = (int32_t)guest_u32(g_esp + 20u);
    const uint32_t saved_ecx = g_ecx;
    const uint32_t saved_esi = g_esi;
    const uint32_t saved_edi = g_edi;
    uint32_t iterations = 0u;

    if (getenv("MERCENARIES_TRACE_LUA_LEX") != NULL && trace_calls++ < 64u) {
        const uint32_t proto = guest_u32(fs);
        fprintf(stderr,
                "[LUA PATCH CALL] fs=%08X proto=%08X code=%08X pc=%d list=%d "
                "tt=%d tr=%d ft=%d fr=%d dt=%d esp=%08X\n",
                fs, proto, proto ? guest_u32(proto + 0x0Cu) : 0u,
                (int32_t)guest_u32(fs + 0x18u), list,
                ttarget, treg, ftarget, freg, dtarget, g_esp);
    }

    while (list != -1) {
        const uint32_t proto = guest_u32(fs);
        const uint32_t code = guest_u32(proto + 0x0Cu);
        const int32_t pc = (int32_t)guest_u32(fs + 0x18u);
        uint32_t instruction;
        uint32_t control_va;
        uint32_t control;
        int32_t encoded;
        int32_t next;
        int32_t destination;
        int32_t reg;
        int32_t offset;

        if (list < 0 || list >= pc || iterations++ > 1000000u) {
            fprintf(stderr,
                    "[LUA PATCH] invalid jump list=%d pc=%d fs=%08X code=%08X\n",
                    list, pc, fs, code);
            break;
        }

        instruction = guest_u32(code + (uint32_t)list * 4u);
        encoded = (int32_t)((instruction >> 6) & 0x3FFFFu) - 0x1FFFF;
        next = encoded == -1 ? -1 : list + 1 + encoded;

        control_va = code + (uint32_t)list * 4u;
        if (list >= 1) {
            const uint32_t previous = guest_u32(control_va - 4u);
            const uint32_t opcode = previous & 0x3Fu;
            if ((guest_u8(0x002FC144u + opcode) & 0x80u) != 0u)
                control_va -= 4u;
        }
        control = guest_u32(control_va);

        if ((control & 0x3Fu) != 0x18u) {
            destination = dtarget;
        } else if ((control & 0x7FC0u) != 0u) {
            destination = ttarget;
            reg = treg;
            if (reg == 0xFF)
                reg = (int32_t)((control >> 15) & 0x1FFu);
            control = (control & 0x00FFFFFFu) |
                      (((uint32_t)reg & 0xFFu) << 24);
            *(volatile uint32_t *)guest_ptr(control_va) = control;
        } else {
            destination = ftarget;
            reg = freg;
            if (reg == 0xFF)
                reg = (int32_t)((control >> 15) & 0x1FFu);
            control = (control & 0x00FFFFFFu) |
                      (((uint32_t)reg & 0xFFu) << 24);
            *(volatile uint32_t *)guest_ptr(control_va) = control;
        }

        offset = destination - (list + 1);
        instruction = guest_u32(code + (uint32_t)list * 4u);
        instruction = (instruction & 0xFF00003Fu) |
                      ((((uint32_t)(offset + 0x1FFFF)) & 0x3FFFFu) << 6);
        *(volatile uint32_t *)guest_ptr(code + (uint32_t)list * 4u) = instruction;
        g_eax = instruction;
        list = next;
    }

    g_edx = 0xFFFFFFFFu;
    g_ecx = saved_ecx;
    g_esi = saved_esi;
    g_edi = saved_edi;
    g_esp += 4u;
}

/*
 * Lua discharge2reg switch tail at 0x001E91ED.  In the retail image this
 * block falls through to the shared RET at 0x001E920C.  Function discovery
 * split the RET into its own function, leaving the generated block without
 * the guest-stack return adjustment and leaking four bytes per invocation.
 */
void sub_001E91ED(void)
{
    const uint32_t expression = g_ebx;
    const uint32_t fs = g_edi;
    const uint32_t reg = g_esi;
    const uint32_t old_info = guest_u32(expression + 4u);

    if (reg != old_info) {
        g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
        g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = old_info;
        g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = reg;
        g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
        g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = fs;
        g_esp -= 4u; *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
        sub_001E8EC0();
        g_esp += 20u;
    }

    *(volatile uint32_t *)guest_ptr(expression + 4u) = reg;
    *(volatile uint32_t *)guest_ptr(expression) = 0x0Bu;
    g_esp += 4u;
}
/*
 * Lua pcall's static f_call callback at 0x001DD580. Function discovery did
 * not emit this tiny body, so protected calls previously returned success
 * without executing their Lua closure. This is the source-equivalent body:
 * luaD_call(L, call->func, call->nresults).
 */
static void sub_001DD580(void)
{
    const uint32_t state = guest_u32(g_esp + 4u);
    const uint32_t call = guest_u32(g_esp + 8u);
    const uint32_t function = guest_u32(call);
    const uint32_t result_count = guest_u32(call + 4u);

    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = result_count;
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = function;
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = state;
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
    sub_001DF0B0();
    g_esp += 12u;
    g_esp += 4u;
}

/*
 * Lua lauxlib getS reader at 0x001DC460.  The retail compiler placed this
 * static callback between two discovered functions, so it has no generated
 * body.  Preserve its cdecl stack behavior and one-shot buffer semantics.
 */
static void sub_001DC460(void)
{
    const uint32_t data = guest_u32(g_esp + 8u);
    const uint32_t size_out = guest_u32(g_esp + 12u);
    const uint32_t size = guest_u32(data + 4u);

    if (size == 0u) {
        g_eax = 0u;
    } else {
        *(volatile uint32_t *)guest_ptr(size_out) = size;
        *(volatile uint32_t *)guest_ptr(data + 4u) = 0u;
        g_eax = guest_u32(data);
    }
    g_esp += 4u;
}

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/* Native replacements for the retail XG math helpers used throughout the
 * engine. Their original bodies are packed-SSE/x87 assembly; implementing the
 * mathematical contract directly is exact, alias-safe, and avoids a partial
 * 128-bit SIMD model in generated scalar C. */
static float merc_guest_f32(uint32_t address)
{
    uint32_t bits = guest_u32(address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void merc_guest_store_f32(uint32_t address, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    *(volatile uint32_t *)guest_ptr(address) = bits;
}

/* Source-matched replacements for PblMath's 8192-entry sine table. The
 * retail table lives in writable guest storage and normally depends on a C++
 * global constructor. Computing the same quantized samples natively avoids
 * propagating an uninitialized/corrupted table into geometry and gameplay. */
static int32_t merc_pbl_trig_index(float angle)
{
    const float table_scale = 8192.0f / 6.28318530717958647692f;
    return (int32_t)(0.5f + angle * table_scale);
}

static float merc_pbl_sine_sample(int32_t index)
{
    const float table_step = 6.28318530717958647692f / 8192.0f;
    return sinf((float)(index & 0x1FFF) * table_step);
}

static void merc_fp_return(float value)
{
    g_fp_stack[--g_fp_top & 7u] = value;
    g_esp += 4u;
}

void sub_001F9D70(void)
{
    const float angle = merc_guest_f32(g_esp + 4u);
    merc_fp_return(merc_pbl_sine_sample(merc_pbl_trig_index(angle)));
}

void recomp_effect_matrix_checkpoint(uint32_t stage, uint32_t saved_esp,
                                     uint32_t actual_esp,
                                     uint32_t saved_edi,
                                     uint32_t actual_edi)
{
    static int enabled = -1;
    static uint32_t count;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_EFFECT_MATRIX_STACK") != NULL;
    if (stage >= 10u && stage != 33u && stage != 34u &&
        !(stage >= 40u && stage < 50u) &&
        saved_edi < 0x10000000u && actual_edi < 0x10000000u)
        return;
    if (stage >= 42u && stage <= 45u && actual_esp == saved_esp)
        return;
    if (stage >= 40u && stage < 64u && actual_esp == saved_esp &&
        actual_edi == saved_edi)
        return;
    if (!enabled || count++ >= 256u)
        return;
    fprintf(stderr,
            "[TRACE EFFECT MATRIX] stage=%u saved_esp=%08X esp=%08X "
            "saved_edi=%08X edi=%08X delta=%d\n",
            stage, saved_esp, actual_esp, saved_edi, actual_edi,
            (int32_t)(actual_esp - saved_esp));
    fflush(stderr);
}
void sub_001F9DA0(void)
{
    const float angle = merc_guest_f32(g_esp + 4u);
    const int32_t index = merc_pbl_trig_index(angle);
    merc_fp_return(-merc_pbl_sine_sample(index - 0x800));
}

void sub_001F9DE0(void)
{
    const uint32_t sine_address = guest_u32(g_esp + 4u);
    const uint32_t cosine_address = guest_u32(g_esp + 8u);
    const float angle = merc_guest_f32(g_esp + 12u);
    const int32_t index = merc_pbl_trig_index(angle);

    merc_guest_store_f32(sine_address, merc_pbl_sine_sample(index));
    merc_guest_store_f32(cosine_address, -merc_pbl_sine_sample(index - 0x800));
    g_esp += 4u;
}

void sub_000112B0(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const uint32_t source = guest_u32(g_esp + 8u);
    const float x = merc_guest_f32(source + 0u);
    const float y = merc_guest_f32(source + 4u);
    const float z = merc_guest_f32(source + 8u);
    const float length = sqrtf(x * x + y * y + z * z);
    float scale = 0.0f;

    if (length != 0.0f)
        scale = 1.0f / length;
    merc_guest_store_f32(destination + 0u, x * scale);
    merc_guest_store_f32(destination + 4u, y * scale);
    merc_guest_store_f32(destination + 8u, z * scale);
    g_eax = destination;
    g_esp += 4u;
}
void sub_0022F49E(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const uint32_t vector_address = guest_u32(g_esp + 8u);
    const uint32_t matrix_address = guest_u32(g_esp + 12u);
    float vector[3], matrix[16], result[4];

    for (uint32_t i = 0; i < 3u; ++i)
        vector[i] = merc_guest_f32(vector_address + i * 4u);
    for (uint32_t i = 0; i < 16u; ++i)
        matrix[i] = merc_guest_f32(matrix_address + i * 4u);
    for (uint32_t column = 0; column < 4u; ++column) {
        result[column] =
            vector[0] * matrix[0u * 4u + column] +
            vector[1] * matrix[1u * 4u + column] +
            vector[2] * matrix[2u * 4u + column] +
                        matrix[3u * 4u + column];
    }
    if (result[3] != 1.0f) {
        const float reciprocal_w = 1.0f / result[3];
        result[0] *= reciprocal_w;
        result[1] *= reciprocal_w;
        result[2] *= reciprocal_w;
    }
    for (uint32_t i = 0; i < 3u; ++i)
        merc_guest_store_f32(destination + i * 4u, result[i]);
    if (getenv("MERCENARIES_TRACE_XG_MATH") != NULL) {
        static uint32_t samples;
        const uint32_t sample = ++samples;
        if (sample <= 32u || (sample & (sample - 1u)) == 0u) {
            fprintf(stderr,
                    "XGVec3TransformCoord n=%u dst=%08X v=(%.6g,%.6g,%.6g) "
                    "result=(%.6g,%.6g,%.6g,%.6g)\n",
                    sample, destination, vector[0], vector[1], vector[2],
                    result[0], result[1], result[2], result[3]);
        }
    }
    g_eax = destination;
    g_esp += 16u;
}
void sub_0022F51C(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const uint32_t vector_address = guest_u32(g_esp + 8u);
    const uint32_t matrix_address = guest_u32(g_esp + 12u);
    float vector[3], matrix[16], result[3];

    for (uint32_t i = 0; i < 3u; ++i)
        vector[i] = merc_guest_f32(vector_address + i * 4u);
    for (uint32_t i = 0; i < 16u; ++i)
        matrix[i] = merc_guest_f32(matrix_address + i * 4u);
    if (getenv("MERCENARIES_TRACE_XG_MATH") != NULL) {
        static uint32_t traced_bad_transform;
        int bad = 0;
        for (uint32_t i = 0; i < 3u; ++i)
            bad = bad || !isfinite(vector[i]);
        for (uint32_t i = 0; i < 16u; ++i)
            bad = bad || !isfinite(matrix[i]);
        if (bad && traced_bad_transform++ < 24u) {
            fprintf(stderr, "XGVec3TransformNormal bad input dst=%08X vec=%08X matrix=%08X v=(%.9g %.9g %.9g)\n",
                    destination, vector_address, matrix_address, vector[0], vector[1], vector[2]);
            for (uint32_t row = 0; row < 4u; ++row)
                fprintf(stderr, "  M%u=(%.9g %.9g %.9g %.9g)\n", row,
                        matrix[row*4u], matrix[row*4u+1u], matrix[row*4u+2u], matrix[row*4u+3u]);
            if (traced_bad_transform == 1u) {
                const uint32_t recent_index = g_recomp_recent_func_idx;
                for (uint32_t i = 0; i < 64u && i < recent_index; ++i) {
                    const uint32_t index = (recent_index - 1u - i) & 63u;
                    fprintf(stderr, "  xg-bad-source-recent[-%u]=%08X\n", i,
                            g_recomp_recent_funcs[index]);
                }
            }
        }
    }
    for (uint32_t column = 0; column < 3u; ++column) {
        result[column] =
            vector[0] * matrix[0u * 4u + column] +
            vector[1] * matrix[1u * 4u + column] +
            vector[2] * matrix[2u * 4u + column];
    }
    for (uint32_t i = 0; i < 3u; ++i)
        merc_guest_store_f32(destination + i * 4u, result[i]);
    g_eax = destination;
    g_esp += 16u;
}
void sub_0022F5A7(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const uint32_t left_address = guest_u32(g_esp + 8u);
    const uint32_t right_address = guest_u32(g_esp + 12u);
    float left[16], right[16], result[16];

    for (uint32_t i = 0; i < 16u; ++i) {
        left[i] = merc_guest_f32(left_address + i * 4u);
        right[i] = merc_guest_f32(right_address + i * 4u);
    }
    if (getenv("MERCENARIES_TRACE_XG_MATH") != NULL) {
        static uint32_t traced_bad_multiply;
        int bad = 0;
        for (uint32_t i = 0; i < 16u; ++i)
            bad = bad || !isfinite(left[i]) || !isfinite(right[i]);
        if (bad && traced_bad_multiply++ < 24u) {
            fprintf(stderr, "XGMatrixMultiply bad input dst=%08X left=%08X right=%08X\n",
                    destination, left_address, right_address);
            for (uint32_t row = 0; row < 4u; ++row) {
                fprintf(stderr, "  L%u=(%.9g %.9g %.9g %.9g) R%u=(%.9g %.9g %.9g %.9g)\n",
                        row, left[row*4u], left[row*4u+1u], left[row*4u+2u], left[row*4u+3u],
                        row, right[row*4u], right[row*4u+1u], right[row*4u+2u], right[row*4u+3u]);
            }
        }
    }
    for (uint32_t row = 0; row < 4u; ++row) {
        for (uint32_t column = 0; column < 4u; ++column) {
            result[row * 4u + column] =
                left[row * 4u + 0u] * right[0u * 4u + column] +
                left[row * 4u + 1u] * right[1u * 4u + column] +
                left[row * 4u + 2u] * right[2u * 4u + column] +
                left[row * 4u + 3u] * right[3u * 4u + column];
        }
    }
    for (uint32_t i = 0; i < 16u; ++i)
        merc_guest_store_f32(destination + i * 4u, result[i]);
    g_eax = destination;
    g_esp += 16u;
}

void sub_0022F6A5(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const uint32_t source = guest_u32(g_esp + 8u);
    float input[16], result[16];

    for (uint32_t i = 0; i < 16u; ++i)
        input[i] = merc_guest_f32(source + i * 4u);
    for (uint32_t row = 0; row < 4u; ++row) {
        for (uint32_t column = 0; column < 4u; ++column)
            result[row * 4u + column] = input[column * 4u + row];
    }
    for (uint32_t i = 0; i < 16u; ++i)
        merc_guest_store_f32(destination + i * 4u, result[i]);
    g_eax = destination;
    g_esp += 12u;
}

static void mercenaries_xg_matrix_scaling(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const float scale_x = merc_guest_f32(g_esp + 8u);
    const float scale_y = merc_guest_f32(g_esp + 12u);
    const float scale_z = merc_guest_f32(g_esp + 16u);
    const float result[16] = {
        scale_x, 0.0f,    0.0f,    0.0f,
        0.0f,    scale_y, 0.0f,    0.0f,
        0.0f,    0.0f,    scale_z, 0.0f,
        0.0f,    0.0f,    0.0f,    1.0f,
    };

    if (getenv("MERCENARIES_TRACE_XG_SCALING") != NULL) {
        static uint32_t samples;
        const uint32_t sample = ++samples;
        if (sample <= 128u ||
            (scale_x == 0.0f && scale_y == 0.0f && scale_z != 0.0f)) {
            fprintf(stderr,
                    "XGMatrixScaling n=%u dst=%08X scale=(%.9g,%.9g,%.9g)\n",
                    sample, destination, scale_x, scale_y, scale_z);
            if (scale_x == 0.0f && scale_y == 0.0f && scale_z != 0.0f) {
                const uint32_t recent_index = g_recomp_recent_func_idx;
                for (uint32_t i = 0u; i < 24u && i < recent_index; ++i) {
                    const uint32_t index = (recent_index - 1u - i) & 63u;
                    fprintf(stderr, "  scale-source-recent[-%u]=%08X\n", i,
                            g_recomp_recent_funcs[index]);
                }
            }
        }
    }
    for (uint32_t i = 0; i < 16u; ++i)
        merc_guest_store_f32(destination + i * 4u, result[i]);
    g_eax = destination;
    g_esp += 20u;
}

void sub_0022FA9E(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const uint32_t source = guest_u32(g_esp + 8u);
    float quaternion[4];
    float length_squared = 0.0f;
    float reciprocal_length = 0.0f;

    for (uint32_t i = 0u; i < 4u; ++i) {
        quaternion[i] = merc_guest_f32(source + i * 4u);
        length_squared += quaternion[i] * quaternion[i];
    }
    if (getenv("MERCENARIES_TRACE_XG_QUATERNION") != NULL) {
        static uint32_t normalize_samples;
        if ((length_squared < 0.999f || length_squared > 1.001f) &&
            normalize_samples++ < 128u) {
            fprintf(stderr,
                    "[XG-QUAT-NORMALIZE] sample=%u caller=%08X "
                    "dst=%08X src=%08X q=(%.9g,%.9g,%.9g,%.9g) len2=%.9g\n",
                    normalize_samples, g_recomp_current_func, destination,
                    source, quaternion[0], quaternion[1], quaternion[2],
                    quaternion[3], length_squared);
            fflush(stderr);
        }
    }
    /* XGQuaternionNormalize preserves an already-unit quaternion verbatim.
     * Besides matching the retail XDK, this avoids introducing needless
     * rounding drift into every pose, camera, and independently tracked
     * vehicle part which passes through the shared PblMatrix constructor. */
    if (length_squared >= 0.99999f && length_squared <= 1.00001f) {
        for (uint32_t i = 0u; i < 4u; ++i)
            merc_guest_store_f32(destination + i * 4u, quaternion[i]);
    } else {
        if (length_squared > 1.0e-10f)
            reciprocal_length = 1.0f / sqrtf(length_squared);
        for (uint32_t i = 0u; i < 4u; ++i)
            merc_guest_store_f32(destination + i * 4u,
                                 quaternion[i] * reciprocal_length);
    }
    g_eax = destination;
    g_esp += 12u;
}

void sub_0022F9BC(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const uint32_t source = guest_u32(g_esp + 8u);
    const float x = merc_guest_f32(source + 0u);
    const float y = merc_guest_f32(source + 4u);
    const float z = merc_guest_f32(source + 8u);
    const float w = merc_guest_f32(source + 12u);
    const float length_squared = x * x + y * y + z * z + w * w;
    const float xx = 2.0f * x * x;
    const float yy = 2.0f * y * y;
    const float zz = 2.0f * z * z;
    const float xy = 2.0f * x * y;
    const float xz = 2.0f * x * z;
    const float yz = 2.0f * y * z;
    const float xw = 2.0f * x * w;
    const float yw = 2.0f * y * w;
    const float zw = 2.0f * z * w;
    const float result[16] = {
        1.0f - yy - zz, xy + zw,          xz - yw,          0.0f,
        xy - zw,          1.0f - xx - zz, yz + xw,          0.0f,
        xz + yw,          yz - xw,          1.0f - xx - yy, 0.0f,
        0.0f,             0.0f,             0.0f,            1.0f,
    };

    if (getenv("MERCENARIES_TRACE_XG_QUATERNION") != NULL) {
        static uint32_t rotation_samples;
        if ((length_squared < 0.999f || length_squared > 1.001f) &&
            rotation_samples++ < 128u) {
            fprintf(stderr,
                    "[XG-QUAT-ROTATION] sample=%u caller=%08X "
                    "dst=%08X src=%08X q=(%.9g,%.9g,%.9g,%.9g) len2=%.9g "
                    "diag=(%.9g,%.9g,%.9g)\n",
                    rotation_samples, g_recomp_current_func, destination,
                    source, x, y, z, w, length_squared,
                    result[0], result[5], result[10]);
            for (uint32_t i = 0u;
                 i < 16u && i < g_recomp_recent_func_idx; ++i) {
                const uint32_t index =
                    (g_recomp_recent_func_idx - 1u - i) & 63u;
                fprintf(stderr, "  quat-rotation-recent[-%u]=%08X\n", i,
                        g_recomp_recent_funcs[index]);
            }
            fflush(stderr);
        }
    }

    for (uint32_t i = 0u; i < 16u; ++i)
        merc_guest_store_f32(destination + i * 4u, result[i]);
    g_eax = destination;
    g_esp += 12u;
}
void sub_0022F7C6(void)
{
    const uint32_t destination = guest_u32(g_esp + 4u);
    const float angle = merc_guest_f32(g_esp + 8u);
    const float sine = sinf(angle);
    const float cosine = cosf(angle);
    if (getenv("MERCENARIES_TRACE_XG_MATH") != NULL && !isfinite(angle))
        fprintf(stderr, "XGMatrixRotationY non-finite angle at dst=%08X bits=%08X\n",
                destination, guest_u32(g_esp + 8u));
    const float result[16] = {
         cosine, 0.0f, -sine, 0.0f,
         0.0f,   1.0f,  0.0f, 0.0f,
         sine,   0.0f,  cosine, 0.0f,
         0.0f,   0.0f,  0.0f, 1.0f,
    };

    for (uint32_t i = 0; i < 16u; ++i)
        merc_guest_store_f32(destination + i * 4u, result[i]);
    g_eax = destination;
    g_esp += 12u;
}

/* Literal retail XDK hardware-ISR thunks. Their addresses are passed
 * indirectly to KeInitializeInterrupt, so the static detector can miss the
 * three-instruction wrappers even though their real handlers are translated. */
static void mercenaries_apu_isr_thunk(void)
{
    g_ecx = guest_u32(g_esp + 8u);
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
    sub_002A1954();
    g_esp += 12u; /* ret 8 */
}

static void mercenaries_apu_dpc_thunk(void)
{
    g_ecx = guest_u32(g_esp + 8u);
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
    sub_002A1774();
    g_esp += 20u; /* ret 16 */
}
static void mercenaries_ac97_isr_thunk(void)
{
    g_ecx = guest_u32(g_esp + 8u);
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
    sub_002A795F();
    g_eax = (g_eax & 0xFFFFFF00u) | (g_eax != 0u); /* setne al */
    g_esp += 12u; /* ret 8 */
}
/* Retail 002830EC..002830FC: DirectSound timer/DPC callback. The older
 * boot-validated generated snapshot did not detect this callback entry, so
 * route its exact recovered retail body manually until the bulk translator
 * regression is fixed. */
static void mercenaries_dsound_timer_dpc_callback(void)
{
    const uint32_t object = guest_u32(g_esp + 8u);
    const uint32_t vtable = guest_u32(object);
    const uint32_t target = guest_u32(vtable + 4u);
    const uint32_t saved_esp = g_esp;
    const uint32_t saved_ebx = g_ebx;
    const uint32_t saved_esi = g_esi;
    const uint32_t saved_edi = g_edi;
    recomp_func_t fn;
    g_recomp_current_func = 0x002830ECu;
    g_recomp_recent_funcs[g_recomp_recent_func_idx++ & 63u] = 0x002830ECu;

    g_eax = object;
    g_ecx = vtable;
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = object;
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
    fn = recomp_lookup_manual(target);
    if (!fn) fn = recomp_lookup(target);
    if (!fn) fn = recomp_lookup_kernel(target);
    if (fn) {
        fn();
        g_ebx = saved_ebx;
        g_esi = saved_esi;
        g_edi = saved_edi;
    } else {
        recomp_icall_fail_log(target);
        g_esp = saved_esp;
        g_eax = 0u;
    }
    g_esp += 20u; /* ret 16 */
}
/* Retail 0011EA00..0011EA2B: RsLuaState::Audio_ProcessVoiceoverCB.
 * Its address is pushed as callback data, not reached by a direct CALL.
 * Preserve the cdecl handle argument and the retail find/invoke/release order;
 * this must not synthesize mission progress or a timer-based completion. */
extern void sub_00113650(void);
extern void sub_00113A50(void);
extern void sub_0011D480(void);
static void mercenaries_voiceover_stop_callback(void)
{
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = g_esi;
    g_esi = guest_u32(g_esp + 8u);
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = g_esi;
    g_ecx = 0x00371BB0u;
    g_esp -= 4u;
    *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
    sub_00113650();
    if (g_eax != 0u) {
        if (!strncmp((const char *)guest_ptr(g_eax + 4u), "PreBriefing", 100))
            xbox_preview_log_event("hq-entry", "voice-complete handle=%08X owner=%08X callback=PreBriefing", g_esi, guest_u32(g_eax));
        if (getenv("MERCENARIES_TRACE_VOICE_CALLBACK") != NULL) {
            fprintf(stderr, "[VOICE-CALLBACK] handle=%08X slot=%08X state=%08X function=%.100s\n",
                    g_esi, g_eax, guest_u32(g_eax),
                    (const char *)guest_ptr(g_eax + 4u));
            fflush(stderr);
        }
        g_ecx = g_eax + 4u;
        g_esp -= 4u;
        *(volatile uint32_t *)guest_ptr(g_esp) = g_ecx;
        g_ecx = guest_u32(g_eax);
        g_esp -= 4u;
        *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
        sub_00113A50();
        g_esp -= 4u;
        *(volatile uint32_t *)guest_ptr(g_esp) = g_esi;
        g_ecx = 0x00371BB0u;
        g_esp -= 4u;
        *(volatile uint32_t *)guest_ptr(g_esp) = 0u;
        sub_0011D480();
    }
    g_esi = guest_u32(g_esp);
    g_esp += 8u; /* POP ESI; RET -- caller still owns the handle argument. */
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    switch (xbox_va) {
    case 0x0011EA00:
        return mercenaries_voiceover_stop_callback;
    case 0x002830EC:
        return mercenaries_dsound_timer_dpc_callback;
    case 0x001DC460:
        return sub_001DC460;
    case 0x001DD580:
        return sub_001DD580;
    case 0x000112B0:
        return sub_000112B0;
    case 0x00178870:
        return sub_00178870;
    case 0x001E26E0:
        return sub_001E26E0;
    case 0x001E89E0:
        return sub_001E89E0;
    case 0x001E91ED:
        return sub_001E91ED;
    case 0x001F9D70:
        return sub_001F9D70;
    case 0x001F9DA0:
        return sub_001F9DA0;
    case 0x001F9DE0:
        return sub_001F9DE0;
    case 0x0022B150:
        return sub_0022B150;
    case 0x0022B178:
        return sub_0022B178;
    case 0x0022DDA4:
        return sub_0022DDA4;
    case 0x0022E51F:
        return sub_0022E51F;
    case 0x0022E713:
        return sub_0022E713;
    case 0x0022F49E:
        return sub_0022F49E;
    case 0x0022F51C:
        return sub_0022F51C;
    case 0x0022F5A7:
        return sub_0022F5A7;
    case 0x0022F6A5:
        return sub_0022F6A5;
    case 0x0022F6F0:
        return mercenaries_xg_matrix_scaling;
    case 0x0022F7C6:
        return sub_0022F7C6;
    case 0x0022F9BC:
        return sub_0022F9BC;
    case 0x0022FA9E:
        return sub_0022FA9E;
    case 0x0023BE3C:
        return sub_0023BE3C;
    case 0x002A6713:
        return sub_002A6713;
    case 0x002A19F0:
        return mercenaries_apu_isr_thunk;
    case 0x002A1810:
        return mercenaries_apu_dpc_thunk;
    case 0x002A7A8C:
        return mercenaries_ac97_isr_thunk;
    /* Unaligned forward-copy entries. */
    case 0x00238C70:
    case 0x00238C9C:
    case 0x00238CC0:
    /* Short aligned dword-copy entries. */
    case 0x00238D00:
    case 0x00238D08:
    case 0x00238D10:
    case 0x00238D18:
    case 0x00238D20:
    case 0x00238D28:
    case 0x00238D30:
    case 0x00238D43:
    /* Zero-to-three-byte remainder entries. */
    case 0x00238D5C:
    case 0x00238D64:
    case 0x00238D70:
    case 0x00238D84:
    /* Overlapping backward-copy entry. */
    case 0x00238D9C:
        return mercenaries_memmove_finish;
    default:
        return (recomp_func_t)0;
    }
}

static uint32_t g_splash_draw_packet_va;

void recomp_d3d_set_stream_source(uint32_t stream, uint32_t vertex_buffer,
                                  uint32_t stride)
{
    static uint32_t trace_samples;
    static uint32_t last_trace_offset;
    static uint32_t brush_trace_samples;
    uint32_t physical_offset = 0;

    /* Xbox vertex buffers are twelve-byte resource headers. The second
     * dword is the unified-memory offset; Lock returns the same offset with
     * the KSEG0 bit set. */
    if (vertex_buffer != 0)
        physical_offset = guest_u32(vertex_buffer + 4u) & 0x07FFFFFFu;

    if (getenv("MERCENARIES_TRACE_STREAM_SOURCE") != NULL &&
        stream == 0u && stride == 24u && physical_offset >= 0x02000000u &&
        physical_offset != last_trace_offset && trace_samples < 128u) {
        uint32_t object_data = guest_u32(vertex_buffer + 12u);
        uint32_t object_offset = object_data & 0x07FFFFFFu;
        uint32_t object_stride = guest_u32(vertex_buffer + 16u);
        uint32_t object_vertices = guest_u32(vertex_buffer + 20u);

        fprintf(stderr,
                "[D3D-STREAM-SOURCE] #%u vb=%08X stride=%u "
                "header=%08X %08X %08X object-data=%08X "
                "header-offset=%08X object-offset=%08X object=%u-bytes/%u-verts\n",
                trace_samples++, vertex_buffer, stride,
                guest_u32(vertex_buffer + 0u),
                guest_u32(vertex_buffer + 4u),
                guest_u32(vertex_buffer + 8u), object_data,
                physical_offset, object_offset, object_stride, object_vertices);
        fprintf(stderr,
                "  header-data=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                guest_u32(physical_offset + 0u),
                guest_u32(physical_offset + 4u),
                guest_u32(physical_offset + 8u),
                guest_u32(physical_offset + 12u),
                guest_u32(physical_offset + 16u),
                guest_u32(physical_offset + 20u),
                guest_u32(physical_offset + 24u),
                guest_u32(physical_offset + 28u));
        fprintf(stderr,
                "  kseg0-data=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                guest_u32(0x80000000u | (physical_offset + 0u)),
                guest_u32(0x80000000u | (physical_offset + 4u)),
                guest_u32(0x80000000u | (physical_offset + 8u)),
                guest_u32(0x80000000u | (physical_offset + 12u)),
                guest_u32(0x80000000u | (physical_offset + 16u)),
                guest_u32(0x80000000u | (physical_offset + 20u)),
                guest_u32(0x80000000u | (physical_offset + 24u)),
                guest_u32(0x80000000u | (physical_offset + 28u)));
        if (object_offset != physical_offset &&
            object_offset >= 0x00010000u && object_offset < 0x04000000u) {
            fprintf(stderr,
                    "  object-data=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                    guest_u32(object_offset + 0u),
                    guest_u32(object_offset + 4u),
                    guest_u32(object_offset + 8u),
                    guest_u32(object_offset + 12u),
                    guest_u32(object_offset + 16u),
                    guest_u32(object_offset + 20u),
                    guest_u32(object_offset + 24u),
                    guest_u32(object_offset + 28u));
        }
        last_trace_offset = physical_offset;
        fflush(stderr);
    }
    if (getenv("MERCENARIES_TRACE_BRUSH3D_BIND") != NULL &&
        stream == 0u && stride == 24u && vertex_buffer != 0u &&
        guest_u32(vertex_buffer + 16u) == 24u &&
        guest_u32(vertex_buffer + 20u) == 6144u &&
        brush_trace_samples++ < 4u) {
        uint32_t recent_index = g_recomp_recent_func_idx;
        fprintf(stderr,
                "[BRUSH3D-BIND] vb=%08X data=%08X lock=%08X recent-index=%u\n",
                vertex_buffer, guest_u32(vertex_buffer + 4u),
                guest_u32(vertex_buffer + 8u), recent_index);
        for (uint32_t i = 0u; i < 64u && i < recent_index; ++i) {
            uint32_t index = (recent_index - 1u - i) & 63u;
            fprintf(stderr, "  brush-recent[-%u]=%08X\n", i,
                    g_recomp_recent_funcs[index]);
        }
        fflush(stderr);
    }
    pgraph_d3d11_set_guest_stream(stream, physical_offset, stride);
}

#define RECOMP_XMV_TRACE_SLOTS 512u
static uint32_t g_recomp_xmv_trace_addrs[RECOMP_XMV_TRACE_SLOTS];
static uint32_t g_recomp_xmv_trace_counts[RECOMP_XMV_TRACE_SLOTS];
static uint32_t g_recomp_xmv_trace_previous[RECOMP_XMV_TRACE_SLOTS];
static int g_recomp_xmv_trace_enabled = -1;
static uint32_t g_recomp_xmv_decode_index;

uint32_t recomp_dsound_clock_value(uint32_t device, uint32_t guest_clock)
{
    static uint32_t clock_device;
    static uint32_t clock_base;
    static uint32_t clock_rate_hz;
    static ULONGLONG clock_base_ms;
    static int trace_clock = -1;
    static int suppress_forward_rebase = -1;
    static ULONGLONG trace_period_ms;
    static uint32_t trace_forward_rebases;
    const ULONGLONG now = GetTickCount64();
    uint32_t rate_hz;
    uint32_t host_clock;
    int32_t guest_ahead;

    if (trace_clock < 0) {
        trace_clock = getenv("MERCENARIES_TRACE_DSOUND_CLOCK") != NULL;
        suppress_forward_rebase =
            getenv("MERCENARIES_TEST_DSOUND_CLOCK_NO_FORWARD_REBASE") != NULL;
    }

    if (device == 0u)
        return guest_clock;

    /* XMV compares presentation timestamps against this Xbox video-field
     * counter.  The retail update at 0x0028B4D7 increments +0x1DE8 once per
     * field, while 0x00289500 reports a 50 or 60 Hz display mode.  Advancing
     * it in milliseconds makes the movie clock 16.67x too fast at 60 Hz. */
    if ((guest_u32(device + 0x2448u) |
         guest_u32(device + 0x244Cu)) != 0u) {
        rate_hz = guest_u32(device + 0x2448u) == 0x01312D00u &&
                  guest_u32(device + 0x244Cu) == 0u ? 50u : 60u;
    } else {
        rate_hz = (guest_u32(device + 0x1DDCu) & 0x00400000u) != 0u ?
                  60u : 50u;
    }

    if (clock_device != device || clock_base_ms == 0u ||
        clock_rate_hz != rate_hz) {
        clock_device = device;
        clock_base = guest_clock;
        clock_rate_hz = rate_hz;
        clock_base_ms = now;
    }
    host_clock = clock_base + (uint32_t)(
        ((now - clock_base_ms) * (ULONGLONG)clock_rate_hz) / 1000ull);
    guest_ahead = (int32_t)(guest_clock - host_clock);
    if (guest_ahead > 0 && !suppress_forward_rebase) {
        clock_base = guest_clock;
        clock_base_ms = now;
        host_clock = guest_clock;
        ++trace_forward_rebases;
    }
    if (trace_clock && (trace_period_ms == 0u || now - trace_period_ms >= 1000u)) {
        fprintf(stderr,
                "[DSOUND-CLOCK] device=%08X guest=%u host=%u ahead=%d "
                "rate=%u forward_rebases=%u suppress=%d\n",
                device, guest_clock, host_clock, guest_ahead, clock_rate_hz,
                trace_forward_rebases, suppress_forward_rebase);
        fflush(stderr);
        trace_period_ms = now;
        trace_forward_rebases = 0u;
    }
    if (g_xbox_mem_offset != 0 && device <= 0x03FFE214u)
        *(volatile uint32_t *)guest_ptr(device + 0x1DE8u) = host_clock;
    return host_clock;
}
void recomp_xmv_func_trace(uint32_t xbox_va)
{
    uint32_t slot;

    if (g_recomp_xmv_trace_enabled < 0)
        g_recomp_xmv_trace_enabled =
            getenv("MERCENARIES_TRACE_XMV_FUNCTIONS") != NULL;
    if (!g_recomp_xmv_trace_enabled)
        return;

    slot = ((xbox_va >> 2u) ^ (xbox_va >> 11u)) &
           (RECOMP_XMV_TRACE_SLOTS - 1u);
    while (g_recomp_xmv_trace_addrs[slot] != 0u &&
           g_recomp_xmv_trace_addrs[slot] != xbox_va)
        slot = (slot + 1u) & (RECOMP_XMV_TRACE_SLOTS - 1u);
    g_recomp_xmv_trace_addrs[slot] = xbox_va;
    ++g_recomp_xmv_trace_counts[slot];
}

static void recomp_dump_xmv_func_histogram(void)
{
    static uint32_t frame;

    if (!g_recomp_xmv_trace_enabled || frame >= 8u)
        return;
    ++frame;
    fprintf(stderr, "[RETAIL-XMV-FUNCTIONS] frame=%u\n", frame);
    for (uint32_t slot = 0u; slot < RECOMP_XMV_TRACE_SLOTS; ++slot) {
        const uint32_t count = g_recomp_xmv_trace_counts[slot];
        const uint32_t delta = count - g_recomp_xmv_trace_previous[slot];
        if (delta != 0u)
            fprintf(stderr, "  xmv=%08X calls=%u\n",
                    g_recomp_xmv_trace_addrs[slot], delta);
        g_recomp_xmv_trace_previous[slot] = count;
    }
    fflush(stderr);
}

void recomp_xmv_idct_checkpoint(uint32_t stage, uint32_t output,
                                uint32_t coefficients, uint32_t block_mask)
{
    static int enabled = -1;
    static uint32_t calls;
    static uint32_t active_call = UINT32_MAX;
    const char *prefix;
    char path[MAX_PATH * 4];
    FILE *dump;
    uint32_t call;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_DUMP_XMV_IDCT_PREFIX") != NULL;
    if (!enabled || g_xbox_mem_offset == 0)
        return;
    if (stage == 0u) {
        if (calls >= 16u) {
            active_call = UINT32_MAX;
            return;
        }
        active_call = calls++;
        call = active_call;
    } else {
        if (active_call == UINT32_MAX)
            return;
        call = active_call;
        active_call = UINT32_MAX;
    }
    if (output < 0x00010000u || output > 0x03FFFF80u ||
        coefficients < 0x00010000u || coefficients > 0x03FFFF80u)
        return;

    prefix = getenv("MERCENARIES_DUMP_XMV_IDCT_PREFIX");
    snprintf(path, sizeof(path), "%s-%02u-%s.bin", prefix, call,
             stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(&block_mask, 1u, sizeof(block_mask), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + coefficients),
           1u, 128u, dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + output),
           1u, 128u, dump);
    fclose(dump);
    fprintf(stderr,
            "[RETAIL-XMV-IDCT] call=%u stage=%u output=%08X coeff=%08X "
            "mask=%08X path=%s\n",
            call, stage, output, coefficients, block_mask, path);
    fflush(stderr);
}
void recomp_xmv_transform_checkpoint(uint32_t stage, uint32_t routine,
                                     uint32_t output, uint32_t coefficients,
                                     uint32_t block_index)
{
    static int enabled = -1;
    static uint32_t calls[2];
    static uint32_t active_call[2] = { UINT32_MAX, UINT32_MAX };
    static uint32_t active_output[2];
    static uint32_t active_coefficients[2];
    static uint32_t active_block_index[2];
    const char *prefix;
    uint32_t header[2];
    uint32_t slot;
    uint32_t call;
    char path[MAX_PATH * 4];
    FILE *dump;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_DUMP_XMV_TRANSFORM_PREFIX") != NULL;
    if (!enabled || g_xbox_mem_offset == 0)
        return;
    if (routine == 0x002598FCu)
        slot = 0u;
    else if (routine == 0x00259B73u)
        slot = 1u;
    else
        return;

    if (stage == 0u) {
        if (calls[slot] >= 32u || output < 0x00010000u ||
            output > 0x03FFFF80u || coefficients < 0x00010000u ||
            coefficients > 0x03FFFF80u) {
            active_call[slot] = UINT32_MAX;
            return;
        }
        active_call[slot] = calls[slot]++;
        active_output[slot] = output;
        active_coefficients[slot] = coefficients;
        active_block_index[slot] = block_index;
    } else if (active_call[slot] == UINT32_MAX) {
        return;
    }

    call = active_call[slot];
    output = active_output[slot];
    coefficients = active_coefficients[slot];
    block_index = active_block_index[slot];
    if (stage != 0u)
        active_call[slot] = UINT32_MAX;
    header[0] = routine;
    header[1] = block_index;
    prefix = getenv("MERCENARIES_DUMP_XMV_TRANSFORM_PREFIX");
    snprintf(path, sizeof(path), "%s-%08X-%02u-%s.bin", prefix, routine,
             call, stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + coefficients),
           1u, 128u, dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + output),
           1u, 128u, dump);
    fclose(dump);
    fprintf(stderr,
            "[RETAIL-XMV-TRANSFORM] routine=%08X call=%u stage=%u "
            "output=%08X coeff=%08X block=%u path=%s\n",
            routine, call, stage, output, coefficients, block_index, path);
    fflush(stderr);
}
void recomp_xmv_coeff_checkpoint(uint32_t stage, uint32_t bitreader,
                                 uint32_t quantizer, uint32_t vlc_table,
                                 uint32_t scan_table, uint32_t coefficients,
                                 uint32_t state1, uint32_t state2,
                                 uint32_t result)
{
    enum { STREAM_BYTES = 1024u };
    static int enabled = -1;
    static uint32_t calls;
    static uint32_t seen_calls;
    static uint32_t skip_calls;
    static uint32_t max_calls = 32u;
    static uint32_t active_call = UINT32_MAX;
    static uint32_t active_bitreader;
    static uint32_t active_quantizer;
    static uint32_t active_vlc_table;
    static uint32_t active_scan_table;
    static uint32_t active_coefficients;
    static uint32_t active_state1;
    static uint32_t active_state2;
    static uint32_t active_stream_base;
    const char *prefix;
    uint32_t header[12];
    uint32_t call;
    uint32_t current_stream;
    char path[MAX_PATH * 4];
    FILE *dump;
    const uintptr_t guest = (uintptr_t)g_xbox_mem_offset;

    if (enabled < 0) {
        const char *skip = getenv("MERCENARIES_DUMP_XMV_COEFF_SKIP");
        const char *limit = getenv("MERCENARIES_DUMP_XMV_COEFF_COUNT");
        enabled = getenv("MERCENARIES_DUMP_XMV_COEFF_PREFIX") != NULL;
        if (skip != NULL)
            skip_calls = (uint32_t)strtoul(skip, NULL, 0);
        if (limit != NULL) {
            const unsigned long parsed = strtoul(limit, NULL, 0);
            if (parsed > 0u && parsed <= 4096u)
                max_calls = (uint32_t)parsed;
        }
    }
    if (!enabled || g_xbox_mem_offset == 0)
        return;

    if (stage == 0u) {
        const uint32_t seen = seen_calls++;
        if (seen < skip_calls || calls >= max_calls ||
            bitreader < 0x00010000u ||
            bitreader > 0x03FFFFF4u || coefficients < 0x00010000u ||
            coefficients > 0x03FFFF80u || state1 < 0x00010000u ||
            state1 > 0x03FFFFFCu || state2 < 0x00010000u ||
            state2 > 0x03FFFFFCu) {
            active_call = UINT32_MAX;
            return;
        }
        current_stream = *(const uint32_t *)(guest + bitreader + 8u);
        if (current_stream < 0x00010000u ||
            current_stream > 0x04000000u - STREAM_BYTES) {
            active_call = UINT32_MAX;
            return;
        }
        active_call = calls++;
        active_bitreader = bitreader;
        active_quantizer = quantizer;
        active_vlc_table = vlc_table;
        active_scan_table = scan_table;
        active_coefficients = coefficients;
        active_state1 = state1;
        active_state2 = state2;
        active_stream_base = current_stream;
    } else if (active_call == UINT32_MAX) {
        return;
    }

    call = active_call;
    bitreader = active_bitreader;
    quantizer = active_quantizer;
    vlc_table = active_vlc_table;
    scan_table = active_scan_table;
    coefficients = active_coefficients;
    state1 = active_state1;
    state2 = active_state2;
    current_stream = *(const uint32_t *)(guest + bitreader + 8u);
    if (stage != 0u)
        active_call = UINT32_MAX;

    header[0] = 0x434D5658u;
    header[1] = quantizer;
    header[2] = vlc_table;
    header[3] = scan_table;
    header[4] = *(const uint32_t *)(guest + state1);
    header[5] = *(const uint32_t *)(guest + state2);
    header[6] = result;
    header[7] = *(const uint32_t *)(guest + bitreader);
    header[8] = *(const uint32_t *)(guest + bitreader + 4u);
    header[9] = current_stream;
    header[10] = active_stream_base;
    header[11] = STREAM_BYTES;

    prefix = getenv("MERCENARIES_DUMP_XMV_COEFF_PREFIX");
    snprintf(path, sizeof(path), "%s-%02u-%s.bin", prefix, call,
             stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)(guest + active_stream_base), 1u, STREAM_BYTES, dump);
    fwrite((const void *)(guest + coefficients), 1u, 128u, dump);
    fclose(dump);
    fprintf(stderr,
            "[RETAIL-XMV-COEFF] call=%u stage=%u reader=%08X stream=%08X/%08X "
            "bits=%u quant=%u result=%08X path=%s\n",
            call, stage, bitreader, active_stream_base, current_stream,
            header[8], quantizer, result, path);
    fflush(stderr);
}
void recomp_xmv_mc_checkpoint(uint32_t stage, uint32_t source,
                              uint32_t source_stride, uint32_t destination,
                              uint32_t destination_stride, uint32_t horizontal,
                              uint32_t vertical, uint32_t residual)
{
    static int enabled = -1;
    static uint32_t calls;
    static uint32_t active_call = UINT32_MAX;
    static uint32_t active_source;
    static uint32_t active_source_stride;
    static uint32_t active_destination;
    static uint32_t active_destination_stride;
    static uint32_t active_horizontal;
    static uint32_t active_vertical;
    static uint32_t active_residual;
    const char *prefix;
    uint32_t source_bytes;
    uint32_t destination_bytes;
    uint32_t header[7];
    uint32_t call;
    char path[MAX_PATH * 4];
    FILE *dump;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_DUMP_XMV_MC_PREFIX") != NULL;
    if (!enabled || g_xbox_mem_offset == 0)
        return;

    if (stage == 0u) {
        if (calls >= 16u) {
            active_call = UINT32_MAX;
            return;
        }
        if (source_stride < 8u || source_stride > 4096u ||
            destination_stride < 8u || destination_stride > 4096u ||
            horizontal > 1u || vertical > 1u) {
            active_call = UINT32_MAX;
            return;
        }
        source_bytes = source_stride * (8u + vertical);
        destination_bytes = destination_stride * 8u;
        if (source < 0x00010000u || destination < 0x00010000u ||
            residual < 0x00010000u ||
            source > 0x04000000u - source_bytes ||
            destination > 0x04000000u - destination_bytes ||
            residual > 0x03FFFF80u) {
            active_call = UINT32_MAX;
            return;
        }
        active_call = calls++;
        active_source = source;
        active_source_stride = source_stride;
        active_destination = destination;
        active_destination_stride = destination_stride;
        active_horizontal = horizontal;
        active_vertical = vertical;
        active_residual = residual;
    } else if (active_call == UINT32_MAX) {
        return;
    }

    call = active_call;
    source = active_source;
    source_stride = active_source_stride;
    destination = active_destination;
    destination_stride = active_destination_stride;
    horizontal = active_horizontal;
    vertical = active_vertical;
    residual = active_residual;
    source_bytes = source_stride * (8u + vertical);
    destination_bytes = destination_stride * 8u;
    if (stage != 0u)
        active_call = UINT32_MAX;

    header[0] = source_stride;
    header[1] = destination_stride;
    header[2] = horizontal;
    header[3] = vertical;
    header[4] = source_bytes;
    header[5] = destination_bytes;
    header[6] = 128u;
    prefix = getenv("MERCENARIES_DUMP_XMV_MC_PREFIX");
    snprintf(path, sizeof(path), "%s-%02u-%s.bin", prefix, call,
             stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + source),
           1u, source_bytes, dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + destination),
           1u, destination_bytes, dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + residual),
           1u, 128u, dump);
    fclose(dump);
    fprintf(stderr,
            "[RETAIL-XMV-MC] call=%u stage=%u src=%08X/%u dst=%08X/%u "
            "half=%u,%u residual=%08X path=%s\n",
            call, stage, source, source_stride, destination,
            destination_stride, horizontal, vertical, residual, path);
    fflush(stderr);
}
void recomp_xmv_predictor_checkpoint(uint32_t stage, uint32_t routine,
                                     uint32_t source, uint32_t source_stride,
                                     uint32_t destination,
                                     uint32_t destination_stride,
                                     uint32_t horizontal, uint32_t vertical,
                                     uint32_t auxiliary, uint32_t residual)
{
    static int dump_enabled = -1;
    if (dump_enabled < 0)
        dump_enabled = getenv("MERCENARIES_DUMP_XMV_PREDICTOR_PREFIX") != NULL;
    if (!dump_enabled)
        return;
    static int initialized;
    static uint32_t target_frame;
    static uint32_t routine_filter;
    static uint32_t captures;
    static uint32_t routine_captures[6];
    static uint32_t active_call = UINT32_MAX;
    static uint32_t active_routine, active_source, active_source_stride;
    static uint32_t active_destination, active_destination_stride;
    static uint32_t active_horizontal, active_vertical, active_auxiliary;
    static uint32_t active_residual, active_source_base, active_source_bytes;
    static uint32_t active_destination_bytes, active_residual_bytes;
    const char *prefix;
    uint32_t header[12];
    uint32_t call;
    char path[MAX_PATH * 4];
    FILE *dump;

    prefix = getenv("MERCENARIES_DUMP_XMV_PREDICTOR_PREFIX");
    if (prefix == NULL || g_xbox_mem_offset == 0)
        return;
    if (!initialized) {
        const char *frame = getenv("MERCENARIES_DUMP_XMV_PREDICTOR_FRAME");
        const char *filter = getenv("MERCENARIES_DUMP_XMV_PREDICTOR_ROUTINE");
        target_frame = frame != NULL ? (uint32_t)strtoul(frame, NULL, 0) : 30u;
        routine_filter = filter != NULL ? (uint32_t)strtoul(filter, NULL, 0) : 0u;
        initialized = 1;
    }

    if (stage == 0u) {
        uint32_t source_prefix;
        uint32_t routine_slot;
        switch (routine) {
        case 0x0025A55Cu: routine_slot = 0u; break;
        case 0x0025A7DDu: routine_slot = 1u; break;
        case 0x0025AB55u: routine_slot = 2u; break;
        case 0x0025AD54u: routine_slot = 3u; break;
        case 0x0025AF39u: routine_slot = 4u; break;
        case 0x0025B5A6u: routine_slot = 5u; break;
        default: active_call = UINT32_MAX; return;
        }
        if (g_recomp_xmv_decode_index != target_frame ||
            routine_captures[routine_slot] >= 8u ||
            (routine_filter != 0u && routine_filter != routine)) {
            active_call = UINT32_MAX;
            return;
        }
        if (source_stride < 8u || source_stride > 4096u ||
            destination_stride < 8u || destination_stride > 4096u ||
            horizontal > 1u || vertical > 1u) {
            active_call = UINT32_MAX;
            return;
        }
        source_prefix = source_stride * 3u + 16u;
        active_source_bytes = source_stride * 15u + 32u;
        {
            const int is_16x16 = routine == 0x0025AF39u ||
                                 routine == 0x0025B5A6u;
            active_destination_bytes = destination_stride *
                                       (is_16x16 ? 16u : 8u);
            active_residual_bytes = residual != 0u ?
                                    (is_16x16 ? 512u : 128u) : 0u;
        }
        if (source < 0x00010000u + source_prefix ||
            destination < 0x00010000u ||
            source - source_prefix > 0x04000000u - active_source_bytes ||
            destination > 0x04000000u - active_destination_bytes ||
            (active_residual_bytes != 0u &&
             (residual < 0x00010000u ||
              residual > 0x04000000u - active_residual_bytes))) {
            active_call = UINT32_MAX;
            return;
        }
        active_call = captures++;
        ++routine_captures[routine_slot];
        active_routine = routine;
        active_source = source;
        active_source_stride = source_stride;
        active_destination = destination;
        active_destination_stride = destination_stride;
        active_horizontal = horizontal;
        active_vertical = vertical;
        active_auxiliary = auxiliary;
        active_residual = residual;
        active_source_base = source - source_prefix;
    } else if (active_call == UINT32_MAX) {
        return;
    }

    call = active_call;
    if (stage != 0u)
        active_call = UINT32_MAX;
    header[0] = active_routine;
    header[1] = active_source_stride;
    header[2] = active_destination_stride;
    header[3] = active_horizontal;
    header[4] = active_vertical;
    header[5] = active_auxiliary;
    header[6] = active_source - active_source_base;
    header[7] = active_source_bytes;
    header[8] = active_destination_bytes;
    header[9] = active_residual_bytes;
    header[10] = g_recomp_xmv_decode_index;
    header[11] = active_residual != 0u;
    snprintf(path, sizeof(path), "%s-%08X-%02u-f%u-%s.bin", prefix,
             active_routine, call, g_recomp_xmv_decode_index,
             stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + active_source_base),
           1u, active_source_bytes, dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + active_destination),
           1u, active_destination_bytes, dump);
    if (active_residual_bytes != 0u)
        fwrite((const void *)((uintptr_t)g_xbox_mem_offset + active_residual),
               1u, active_residual_bytes, dump);
    fclose(dump);
    fprintf(stderr,
            "[RETAIL-XMV-PREDICTOR] frame=%u call=%u stage=%u routine=%08X"
            " src=%08X/%u dst=%08X/%u half=%u,%u aux=%08X residual=%08X"
            " path=%s\n",
            g_recomp_xmv_decode_index, call, stage, active_routine,
            active_source, active_source_stride, active_destination,
            active_destination_stride, active_horizontal, active_vertical,
            active_auxiliary, active_residual, path);
    fflush(stderr);
}

void recomp_xmv_macroblock_checkpoint(uint32_t stage)
{
    static int dump_enabled = -1;
    if (dump_enabled < 0)
        dump_enabled = getenv("MERCENARIES_DUMP_XMV_MACROBLOCK_PREFIX") != NULL;
    if (!dump_enabled)
        return;
    static int active;
    static int captured;
    static uint32_t calls;
    static uint32_t arguments[18];
    const char *prefix = getenv("MERCENARIES_DUMP_XMV_MACROBLOCK_PREFIX");
    const char *target_text = getenv("MERCENARIES_DUMP_XMV_MACROBLOCK_FRAME");
    const char *skip_text = getenv("MERCENARIES_DUMP_XMV_MACROBLOCK_SKIP");
    const uint32_t target = target_text != NULL ?
        (uint32_t)strtoul(target_text, NULL, 0) : 30u;
    const uint32_t skip = skip_text != NULL ?
        (uint32_t)strtoul(skip_text, NULL, 0) : 0u;
    uint32_t header[21];
    char path[MAX_PATH * 4];
    FILE *dump;

    if (prefix == NULL || g_xbox_mem_offset == 0 || captured)
        return;
    if (stage == 0u) {
        if (g_recomp_xmv_decode_index != target)
            return;
        if (calls++ != skip)
            return;
        for (uint32_t i = 0u; i < 18u; ++i)
            arguments[i] = guest_u32(g_esp + i * 4u);
        active = 1;
    } else if (!active) {
        return;
    }
    header[0] = 0x00257F93u;
    header[1] = g_recomp_xmv_decode_index;
    header[2] = 18u;
    memcpy(header + 3, arguments, sizeof(arguments));
    snprintf(path, sizeof(path), "%s-f%u-%s.bin", prefix,
             g_recomp_xmv_decode_index, stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x00010000u),
           1u, 0x03FF0000u, dump);
    fclose(dump);
    fprintf(stderr, "[RETAIL-XMV-MACROBLOCK] frame=%u call=%u stage=%u path=%s\n",
            g_recomp_xmv_decode_index, skip, stage, path);
    fflush(stderr);
    if (stage != 0u) {
        active = 0;
        captured = 1;
    }
}
void recomp_xmv_macroblock_args_trace(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_XMV_MACROBLOCK_ARGS") != NULL;
    if (!enabled)
        return;
    static uint32_t calls;
    const char *target_text = getenv("MERCENARIES_TRACE_XMV_MACROBLOCK_FRAME");
    const char *skip_text = getenv("MERCENARIES_TRACE_XMV_MACROBLOCK_SKIP");
    const uint32_t target = target_text != NULL ?
        (uint32_t)strtoul(target_text, NULL, 0) : 30u;
    const uint32_t skip = skip_text != NULL ?
        (uint32_t)strtoul(skip_text, NULL, 0) : 628u;
    uint32_t call, state17, state18;
    if (getenv("MERCENARIES_TRACE_XMV_MACROBLOCK_ARGS") == NULL ||
        g_recomp_xmv_decode_index != target)
        return;
    call = calls++;
    if (call < skip || call >= skip + 20u)
        return;
    fprintf(stderr, "recomp-macroblock[%u]", call);
    for (uint32_t index = 0u; index < 18u; ++index)
        fprintf(stderr, " %08X", guest_u32(g_esp + index * 4u));
    state17 = guest_u32(g_esp + 16u * 4u);
    state18 = guest_u32(g_esp + 17u * 4u);
    fprintf(stderr, " s17");
    for (uint32_t index = 0u; index < 5u; ++index)
        fprintf(stderr, " %08X", guest_u32(state17 + index * 4u));
    fprintf(stderr, " s18");
    for (uint32_t index = 0u; index < 5u; ++index)
        fprintf(stderr, " %08X", guest_u32(state18 + index * 4u));
    fprintf(stderr, "\n");
    fflush(stderr);
}
void recomp_xmv_frame_decode_checkpoint(uint32_t stage, uint32_t decoder)
{
    static int timing_enabled = -1;
    static int timing_active;
    static LARGE_INTEGER timing_frequency;
    static LARGE_INTEGER timing_period_start;
    static LARGE_INTEGER timing_decode_start;
    static uint64_t timing_total_ticks;
    static uint64_t timing_max_ticks;
    static uint32_t timing_calls;
    static int active;
    static int captured;
    static uint32_t active_decoder;
    const char *prefix = getenv("MERCENARIES_DUMP_XMV_FRAME_DECODE_PREFIX");
    const char *target_text = getenv("MERCENARIES_DUMP_XMV_FRAME_DECODE_FRAME");
    const uint32_t target = target_text != NULL ?
        (uint32_t)strtoul(target_text, NULL, 0) : 30u;
    uint32_t header[4];
    char path[MAX_PATH * 4];
    FILE *dump;

    if (timing_enabled < 0) {
        timing_enabled = getenv("MERCENARIES_TRACE_XMV_FRAME_DECODE_TIMING") != NULL;
        if (timing_enabled) {
            QueryPerformanceFrequency(&timing_frequency);
            QueryPerformanceCounter(&timing_period_start);
        }
    }
    if (timing_enabled && timing_frequency.QuadPart > 0) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (stage == 0u) {
            timing_decode_start = now;
            timing_active = 1;
        } else if (timing_active) {
            const uint64_t elapsed = (uint64_t)(now.QuadPart - timing_decode_start.QuadPart);
            timing_total_ticks += elapsed;
            if (elapsed > timing_max_ticks)
                timing_max_ticks = elapsed;
            ++timing_calls;
            timing_active = 0;
            if ((uint64_t)(now.QuadPart - timing_period_start.QuadPart) >=
                (uint64_t)timing_frequency.QuadPart) {
                const double tick_ms = 1000.0 / (double)timing_frequency.QuadPart;
                fprintf(stderr,
                        "[XMV-DECODE-TIMING] frames=%u avg_ms=%.3f max_ms=%.3f index=%u\n",
                        timing_calls,
                        timing_calls != 0u ? (double)timing_total_ticks * tick_ms /
                            (double)timing_calls : 0.0,
                        (double)timing_max_ticks * tick_ms,
                        g_recomp_xmv_decode_index);
                fflush(stderr);
                timing_period_start = now;
                timing_total_ticks = 0u;
                timing_max_ticks = 0u;
                timing_calls = 0u;
            }
        }
    }

    if (prefix == NULL || g_xbox_mem_offset == 0 || captured)
        return;
    if (stage == 0u) {
        if (g_recomp_xmv_decode_index != target)
            return;
        active_decoder = decoder;
        active = 1;
    } else if (!active) {
        return;
    }
    header[0] = 0x002582FFu;
    header[1] = g_recomp_xmv_decode_index;
    header[2] = 1u;
    header[3] = active_decoder;
    snprintf(path, sizeof(path), "%s-f%u-%s.bin", prefix,
             g_recomp_xmv_decode_index, stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x00010000u),
           1u, 0x03FF0000u, dump);
    fclose(dump);
    fprintf(stderr, "[RETAIL-XMV-FRAME-DECODE] frame=%u stage=%u decoder=%08X path=%s\n",
            g_recomp_xmv_decode_index, stage, active_decoder, path);
    fflush(stderr);
    if (stage != 0u) {
        active = 0;
        captured = 1;
    }
}
void recomp_xmv_mmx_checkpoint(uint32_t stage)
{
    static int dump_enabled = -1;
    if (dump_enabled < 0)
        dump_enabled = getenv("MERCENARIES_DUMP_XMV_MMX_PREFIX") != NULL;
    if (!dump_enabled)
        return;
    static int active;
    static int captured;
    static uint32_t arguments[3];
    const char *prefix = getenv("MERCENARIES_DUMP_XMV_MMX_PREFIX");
    const char *target_text = getenv("MERCENARIES_DUMP_XMV_MMX_FRAME");
    const char *output_text = getenv("MERCENARIES_DUMP_XMV_MMX_OUTPUT");
    const uint32_t target = target_text != NULL ?
        (uint32_t)strtoul(target_text, NULL, 0) : 30u;
    const uint32_t output = output_text != NULL ?
        (uint32_t)strtoul(output_text, NULL, 0) : 0x00950640u;
    uint32_t header[6];
    char path[MAX_PATH * 4];
    FILE *dump;
    if (prefix == NULL || g_xbox_mem_offset == 0 || captured)
        return;
    if (stage == 0u) {
        if (g_recomp_xmv_decode_index != target || guest_u32(g_esp) != output)
            return;
        for (uint32_t index = 0u; index < 3u; ++index)
            arguments[index] = guest_u32(g_esp + index * 4u);
        active = 1;
    } else if (!active) {
        return;
    }
    header[0] = 0x00258E0Du;
    header[1] = g_recomp_xmv_decode_index;
    header[2] = 3u;
    memcpy(header + 3, arguments, sizeof(arguments));
    snprintf(path, sizeof(path), "%s-f%u-%s.bin", prefix,
             g_recomp_xmv_decode_index, stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x00010000u),
           1u, 0x00FF0000u, dump);
    fclose(dump);
    fprintf(stderr, "[RETAIL-XMV-MMX] frame=%u stage=%u output=%08X path=%s\n",
            g_recomp_xmv_decode_index, stage, arguments[0], path);
    fflush(stderr);
    if (stage != 0u) { active = 0; captured = 1; }
}
void recomp_xmv_interblock_checkpoint(uint32_t stage)
{
    static int dump_enabled = -1;
    if (dump_enabled < 0)
        dump_enabled = getenv("MERCENARIES_DUMP_XMV_INTERBLOCK_PREFIX") != NULL;
    if (!dump_enabled)
        return;
    static int active;
    static int captured;
    static uint32_t trace_calls;
    static uint32_t arguments[25];
    const char *prefix = getenv("MERCENARIES_DUMP_XMV_INTERBLOCK_PREFIX");
    const char *target_text = getenv("MERCENARIES_DUMP_XMV_INTERBLOCK_FRAME");
    const char *output_text = getenv("MERCENARIES_DUMP_XMV_INTERBLOCK_OUTPUT");
    const uint32_t target = target_text != NULL ?
        (uint32_t)strtoul(target_text, NULL, 0) : 30u;
    const uint32_t output = output_text != NULL ?
        (uint32_t)strtoul(output_text, NULL, 0) : 0x00950640u;
    uint32_t header[34];
    char path[MAX_PATH * 4];
    FILE *dump;
    if (prefix == NULL || g_xbox_mem_offset == 0 || captured)
        return;
    if (stage == 0u) {
        if (g_recomp_xmv_decode_index == target &&
            getenv("MERCENARIES_TRACE_XMV_INTERBLOCK_ARGS") != NULL &&
            trace_calls < 64u) {
            const uint32_t state_array = guest_u32(g_esp + 13u * 4u);
            fprintf(stderr,
                    "[RETAIL-XMV-INTERBLOCK-ARGS] call=%u output=%08X state13_28=%08X\n",
                    trace_calls++, guest_u32(g_esp + 9u * 4u),
                    guest_u32(state_array + 28u));
        }
        if (g_recomp_xmv_decode_index != target || guest_u32(g_esp + 36u) != output)
            return;
        for (uint32_t index = 0u; index < 25u; ++index)
            arguments[index] = guest_u32(g_esp + index * 4u);
        active = 1;
    } else if (!active) {
        return;
    }
    header[0] = 0x002576F8u;
    header[1] = g_recomp_xmv_decode_index;
    header[2] = 25u;
    memcpy(header + 3, arguments, sizeof(arguments));
    header[28] = g_eax;
    header[29] = g_ecx;
    header[30] = g_edx;
    header[31] = g_ebx;
    header[32] = g_esi;
    header[33] = g_edi;
    snprintf(path, sizeof(path), "%s-f%u-%s.bin", prefix,
             g_recomp_xmv_decode_index, stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x00010000u),
           1u, 0x03FF0000u, dump);
    fclose(dump);
    fprintf(stderr,
            "[RETAIL-XMV-INTERBLOCK] frame=%u stage=%u output=%08X path=%s\n",
            g_recomp_xmv_decode_index, stage, arguments[9], path);
    fflush(stderr);
    if (stage != 0u) { active = 0; captured = 1; }
}
void recomp_xmv_block_checkpoint(uint32_t stage)
{
    static int dump_enabled = -1;
    if (dump_enabled < 0)
        dump_enabled = getenv("MERCENARIES_DUMP_XMV_BLOCK_PREFIX") != NULL;
    if (!dump_enabled)
        return;
    static int active;
    static int captured;
    static uint32_t trace_calls;
    static uint32_t arguments[15];
    const char *prefix = getenv("MERCENARIES_DUMP_XMV_BLOCK_PREFIX");
    const char *target_text = getenv("MERCENARIES_DUMP_XMV_BLOCK_FRAME");
    const char *output_text = getenv("MERCENARIES_DUMP_XMV_BLOCK_OUTPUT");
    const uint32_t target = target_text != NULL ? (uint32_t)strtoul(target_text, NULL, 0) : 30u;
    const uint32_t output = output_text != NULL ? (uint32_t)strtoul(output_text, NULL, 0) : 0x00950640u;
    uint32_t header[18];
    char path[MAX_PATH * 4];
    FILE *dump;
    if (prefix == NULL || g_xbox_mem_offset == 0 || captured)
        return;
    if (stage == 0u) {
        if (g_recomp_xmv_decode_index == target &&
            getenv("MERCENARIES_TRACE_XMV_BLOCK_ARGS") != NULL &&
            trace_calls < 2048u) {
            fprintf(stderr,
                    "[RETAIL-XMV-BLOCK-ARGS] call=%u a1=%08X a2=%08X a3=%08X a4=%08X a5=%08X a6=%08X\n",
                    trace_calls++, guest_u32(g_esp), guest_u32(g_esp + 4u),
                    guest_u32(g_esp + 8u), guest_u32(g_esp + 12u),
                    guest_u32(g_esp + 16u), guest_u32(g_esp + 20u));
        }
        if (g_recomp_xmv_decode_index != target || guest_u32(g_esp + 4u) != output)
            return;
        for (uint32_t index = 0u; index < 15u; ++index)
            arguments[index] = guest_u32(g_esp + index * 4u);
        active = 1;
    } else if (!active) {
        return;
    }
    header[0] = 0x002573C8u;
    header[1] = g_recomp_xmv_decode_index;
    header[2] = 15u;
    memcpy(header + 3, arguments, sizeof(arguments));
    snprintf(path, sizeof(path), "%s-f%u-%s.bin", prefix, g_recomp_xmv_decode_index, stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x00010000u), 1u, 0x03FF0000u, dump);
    fclose(dump);
    fprintf(stderr, "[RETAIL-XMV-BLOCK] frame=%u stage=%u output=%08X path=%s\n", g_recomp_xmv_decode_index, stage, arguments[1], path);
    fflush(stderr);
    if (stage != 0u) { active = 0; captured = 1; }
}
void recomp_xmv_block_final_trace(uint32_t eax, uint32_t ebx, uint32_t ecx,
                                  uint32_t edx, uint32_t esi, uint32_t edi,
                                  uint32_t ebp)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_XMV_BLOCK_FINAL") != NULL;
    if (!enabled)
        return;
    const char *output_text = getenv("MERCENARIES_TRACE_XMV_BLOCK_FINAL_OUTPUT");
    const uint32_t output = output_text != NULL ?
        (uint32_t)strtoul(output_text, NULL, 0) : 0x008DD4F8u;
    if (getenv("MERCENARIES_TRACE_XMV_BLOCK_FINAL") == NULL ||
        g_recomp_xmv_decode_index != 3u || guest_u32(ebp + 0xCu) != output)
        return;
    fprintf(stderr,
            "[RETAIL-XMV-BLOCK-FINAL] eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X round=%08X\n",
            eax, ebx, ecx, edx, esi, edi, ebp, guest_u32(ebp + 0x24u));
    for (uint32_t index = 1u; index < 8u; ++index)
        fprintf(stderr,
                "[RETAIL-XMV-BLOCK-FINAL] index=%u first=%08X second=%08X\n",
                index, guest_u32(ebx + edi + index * 4u),
                guest_u32(eax + edi + index * 4u));
    fflush(stderr);
}
void recomp_xmv_block_residual_trace(uint32_t site, uint32_t address,
                                     uint32_t old_value, uint32_t new_value,
                                     uint32_t ebp)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_XMV_BLOCK_RESIDUAL") != NULL;
    if (!enabled)
        return;
    const char *output_text = getenv("MERCENARIES_TRACE_XMV_BLOCK_RESIDUAL_OUTPUT");
    const uint32_t output = output_text != NULL ?
        (uint32_t)strtoul(output_text, NULL, 0) : 0x008DD4F8u;
    if (getenv("MERCENARIES_TRACE_XMV_BLOCK_RESIDUAL") == NULL ||
        g_recomp_xmv_decode_index != 3u || guest_u32(ebp + 0xCu) != output)
        return;
    fprintf(stderr,
            "[RETAIL-XMV-BLOCK-RESIDUAL] site=%08X address=%08X old=%08X new=%08X "
            "index=%08X run=%08X bit=%08X scale=%08X round=%08X\n",
            site, address, old_value, new_value,
            guest_u32(ebp + 0x34u), guest_u32(ebp + 0x1Cu),
            guest_u32(ebp + 0x30u), guest_u32(ebp + 0x14u),
            guest_u32(ebp + 0x24u));
    fflush(stderr);
}
void recomp_xmv_block_sign_trace(uint32_t result, uint32_t ebp)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_XMV_BLOCK_RESIDUAL") != NULL;
    if (!enabled)
        return;
    const uint32_t bitreader = guest_u32(ebp + 8u);
    const uint32_t width_pointer = guest_u32(ebp + 0x40u);
    const char *output_text = getenv("MERCENARIES_TRACE_XMV_BLOCK_RESIDUAL_OUTPUT");
    const uint32_t output = output_text != NULL ?
        (uint32_t)strtoul(output_text, NULL, 0) : 0x008DD4F8u;
    if (getenv("MERCENARIES_TRACE_XMV_BLOCK_RESIDUAL") == NULL ||
        g_recomp_xmv_decode_index != 3u || guest_u32(ebp + 0xCu) != output)
        return;
    fprintf(stderr,
            "[RETAIL-XMV-BLOCK-SIGN] result=%08X selector=%08X width=%08X "
            "reader=%08X bits=%08X available=%08X cursor=%08X\n",
            result, guest_u32(ebp + 0x1Cu), guest_u32(width_pointer),
            bitreader, guest_u32(bitreader), guest_u32(bitreader + 4u),
            guest_u32(bitreader + 8u));
    fflush(stderr);
}
void recomp_xmv_intra_checkpoint(uint32_t stage)
{
    static int active;
    static int captured;
    static uint32_t arguments[2];
    const char *prefix = getenv("MERCENARIES_DUMP_XMV_INTRA_PREFIX");
    const char *target_text = getenv("MERCENARIES_DUMP_XMV_INTRA_FRAME");
    const uint32_t target = target_text != NULL ? (uint32_t)strtoul(target_text, NULL, 0) : 3u;
    uint32_t header[5];
    char path[MAX_PATH * 4];
    FILE *dump;
    if (prefix == NULL || g_xbox_mem_offset == 0 || captured)
        return;
    if (stage == 0u) {
        if (g_recomp_xmv_decode_index != target)
            return;
        arguments[0] = guest_u32(g_esp);
        arguments[1] = guest_u32(g_esp + 4u);
        active = 1;
    } else if (!active) {
        return;
    }
    header[0] = 0x00257825u;
    header[1] = g_recomp_xmv_decode_index;
    header[2] = 2u;
    memcpy(header + 3, arguments, sizeof(arguments));
    snprintf(path, sizeof(path), "%s-f%u-%s.bin", prefix, g_recomp_xmv_decode_index, stage == 0u ? "before" : "after");
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    fwrite(header, 1u, sizeof(header), dump);
    fwrite((const void *)((uintptr_t)g_xbox_mem_offset + 0x00010000u), 1u, 0x03FF0000u, dump);
    fclose(dump);
    fprintf(stderr, "[RETAIL-XMV-INTRA] frame=%u stage=%u path=%s\n", g_recomp_xmv_decode_index, stage, path);
    fflush(stderr);
    if (stage != 0u) { active = 0; captured = 1; }
}
static void recomp_dump_xmv_planes(uint32_t decoder)
{
    static uint32_t dump_index;
    static uint32_t eligible_index;
    const char *prefix = getenv("MERCENARIES_DUMP_XMV_PLANES_PREFIX");
    const char *skip_text = getenv("MERCENARIES_DUMP_XMV_PLANES_SKIP");
    uint32_t skip = 0u;
    uint32_t macroblock_width;
    uint32_t macroblock_height;
    uint32_t width;
    uint32_t height;
    uint32_t y_plane;
    uint32_t u_plane;
    uint32_t v_plane;
    uint32_t y_stride;
    uint32_t chroma_stride;
    char path[MAX_PATH * 4];
    FILE *dump;

    if (prefix == NULL || prefix[0] == '\0' || dump_index >= 8u ||
        decoder < 0x00010000u || decoder > 0x03FFFF00u)
        return;
    if (skip_text != NULL && skip_text[0] != '\0')
        skip = (uint32_t)strtoul(skip_text, NULL, 0);
    ++eligible_index;
    if (eligible_index <= skip)
        return;

    macroblock_width = guest_u32(decoder + 0xDCu);
    macroblock_height = guest_u32(decoder + 0xE0u);
    y_plane = guest_u32(decoder + 0xECu);
    u_plane = guest_u32(decoder + 0xF0u);
    v_plane = guest_u32(decoder + 0xF4u);

    if (macroblock_width == 0u || macroblock_width > 128u ||
        macroblock_height == 0u || macroblock_height > 128u)
        return;
    width = macroblock_width * 16u;
    height = macroblock_height * 16u;
    y_stride = macroblock_width * 16u;
    chroma_stride = macroblock_width * 8u;
    if (y_plane < 0x00010000u || u_plane < 0x00010000u ||
        v_plane < 0x00010000u ||
        y_plane > 0x04000000u - y_stride * height ||
        u_plane > 0x04000000u - chroma_stride * (height / 2u) ||
        v_plane > 0x04000000u - chroma_stride * (height / 2u))
        return;

    ++dump_index;
    snprintf(path, sizeof(path), "%s-%04u-%ux%u.yuv", prefix,
             dump_index, width, height);
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;

    for (uint32_t row = 0u; row < height; ++row)
        fwrite((const void *)((uintptr_t)g_xbox_mem_offset + y_plane +
                              row * y_stride),
               1u, width, dump);
    for (uint32_t row = 0u; row < height / 2u; ++row)
        fwrite((const void *)((uintptr_t)g_xbox_mem_offset + u_plane +
                              row * chroma_stride),
               1u, width / 2u, dump);
    for (uint32_t row = 0u; row < height / 2u; ++row)
        fwrite((const void *)((uintptr_t)g_xbox_mem_offset + v_plane +
                              row * chroma_stride),
               1u, width / 2u, dump);
    fclose(dump);

    fprintf(stderr,
            "[RETAIL-XMV-PLANES] dump=%u path=%s mb=%ux%u y=%08X/%u "
            "u=%08X v=%08X uv-stride=%u\n",
            dump_index, path, macroblock_width, macroblock_height, y_plane,
            y_stride, u_plane, v_plane, chroma_stride);
    fflush(stderr);
}
static void recomp_dump_xmv_surface(uint32_t decoder, uint32_t surface,
                                    uint32_t pitch)
{
    static uint32_t eligible_index;
    static uint32_t dump_index;
    const char *prefix = getenv("MERCENARIES_DUMP_XMV_SURFACE_PREFIX");
    const char *skip_text = getenv("MERCENARIES_DUMP_XMV_SURFACE_SKIP");
    uint32_t skip = 0u;
    uint32_t width, height, data, row;
    char path[MAX_PATH * 4];
    FILE *dump;

    if (prefix == NULL || prefix[0] == '\0' || dump_index >= 8u ||
        decoder < 0x00010000u || decoder > 0x03FFFF00u ||
        surface < 0x00010000u || surface > 0x03FFFFF0u)
        return;
    if (skip_text != NULL && skip_text[0] != '\0')
        skip = (uint32_t)strtoul(skip_text, NULL, 0);
    ++eligible_index;
    if (eligible_index <= skip)
        return;

    width = guest_u32(decoder + 0xDCu) * 16u;
    height = guest_u32(decoder + 0xE0u) * 16u;
    data = guest_u32(surface + 4u);
    if (width == 0u || width > 2048u || height == 0u || height > 2048u ||
        pitch < width * 2u || pitch > 16384u ||
        data < 0x00010000u ||
        (uint64_t)data + (uint64_t)pitch * height > 0x04000000u)
        return;

    ++dump_index;
    snprintf(path, sizeof(path), "%s-%04u-%ux%u.yuy2", prefix,
             dump_index, width, height);
    dump = fopen(path, "wb");
    if (dump == NULL)
        return;
    for (row = 0u; row < height; ++row)
        fwrite((const void *)((uintptr_t)g_xbox_mem_offset + data +
                              (size_t)row * pitch),
               1u, width * 2u, dump);
    fclose(dump);
    fprintf(stderr,
            "[RETAIL-XMV-SURFACE-DUMP] frame=%u path=%s data=%08X "
            "pitch=%u\n",
            eligible_index, path, data, pitch);
    fflush(stderr);
}
void recomp_movie_checkpoint(uint32_t stage, uint32_t object,
                             uint32_t path_address, uint32_t value0,
                             uint32_t value1)
{
    static uint32_t capture_movie_object;
    static uint32_t capture_movie_decoder;
    static uint32_t capture_decoded_results;
    static int movie_capture_armed;
    static uint32_t stage6_object;
    static uint32_t stage6_zero_results;
    static uint32_t stage6_calls;
    static int diagnostic_skip_applied;
    static int decoder_arrays_dumped;
    static int apu_stream_dumped;
    static int noframe_trace_dumped;
    static uint32_t test_natural_movie_object;
    static ULONGLONG test_natural_movie_start_ms;
    static uint32_t test_skip_movie_object;
    static uint32_t test_skip_movie_calls;
    static uint32_t test_skip_movie_decoded;
    static int test_skip_movie_applied;
    const char *path = "<invalid>";

    if (stage == 1u &&
        path_address >= 0x00010000u && path_address < 0x04000000u &&
        strstr((const char *)((uintptr_t)g_xbox_mem_offset + path_address),
               "e3demo.xmv") != NULL) {
        if (getenv("MERCENARIES_TEST_AUTO_Y_AFTER_MOVIE") != NULL ||
            getenv("MERCENARIES_TRACE_SCRIPT_USE_AFTER_MOVIE") != NULL ||
            getenv("MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY") != NULL) {
            test_natural_movie_object = object;
            test_natural_movie_start_ms = GetTickCount64();
        }
        if (getenv("MERCENARIES_TEST_SKIP_MOVIE_AFTER_POLLS") != NULL) {
            test_skip_movie_object = object;
            test_skip_movie_calls = 0u;
            test_skip_movie_decoded = 0u;
            test_skip_movie_applied = 0;
        }
    } else if (stage == 9u && object == test_natural_movie_object &&
               object != 0u) {
        const ULONGLONG movie_duration_ms =
            GetTickCount64() - test_natural_movie_start_ms;
        test_natural_movie_object = 0u;
        fprintf(stderr,
                "[RETAIL-MOVIE-NATURAL-CLOSE] duration_ms=%llu\n",
                (unsigned long long)movie_duration_ms);
        fflush(stderr);
        if (getenv("MERCENARIES_TRACE_RAIN_AFTER_MOVIE") != NULL)
            SetEnvironmentVariableA("MERCENARIES_RUNTIME_MOVIE_CLOSED", "1");
        if (getenv("MERCENARIES_TRACE_XACT_CUES_AFTER_MOVIE") != NULL) {
            g_trace_xact_cues_after_movie_active = 1;
            fprintf(stderr, "[XACT-CUE] trace enabled after movie close\n");
            fflush(stderr);
        }
        if (getenv("MERCENARIES_TRACE_SCRIPT_USE_AFTER_MOVIE") != NULL ||
            getenv("MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY") != NULL) {
            g_recomp_entry_trace_enabled = 1u;
            fprintf(stderr, "[SCRIPT-USE-TRACE] enabled after movie close\n");
            fflush(stderr);
        }
        if (getenv("MERCENARIES_TEST_AUTO_Y_AFTER_USE_ONLY") == NULL &&
            getenv("MERCENARIES_TEST_AUTO_Y_AFTER_COLLISION_DISABLED") == NULL)
            xbox_InputArmTestAutoY();
        return;
    }

    if (stage == 6u && object == test_skip_movie_object && object != 0u &&
        !test_skip_movie_applied) {
        const char *skip_text =
            getenv("MERCENARIES_TEST_SKIP_MOVIE_AFTER_POLLS");
        const unsigned long skip_poll = skip_text != NULL ?
            strtoul(skip_text, NULL, 0) : 0u;
        ++test_skip_movie_calls;
        if (value0 == 1u)
            ++test_skip_movie_decoded;
        if (skip_poll != 0u && test_skip_movie_decoded != 0u &&
            test_skip_movie_calls >= skip_poll) {
            *(volatile uint8_t *)guest_ptr(object + 0x40u) = 0u;
            test_skip_movie_applied = 1;
            fprintf(stderr,
                    "[RETAIL-MOVIE-TEST-SKIP] object=%08X poll=%u "
                    "decoded=%u lightweight=1\n",
                    object, test_skip_movie_calls, test_skip_movie_decoded);
            fflush(stderr);
        }
    }


    if (stage == 6u && value0 == 1u &&
        getenv("MERCENARIES_TRACE_XMV_SCHEDULER") != NULL) {
        static uint32_t scheduler_object;
        static uint32_t scheduler_previous_target;
        static ULONGLONG scheduler_previous_wall_ms;
        static ULONGLONG scheduler_period_ms;
        static uint32_t scheduler_frames;
        static uint64_t scheduler_target_delta_sum;
        static uint32_t scheduler_target_delta_min;
        static uint32_t scheduler_target_delta_max;
        static uint32_t scheduler_target_zero_deltas;
        static uint32_t scheduler_target_large_deltas;
        static uint64_t scheduler_wall_gap_sum;
        static uint32_t scheduler_wall_gap_max;
        static int32_t scheduler_skew_min;
        static int32_t scheduler_skew_max;
        const uint32_t decoder = path_address;
        if (decoder >= 0x00010000u && decoder <= 0x04000000u - 0xFCu) {
            const uint32_t device = guest_u32(0x00299378u);
            const uint32_t pts = guest_u32(decoder + 0xE4u);
            const uint32_t pending_pts = guest_u32(decoder + 0xF8u);
            const uint32_t offset = guest_u32(decoder + 0xC8u);
            const uint32_t rate = guest_u32(decoder + 0xBCu);
            const uint32_t base = guest_u32(decoder + 0xB8u);
            const uint32_t clock = device >= 0x00010000u &&
                device <= 0x04000000u - 0x1DECu ?
                guest_u32(device + 0x1DE8u) : 0u;
            const uint32_t elapsed = clock - base;
            const uint32_t biased_pts = pts + offset;
            const uint32_t target = rate != 0u ?
                (uint32_t)(((uint64_t)biased_pts * 0x10000ull + 0xFFFFull) /
                           (uint64_t)rate) : 0u;
            const int32_t skew = (int32_t)(target - elapsed);
            const ULONGLONG wall_ms = GetTickCount64();
            if (object != scheduler_object) {
                scheduler_object = object;
                scheduler_previous_target = target;
                scheduler_previous_wall_ms = wall_ms;
                scheduler_period_ms = wall_ms;
                scheduler_frames = 0u;
                scheduler_target_delta_sum = 0u;
                scheduler_target_delta_min = UINT32_MAX;
                scheduler_target_delta_max = 0u;
                scheduler_target_zero_deltas = 0u;
                scheduler_target_large_deltas = 0u;
                scheduler_wall_gap_sum = 0u;
                scheduler_wall_gap_max = 0u;
                scheduler_skew_min = INT32_MAX;
                scheduler_skew_max = INT32_MIN;
            } else {
                const uint32_t target_delta = target - scheduler_previous_target;
                const uint32_t wall_gap = (uint32_t)(wall_ms - scheduler_previous_wall_ms);
                scheduler_target_delta_sum += target_delta;
                if (target_delta < scheduler_target_delta_min)
                    scheduler_target_delta_min = target_delta;
                if (target_delta > scheduler_target_delta_max)
                    scheduler_target_delta_max = target_delta;
                if (target_delta == 0u)
                    ++scheduler_target_zero_deltas;
                if (target_delta > 50u)
                    ++scheduler_target_large_deltas;
                scheduler_wall_gap_sum += wall_gap;
                if (wall_gap > scheduler_wall_gap_max)
                    scheduler_wall_gap_max = wall_gap;
            }
            if (skew < scheduler_skew_min)
                scheduler_skew_min = skew;
            if (skew > scheduler_skew_max)
                scheduler_skew_max = skew;
            ++scheduler_frames;
            scheduler_previous_target = target;
            scheduler_previous_wall_ms = wall_ms;
            if (wall_ms - scheduler_period_ms >= 1000u) {
                const uint32_t intervals = scheduler_frames > 1u ?
                    scheduler_frames - 1u : 0u;
                fprintf(stderr,
                        "[XMV-SCHED] frames=%u target_delta=%llu/%u/%u "
                        "zero=%u large=%u wall_gap=%llu/%u skew=%d/%d "
                        "pts=%u pending=%u offset=%u rate=%u target=%u "
                        "elapsed=%u\n",
                        scheduler_frames,
                        (unsigned long long)(intervals != 0u ?
                            scheduler_target_delta_sum / intervals : 0u),
                        scheduler_target_delta_min == UINT32_MAX ? 0u :
                            scheduler_target_delta_min,
                        scheduler_target_delta_max,
                        scheduler_target_zero_deltas,
                        scheduler_target_large_deltas,
                        (unsigned long long)(intervals != 0u ?
                            scheduler_wall_gap_sum / intervals : 0u),
                        scheduler_wall_gap_max,
                        scheduler_skew_min, scheduler_skew_max,
                        pts, pending_pts, offset, rate, target, elapsed);
                fflush(stderr);
                scheduler_period_ms = wall_ms;
                scheduler_frames = 0u;
                scheduler_target_delta_sum = 0u;
                scheduler_target_delta_min = UINT32_MAX;
                scheduler_target_delta_max = 0u;
                scheduler_target_zero_deltas = 0u;
                scheduler_target_large_deltas = 0u;
                scheduler_wall_gap_sum = 0u;
                scheduler_wall_gap_max = 0u;
                scheduler_skew_min = INT32_MAX;
                scheduler_skew_max = INT32_MIN;
            }
        }
    }
    if (stage == 6u &&
        getenv("MERCENARIES_TRACE_MOVIE_CADENCE") != NULL) {
        static uint32_t cadence_object;
        static ULONGLONG cadence_period_ms;
        static ULONGLONG cadence_last_frame_ms;
        static uint32_t cadence_calls;
        static uint32_t cadence_results[4];
        static uint32_t cadence_other_results;
        static uint32_t cadence_max_frame_gap_ms;
        static ULONGLONG cadence_last_call_ms;
        static uint32_t cadence_stalls;
        const ULONGLONG now_ms = GetTickCount64();
        const uint32_t result = value0;

        if (object != cadence_object) {
            cadence_object = object;
            cadence_period_ms = now_ms;
            cadence_last_frame_ms = 0u;
            cadence_calls = 0u;
            memset(cadence_results, 0, sizeof(cadence_results));
            cadence_other_results = 0u;
            cadence_max_frame_gap_ms = 0u;
            cadence_last_call_ms = 0u;
            cadence_stalls = 0u;
        }
        if (cadence_last_call_ms != 0u &&
            now_ms - cadence_last_call_ms > 80u && cadence_stalls++ < 128u) {
            const ULONGLONG call_gap_ms = now_ms - cadence_last_call_ms;
            const uint32_t decoder = path_address;
            const int decoder_valid = decoder >= 0x00010000u &&
                                      decoder <= 0x04000000u - 0xFCu;
            fprintf(stderr,
                    "[MOVIE-STALL] object=%08X decoder=%08X gap_ms=%llu "
                    "result=%u state=%08X/%08X/%08X/%08X "
                    "pts=%08X ready=%08X next=%08X queue=%08X/%08X\n",
                    object, decoder, (unsigned long long)call_gap_ms, result,
                    decoder_valid ? guest_u32(decoder + 0xB8u) : 0u,
                    decoder_valid ? guest_u32(decoder + 0xBCu) : 0u,
                    decoder_valid ? guest_u32(decoder + 0xC0u) : 0u,
                    decoder_valid ? guest_u32(decoder + 0xC4u) : 0u,
                    decoder_valid ? guest_u32(decoder + 0xE4u) : 0u,
                    decoder_valid ? guest_u32(decoder + 0xE8u) : 0u,
                    decoder_valid ? guest_u32(decoder + 0xF8u) : 0u,
                    decoder_valid ? guest_u32(decoder + 0x94u) : 0u,
                    decoder_valid ? guest_u32(decoder + 0x98u) : 0u);
            fflush(stderr);
        }
        cadence_last_call_ms = now_ms;
        ++cadence_calls;
        if (result < 4u)
            ++cadence_results[result];
        else
            ++cadence_other_results;
        if (result == 1u) {
            if (cadence_last_frame_ms != 0u) {
                const ULONGLONG gap = now_ms - cadence_last_frame_ms;
                if (gap > cadence_max_frame_gap_ms)
                    cadence_max_frame_gap_ms = (uint32_t)gap;
            }
            cadence_last_frame_ms = now_ms;
        }
        if (now_ms - cadence_period_ms >= 1000u) {
            fprintf(stderr,
                    "[MOVIE-CADENCE] object=%08X decoder=%08X calls=%u "
                    "results=%u/%u/%u/%u other=%u max_frame_gap_ms=%u "
                    "queued=%08X/%08X\n",
                    object, path_address, cadence_calls,
                    cadence_results[0], cadence_results[1],
                    cadence_results[2], cadence_results[3],
                    cadence_other_results, cadence_max_frame_gap_ms,
                    path_address ? guest_u32(path_address + 0x94u) : 0u,
                    path_address ? guest_u32(path_address + 0x98u) : 0u);
            cadence_period_ms = now_ms;
            cadence_calls = 0u;
            memset(cadence_results, 0, sizeof(cadence_results));
            cadence_other_results = 0u;
            cadence_max_frame_gap_ms = 0u;
        }
    }

    if (getenv("MERCENARIES_TRACE_MOVIE") == NULL) {
        /* Test automation must be able to release its synthetic A presses as
         * soon as the requested movie opens without enabling the heavyweight
         * per-frame movie tracer. */
        if (stage == 1u) {
            const char *auto_a_stop_match =
                getenv("MERCENARIES_TEST_AUTO_A_STOP_MOVIE_MATCH");
            const char *test_path = path_address >= 0x00010000u &&
                path_address < 0x04000000u ?
                (const char *)((uintptr_t)g_xbox_mem_offset + path_address) :
                "<invalid>";
            if (getenv("MERCENARIES_TEST_AUTO_A_IGNORE_MOVIES") == NULL &&
                (auto_a_stop_match == NULL || auto_a_stop_match[0] == '\0' ||
                 strstr(test_path, auto_a_stop_match) != NULL))
                recomp_release_test_auto_a();
        }
        return;
    }
    if (stage == 4u || stage == 5u) {
        static uint32_t samples;
        static uint32_t last_menu = UINT32_MAX;
        static uint32_t last_next = UINT32_MAX;
        static uint32_t last_current = UINT32_MAX;
        const uint32_t menu = path_address;
        int32_t highlight = -2;
        uint32_t item_count = 0u;
        uint32_t item = 0u;
        if (menu >= 0x00010000u && menu <= 0x03FFFFB0u) {
            highlight = (int32_t)guest_u32(menu + 0x48u);
            item_count = guest_u32(menu + 0x44u);
            if (highlight >= 0 && (uint32_t)highlight < item_count &&
                (uint32_t)highlight < 16u)
                item = guest_u32(menu + 4u + (uint32_t)highlight * 4u);
        }
        if (samples < 16u || menu != last_menu || value0 != last_next ||
            value1 != last_current) {
            fprintf(stderr,
                    "[RETAIL-MOVIE] %s owner=%08X menu=%08X "
                    "vtable=%08X items=%u highlight=%d item=%08X "
                    "current=%08X next=%08X wait=%u looping=%u frontend=%u\n",
                    stage == 4u ? "background" : "update", object, menu,
                    menu ? guest_u32(menu) : 0u, item_count,
                    highlight, item, value1, value0,
                    (unsigned int)guest_u8(object + 6u),
                    (unsigned int)guest_u8(object + 5u),
                    stage == 4u ? guest_u32(object + 0x3EC4u) : value0);
            fflush(stderr);
            ++samples;
            last_menu = menu;
            last_next = value0;
            last_current = value1;
        }
        return;
    }
    if (stage == 6u) {
        static uint32_t samples;
        const uint32_t decoder = path_address;
        const uint32_t result = value0;
        static uint32_t recent_dumps;
        if (object != stage6_object) {
            stage6_object = object;
            stage6_zero_results = 0u;
            stage6_calls = 0u;
            diagnostic_skip_applied = 0;
            decoder_arrays_dumped = 0;
            apu_stream_dumped = 0;
            noframe_trace_dumped = 0;
        }
        if (result == 0u)
            ++stage6_zero_results;
        else
            stage6_zero_results = 0u;
        ++stage6_calls;
        if (result == 1u && object == capture_movie_object)
            ++capture_decoded_results;
        if (!movie_capture_armed && capture_decoded_results != 0u) {
            const char *capture_poll_text =
                getenv("MERCENARIES_CAPTURE_MOVIE_POLL");
            const char *capture_path =
                getenv("MERCENARIES_CAPTURE_MOVIE_PATH");
            if (capture_poll_text != NULL && capture_poll_text[0] != '\0' &&
                capture_path != NULL && capture_path[0] != '\0') {
                const unsigned long capture_poll =
                    strtoul(capture_poll_text, NULL, 0);
                if (capture_poll != 0u && stage6_calls >= capture_poll) {
                    d3d8_DebugArmFlipCapture(capture_path);
                    movie_capture_armed = 1;
                    fprintf(stderr,
                            "[RETAIL-MOVIE-CAPTURE-POLL] object=%08X "
                            "poll=%u decoded=%u path=%s\n",
                            object, stage6_calls, capture_decoded_results,
                            capture_path);
                    fflush(stderr);
                }
            }
        }
        if (!diagnostic_skip_applied && capture_decoded_results != 0u) {
            const char *skip_poll_text =
                getenv("MERCENARIES_TEST_SKIP_MOVIE_AFTER_POLLS");
            if (skip_poll_text != NULL && skip_poll_text[0] != '\0') {
                const unsigned long skip_poll =
                    strtoul(skip_poll_text, NULL, 0);
                if (skip_poll != 0u && stage6_calls >= skip_poll) {
                    *(volatile uint8_t *)guest_ptr(object + 0x40u) = 0u;
                    diagnostic_skip_applied = 1;
                    fprintf(stderr,
                            "[RETAIL-MOVIE-TEST-SKIP] object=%08X "
                            "poll=%u decoded=%u\n",
                            object, stage6_calls, capture_decoded_results);
                    fflush(stderr);
                }
            }
        }
        if (result == 1u && object == capture_movie_object &&
            !movie_capture_armed) {
            const char *capture_path =
                getenv("MERCENARIES_CAPTURE_MOVIE_PATH");
            const char *capture_frame_text =
                getenv("MERCENARIES_CAPTURE_MOVIE_FRAME");
            uint32_t capture_frame = 12u;
            if (capture_frame_text != NULL && capture_frame_text[0] != '\0') {
                const unsigned long parsed = strtoul(capture_frame_text, NULL, 0);
                if (parsed > 0u && parsed <= UINT32_MAX)
                    capture_frame = (uint32_t)parsed;
            }
            if (capture_path != NULL && capture_path[0] != '\0' &&
                capture_decoded_results >= capture_frame) {
                d3d8_DebugArmFlipCapture(capture_path);
                movie_capture_armed = 1;
                fprintf(stderr,
                        "[RETAIL-MOVIE-CAPTURE] decoded-result=%u path=%s\n",
                        capture_decoded_results, capture_path);
                fflush(stderr);
            }
        }
        if (result == 1u && recent_dumps < 3u) {
            const uint32_t recent_index = g_recomp_recent_func_idx;
            fprintf(stderr, "[RETAIL-XMV-RECENT] frame=%u index=%u\n",
                    recent_dumps + 1u, recent_index);
            for (uint32_t i = 0u; i < 64u && i < recent_index; ++i) {
                const uint32_t index = (recent_index - 1u - i) & 63u;
                fprintf(stderr, "  xmv-recent[-%u]=%08X\n", i,
                        g_recomp_recent_funcs[index]);
            }
            fflush(stderr);
            ++recent_dumps;
        }
        if (samples < 128u) {
            const uint32_t device = guest_u32(0x00299378u);
            fprintf(stderr,
                    "[RETAIL-MOVIE-FRAME] object=%08X decoder=%08X "
                    "surface=%08X result=%u playing=%u decoder_state="
                    "%08X/%08X/%08X/%08X pending=%08X/%08X "
                    "file=%08X iosb=%08X/%08X queued=%08X/%08X\n",
                    object, decoder, value1, result,
                    (unsigned int)guest_u8(object + 0x40u),
                    decoder ? guest_u32(decoder + 0x50u) : 0u,
                    decoder ? guest_u32(decoder + 0x60u) : 0u,
                    decoder ? guest_u32(decoder + 0x68u) : 0u,
                    decoder ? guest_u32(decoder + 0x74u) : 0u,
                    decoder ? guest_u32(decoder + 0x64u) : 0u,
                    decoder ? guest_u32(decoder + 0x6Cu) : 0u,
                    decoder ? guest_u32(decoder + 0x7Cu) : 0u,
                    decoder ? guest_u32(decoder + 0x8Cu) : 0u,
                    decoder ? guest_u32(decoder + 0x90u) : 0u,
                    decoder ? guest_u32(decoder + 0x94u) : 0u,
                    decoder ? guest_u32(decoder + 0x98u) : 0u);
            fprintf(stderr,
                    "  [RETAIL-MOVIE-CLOCK] device=%08X field=%08X "
                    "flags=%08X source=%08X status=%08X streams=%08X "
                    "clock=%08X/%08X/%08X/%08X pts=%08X ready=%08X "
                    "next=%08X\n",
                    device, device ? guest_u32(device + 0x1DE8u) : 0u,
                    device ? guest_u32(device + 0x1DDCu) : 0u,
                    device ? guest_u32(device + 0x1DF8u) : 0u,
                    decoder ? guest_u32(decoder + 0x3Cu) : 0u,
                    decoder ? guest_u32(decoder + 0x48u) : 0u,
                    decoder ? guest_u32(decoder + 0xB8u) : 0u,
                    decoder ? guest_u32(decoder + 0xBCu) : 0u,
                    decoder ? guest_u32(decoder + 0xC0u) : 0u,
                    decoder ? guest_u32(decoder + 0xC4u) : 0u,
                    decoder ? guest_u32(decoder + 0xE4u) : 0u,
                    decoder ? guest_u32(decoder + 0xE8u) : 0u,
                    decoder ? guest_u32(decoder + 0xF8u) : 0u);
            fflush(stderr);
            ++samples;
        }
        if (!decoder_arrays_dumped && decoder && stage6_zero_results >= 8u) {
            static const uint32_t offsets[] = {
                0x134u, 0x138u, 0x13Cu, 0x140u, 0x144u,
                0x148u, 0x14Cu, 0x150u, 0x154u, 0x158u, 0x15Cu
            };
            fprintf(stderr, "[RETAIL-XMV-AUDIO-ARRAYS] decoder=%08X count=%u",
                    decoder, guest_u32(decoder + 0x48u));
            for (uint32_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
                const uint32_t pointer = guest_u32(decoder + offsets[i]);
                const uint32_t first = pointer >= 0x00010000u &&
                    pointer <= 0x03FFFFFCu ? guest_u32(pointer) : 0u;
                fprintf(stderr, " +%03X=%08X[%08X]", offsets[i], pointer, first);
            }
            {
                const uint32_t streams = guest_u32(decoder + 0x134u);
                const uint32_t stream = streams >= 0x00010000u &&
                    streams <= 0x03FFFFFCu ? guest_u32(streams) : 0u;
                const uint32_t vtable = stream >= 0x00010000u &&
                    stream <= 0x03FFFFFCu ? guest_u32(stream) : 0u;
                fprintf(stderr,
                        " stream=%08X vtable=%08X process=%08X flush=%08X",
                        stream, vtable,
                        vtable >= 0x00010000u && vtable <= 0x03FFFFE8u ?
                            guest_u32(vtable + 0x10u) : 0u,
                        vtable >= 0x00010000u && vtable <= 0x03FFFFE8u ?
                            guest_u32(vtable + 0x14u) : 0u);
            }            fprintf(stderr, "\n");
            fflush(stderr);
            decoder_arrays_dumped = 1;
            if (getenv("MERCENARIES_TRACE_APU_STREAM") != NULL) {
                mcpx_apu_debug_dump_stream_voices();
                apu_stream_dumped = 1;
            }
        }
        if (!apu_stream_dumped && stage6_zero_results >= 32u &&
            getenv("MERCENARIES_TRACE_APU_STREAM") != NULL) {
            mcpx_apu_debug_dump_stream_voices();
            apu_stream_dumped = 1;
        }
        if (result == 0u &&
            getenv("MERCENARIES_TRACE_APU_STREAM_PERIODIC") != NULL &&
            (stage6_zero_results == 8u || stage6_zero_results == 32u ||
             stage6_zero_results == 64u || stage6_zero_results == 96u ||
             stage6_zero_results == 128u)) {
            mcpx_apu_debug_dump_progress();
            mcpx_apu_debug_dump_stream_voices();
        }
        if (!noframe_trace_dumped && stage6_zero_results >= 32u &&
            getenv("MERCENARIES_TRACE_XMV_FUNCTIONS") != NULL) {
            const uint32_t recent_index = g_recomp_recent_func_idx;
            fprintf(stderr, "[RETAIL-XMV-NOFRAME-PATH] index=%u\n",
                    recent_index);
            for (uint32_t i = 0u; i < 64u && i < recent_index; ++i) {
                const uint32_t index = (recent_index - 1u - i) & 63u;
                fprintf(stderr, "  noframe-recent[-%u]=%08X\n", i,
                        g_recomp_recent_funcs[index]);
            }
            recomp_dump_xmv_func_histogram();
            fflush(stderr);
            noframe_trace_dumped = 1;
        }
        return;
    }

    if (stage == 7u || stage == 8u) {
        static uint32_t samples;
        const uint32_t bits = value1;
        if (stage == 7u) {
            ++g_recomp_xmv_decode_index;
            recomp_dump_xmv_func_histogram();
            static uint32_t path_dumps;
            if (path_dumps < 3u) {
                const uint32_t recent_index = g_recomp_recent_func_idx;
                fprintf(stderr,
                        "[RETAIL-XMV-DECODE-PATH] frame=%u index=%u\n",
                        path_dumps + 1u, recent_index);
                for (uint32_t i = 0u; i < 64u && i < recent_index; ++i) {
                    const uint32_t index = (recent_index - 1u - i) & 63u;
                    fprintf(stderr, "  decode-recent[-%u]=%08X\n", i,
                            g_recomp_recent_funcs[index]);
                }
                fflush(stderr);
                ++path_dumps;
            }
            if (getenv("MERCENARIES_DUMP_XMV_CAPTURE_ONLY") == NULL ||
                object == capture_movie_decoder)
                recomp_dump_xmv_planes(object);
        } else if (object == capture_movie_decoder) {
            recomp_dump_xmv_surface(object, path_address, bits);
        }
        if (samples < 16u) {
            const uint32_t surface_data = guest_u32(path_address + 4u);
            const uint32_t macroblock_width = guest_u32(object + 0xDCu);
            const uint32_t macroblock_height = guest_u32(object + 0xE0u);
            const uint64_t surface_size64 =
                (uint64_t)macroblock_width * 16u *
                (uint64_t)macroblock_height * 16u * 2u;
            uint32_t surface_hash = 2166136261u;
            uint32_t surface_nonzero = 0u;
            if (surface_data >= 0x00010000u &&
                surface_size64 <= UINT32_MAX &&
                (uint64_t)surface_data + surface_size64 <= 0x04000000u) {
                const uint8_t *surface_bytes =
                    (const uint8_t *)((uintptr_t)g_xbox_mem_offset +
                                      surface_data);
                for (uint32_t i = 0u; i < (uint32_t)surface_size64; ++i) {
                    surface_hash ^= surface_bytes[i];
                    surface_hash *= 16777619u;
                    surface_nonzero += surface_bytes[i] != 0u;
                }
            }
            fprintf(stderr,
                    "[RETAIL-MOVIE-SURFACE] phase=%s data=%08X "
                    "size=%llu hash=%08X nonzero=%u locked=%08X/%08X\n",
                    stage == 7u ? "before" : "after", surface_data,
                    (unsigned long long)surface_size64, surface_hash,
                    surface_nonzero, value0, value1);
            fprintf(stderr,
                    "[RETAIL-MOVIE-DECODE] phase=%s decoder=%08X "
                    "surface=%08X resource=%08X/%08X/%08X/%08X "
                    "decode=%08X/%08X/%08X/%08X/%08X "
                    "%s=%08X bits=%08X first=%08X/%08X\n",
                    stage == 7u ? "before" : "after", object, path_address,
                    guest_u32(path_address), guest_u32(path_address + 4u),
                    guest_u32(path_address + 8u), guest_u32(path_address + 0xCu),
                    guest_u32(object + 0xDCu), guest_u32(object + 0xE0u),
                    guest_u32(object + 0xECu), guest_u32(object + 0xF0u),
                    guest_u32(object + 0xF4u),
                    stage == 7u ? "format" : "pitch", value0, bits,
                    bits ? guest_u32(bits) : 0u,
                    bits ? guest_u32(bits + 4u) : 0u);
            fflush(stderr);
            ++samples;
        }
        return;
    }

    if (stage >= 10u && stage <= 24u) {
        static uint32_t movie_texture_samples;
        const int brush_valid = object >= 0x00010000u &&
                                object <= 0x04000000u - 0x60u;
        const int pointer_valid = path_address >= 0x00010000u &&
                                  path_address <= 0x04000000u - 0x68u;
        int movie_relevant = 1;
        if (stage == 10u || stage == 12u || stage == 13u || stage == 14u)
            movie_relevant = value0 == 0x1D6FB11Au;
        else if (stage == 11u)
            movie_relevant = brush_valid &&
                             guest_u32(object + 0x24u) == 0x1D6FB11Au;
        else if (stage == 15u)
            movie_relevant = brush_valid &&
                             guest_u32(object + 0x58u) == 0x1D6FB11Au;
        else if (stage == 21u || stage == 22u || stage == 24u)
            movie_relevant = brush_valid &&
                             guest_u32(object + 0x24u) == 0x1D6FB11Au;
        if (!movie_relevant)
            return;
        if (movie_texture_samples++ < 128u) {
            fprintf(stderr,
                    "[RETAIL-MOVIE-TEXTURE] stage=%u brush=%08X "
                    "pointer=%08X hash=%08X target=%08X "
                    "brush_vt=%08X bound=%08X/%08X "
                    "resource=%08X/%08X/%08X/%08X/%08X "
                    "context=%08X/%08X/%08X/%08X/%08X\n",
                    stage, object, path_address, value0, value1,
                    brush_valid ? guest_u32(object) : 0u,
                    brush_valid ? guest_u32(object + 0x24u) : 0u,
                    brush_valid ? guest_u32(object + 0x44u) : 0u,
                    pointer_valid ? guest_u32(path_address + 0u) : 0u,
                    pointer_valid ? guest_u32(path_address + 4u) : 0u,
                    pointer_valid ? guest_u32(path_address + 8u) : 0u,
                    pointer_valid ? guest_u32(path_address + 0xCu) : 0u,
                    pointer_valid ? guest_u32(path_address + 0x10u) : 0u,
                    pointer_valid ? guest_u32(path_address + 0x48u) : 0u,
                    pointer_valid ? guest_u32(path_address + 0x4Cu) : 0u,
                    pointer_valid ? guest_u32(path_address + 0x50u) : 0u,
                    pointer_valid ? guest_u32(path_address + 0x54u) : 0u,
                    pointer_valid ? guest_u32(path_address + 0x64u) : 0u);
            fflush(stderr);
        }
        return;
    }

    if (path_address >= 0x00010000u && path_address < 0x04000000u)
        path = (const char *)((uintptr_t)g_xbox_mem_offset + path_address);
    if (stage == 1u) {
        const char *auto_a_stop_match =
            getenv("MERCENARIES_TEST_AUTO_A_STOP_MOVIE_MATCH");
        if (getenv("MERCENARIES_TEST_AUTO_A_IGNORE_MOVIES") == NULL &&
            (auto_a_stop_match == NULL || auto_a_stop_match[0] == '\0' ||
             strstr(path, auto_a_stop_match) != NULL))
            recomp_release_test_auto_a();
        const char *capture_match =
            getenv("MERCENARIES_CAPTURE_MOVIE_MATCH");
        if (capture_match == NULL || capture_match[0] == '\0' ||
            strstr(path, capture_match) != NULL) {
            capture_movie_object = object;
            capture_movie_decoder = 0u;
            capture_decoded_results = 0u;
            movie_capture_armed = 0;
        }
        fprintf(stderr,
                "[RETAIL-MOVIE] init object=%08X path_va=%08X path=%.240s "
                "audio=%u loop=%u\n",
                object, path_address, path, value0, value1);
    } else if (stage == 2u) {
        if (object == capture_movie_object && value0 == 0u)
            capture_movie_decoder = value1;
        fprintf(stderr,
                "[RETAIL-MOVIE] decoder-create object=%08X path=%.240s "
                "hr=%08X decoder=%08X\n",
                object, path, value0, value1);
    } else {
        fprintf(stderr,
                "[RETAIL-MOVIE] ready object=%08X path=%.240s decoder=%08X "
                "texture=%08X video=%ux%u playing=%u\n",
                object, path, value0, value1,
                guest_u32(object + 0x18u), guest_u32(object + 0x1Cu),
                (unsigned int)guest_u8(object + 0x40u));
    }
    fflush(stderr);
}

void recomp_brush3d_checkpoint(uint32_t stage, uint32_t pointer)
{
    static uint32_t stage_samples[4];
    static uint32_t brush2d_texture_samples;
    uint32_t low;

    if (stage == 4u) {
        if (g_menu_text_trace_armed != 0u &&
            getenv("MERCENARIES_TRACE_BRUSH2D_MENU") != NULL &&
            brush2d_texture_samples++ < 128u) {
            const uint32_t hash = guest_u32(g_esi + 8u);
            const uint32_t d3d_texture = pointer != 0u ? guest_u32(pointer + 0x44u) : 0u;
            fprintf(stderr,
                    "[BRUSH2D-TEXTURE] sample=%u prim=%08X hash=%08X "
                    "object=%08X d3d=%08X format=%08X size=%08X data=%08X "
                    "dims=%08X flags=%02X\n",
                    brush2d_texture_samples, g_esi, hash, pointer, d3d_texture,
                    d3d_texture != 0u ? guest_u32(d3d_texture + 0x0Cu) : 0u,
                    d3d_texture != 0u ? guest_u32(d3d_texture + 0x10u) : 0u,
                    d3d_texture != 0u ? guest_u32(d3d_texture + 0x04u) : 0u,
                    pointer != 0u ? guest_u32(pointer + 0x58u) : 0u,
                    pointer != 0u ? guest_u8(pointer + 0x5Cu) : 0u);
            fflush(stderr);
        }
        return;
    }

    if (getenv("MERCENARIES_TRACE_BRUSH3D_PAINT") == NULL || stage > 3u ||
        stage_samples[stage] >= (stage == 2u ? 256u : 16u))
        return;
    low = pointer & 0x07FFFFFFu;
    fprintf(stderr,
            "[BRUSH3D-PAINT] #%u stage=%u pointer=%08X low=%08X "
            "active=%u first=%08X cur=%08X end=%08X prims=%u\n",
            stage_samples[stage]++, stage, pointer, low,
            guest_u32(0x7AC820u),
            guest_u32(0x7AC7C0u), guest_u32(0x7AC7C4u),
            guest_u32(0x7AC7A4u), guest_u32(0x7AC7D4u));
    if (low >= 0x00010000u && low < 0x04000000u) {
        fprintf(stderr,
                "  data=%08X %08X %08X %08X %08X %08X\n",
                guest_u32(low + 0u), guest_u32(low + 4u),
                guest_u32(low + 8u), guest_u32(low + 12u),
                guest_u32(low + 16u), guest_u32(low + 20u));
    }
    fflush(stderr);
}

void recomp_brush2d_menu_checkpoint(void)
{
    static uint32_t samples;
    const uint32_t primitive_base = guest_u32(0x7AC810u);
    const uint32_t primitive_count = guest_u32(0x7AC814u);
    const uint32_t begin = primitive_count > 24u ? primitive_count - 24u : 0u;

    if (g_menu_text_trace_armed == 0u ||
        getenv("MERCENARIES_TRACE_BRUSH2D_MENU") == NULL || samples++ >= 16u)
        return;

    fprintf(stderr,
            "[BRUSH2D-MENU] sample=%u draw2d=%u buffer=%u "
            "vb=%08X first=%08X cur=%08X last=%08X primbase=%08X prims=%u\n",
            samples, guest_u32(0x30F1BCu), guest_u32(0x7AC820u),
            guest_u32(guest_u32(0x7AC820u) * 4u + 0x7AC808u),
            guest_u32(0x7AC800u), guest_u32(0x7AC804u),
            guest_u32(0x7AC7E8u), primitive_base, primitive_count);
    for (uint32_t i = begin; i < primitive_count && i < 2500u; ++i) {
        const uint32_t record = primitive_base + i * 0x1Cu;
        fprintf(stderr,
                "  prim[%u]=%08X type=%u flags=%08X hash=%08X color=%08X "
                "start=%d count=%d clip=%d,%d,%d,%d\n",
                i, record, guest_u32(record), guest_u32(record + 4u),
                guest_u32(record + 8u), guest_u32(record + 12u),
                (int16_t)guest_u16(record + 0x18u),
                (int16_t)guest_u16(record + 0x1Au),
                (int16_t)guest_u16(record + 0x10u),
                (int16_t)guest_u16(record + 0x12u),
                (int16_t)guest_u16(record + 0x14u),
                (int16_t)guest_u16(record + 0x16u));
    }
    fflush(stderr);
}

void recomp_d3d_set_vertex_shader(uint32_t shader)
{
    static uint32_t trace_calls;
    static uint32_t last_shader = UINT32_MAX;

    if (getenv("MERCENARIES_TRACE_VERTEX_SHADER_RECORDS") != NULL &&
        shader != last_shader) {
        PgraphD3D11Stats stats;

        pgraph_d3d11_get_stats(&stats);
        fprintf(stderr,
                "[D3D-VSH-RECORD] call=%u draw=%u shader=%08X kind=%s\n",
                trace_calls, stats.draw_calls, shader,
                shader & 1u ? "handle" : "fvf");
        if ((shader & 1u) != 0u) {
            const uint32_t record = shader - 1u;
            if (record >= 0x00010000u && record <= 0x03FFFFC0u) {
                for (uint32_t row = 0u; row < 8u; ++row) {
                    const uint32_t address = record + row * 16u;
                    fprintf(stderr,
                            "  +%02X %08X %08X %08X %08X\n",
                            row * 16u, guest_u32(address + 0u),
                            guest_u32(address + 4u),
                            guest_u32(address + 8u),
                            guest_u32(address + 12u));
                }
            }
        }
        fflush(stderr);
        last_shader = shader;
    }
    ++trace_calls;
    pgraph_d3d11_set_guest_fvf(shader);
}

void recomp_d3d_select_vertex_shader_direct(uint32_t format, uint32_t start)
{
    static uint32_t samples;
    static uint32_t last_format = UINT32_MAX;
    static uint32_t last_start = UINT32_MAX;
    PgraphD3D11Stats stats;

    if (getenv("MERCENARIES_TRACE_VERTEX_SHADER_DIRECT") == NULL ||
        format < 0x00010000u || format > 0x03FFFF00u)
        return;
    pgraph_d3d11_get_stats(&stats);
    if (stats.draw_calls < 100000u || samples >= 64u ||
        (format == last_format && start == last_start))
        return;
    fprintf(stderr,
            "[D3D-VSH-DIRECT] sample=%u draw=%u format=%08X start=%u\n",
            samples++, stats.draw_calls, format, start);
    for (uint32_t input = 0u; input < 16u; ++input) {
        const uint32_t address = format + input * 16u;
        fprintf(stderr, "  input%u=%08X %08X %08X %08X\n", input,
                guest_u32(address + 0u), guest_u32(address + 4u),
                guest_u32(address + 8u), guest_u32(address + 12u));
    }
    fflush(stderr);
    last_format = format;
    last_start = start;
}

void recomp_d3d_constant_checkpoint(uint32_t constant, uint32_t source,
                                    uint32_t dword_count)
{
    static uint32_t samples;
    uint32_t recent_index;

    if (getenv("MERCENARIES_TRACE_D3D_CONSTANT_SOURCE") == NULL ||
        source < 0x00010000u || source > 0x03FFFFC0u ||
        dword_count != 12u || samples >= 16u)
        return;

    fprintf(stderr,
            "[D3D-CONSTANT-SOURCE] #%u constant=%u source=%08X dwords=%u\n",
            samples++, constant, source, dword_count);
    for (uint32_t row = 0; row < 3u; ++row) {
        fprintf(stderr, "  row%u=%08X %08X %08X %08X\n", row,
                guest_u32(source + row * 16u + 0u),
                guest_u32(source + row * 16u + 4u),
                guest_u32(source + row * 16u + 8u),
                guest_u32(source + row * 16u + 12u));
    }
    recent_index = g_recomp_recent_func_idx;
    for (uint32_t i = 0; i < 32u && i < recent_index; ++i) {
        uint32_t index = (recent_index - 1u - i) & 63u;
        fprintf(stderr, "  constant-source-recent[-%u]=%08X\n", i,
                g_recomp_recent_funcs[index]);
    }
    fflush(stderr);
}

void recomp_vehicle_wheel_actor_checkpoint(uint32_t actor, uint32_t quality)
{
    static int enabled = -1;
    static uint32_t cold_seen[64];
    static uint32_t cold_seen_count;
    static uint32_t seen[64];
    static uint32_t seen_count;
    static int route_was_complete;
    uint32_t count;
    uint32_t active_count = 0u;
    int cold_already_seen = 0;

    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_VEHICLE_WHEEL_ACTORS") != NULL ||
                  getenv("MERCENARIES_TEST_AUTO_DRIVE_GATE_ROUTE") != NULL;
    if (!enabled ||
        actor < 0x00010000u || actor > 0x03FFE000u)
        return;

    if (g_recomp_aircraft_route_complete &&
        guest_u32(actor) == 0x002E1968u &&
        recomp_find_player_occupied_vehicle() == actor)
        g_recomp_live_vehicle_actor = actor;

    /* The aircraft Humvee persists into player control and can retain the
     * same guest address. Start a fresh inventory at that scene boundary so
     * the tracer validates its live gameplay transforms as well as its
     * pre-aircraft/cutscene state. */
    if (g_recomp_aircraft_route_complete && !route_was_complete) {
        cold_seen_count = 0u;
        seen_count = 0u;
        route_was_complete = 1;
    }

    for (uint32_t i = 0u; i < seen_count; ++i) {
        if (seen[i] == actor)
            return;
    }
    count = guest_u32(actor + 0x107Cu);
    if (count == 0u || count > 8u)
        return;
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t renderable = actor + 0xB04u + i * 0x5Cu;
        active_count += guest_u8(renderable + 0x14u) & 1u;
    }
    for (uint32_t i = 0u; i < cold_seen_count; ++i)
        cold_already_seen = cold_already_seen || cold_seen[i] == actor;
    if (active_count == 0u) {
        if (cold_already_seen ||
            cold_seen_count >= sizeof(cold_seen) / sizeof(cold_seen[0]))
            return;
        cold_seen[cold_seen_count++] = actor;
    } else {
        if (seen_count >= sizeof(seen) / sizeof(seen[0]))
            return;
        seen[seen_count++] = actor;
    }

    fprintf(stderr,
            "[VEHICLE-WHEEL-ACTOR] actor=%08X vtable=%08X phase=%s "
            "quality=%u count=%u active=%u wheel_model=%08X "
            "world=(%.7g,%.7g,%.7g)\n",
            actor, guest_u32(actor), active_count != 0u ? "active" : "cold",
            quality, count, active_count, guest_u32(actor + 0x1080u),
            merc_guest_f32(actor + 0xE0u),
            merc_guest_f32(actor + 0xE4u),
            merc_guest_f32(actor + 0xE8u));
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t matrix = actor + 0xDF0u + i * 0x40u;
        const uint32_t renderable = actor + 0xB04u + i * 0x5Cu;
        fprintf(stderr,
                "  wheel[%u] matrix=%08X active=%u "
                "x=(%.6g,%.6g,%.6g) y=(%.6g,%.6g,%.6g) "
                "z=(%.6g,%.6g,%.6g) pos=(%.7g,%.7g,%.7g)\n",
                i, matrix, guest_u8(renderable + 0x14u) & 1u,
                merc_guest_f32(matrix + 0x00u),
                merc_guest_f32(matrix + 0x04u),
                merc_guest_f32(matrix + 0x08u),
                merc_guest_f32(matrix + 0x10u),
                merc_guest_f32(matrix + 0x14u),
                merc_guest_f32(matrix + 0x18u),
                merc_guest_f32(matrix + 0x20u),
                merc_guest_f32(matrix + 0x24u),
                merc_guest_f32(matrix + 0x28u),
                merc_guest_f32(matrix + 0x30u),
                merc_guest_f32(matrix + 0x34u),
                merc_guest_f32(matrix + 0x38u));
    }
    fflush(stderr);
}
void recomp_humvee_render_item_checkpoint(uint32_t item)
{
    static uint32_t reports;

    if (getenv("MERCENARIES_TRACE_HUMVEE_RENDER_ITEM") == NULL ||
        !g_recomp_aircraft_route_complete || reports != 0u ||
        item < 0x00010000u || item > 0x03FFFF70u ||
        guest_u32(item + 0x00u) != 0u ||
        guest_u32(item + 0x04u) != 0u ||
        guest_u32(item + 0x08u) != 0u ||
        guest_u32(item + 0x10u) != 0u ||
        guest_u32(item + 0x14u) != 0u ||
        guest_u32(item + 0x18u) != 0u)
        return;

    reports = 1u;
    fprintf(stderr,
            "[HUMVEE-RENDER-ITEM] item=%08X prim=%08X flags=%04X "
            "bones=%u boneptr=%08X state=%08X texture=%08X invscale=%08X\n",
            item, guest_u32(item + 0x60u),
            guest_u32(item + 0x64u) & 0xFFFFu,
            guest_u32(item + 0x64u) >> 16,
            guest_u32(item + 0x68u), guest_u32(item + 0x6Cu),
            guest_u32(item + 0x70u), guest_u32(item + 0x88u));
    for (uint32_t row = 0u; row < 4u; ++row) {
        fprintf(stderr, "  world%u=%08X %08X %08X %08X\n", row,
                guest_u32(item + row * 16u + 0u),
                guest_u32(item + row * 16u + 4u),
                guest_u32(item + row * 16u + 8u),
                guest_u32(item + row * 16u + 12u));
    }
    fflush(stderr);
    recomp_arm_redscene_watchpoint(item);
}
void recomp_dsound_object_checkpoint(uint32_t stage, uint32_t wrapper,
                                      uint32_t inner)
{
    static uint32_t samples;
    int wrapper_valid;
    int inner_valid;

    if (getenv("MERCENARIES_TRACE_DSOUND_OBJECT") == NULL || samples++ >= 64u)
        return;
    wrapper_valid = wrapper >= 0x00010000u && wrapper < 0x04000000u;
    inner_valid = inner >= 0x00010000u && inner < 0x04000000u;
    fprintf(stderr,
            "[DSOUND-OBJECT] stage=%u wrapper=%08X inner=%08X aligned=%u",
            stage, wrapper, inner, (inner & 3u) == 0u);
    if (wrapper_valid) {
        fprintf(stderr,
                " wrapper00=%08X wrapper04=%08X wrapper08=%08X wrapper0C=%08X"
                " wrapper10=%08X wrapper14=%08X wrapper18=%08X wrapper1C=%08X"
                " wrapper20=%08X wrapper24=%08X wrapper28=%08X",
                guest_u32(wrapper), guest_u32(wrapper + 4u),
                guest_u32(wrapper + 8u), guest_u32(wrapper + 0xCu),
                guest_u32(wrapper + 0x10u), guest_u32(wrapper + 0x14u),
                guest_u32(wrapper + 0x18u), guest_u32(wrapper + 0x1Cu),
                guest_u32(wrapper + 0x20u), guest_u32(wrapper + 0x24u),
                guest_u32(wrapper + 0x28u));
        if (wrapper >= 0x0001001Cu)
            fprintf(stderr,
                    " back1C=%08X back00=%08X back04=%08X back08=%08X"
                    " back1Cval=%08X back20=%08X back24=%08X",
                    wrapper - 0x1Cu, guest_u32(wrapper - 0x1Cu),
                    guest_u32(wrapper - 0x18u), guest_u32(wrapper - 0x14u),
                    guest_u32(wrapper), guest_u32(wrapper + 4u),
                    guest_u32(wrapper + 8u));
    }
    if (inner_valid)
        fprintf(stderr,
                " inner00=%08X inner08=%08X inner0C=%08X inner12=%04X inner53C=%08X",
                guest_u32(inner), guest_u32(inner + 8u),
                guest_u32(inner + 0xCu), guest_u32(inner + 0x12u) & 0xFFFFu,
                guest_u32(inner + 0x53Cu));
    fputc('\n', stderr);
    fflush(stderr);
}
void recomp_dsound_stream_checkpoint(uint32_t stage, uint32_t inner,
                                      uint32_t packet, uint32_t value)
{
    static uint32_t samples;
    int inner_valid;
    int packet_valid;

    if (getenv("MERCENARIES_TRACE_DSOUND_STREAM") == NULL || samples++ >= 4096u)
        return;

    inner_valid = inner >= 0x00010000u && inner < 0x04000000u;
    packet_valid = packet >= 0x00010000u && packet < 0x04000000u;
    fprintf(stderr,
            "[DSOUND-STREAM] stage=%u inner=%08X packet=%08X value=%08X",
            stage, inner, packet, value);
    if (packet_valid)
        fprintf(stderr,
                " pkt=%08X/%08X/%08X/%08X/%08X/%08X",
                guest_u32(packet + 0u), guest_u32(packet + 4u),
                guest_u32(packet + 8u), guest_u32(packet + 0xCu),
                guest_u32(packet + 0x10u), guest_u32(packet + 0x14u));
    if (stage == 3u && packet_valid &&
        getenv("MERCENARIES_COMPLETE_DSOUND_PACKETS") != NULL) {
        const uint32_t completed_size = guest_u32(packet + 8u);
        const uint32_t status = guest_u32(packet + 0xCu);
        if (completed_size >= 0x00010000u && completed_size < 0x04000000u)
            *(volatile uint32_t *)guest_ptr(completed_size) = guest_u32(packet + 4u);
        if (status >= 0x00010000u && status < 0x04000000u)
            *(volatile uint32_t *)guest_ptr(status) = 0u;
        if (inner_valid)
            *(volatile uint8_t *)guest_ptr(inner + 0x12u) |= 0x20u;
    }
    if (inner_valid) {
        uint32_t voice_table = guest_u32(inner + 0x68u);
        fprintf(stderr,
                " flags=%04X fmt=%08X voices=%08X listA8=%08X/%08X"
                " freeB0=%08X/%08X index=%u count=%u",
                guest_u32(inner + 0x12u) & 0xFFFFu,
                guest_u32(inner + 0x80u), voice_table,
                guest_u32(inner + 0xA8u), guest_u32(inner + 0xACu),
                guest_u32(inner + 0xB0u), guest_u32(inner + 0xB4u),
                guest_u32(inner + 0x190u), guest_u32(inner + 0x194u));
        if (voice_table >= 0x00010000u && voice_table < 0x04000000u)
            fprintf(stderr,
                    " slot0=%08X/%08X/%08X/%02X/%02X slot1=%08X/%08X/%08X/%02X/%02X",
                    guest_u32(voice_table + 0x00u),
                    guest_u32(voice_table + 0x08u),
                    guest_u32(voice_table + 0x0Cu),
                    guest_u8(voice_table + 0x0Eu),
                    guest_u8(voice_table + 0x0Fu),
                    guest_u32(voice_table + 0x10u),
                    guest_u32(voice_table + 0x18u),
                    guest_u32(voice_table + 0x1Cu),
                    guest_u8(voice_table + 0x1Eu),
                    guest_u8(voice_table + 0x1Fu));
    }
    fputc('\n', stderr);
    fflush(stderr);
}

void recomp_xmv_audio_selector_checkpoint(uint32_t decoder, uint32_t index,
                                           uint32_t state_a, uint32_t state_b,
                                           uint32_t phase)
{
    static uint32_t samples;
    if (getenv("MERCENARIES_TRACE_XMV_AUDIO_SELECTOR") == NULL ||
        samples >= 256u)
        return;
    fprintf(stderr,
            "[XMV-AUDIO-SELECTOR] sample=%u phase=%u decoder=%08X index=%u"
            " stateA=%08X stateB=%08X\n",
            ++samples, phase, decoder, index, state_a, state_b);
    fflush(stderr);
}

uint32_t recomp_movie_audio_enabled(uint32_t requested)
{
    return getenv("MERCENARIES_TEST_DISABLE_MOVIE_AUDIO") == NULL && requested != 0u;
}

void recomp_d3d_sync_checkpoint(uint32_t phase)
{
    static uint32_t samples;
    static DWORD start_tick;
    static DWORD delay_ms;
    static int initialized;
    static int trace_enabled = -1;
    uint32_t root;
    uint32_t device;

    if (trace_enabled < 0)
        trace_enabled = getenv("MERCENARIES_TRACE_D3D_SYNC") != NULL;
    if (!trace_enabled)
        return;
    if (!initialized) {
        const char *delay = getenv("MERCENARIES_TRACE_D3D_SYNC_DELAY_MS");
        start_tick = GetTickCount();
        delay_ms = delay ? (DWORD)strtoul(delay, NULL, 0) : 0u;
        initialized = 1;
    }
    if (GetTickCount() - start_tick < delay_ms || samples++ >= 64u)
        return;
    root = guest_u32(0x00299378u);
    device = root ? guest_u32(root + 0x934u) : 0u;
    fprintf(stderr,
            "[D3D-SYNC] phase=%u eax=%08X ebx=%08X ecx=%08X edx=%08X "
            "esi=%08X edi=%08X esp=%08X root=%08X device=%08X "
            "queued=%08X completed=%08X status=%08X\n",
            phase, g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_esp,
            root, device,
            device ? guest_u32(device + 0x3240u) : 0u,
            device ? guest_u32(device + 0x3244u) : 0u,
            device ? guest_u32(device + 0x400700u) : 0u);
    fflush(stderr);
}

void recomp_splash_d3d_checkpoint(uint32_t phase)
{
    if (getenv("MERCENARIES_TRACE_SPLASH") != NULL) {
        const uint32_t output_pointer = guest_u32(g_esp + 0x20u);
        fprintf(stderr,
                "[D3D-RESULT] phase=%u eax=%08X esi=%08X esp=%08X "
                "outptr=%08X outval=%08X device=%08X\n",
                phase, g_eax, g_esi, g_esp, output_pointer,
                output_pointer ? guest_u32(output_pointer) : 0u,
                guest_u32(0x007AD604u));
        if (phase == 12u && g_eax >= 0x00010000u && g_eax < 0x04000000u)
            g_splash_draw_packet_va = g_eax;
        if (g_splash_draw_packet_va != 0u &&
            (phase == 12u || (phase >= 20u && phase <= 39u))) {
            const uint32_t va = g_splash_draw_packet_va;
            fprintf(stderr,
                    "[DRAW-RAW] phase=%u esp=%08X %08X: "
                    "%08X %08X %08X %08X %08X %08X\n",
                    phase, g_esp, va, guest_u32(va + 0u), guest_u32(va + 4u),
                    guest_u32(va + 8u), guest_u32(va + 12u),
                    guest_u32(va + 16u), guest_u32(va + 20u));
        }
    }
}
/* ICALL failure logging */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}


/* Called only at the renderer's CreateDevice result boundary. It does not
 * change guest registers or resource ownership. Stage 3 replaces the retail
 * debug assertion, stripped from the release XBE, with a useful fatal error. */
void recomp_renderer_recovery_report(uint32_t result, uint32_t stage)
{
    xbox_preview_log_event("renderer-recovery", "stage=%u HRESULT=%08X",stage,result);
    if (stage == 1u)
        fprintf(stderr, "[RENDERER-RECOVERY] CreateDevice allocation failure %08X; retrying retail non-AA mode\n", result);
    else if (stage == 2u)
        fprintf(stderr, "[RENDERER-RECOVERY] non-AA CreateDevice result=%08X\n", result);
    else {
        fprintf(stderr, "[RENDERER-RECOVERY] Cannot create graphics device (HRESULT=%08X); refusing null-device rendering\n", result);
        fflush(stderr);
        ExitProcess(result ? result : 0x80004005u);
    }
    fflush(stderr);
}


uint32_t recomp_renderer_test_oom(uint32_t multisample)
{
    static int injected;
    if (!injected && multisample != 0x11u &&
        getenv("XBOXRECOMP_TEST_RENDERER_OOM_ONCE")) {
        /* Diagnostic selection permits a gameplay-device failure after the
         * front end succeeded. A startup-only failure can be hidden by the
         * next successful AA recreation and does not test gameplay fallback. */
        const char *selected = getenv("XBOXRECOMP_TEST_RENDERER_OOM_MULTISAMPLE");
        const char *gate = getenv("XBOXRECOMP_TEST_RENDERER_OOM_GATE");
        if (selected && multisample != (uint32_t)strtoul(selected, NULL, 0))
            return 0u;
        if (gate) {
            FILE *ready = fopen(gate, "rb");
            if (!ready) return 0u;
            fclose(ready);
        }
        injected = 1;
        fprintf(stderr, "[RENDERER-RECOVERY] TEST injected allocation failure before CreateDevice\n");
        return 1u;
    }
    return 0u;
}


/* XapiFormatFATVolume, retail 0022C5C6 (ANSI_STRING*, cluster size).
 * The original cache selection/ownership bookkeeping still runs normally.
 * Only the empty folder-backed volume needs no raw FATX sector writes. */
int recomp_prepare_empty_cache_volume(void)
{
    extern BOOL xbox_prepare_empty_cache_volume(const char *path);
    uint8_t *base = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t string, buffer;
    uint16_t length;
    char path[64];
    if (g_esp < 0x10000u || g_esp > 0x03FFFFF4u) return 0;
    string = *(uint32_t *)(base + g_esp + 4u);
    if (string < 0x10000u || string > 0x03FFFFF8u) return 0;
    length = *(uint16_t *)(base + string);
    buffer = *(uint32_t *)(base + string + 4u);
    if (!length || length >= sizeof(path) || buffer < 0x10000u ||
        buffer > 0x04000000u - length) return 0;
    memcpy(path, base + buffer, length);
    if (memchr(path, 0, length)) return 0;
    path[length] = 0;
    if (!xbox_prepare_empty_cache_volume(path)) return 0;
    fprintf(stderr, "[CACHE] Prepared empty host-backed volume %s\n", path);
    g_eax = 1;
    g_esp += 12u;
    return 1;
}

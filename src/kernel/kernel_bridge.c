/**
 * kernel_bridge.c - Bridge between translated game code and kernel functions
 *
 * Problem:
 *   Translated game code calls kernel functions via indirect calls through
 *   the kernel thunk table at VA 0x0036B7C0. In the XBE file, these entries
 *   contain unresolved ordinals (0x80000000 | ordinal). On real Xbox hardware,
 *   the kernel loader replaces these with actual function pointers before the
 *   game runs.
 *
 * Solution:
 *   1. After xbox_MemoryLayoutInit copies .rdata, call xbox_kernel_bridge_init()
 *   2. Replace each ordinal entry in Xbox memory with a synthetic VA
 *   3. When RECOMP_ICALL encounters a synthetic VA, route it to a per-ordinal
 *      bridge function that reads args from the simulated Xbox stack, translates
 *      pointer arguments from Xbox VA->native, and calls the kernel function.
 *
 * Synthetic VA scheme:
 *   Each thunk slot i gets VA 0xFE000000 + i*4
 *   The lookup function checks this range and dispatches appropriately.
 *
 * Why per-ordinal bridges instead of a generic trampoline:
 *   Kernel functions receive Xbox pointers (32-bit VAs) that must be translated
 *   to native pointers by adding g_xbox_mem_offset. Different functions have
 *   different parameter layouts (pointer vs value), so each needs its own bridge.
 */

#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

/* Access to recompiled code globals */
extern uint32_t g_eax, g_ecx, g_edx, g_esp;
extern uint32_t g_ebx, g_esi, g_edi;
extern uint32_t g_seh_ebp;
extern double g_fp_stack[8];
extern uint32_t g_fp_top;
extern uint16_t g_x87_control_word;
extern uint16_t g_x87_status_word;
extern float g_xmm0[4], g_xmm1[4], g_xmm2[4], g_xmm3[4];
extern float g_xmm4[4], g_xmm5[4], g_xmm6[4], g_xmm7[4];
extern uint64_t g_mm0, g_mm1, g_mm2, g_mm3;
extern uint64_t g_mm4, g_mm5, g_mm6, g_mm7;
extern volatile uint32_t g_recomp_current_func;
extern volatile uint32_t g_recomp_recent_funcs[64];
extern volatile uint32_t g_recomp_recent_func_idx;
extern volatile uint32_t g_recomp_recent_game_funcs[256];
extern volatile uint32_t g_recomp_recent_game_func_idx;
extern volatile uint32_t g_recomp_trace_dump_requested;
extern volatile uint32_t g_recomp_func_trace_armed;
extern ptrdiff_t g_xbox_mem_offset;

/* Dispatch table lookup (for function pointer args) */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

/* Memory access - same as recomp_types.h MEM32 but without the #define guard */
#define BRIDGE_MEM32(addr) (*(volatile uint32_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM16(addr) (*(volatile uint16_t *)((uintptr_t)(addr) + g_xbox_mem_offset))
#define BRIDGE_MEM8(addr)  (*(volatile uint8_t  *)((uintptr_t)(addr) + g_xbox_mem_offset))

static int bridge_trace_kernel_calls_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MERCENARIES_TRACE_KERNEL_CALLS");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return enabled;
}
static int bridge_trace_interrupts_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("MERCENARIES_TRACE_INTERRUPTS");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    return enabled;
}

/* Translate Xbox VA to native pointer (NULL-safe: 0 -> NULL) */
#define XBOX_TO_NATIVE(va) ((va) ? (void*)((uintptr_t)(va) + g_xbox_mem_offset) : NULL)
/* NtReadFile completes synchronously on the host, but Xbox ReadFileEx expects
 * its guest APC to run only when the thread enters an alertable wait. Keep the
 * completed guest IOSB/context tuple until SleepEx reaches KeDelayExecutionThread. */
#define BRIDGE_GUEST_APC_CAPACITY 64u
typedef struct bridge_guest_apc {
    uint32_t routine_va;
    uint32_t context_va;
    uint32_t iostatus_va;
} bridge_guest_apc;
static bridge_guest_apc g_guest_apcs[BRIDGE_GUEST_APC_CAPACITY];
static uint32_t g_guest_apc_head;
static uint32_t g_guest_apc_tail;
static uint32_t g_guest_apc_log_count;

#define BRIDGE_GUEST_INTERRUPT_CAPACITY 16u
typedef struct bridge_guest_interrupt {
    uint32_t interrupt_va;
    uint32_t service_routine_va;
    uint32_t service_context_va;
    uint32_t bus_level;
    uint32_t vector;
    uint32_t irql;
    volatile LONG connected;
} bridge_guest_interrupt;
static bridge_guest_interrupt
    g_guest_interrupts[BRIDGE_GUEST_INTERRUPT_CAPACITY];
static uint32_t g_guest_interrupt_count;
static uint32_t g_last_interrupt_bus_level;
/* Read directly by the translated-function fast path. Producers still use
 * interlocked operations; a plain aligned 32-bit read avoids a bridge call
 * when no interrupt is pending. */
volatile int32_t g_kernel_pending_hardware_interrupts;
static int g_delivering_hardware_interrupt;
static uint32_t g_guest_interrupt_log_count;

/* Only the translated CPU thread manipulates this queue. Host interrupt/timer
 * producers set their existing pending flags; they never execute guest code.
 * Queue insertion must not call a DPC from the middle of an ISR/list update. */
#define BRIDGE_GUEST_DPC_CAPACITY 256u
typedef struct bridge_guest_dpc {
    uint32_t dpc_va, argument1, argument2;
} bridge_guest_dpc;
static bridge_guest_dpc g_guest_dpcs[BRIDGE_GUEST_DPC_CAPACITY];
static uint32_t g_guest_dpc_count;
static int g_delivering_guest_dpcs;
volatile int32_t g_kernel_pending_guest_dpcs;
static BOOLEAN bridge_queue_guest_dpc(uint32_t dpc_va, uint32_t argument1,
                                      uint32_t argument2);
static BOOLEAN bridge_remove_guest_dpc(uint32_t dpc_va);
static int bridge_deliver_guest_dpcs(void);

#define BRIDGE_GUEST_TIMER_CAPACITY 64u
typedef struct bridge_guest_timer {
    uint32_t timer_va;
    uint32_t dpc_va;
    int64_t due_time_100ns;
    uint32_t period_ms;
    int active;
} bridge_guest_timer;
static bridge_guest_timer g_guest_timers[BRIDGE_GUEST_TIMER_CAPACITY];
static int g_delivering_guest_timers;
__declspec(align(8)) static volatile LONG64 g_guest_timer_next_due_100ns;
volatile int32_t g_kernel_pending_guest_timers;

typedef struct bridge_guest_cpu_context {
    uint32_t eax, ecx, edx, esp, ebx, esi, edi, seh_ebp;
    double fp_stack[8];
    uint32_t fp_top;
    uint16_t x87_control_word;
    uint16_t x87_status_word;
    float xmm[8][4];
    uint64_t mm[8];
    uint32_t current_func;
} bridge_guest_cpu_context;

static void bridge_capture_guest_context(bridge_guest_cpu_context *context)
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

static void bridge_restore_guest_context(const bridge_guest_cpu_context *context)
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

static void bridge_NtUserIoApcDispatcher(void);

static int bridge_queue_guest_apc(uint32_t routine_va, uint32_t context_va,
                                  uint32_t iostatus_va)
{
    const uint32_t next =
        (g_guest_apc_tail + 1u) % BRIDGE_GUEST_APC_CAPACITY;
    if (next == g_guest_apc_head) {
        fprintf(stderr, "[GUEST-APC] queue full; dropping routine=%08X\n",
                routine_va);
        return 0;
    }
    g_guest_apcs[g_guest_apc_tail].routine_va = routine_va;
    g_guest_apcs[g_guest_apc_tail].context_va = context_va;
    g_guest_apcs[g_guest_apc_tail].iostatus_va = iostatus_va;
    g_guest_apc_tail = next;
    if (g_guest_apc_log_count < 12u) {
        fprintf(stderr,
                "[GUEST-APC] queued routine=%08X context=%08X iosb=%08X\n",
                routine_va, context_va, iostatus_va);
        ++g_guest_apc_log_count;
    }
    return 1;
}

static int bridge_deliver_guest_apc(void)
{
    bridge_guest_apc apc;
    bridge_guest_cpu_context saved_context;
    recomp_func_t fn;
    uint32_t saved_esp;

    if (g_guest_apc_head == g_guest_apc_tail)
        return 0;
    apc = g_guest_apcs[g_guest_apc_head];
    g_guest_apc_head =
        (g_guest_apc_head + 1u) % BRIDGE_GUEST_APC_CAPACITY;

    fn = recomp_lookup_manual(apc.routine_va);
    if (!fn) fn = recomp_lookup(apc.routine_va);
    if (!fn) fn = recomp_lookup_kernel(apc.routine_va);
    if (!fn) {
        fprintf(stderr, "[GUEST-APC] unresolved routine=%08X\n", apc.routine_va);
        return 1;
    }

    bridge_capture_guest_context(&saved_context);
    saved_esp = saved_context.esp;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0u;                 /* Reserved */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = apc.iostatus_va;   /* IoStatusBlock */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = apc.context_va;    /* ApcContext */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0u;                /* return address */
    fn();
    if (g_esp != saved_esp) {
        fprintf(stderr,
                "[GUEST-APC] stack mismatch routine=%08X expected=%08X got=%08X\n",
                apc.routine_va, saved_esp, g_esp);
    }
    /* KiUserApcDispatcher returns through NtContinue on Xbox, restoring the
     * interrupted thread's complete CPU context. Callback register changes
     * must not leak into the alertable waiter. */
    bridge_restore_guest_context(&saved_context);
    if (g_guest_apc_log_count < 24u) {
        fprintf(stderr, "[GUEST-APC] delivered routine=%08X\n", apc.routine_va);
        ++g_guest_apc_log_count;
    }
    return 1;
}

void xbox_kernel_raise_hardware_interrupt(uint32_t bus_level)
{
    if (bus_level < 32u)
        InterlockedOr((volatile LONG *)&g_kernel_pending_hardware_interrupts,
                      (LONG)(1u << bus_level));
}

void xbox_kernel_lower_hardware_interrupt(uint32_t bus_level)
{
    if (bus_level < 32u)
        InterlockedAnd((volatile LONG *)&g_kernel_pending_hardware_interrupts,
                       (LONG)~(1u << bus_level));
}

static recomp_func_t bridge_lookup_guest_function(uint32_t va)
{
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_kernel(va);
    return fn;
}

static int bridge_deliver_hardware_interrupts(void)
{
    LONG pending;
    uint32_t bus_level;
    uint32_t i;
    int delivered = 0;

    if (g_delivering_hardware_interrupt)
        return 0;
    pending = InterlockedExchange(
        (volatile LONG *)&g_kernel_pending_hardware_interrupts, 0);
    if (!pending)
        return 0;

    g_delivering_hardware_interrupt = 1;
    for (bus_level = 0; bus_level < 32u; ++bus_level) {
        const uint32_t mask = 1u << bus_level;
        int registered = 0;
        if (!((uint32_t)pending & mask))
            continue;
        for (i = 0; i < g_guest_interrupt_count; ++i) {
            bridge_guest_interrupt *interrupt = &g_guest_interrupts[i];
            bridge_guest_cpu_context saved_context;
            recomp_func_t fn;
            uint32_t saved_esp;
            uint32_t handled;
            KIRQL old_irql;

            if (interrupt->bus_level != bus_level ||
                !InterlockedCompareExchange(&interrupt->connected, 0, 0))
                continue;
            registered = 1;
            if (BRIDGE_MEM8(0x24u) >= interrupt->irql) {
                InterlockedOr((volatile LONG *)&g_kernel_pending_hardware_interrupts,
                              (LONG)mask);
                continue;
            }
            fn = bridge_lookup_guest_function(interrupt->service_routine_va);
            if (!fn) {
                fprintf(stderr,
                        "[GUEST-IRQ] unresolved ISR bus=%u routine=%08X\n",
                        bus_level, interrupt->service_routine_va);
                continue;
            }

            bridge_capture_guest_context(&saved_context);
            saved_esp = saved_context.esp;
            old_irql = xbox_KfRaiseIrql((KIRQL)interrupt->irql);
            BRIDGE_MEM8(0x24u) = (uint8_t)interrupt->irql;
            g_esp -= 4; BRIDGE_MEM32(g_esp) = interrupt->service_context_va;
            g_esp -= 4; BRIDGE_MEM32(g_esp) = interrupt->interrupt_va;
            g_esp -= 4; BRIDGE_MEM32(g_esp) = 0u;
            fn();
            handled = g_eax;
            xbox_KfLowerIrql(old_irql);
            BRIDGE_MEM8(0x24u) = old_irql;
            if (g_esp != saved_esp && g_guest_interrupt_log_count < 32u) {
                fprintf(stderr,
                        "[GUEST-IRQ] ISR stack mismatch routine=%08X expected=%08X got=%08X\n",
                        interrupt->service_routine_va, saved_esp, g_esp);
            }
            bridge_restore_guest_context(&saved_context);
            delivered = 1;
            if (bridge_trace_interrupts_enabled() &&
                g_guest_interrupt_log_count < 64u) {
                fprintf(stderr,
                        "[GUEST-IRQ] delivered bus=%u object=%08X isr=%08X handled=%u\n",
                        bus_level, interrupt->interrupt_va,
                        interrupt->service_routine_va, handled != 0u);
                ++g_guest_interrupt_log_count;
            }
        }
        if (!registered && bridge_trace_interrupts_enabled() &&
            g_guest_interrupt_log_count < 64u) {
            fprintf(stderr, "[GUEST-IRQ] no connected ISR for bus=%u\n",
                    bus_level);
            ++g_guest_interrupt_log_count;
        }
    }
    g_delivering_hardware_interrupt = 0;
    bridge_deliver_guest_dpcs();
    return delivered;
}

void xbox_kernel_service_hardware_interrupts(void)
{
    if (InterlockedCompareExchange(
            (volatile LONG *)&g_kernel_pending_hardware_interrupts, 0, 0) != 0)
        bridge_deliver_hardware_interrupts();
}

/* -- Synthetic VA range (for function exports) ----------- */

#define KERNEL_VA_BASE  0xFE000000u
#define KERNEL_VA_END   (KERNEL_VA_BASE + XBOX_KERNEL_THUNK_TABLE_SIZE * 4)

/* -- Kernel data exports ----------------------------------
 *
 * Some kernel ordinals are DATA exports (structs/variables), not functions.
 * The game reads their thunk entries and dereferences the result to access
 * the data. These cannot use synthetic VAs -- they must point to real,
 * dereferenceable addresses in the Xbox VA space.
 *
 * We allocate a "kernel data area" at XBOX_KERNEL_DATA_BASE and populate
 * it with the expected structures.
 */

/**
 * Get the Xbox VA of data for a kernel DATA export ordinal.
 * Returns 0 if the ordinal is not a data export (i.e., it's a function).
 */
static uint32_t kernel_data_va_for_ordinal(ULONG ordinal)
{
    if (ordinal == 357) return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL_DATA;
    switch (ordinal) {
    case  16: return XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE;
    case  40: return XBOX_KERNEL_DATA_BASE + KDATA_HAL_CACHE_PARTITIONS;
    case  64: return XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE;
    case  70: return XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE;
    case  71: return XBOX_KERNEL_DATA_BASE + KDATA_IO_FILE_TYPE;
    case 156: return XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT;
    case 164: return XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE;
    case 259: return XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE;
    case 321: return XBOX_KERNEL_DATA_BASE + KDATA_EEPROM_KEY;
    case 322: return XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO;
    case 323: return XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY;
    case 324: return XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION;
    case 325: return XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY;
    case 326: return XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME;
    case 353: return XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY;
    case 354: return XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS;
    case 355: return XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY;
    case 356: return XBOX_KERNEL_DATA_BASE + KDATA_HAL_BOOT_VIDEO_MODE;
    case 357: return XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL_OBJECT;
    default:  return 0;  /* Not a data export */
    }
}

/**
 * Initialize kernel data export values at the kernel data area.
 * Called during bridge init, after Xbox memory is mapped.
 */
static void kernel_data_init(void)
{
    /* XboxHardwareInfo (ordinal 322) - XBOX_HARDWARE_INFO
     *   +0: ULONG Flags (0 = retail, 0x20 = devkit)
     *   +4: UCHAR GpuRevision
     *   +5: UCHAR McpRevision
     */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 0) = 0;   /* Retail */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 4) = 0xA1; /* NV2A A1 */
    BRIDGE_MEM8(XBOX_KERNEL_DATA_BASE + KDATA_HARDWARE_INFO + 5) = 0xB1; /* MCPX B1 */

    /* XboxKrnlVersion (ordinal 324) - XBOX_KRNL_VERSION
     *   +0: USHORT Major (1)
     *   +2: USHORT Minor (0)
     *   +4: USHORT Build (5849 = XDK version)
     *   +6: USHORT Qfe (0)
     */
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 0) = 1;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 2) = 0;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 4) = 5849;
    BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_KRNL_VERSION + 6) = 0;

    /* KeTickCount (ordinal 156) - initialized to current tick count.
     * A background thread in main.c updates this every ~1ms. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = GetTickCount();

    /* LaunchDataPage (ordinal 164) - NULL (no launch data) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = 0;

    /* PsThreadObjectType (ordinal 259) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_THREAD_OBJ_TYPE) = 0;

    /* ExEventObjectType (ordinal 16) - type object (stub: 0) */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE) = 0;

    /* I/O manager object-type exports (ordinals 64, 70, and 71). */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_COMPLETION_TYPE) = 0;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_DEVICE_TYPE) = 0;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IO_FILE_TYPE) = 0;

    /* HAL globals used by the XDK startup code. */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HAL_CACHE_PARTITIONS) = 3;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_HAL_BOOT_VIDEO_MODE) = 0;
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL_DATA) = 0x49444558; /* 'IDEX' */
    BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL_OBJECT) =
        XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL_DATA;
    {
        uint32_t list_head = XBOX_KERNEL_DATA_BASE + KDATA_IDEX_CHANNEL_DATA + 0x28;
        BRIDGE_MEM32(list_head + 0) = list_head;
        BRIDGE_MEM32(list_head + 4) = list_head;
    }


    /* Per-console keys are deterministic zeroes in the PC runtime. */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_EEPROM_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxHDKey (ordinal 323) - 16 bytes of zeros (no key) */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_HD_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxSignatureKey (ordinal 325) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_SIGNATURE_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxLANKey (ordinal 353) - 16 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_LAN_KEY) + g_xbox_mem_offset), 0, 16);

    /* XboxAlternateSignatureKeys (ordinal 354) - 256 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_ALT_SIGNATURE_KEYS) + g_xbox_mem_offset), 0, 256);

    /* XePublicKeyData (ordinal 355) - 284 bytes of zeros */
    memset((void*)((uintptr_t)(XBOX_KERNEL_DATA_BASE + KDATA_XE_PUBLIC_KEY) + g_xbox_mem_offset), 0, 284);

    /* XeImageFileName (ordinal 326) - guest-layout ANSI_STRING. */
    {
        static const char image_filename[] = "\\Device\\CdRom0\\default.xbe";
        uint32_t buffer_va = XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_BUFFER;

        memcpy((void*)((uintptr_t)buffer_va + g_xbox_mem_offset),
               image_filename, sizeof(image_filename));
        BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME + 0) =
            (uint16_t)(sizeof(image_filename) - 1);
        BRIDGE_MEM16(XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME + 2) =
            (uint16_t)sizeof(image_filename);
        BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_XE_IMAGE_FILENAME + 4) =
            buffer_va;
    }

    fprintf(stderr, "  Kernel data exports: initialized at Xbox VA 0x%08X\n",
            XBOX_KERNEL_DATA_BASE);
}

/* -- Per-slot ordinal and bridge function ------------------ */

/* Ordinal for each slot (read from Xbox memory during init) */
static ULONG g_slot_ordinals[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Log counter - limit output to avoid flooding */
static int g_kernel_call_count = 0;

/* Read Xbox stack arg as uint32_t.
 * After kernel_thunk_dispatch pops the dummy return address (g_esp += 4),
 * arg0 is at g_esp+0, arg1 at g_esp+4, etc. */
#define STACK_ARG(n) ((uint32_t)BRIDGE_MEM32(g_esp + (n) * 4))

/* -- Per-ordinal bridge functions -------------------------
 *
 * Each bridge reads args from the Xbox stack, translates pointer
 * args from Xbox VA->native, calls the kernel function, and stores
 * the result in g_eax.
 *
 * Xbox cdecl: args pushed right-to-left, caller cleans stack.
 * Xbox stdcall: args pushed right-to-left, callee cleans stack.
 * In our case the caller (translated code) does "PUSH32" for each arg
 * before calling, and the kernel function's ret-N is handled by the
 * translated code's own stack adjustment.
 */

/* -- PsCreateSystemThreadEx (ordinal 255) ----------------
 * NTSTATUS PsCreateSystemThreadEx(
 *   PHANDLE ThreadHandle,      // arg0: Xbox VA -> pointer
 *   ULONG ThreadExtraSize,     // arg1: value
 *   ULONG KernelStackSize,     // arg2: value
 *   ULONG TlsDataSize,         // arg3: value
 *   PULONG ThreadId,           // arg4: Xbox VA -> pointer (can be NULL)
 *   PVOID StartContext1,       // arg5: Xbox VA -> opaque
 *   PVOID StartContext2,       // arg6: Xbox VA -> opaque
 *   BOOLEAN CreateSuspended,   // arg7: value
 *   BOOLEAN DebugStack,        // arg8: value
 *   PXBOX_SYSTEM_ROUTINE StartRoutine  // arg9: Xbox function pointer
 * )
 *
 * For static recompilation, we don't create a real thread.
 * Instead we call the StartRoutine synchronously via RECOMP_ICALL.
 * This is correct because on Xbox, the entry point creates a system
 * thread and returns, and the thread runs the actual game.
 */
static int g_thread_call_count = 0;

static void bridge_PsCreateSystemThreadEx(void)
{
    uint32_t xbox_handle_ptr = STACK_ARG(0);
    uint32_t start_context1  = STACK_ARG(5);
    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);
    int is_first_call = (g_thread_call_count == 0);
    g_thread_call_count++;

    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx #%d: routine=0x%08X ctx1=0x%08X ctx2=0x%08X\n",
            g_thread_call_count, start_routine, start_context1, start_context2);
    fflush(stderr);

    /* Write a fake handle to the output pointer */
    if (xbox_handle_ptr) {
        BRIDGE_MEM32(xbox_handle_ptr) = 0xBEEF0001;  /* fake handle */
    }

    /* Call the start routine synchronously through the recomp dispatch.
     * Xbox thread start routines receive two parameters:
     *   void ThreadRoutine(PVOID StartContext1, PVOID StartContext2)
     * We push both onto the simulated stack (right-to-left).
     *
     * First call: the game's main thread entry point. Must run synchronously
     * and inherit the current register state (this IS the game starting).
     *
     * Subsequent calls: worker threads. Must save/restore ALL global registers
     * because on real Xbox each thread has its own register set. Without this,
     * the worker clobbers the caller's g_esi, g_ebx, etc. */
    if (start_routine) {
        recomp_func_t fn = recomp_lookup(start_routine);
        if (!fn) fn = recomp_lookup_manual(start_routine);
        if (fn) {
            if (is_first_call) {
                /* Main game thread: run directly, inheriting register state */
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                fn();
                g_esp += 12;
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: main thread returned (g_eax=0x%08X)\n", g_eax);
                fflush(stderr);
            } else {
                /* Worker thread: run synchronously but save/restore all
                 * global registers (each Xbox thread has its own register set).
                 * The XIP file loader runs as a worker and must complete
                 * before the scene graph can be built. */
                bridge_guest_cpu_context saved_context;
                bridge_capture_guest_context(&saved_context);
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: running worker 0x%08X (ctx=0x%08X)\n",
                        start_routine, start_context1);
                fflush(stderr);

                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context2;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = start_context1;
                g_esp -= 4; BRIDGE_MEM32(g_esp) = 0;
                g_seh_ebp = g_esp;
                fn();
                fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: worker returned\n");
                fflush(stderr);

                /* Restore all registers */
                bridge_restore_guest_context(&saved_context);
            }
        } else {
            fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: start routine 0x%08X not found in dispatch!\n",
                    start_routine);
        }
    }

    g_eax = 0; /* STATUS_SUCCESS */
}

/* -- NtClose (ordinal 187) -------------------------------
 * NTSTATUS NtClose(HANDLE Handle)
 * Handle is a value (not a pointer), so safe for generic call.
 */
/* Handle-table helpers; defined further below. Xbox memory slots are 32-bit
 * but native HANDLEs are 64-bit pointers, so handles are kept in a table and
 * referenced by tagged 32-bit tokens. */
static void   bridge_write_handle(uint32_t handle_va, HANDLE h);
static HANDLE bridge_read_handle(uint32_t token);
static HANDLE bridge_take_handle(uint32_t token);
static int    bridge_trace_save_io_enabled(void);
static uint32_t s_save_trace_token;

static void bridge_NtClose(void)
{
    uint32_t raw_handle = STACK_ARG(0);

    if (bridge_trace_kernel_calls_enabled() && g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] NtClose: handle=0x%08X\n", raw_handle);
        fflush(stderr);
    }

    g_eax = 0; /* STATUS_SUCCESS */
    /* Close real handles but skip fake/synthetic ones. Route all native
     * handles through the shared implementation so per-handle kernel state
     * (including directory enumeration contexts) is released before Windows
     * can reuse the handle value. */
    if (raw_handle && raw_handle != 0xDEAD0001u && raw_handle != 0xBEEF0010u) {
        if (bridge_trace_save_io_enabled() && raw_handle == s_save_trace_token) {
            fprintf(stderr,
                    "[SAVE-IO] close func=%08X token=%08X host=%p\n",
                    g_recomp_current_func, raw_handle,
                    bridge_read_handle(raw_handle));
            fflush(stderr);
            s_save_trace_token = 0u;
        }
        HANDLE h = bridge_take_handle(raw_handle);
        if (h && h != INVALID_HANDLE_VALUE)
            g_eax = (uint32_t)xbox_NtClose(h);
    }
}

static uint32_t s_trace_contig_36k_allocs[160];
static uint32_t s_trace_contig_sizes[160];
static uint32_t s_trace_contig_36k_count;

static int bridge_trace_contig_36k_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_CONTIG_36K") != NULL;
    return enabled;
}

static int bridge_trace_contig_surfaces_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_CONTIG_SURFACES") != NULL;
    return enabled;
}

static int bridge_is_traced_contig_size(uint32_t size)
{
    if (bridge_trace_contig_36k_enabled() &&
        (size == 18432u || size == 36864u))
        return 1;
    if (bridge_trace_contig_surfaces_enabled() &&
        (size == 614400u || size == 1228800u || size == 2457600u))
        return 1;
    return 0;
}

static void bridge_trace_contig_36k_ring(const char *phase, uint32_t address,
                                         uint32_t size, uint32_t alignment)
{
    const uint32_t game_end = g_recomp_recent_game_func_idx;
    const uint32_t game_count = game_end < 24u ? game_end : 24u;
    const uint32_t full_end = g_recomp_recent_func_idx;
    const uint32_t full_count = full_end < 48u ? full_end : 48u;
    uint32_t i;

    fprintf(stderr, "[CONTIG-TRACE] %s=%08X size=%u align=%u guest=%08X game=",
            phase, address, size, alignment, g_recomp_current_func);
    for (i = 0u; i < game_count; ++i)
        fprintf(stderr, "%s%08X", i != 0u ? "," : "",
                g_recomp_recent_game_funcs[(game_end - game_count + i) & 255u]);
    fprintf(stderr, " full=");
    for (i = 0u; i < full_count; ++i)
        fprintf(stderr, "%s%08X", i != 0u ? "," : "",
                g_recomp_recent_funcs[(full_end - full_count + i) & 63u]);
    fputc('\n', stderr);
    fflush(stderr);
}

/* -- MmAllocateContiguousMemory (ordinal 165) -------------
 * PVOID MmAllocateContiguousMemory(ULONG NumberOfBytes)
 */
static void bridge_MmAllocateContiguousMemory(void)
{
    uint32_t size = STACK_ARG(0);

    /* Reserve every physical page touched by a contiguous allocation. */
    uint32_t xbox_va = xbox_HeapAllocPageRounded(size, 4096);

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemory: size=%u -> Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* -- MmAllocateContiguousMemoryEx (ordinal 166) -----------
 * PVOID MmAllocateContiguousMemoryEx(SIZE_T size, ULONG_PTR low, ULONG_PTR high,
 *                                     ULONG alignment, ULONG protect)
 */
static void bridge_MmAllocateContiguousMemoryEx(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t low = STACK_ARG(1);
    uint32_t high = STACK_ARG(2);
    uint32_t align = STACK_ARG(3);
    uint32_t prot = STACK_ARG(4);

    /* Allocate from Xbox heap with requested alignment */
    if (align < 4096) align = 4096;
    uint32_t xbox_va = xbox_HeapAllocPageRounded(size, align);

    if (bridge_is_traced_contig_size(size) &&
        s_trace_contig_36k_count < 160u) {
        s_trace_contig_36k_allocs[s_trace_contig_36k_count++] = xbox_va;
        s_trace_contig_sizes[s_trace_contig_36k_count - 1u] = size;
        bridge_trace_contig_36k_ring("alloc", xbox_va, size, align);
    }

    if (g_kernel_call_count <= 100) {
        fprintf(stderr, "  [KERNEL] MmAllocateContiguousMemoryEx: size=%u align=%u -> Xbox VA 0x%08X\n",
                size, align, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

/* PVOID MmAllocateSystemMemory(ULONG NumberOfBytes, ULONG Protect)
 * (ordinal 167)
 *
 * The host-side xbox_MmAllocateSystemMemory implementation returns a native
 * pointer. Recompiled code must instead receive an Xbox VA so MEM8/MEM32 can
 * translate it through the shared guest-memory mapping. Keep system-memory
 * allocations in the same tracked guest heap as the other bridge allocators.
 */
static void bridge_MmAllocateSystemMemory(void)
{
    const uint32_t size = STACK_ARG(0);
    const uint32_t protect = STACK_ARG(1);
    const uint32_t xbox_va = xbox_HeapAlloc(size, 4096);

    XBOX_TRACE(XBOX_LOG_MEM,
               "MmAllocateSystemMemory: size=%u protect=0x%08X -> Xbox VA 0x%08X",
               size, protect, xbox_va);
    g_eax = xbox_va;
}

/* ULONG MmFreeSystemMemory(PVOID BaseAddress, ULONG NumberOfBytes)
 * (ordinal 172). The guest heap records the allocation size, so the Xbox
 * NumberOfBytes argument is retained for ABI fidelity but is not needed by
 * the host allocator.
 */
static void bridge_MmFreeSystemMemory(void)
{
    const uint32_t xbox_va = STACK_ARG(0);
    const uint32_t size = STACK_ARG(1);
    const uint32_t allocation_size = xbox_HeapGetAllocationSize(xbox_va);
    const uint32_t freed_pages = (allocation_size + 4095u) / 4096u;

    XBOX_TRACE(XBOX_LOG_MEM,
               "MmFreeSystemMemory: Xbox VA 0x%08X size=%u pages=%u",
               xbox_va, size, freed_pages);
    xbox_HeapFree(xbox_va);
    g_eax = freed_pages;
}

/* ULONG MmQueryAllocationSize(PVOID BaseAddress) (ordinal 180) */
static void bridge_MmQueryAllocationSize(void)
{
    uint32_t xbox_va = STACK_ARG(0);
    uint32_t size = xbox_HeapGetAllocationSize(xbox_va);

    XBOX_TRACE(XBOX_LOG_MEM,
               "MmQueryAllocationSize(0x%08X) = %u", xbox_va, size);
    g_eax = size;
}

/* -- MmFreeContiguousMemory (ordinal 171) -----------------
 * VOID MmFreeContiguousMemory(PVOID BaseAddress)
 */
static void bridge_MmFreeContiguousMemory(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t i;
    if (bridge_trace_contig_36k_enabled() ||
        bridge_trace_contig_surfaces_enabled()) {
        for (i = 0u; i < s_trace_contig_36k_count; ++i) {
            if (s_trace_contig_36k_allocs[i] == addr) {
                s_trace_contig_36k_allocs[i] = 0u;
                bridge_trace_contig_36k_ring(
                    "free", addr, s_trace_contig_sizes[i], 0u);
                break;
            }
        }
    }
    xbox_HeapFree(addr);
    g_eax = 0;
}

/* MmClaimGpuInstanceMemory (ordinal 168)
 * PVOID MmClaimGpuInstanceMemory(ULONG NumberOfBytes,
 *                                PULONG NumberOfPaddingBytes)
 */
static void bridge_MmClaimGpuInstanceMemory(void)
{
    const uint32_t size = STACK_ARG(0);
    const uint32_t padding_va = STACK_ARG(1);
    ULONG padding = 0;
    PVOID result = xbox_MmClaimGpuInstanceMemory(size, &padding);

    if (padding_va >= 0x00010000u && padding_va < 0x04000000u)
        BRIDGE_MEM32(padding_va) = padding;

    if (bridge_trace_kernel_calls_enabled() && g_kernel_call_count <= 200) {
        fprintf(stderr,
                "  [KERNEL] MmClaimGpuInstanceMemory: size=%u "
                "padding_va=%08X padding=%08X result=%08X\n",
                size, padding_va, padding, (uint32_t)(uintptr_t)result);
        fflush(stderr);
    }

    g_eax = (uint32_t)(uintptr_t)result;
}

/* -- NtAllocateVirtualMemory (ordinal 184) ----------------
 * NTSTATUS NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG ZeroBits,
 *     PULONG AllocationSize, ULONG AllocationType, ULONG Protect)
 */
static void bridge_NtAllocateVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);  /* PVOID* in Xbox VA */
    uint32_t zero_bits = STACK_ARG(1);
    uint32_t size_ptr = STACK_ARG(2);  /* PULONG in Xbox VA */
    uint32_t alloc_type = STACK_ARG(3);
    uint32_t protect = STACK_ARG(4);

    /* Read the requested size from Xbox memory */
    uint32_t size = size_ptr ? BRIDGE_MEM32(size_ptr) : 0;
    /* Read the base address hint (0 = let kernel choose) */
    uint32_t base_hint = base_ptr ? BRIDGE_MEM32(base_ptr) : 0;

    if (bridge_trace_kernel_calls_enabled() && g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] NtAllocateVirtualMemory: base=0x%08X size=%u type=0x%X prot=0x%X\n",
                base_hint, size, alloc_type, protect);
        fflush(stderr);
    }

    if (size == 0) {
        g_eax = 0xC0000045u; /* STATUS_INVALID_PAGE_PROTECTION */
        return;
    }

    /*
     * Xbox NtAllocateVirtualMemory supports two modes:
     * - MEM_RESERVE (0x2000): Reserve virtual address space
     * - MEM_COMMIT  (0x1000): Commit pages within a reserved region
     * - MEM_RESERVE|MEM_COMMIT (0x3000): Both in one call
     *
     * Our Xbox heap (bump allocator) always commits memory immediately,
     * so MEM_COMMIT on an already-reserved region is a no-op.
     * Only allocate new memory when MEM_RESERVE is requested.
     */
    if (base_hint != 0 && (alloc_type & 0x2000) == 0) {
        /* MEM_COMMIT only, on an already-reserved region.
         * The memory is already committed by our bump allocator.
         * Don't change the base address - just return success. */
        if (bridge_trace_kernel_calls_enabled() && g_kernel_call_count <= 200) {
            fprintf(stderr, "  [KERNEL] -> MEM_COMMIT on existing region 0x%08X, no-op\n", base_hint);
            fflush(stderr);
        }
        g_eax = 0; /* STATUS_SUCCESS */
        return;
    }

    /* Allocate from Xbox heap (MEM_RESERVE or MEM_RESERVE|MEM_COMMIT) */
    uint32_t xbox_va = xbox_HeapAlloc(size, 4096);
    if (!xbox_va) {
        g_eax = 0xC0000017u; /* STATUS_NO_MEMORY */
        return;
    }

    /* Write back the allocated address and actual size */
    if (base_ptr) BRIDGE_MEM32(base_ptr) = xbox_va;
    if (size_ptr) BRIDGE_MEM32(size_ptr) = size;

    g_eax = 0; /* STATUS_SUCCESS */
}

/* -- NtFreeVirtualMemory (ordinal 199) --------------------
 * NTSTATUS NtFreeVirtualMemory(PVOID *BaseAddress, PULONG FreeSize,
 *     ULONG FreeType)
 */
static void bridge_NtFreeVirtualMemory(void)
{
    uint32_t base_ptr = STACK_ARG(0);
    uint32_t size_ptr = STACK_ARG(1);
    uint32_t free_type = STACK_ARG(2);

    g_eax = (uint32_t)xbox_NtFreeVirtualMemory(
        XBOX_TO_NATIVE(base_ptr), XBOX_TO_NATIVE(size_ptr), free_type);
}

/* -- ExAllocatePool / ExAllocatePoolWithTag (ordinals 14, 15) -
 * Must allocate from Xbox heap so the returned pointer is an Xbox VA
 * that can be accessed via MEM32(). Native HeapAlloc returns 64-bit
 * pointers that get truncated and produce garbage Xbox VAs.
 */
static void bridge_ExAllocatePool(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (bridge_trace_kernel_calls_enabled() && g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] ExAllocatePool: size=%u -> Xbox VA 0x%08X\n",
                size, xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}

static void bridge_ExAllocatePoolWithTag(void)
{
    uint32_t size = STACK_ARG(0);
    uint32_t tag = STACK_ARG(1);
    uint32_t xbox_va = xbox_HeapAlloc(size, 16);

    if (bridge_trace_kernel_calls_enabled() && g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] ExAllocatePoolWithTag: size=%u tag='%c%c%c%c' -> Xbox VA 0x%08X\n",
                size,
                (char)(tag & 0xFF), (char)((tag >> 8) & 0xFF),
                (char)((tag >> 16) & 0xFF), (char)((tag >> 24) & 0xFF),
                xbox_va);
        fflush(stderr);
    }

    g_eax = xbox_va;
}
static void bridge_ExFreePool(void)
{
    uint32_t xbox_va = STACK_ARG(0);
    xbox_HeapFree(xbox_va);
    g_eax = 0;
}


/* ULONG ExQueryPoolBlockSize(PVOID PoolBlock) (ordinal 23) */
static void bridge_ExQueryPoolBlockSize(void)
{
    uint32_t xbox_va = STACK_ARG(0);
    uint32_t size = xbox_HeapGetRequestedSize(xbox_va);

    if (!size) size = xbox_HeapGetAllocationSize(xbox_va);
    XBOX_TRACE(XBOX_LOG_POOL,
               "ExQueryPoolBlockSize(0x%08X) = %u", xbox_va, size);
    g_eax = size;
}

/* -- KfRaiseIrql / KfLowerIrql (ordinals 160, 161) ------ */
static void bridge_KfRaiseIrql(void)
{
    /* Xbox Kf* uses fastcall: CL, not the caller's stack, holds NewIrql. */
    uint32_t new_irql = g_ecx & 0xFFu;
    g_eax = (uint32_t)xbox_KfRaiseIrql((UCHAR)new_irql);
    /* Translated fs:[0x24] reads the guest KPCR's CurrentIrql byte. */
    BRIDGE_MEM8(0x24u) = (uint8_t)new_irql;
}

static void bridge_KfLowerIrql(void)
{
    uint32_t new_irql = g_ecx & 0xFFu;
    xbox_KfLowerIrql((UCHAR)new_irql);
    BRIDGE_MEM8(0x24u) = (uint8_t)new_irql;
    g_eax = 0;
}

static void bridge_KeStallExecutionProcessor(void)
{
    uint32_t microseconds = STACK_ARG(0);
    xbox_KeStallExecutionProcessor(microseconds);
    g_eax = 0;
}

/* -- KeRaiseIrqlToDpcLevel (ordinal 129) ------------------- */
static void bridge_KeRaiseIrqlToDpcLevel(void)
{
    g_eax = (uint32_t)xbox_KeRaiseIrqlToDpcLevel();
    BRIDGE_MEM8(0x24u) = DISPATCH_LEVEL;
}

/* -- RtlInitializeCriticalSection / Enter / Leave (ordinals 291, 277, 294) - */
static void bridge_RtlInitializeCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlInitializeCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlEnterCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlEnterCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

static void bridge_RtlLeaveCriticalSection(void)
{
    uint32_t cs_va = STACK_ARG(0);
    xbox_RtlLeaveCriticalSection(XBOX_TO_NATIVE(cs_va));
    g_eax = 0;
}

/* -- KeQueryPerformanceCounter / Frequency (ordinals 126, 127) - */
static void bridge_KeQueryPerformanceCounter(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceCounter();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

static void bridge_KeQueryPerformanceFrequency(void)
{
    LARGE_INTEGER li = xbox_KeQueryPerformanceFrequency();
    g_eax = (uint32_t)li.LowPart;
    g_edx = (uint32_t)li.HighPart;
}

/* -- KeQuerySystemTime (ordinal 128) ----------------------- */
static void bridge_KeQuerySystemTime(void)
{
    uint32_t time_ptr = STACK_ARG(0);
    xbox_KeQuerySystemTime(XBOX_TO_NATIVE(time_ptr));
    g_eax = 0;
}


/* RtlTimeFieldsToTime / RtlTimeToTimeFields (ordinals 304, 305).
 * Both structures live in guest memory, so translate both pointers before
 * delegating to the shared kernel RTL implementation. */
static void bridge_RtlTimeFieldsToTime(void)
{
    uint32_t fields_va = STACK_ARG(0);
    uint32_t time_va = STACK_ARG(1);

    g_eax = (uint32_t)xbox_RtlTimeFieldsToTime(
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(fields_va),
        (PLARGE_INTEGER)XBOX_TO_NATIVE(time_va));
}

static void bridge_RtlTimeToTimeFields(void)
{
    uint32_t time_va = STACK_ARG(0);
    uint32_t fields_va = STACK_ARG(1);

    xbox_RtlTimeToTimeFields(
        (PLARGE_INTEGER)XBOX_TO_NATIVE(time_va),
        (PXBOX_TIME_FIELDS)XBOX_TO_NATIVE(fields_va));
    g_eax = 0;
}
/* KeDelayExecutionThread (ordinal 99). */
static void bridge_KeDelayExecutionThread(void)
{
    uint32_t wait_mode = STACK_ARG(0);
    uint32_t alertable = STACK_ARG(1);
    uint32_t interval_va = STACK_ARG(2);
    static int trace_enabled = -1;
    static uint32_t trace_count = 0;
    static uint32_t trace_dump_at = 1024u;
    static uint32_t trace_func_arm_at = 0u;

    if (trace_enabled < 0) {
        const char *trace = getenv("MERCENARIES_TRACE_DELAYS");
        const char *dump_at = getenv("MERCENARIES_TRACE_DELAY_DUMP_AT");
        const char *func_arm_at = getenv("MERCENARIES_TRACE_FUNC_ARM_AT_DELAY");
        trace_enabled = trace && trace[0] && trace[0] != '0';
        if (dump_at && dump_at[0]) {
            const unsigned long parsed = strtoul(dump_at, NULL, 0);
            if (parsed > 0u && parsed <= UINT32_MAX)
                trace_dump_at = (uint32_t)parsed;
        }
        if (func_arm_at && func_arm_at[0]) {
            const unsigned long parsed = strtoul(func_arm_at, NULL, 0);
            if (parsed > 0u && parsed <= UINT32_MAX) {
                trace_func_arm_at = (uint32_t)parsed;
                g_recomp_func_trace_armed = 0u;
            }
        }
    }
    if (trace_enabled && trace_count < trace_dump_at) {
        const LARGE_INTEGER *interval =
            (const LARGE_INTEGER *)XBOX_TO_NATIVE(interval_va);
        const uint32_t stream_manager = BRIDGE_MEM32(0x0085BF18u);
        const uint32_t stream_active =
            (stream_manager >= 0x00010000u && stream_manager < 0x08000000u)
                ? BRIDGE_MEM32(stream_manager + 0x218u) : 0u;
        const uint32_t stream_pending_begin =
            (stream_manager >= 0x00010000u && stream_manager < 0x08000000u)
                ? BRIDGE_MEM32(stream_manager + 0x110u) : 0u;
        const uint32_t stream_pending_end =
            (stream_manager >= 0x00010000u && stream_manager < 0x08000000u)
                ? BRIDGE_MEM32(stream_manager + 0x114u) : 0u;
        const uint32_t texture_pending_root = BRIDGE_MEM32(0x007934ECu);
        const uint32_t texture_pending_object =
            (texture_pending_root >= 0x00010000u &&
             texture_pending_root < 0x08000000u)
                ? BRIDGE_MEM32(texture_pending_root + 8u) : 0u;
        const uint32_t texture_vtable =
            (texture_pending_object >= 0x00010000u &&
             texture_pending_object < 0x08000000u)
                ? BRIDGE_MEM32(texture_pending_object) : 0u;
        const uint32_t texture_request =
            (texture_pending_object >= 0x00010000u &&
             texture_pending_object < 0x08000000u)
                ? BRIDGE_MEM32(texture_pending_object + 0x10u) : 0u;
        const uint32_t texture_states =
            (texture_pending_object >= 0x00010000u &&
             texture_pending_object < 0x08000000u)
                ? BRIDGE_MEM32(texture_pending_object + 0x20u) : 0u;
        const uint32_t texture_name =
            (texture_pending_object >= 0x00010000u &&
             texture_pending_object < 0x08000000u)
                ? BRIDGE_MEM32(texture_pending_object + 0x24u) : 0u;
        const uint32_t load_texture = BRIDGE_MEM32(0x00365D50u);
        const int load_texture_valid =
            load_texture >= 0x00010000u && load_texture < 0x08000000u;
        const uint32_t d3d_device = BRIDGE_MEM32(0x00299378u);
        const uint32_t vblank_callback =
            (d3d_device >= 0x00010000u && d3d_device < 0x08000000u)
                ? BRIDGE_MEM32(d3d_device + 0x1DB8u) : 0u;
        fprintf(stderr,
                "[DELAY-TRACE] #%u func=%08X esp=%08X ebp=%08X mode=%u alert=%u "
                "interval=%lld model_pending=%u texture_pending=%u "
                "vblank=%u device=%08X vblank_cb=%08X "
                "load_hash=%08X load_texture=%08X stream=%08X active=%08X "
                "pending=%u/%u tex_root=%08X tex_obj=%08X tex_vtbl=%08X "
                "tex_req=%08X tex_state=%02X/%02X tex_list=%02X tex_name=%08X\n",
                trace_count, g_recomp_current_func, g_esp, g_seh_ebp, wait_mode,
                alertable, interval ? (long long)interval->QuadPart : 0ll,
                BRIDGE_MEM32(0x00643890u), BRIDGE_MEM32(0x007934F8u),
                BRIDGE_MEM32(0x00793564u), d3d_device, vblank_callback,
                BRIDGE_MEM32(0x00365D48u), BRIDGE_MEM32(0x00365D50u),
                stream_manager, stream_active, stream_pending_begin,
                stream_pending_end, texture_pending_root,
                texture_pending_object, texture_vtable, texture_request,
                texture_states & 0xFFu, (texture_states >> 8) & 0xFFu,
                (texture_states >> 16) & 0xFFu, texture_name);
        if (load_texture_valid) {
            fprintf(stderr,
                    "[REDTEXTURE-TRACE] obj=%08X size=%ux%u mips=%u "
                    "header=%08X/%08X/%08X/%08X/%08X ptex=%08X "
                    "pdata=%08X bytes=%u type=%02X format=%02X\n",
                    load_texture,
                    BRIDGE_MEM16(load_texture + 0x28u),
                    BRIDGE_MEM16(load_texture + 0x2Au),
                    BRIDGE_MEM16(load_texture + 0x2Cu),
                    BRIDGE_MEM32(load_texture + 0x30u),
                    BRIDGE_MEM32(load_texture + 0x34u),
                    BRIDGE_MEM32(load_texture + 0x38u),
                    BRIDGE_MEM32(load_texture + 0x3Cu),
                    BRIDGE_MEM32(load_texture + 0x40u),
                    BRIDGE_MEM32(load_texture + 0x44u),
                    BRIDGE_MEM32(load_texture + 0x48u),
                    BRIDGE_MEM32(load_texture + 0x58u),
                    BRIDGE_MEM8(load_texture + 0x5Cu),
                    BRIDGE_MEM8(load_texture + 0x5Du));
        }
        if (stream_manager >= 0x00010000u &&
            stream_manager < 0x08000000u &&
            stream_pending_begin < 64u) {
            const uint32_t io_request = BRIDGE_MEM32(
                stream_manager + 0x118u + stream_pending_begin * 4u);
            if (io_request >= 0x00010000u && io_request < 0x08000000u) {
                fprintf(stderr,
                        "[PBL-TRACE] req=%08X buffer=%08X length=%u "
                        "type=%u transfer=%u sectors=%u-%u file=%08X "
                        "requested=%u state=%u stream_file=%08X\n",
                        io_request, BRIDGE_MEM32(io_request + 0x0Cu),
                        BRIDGE_MEM32(io_request + 0x14u),
                        BRIDGE_MEM32(io_request + 0x18u),
                        BRIDGE_MEM32(io_request + 0x1Cu),
                        BRIDGE_MEM32(io_request + 0x20u),
                        BRIDGE_MEM32(io_request + 0x24u),
                        BRIDGE_MEM32(io_request + 0x28u),
                        BRIDGE_MEM8(io_request + 0x2Cu),
                        BRIDGE_MEM32(io_request + 0x48u),
                        BRIDGE_MEM32(io_request + 0x4Cu));
            }
        }
        if (texture_request >= 0x00010000u && texture_request < 0x08000000u) {
            const uint32_t read_request = BRIDGE_MEM32(texture_request + 0x14u);
            const uint32_t asset_state = BRIDGE_MEM32(texture_request + 0x18u);
            const uint32_t io_request =
                (read_request >= 0x00010000u && read_request < 0x08000000u)
                    ? BRIDGE_MEM32(read_request + 0x28u) : 0u;
            fprintf(stderr,
                    "[ASSET-TRACE] asset=%08X state=%u pbl_read=%08X "
                    "read_status=%u data=%08X io=%08X io_state=%u\n",
                    texture_request, asset_state, read_request,
                    (read_request >= 0x00010000u && read_request < 0x08000000u)
                        ? BRIDGE_MEM32(read_request + 0x20u) : 0u,
                    (read_request >= 0x00010000u && read_request < 0x08000000u)
                        ? BRIDGE_MEM32(read_request + 0x24u) : 0u,
                    io_request,
                    (io_request >= 0x00010000u && io_request < 0x08000000u)
                        ? BRIDGE_MEM32(io_request + 0x48u) : 0u);
        }
        if (trace_count == 1u) {
            const uint32_t library_count = BRIDGE_MEM32(0x00842E4Cu);
            fprintf(stderr, "[LIBRARY-TRACE] count=%u\n", library_count);
            for (uint32_t i = 0u; i < library_count && i < 4u; ++i) {
                const uint32_t library = 0x00842E78u + i * 0x134u;
                const uint32_t directory_base = BRIDGE_MEM32(library + 0x120u);
                const uint32_t directory_count = BRIDGE_MEM32(library + 0x128u);
                fprintf(stderr,
                        "[LIBRARY-TRACE] lib=%u addr=%08X file=%08X "
                        "dir=%08X count=%u\n",
                        i, library, BRIDGE_MEM32(library + 0x104u),
                        directory_base, directory_count);
                for (uint32_t j = 0u;
                     directory_base >= 0x00010000u &&
                     directory_base < 0x08000000u &&
                     j < directory_count && j < 8u;
                     ++j) {
                    const uint32_t type = BRIDGE_MEM32(directory_base + j * 8u);
                    const uint32_t inner = BRIDGE_MEM32(directory_base + j * 8u + 4u);
                    const uint32_t inner_base =
                        (inner >= 0x00010000u && inner < 0x08000000u)
                            ? BRIDGE_MEM32(inner) : 0u;
                    const uint32_t inner_count =
                        (inner >= 0x00010000u && inner < 0x08000000u)
                            ? BRIDGE_MEM32(inner + 8u) : 0u;
                    fprintf(stderr,
                            "[LIBRARY-TRACE]   type=%08X inner=%08X "
                            "base=%08X count=%u\n",
                            type, inner, inner_base, inner_count);
                }
            }
        }        ++trace_count;
        if (trace_func_arm_at && trace_count == trace_func_arm_at)
            g_recomp_func_trace_armed = 1u;
        if (trace_count == trace_dump_at)
            g_recomp_trace_dump_requested = 1u;
    }

    if (alertable && bridge_deliver_guest_apc()) {
        g_eax = (uint32_t)STATUS_ALERTED;
        return;
    }

    g_eax = (uint32_t)xbox_KeDelayExecutionThread(
        (KPROCESSOR_MODE)wait_mode,
        (BOOLEAN)alertable,
        (PLARGE_INTEGER)XBOX_TO_NATIVE(interval_va));
}

/* -- MmQueryStatistics (ordinal 181) ----------------------- */
static void bridge_MmQueryStatistics(void)
{
    uint32_t stats_ptr = STACK_ARG(0);
    g_eax = (uint32_t)xbox_MmQueryStatistics(XBOX_TO_NATIVE(stats_ptr));
}

/* -- NtCreateEvent (ordinal 189) --------------------------- */
static void bridge_NtCreateEvent(void)
{
    uint32_t handle_ptr = STACK_ARG(0);
    uint32_t obj_attr_ptr = STACK_ARG(1);
    uint32_t event_type = STACK_ARG(2);
    uint32_t initial_state = STACK_ARG(3);

    /* Use local HANDLE to avoid 8-byte write to 4-byte Xbox memory slot.
     * On x64, HANDLE is 8 bytes but Xbox expects 4-byte handles. */
    HANDLE local_handle = NULL;
    NTSTATUS status = xbox_NtCreateEvent(
        &local_handle,
        XBOX_TO_NATIVE(obj_attr_ptr),
        event_type, initial_state);

    if (handle_ptr) {
        bridge_write_handle(handle_ptr, local_handle);
    }

    fprintf(stderr, "  [BRIDGE] NtCreateEvent: handle_ptr=0x%08X type=%u init=%u -> status=0x%08X handle=0x%08X\n",
            handle_ptr, event_type, initial_state, (uint32_t)status,
            (uint32_t)(uintptr_t)local_handle);

    g_eax = (uint32_t)status;
}

/* -- KeSetEvent (ordinal 145) ------------------------------ */
static void bridge_KeSetEvent(void)
{
    uint32_t event_ptr = STACK_ARG(0);
    uint32_t increment = STACK_ARG(1);
    uint32_t wait = STACK_ARG(2);

    g_eax = (uint32_t)xbox_KeSetEvent(XBOX_TO_NATIVE(event_ptr), increment, (BOOLEAN)wait);
}

/* -- KeWaitForSingleObject (ordinal 159) ------------------- */
static void bridge_KeWaitForSingleObject(void)
{
    uint32_t object = STACK_ARG(0);
    uint32_t wait_reason = STACK_ARG(1);
    uint32_t wait_mode = STACK_ARG(2);
    uint32_t alertable = STACK_ARG(3);
    uint32_t timeout_ptr = STACK_ARG(4);

    g_eax = (uint32_t)xbox_KeWaitForSingleObject(
        XBOX_TO_NATIVE(object), wait_reason, wait_mode,
        (BOOLEAN)alertable, XBOX_TO_NATIVE(timeout_ptr));
}

/* -- NtYieldExecution (ordinal 238) ------------------------ */
static void bridge_NtYieldExecution(void)
{
    g_eax = (uint32_t)xbox_NtYieldExecution();
}

/* -- MmGetPhysicalAddress (ordinal 173) -------------------- */
static void bridge_MmGetPhysicalAddress(void)
{
    uint32_t addr = STACK_ARG(0);
    /* Xbox uses identity mapping (physical == virtual) for the lower 64MB.
     * Just return the Xbox VA as-is. Don't call xbox_MmGetPhysicalAddress
     * which would return a native pointer. */
    g_eax = addr;
}

/* -- MmSetAddressProtect (ordinal 182) --------------------- */
static void bridge_MmLockUnlockBufferPages(void)
{
    uint32_t base = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t unlock = STACK_ARG(2);
    xbox_MmLockUnlockBufferPages(
        XBOX_TO_NATIVE(base), size, (BOOLEAN)unlock);
    g_eax = 0;
}

static void bridge_MmSetAddressProtect(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t size = STACK_ARG(1);
    uint32_t prot = STACK_ARG(2);

    xbox_MmSetAddressProtect(XBOX_TO_NATIVE(addr), size, prot);
    g_eax = 0;
}

/* -- AvSetDisplayMode (ordinal 3) -------------------------- */
/* AvSendTVEncoderOption (ordinal 2). */
static void bridge_AvGetSavedDataAddress(void)
{
    g_eax = xbox_AvGetSavedDataAddress();
}

static void bridge_AvSetSavedDataAddress(void)
{
    xbox_AvSetSavedDataAddress(STACK_ARG(0));
    g_eax = 0;
}

static void bridge_AvSendTVEncoderOption(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t option = STACK_ARG(1);
    uint32_t param = STACK_ARG(2);
    uint32_t result = STACK_ARG(3);

    xbox_AvSendTVEncoderOption(
        XBOX_TO_NATIVE(addr), option, param,
        (PULONG)XBOX_TO_NATIVE(result));
    g_eax = 0;
}

static void bridge_AvSetDisplayMode(void)
{
    uint32_t addr = STACK_ARG(0);
    uint32_t step = STACK_ARG(1);
    uint32_t mode = STACK_ARG(2);
    uint32_t format = STACK_ARG(3);
    uint32_t pitch = STACK_ARG(4);
    uint32_t fb = STACK_ARG(5);

    xbox_AvSetDisplayMode(XBOX_TO_NATIVE(addr), step, mode, format, pitch, fb);
    g_eax = 0;
}

/* -- PsTerminateSystemThread (ordinal 258) ---------------
 * VOID PsTerminateSystemThread(NTSTATUS ExitStatus)
 *
 * On real Xbox, this terminates the calling thread (never returns).
 * In our recompiled version, threads run synchronously, so we just
 * return. The caller (sub_001D1818) handles this gracefully.
 */
static void bridge_PsTerminateSystemThread(void)
{
    uint32_t exit_status = STACK_ARG(0);

    fprintf(stderr, "  [KERNEL] PsTerminateSystemThread: status=0x%08X\n", exit_status);
    fflush(stderr);

    g_eax = exit_status;
    /* Simply return - caller will clean up */
}

/* -- HalReadSMCTrayState (ordinal 47) ---------------------
 * VOID HalReadSMCTrayState(PDWORD TrayState, PDWORD TrayStateChangeCount)
 *
 * Returns DVD tray state. 0x10 = no disc, 0x14 = tray closed with disc.
 */
static void bridge_HalReadSMCTrayState(void)
{
    uint32_t state_ptr = STACK_ARG(0);
    uint32_t count_ptr = STACK_ARG(1);

    if (state_ptr) BRIDGE_MEM32(state_ptr) = 0x10;  /* No disc */
    if (count_ptr) BRIDGE_MEM32(count_ptr) = 0;
    g_eax = 0;
}

/* -- KeInitializeDpc (ordinal 107) ------------------------
 * VOID KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
 */
static void bridge_HalGetInterruptVector(void)
{
    uint32_t bus_level = STACK_ARG(0);
    uint32_t irql_va = STACK_ARG(1);

    g_last_interrupt_bus_level = bus_level;
    g_eax = xbox_HalGetInterruptVector(
        bus_level, irql_va ? (PKIRQL)XBOX_TO_NATIVE(irql_va) : NULL);
    if (bridge_trace_interrupts_enabled()) {
        fprintf(stderr,
                "[GUEST-IRQ] HalGetInterruptVector bus=%u vector=%u irql=%u irql_va=%08X\n",
                bus_level, g_eax,
                irql_va ? (unsigned int)BRIDGE_MEM8(irql_va) : 0u, irql_va);
    }
}
static void bridge_HalReadWritePCISpace(void)
{
    uint32_t bus_number = STACK_ARG(0);
    uint32_t slot_number = STACK_ARG(1);
    uint32_t register_number = STACK_ARG(2);
    uint32_t buffer_va = STACK_ARG(3);
    uint32_t length = STACK_ARG(4);
    BOOLEAN write_pci_space = (BOOLEAN)STACK_ARG(5);

    xbox_HalReadWritePCISpace(
        bus_number, slot_number, register_number,
        XBOX_TO_NATIVE(buffer_va), length, write_pci_space);
    g_eax = 0;
}


static void bridge_HalRegisterShutdownNotification(void)
{
    uint32_t registration_va = STACK_ARG(0);
    BOOLEAN should_register = (BOOLEAN)STACK_ARG(1);

    xbox_HalRegisterShutdownNotification(
        XBOX_TO_NATIVE(registration_va), should_register);
    g_eax = 0;
}

static void bridge_HalReturnToFirmware(void)
{
    uint32_t routine = STACK_ARG(0);
    uint32_t recent_end = g_recomp_recent_func_idx;
    uint32_t recent_count = recent_end < 16u ? recent_end : 16u;

    void *frames[8] = {0};
    HMODULE module = GetModuleHandleW(NULL);
    USHORT frame_count;
    USHORT i;
    uint32_t recent_i;

    fprintf(stderr,
            "  [KERNEL] HalReturnToFirmware: routine=%u, native caller RVAs:",
            routine);
    frame_count = CaptureStackBackTrace(0, 8, frames, NULL);
    for (i = 0; i < frame_count; ++i) {
        uintptr_t address = (uintptr_t)frames[i];
        uintptr_t base = (uintptr_t)module;
        if (address >= base) {
            fprintf(stderr, " 0x%llX",
                    (unsigned long long)(address - base));
        }
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "  [KERNEL] HalReturnToFirmware: guest current=%08X recent:",
            (uint32_t)g_recomp_current_func);
    for (recent_i = recent_count; recent_i > 0u; --recent_i) {
        fprintf(stderr, " %08X",
                (uint32_t)g_recomp_recent_funcs[(recent_end - recent_i) & 63u]);
    }
    fprintf(stderr, "\n");
    xbox_HalReturnToFirmware(routine);
    /* Does not return. */
}

static void bridge_KeInitializeInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t service_routine = STACK_ARG(1);
    uint32_t service_context = STACK_ARG(2);
    uint32_t vector = STACK_ARG(3);
    uint32_t irql = STACK_ARG(4) & 0xFFu; /* KIRQL is an 8-bit argument. */
    uint32_t interrupt_mode = STACK_ARG(5);
    uint32_t share_vector = STACK_ARG(6);
    bridge_guest_interrupt *interrupt = NULL;
    uint32_t i;

    for (i = 0; i < g_guest_interrupt_count; ++i) {
        if (g_guest_interrupts[i].interrupt_va == interrupt_va) {
            interrupt = &g_guest_interrupts[i];
            break;
        }
    }
    if (!interrupt &&
        g_guest_interrupt_count < BRIDGE_GUEST_INTERRUPT_CAPACITY) {
        interrupt = &g_guest_interrupts[g_guest_interrupt_count++];
    }
    if (interrupt) {
        interrupt->interrupt_va = interrupt_va;
        interrupt->service_routine_va = service_routine;
        interrupt->service_context_va = service_context;
        interrupt->bus_level = g_last_interrupt_bus_level;
        interrupt->vector = vector;
        interrupt->irql = irql;
        InterlockedExchange(&interrupt->connected, 0);
    }

    if (bridge_trace_interrupts_enabled()) {
        fprintf(stderr,
                "[GUEST-IRQ] KeInitializeInterrupt bus=%u object=%08X isr=%08X context=%08X "
                "vector=%u irql=%u mode=%u share=%u\n",
                g_last_interrupt_bus_level, interrupt_va, service_routine,
                service_context, vector, irql, interrupt_mode, share_vector);
    }
    g_eax = 0;
}

static void bridge_KeConnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t i;

    for (i = 0; i < g_guest_interrupt_count; ++i) {
        if (g_guest_interrupts[i].interrupt_va == interrupt_va) {
            InterlockedExchange(&g_guest_interrupts[i].connected, 1);
            break;
        }
    }
    if (bridge_trace_interrupts_enabled())
        fprintf(stderr, "[GUEST-IRQ] KeConnectInterrupt object=%08X found=%u\n",
                interrupt_va, i < g_guest_interrupt_count);
    g_eax = i < g_guest_interrupt_count ? TRUE : FALSE;
}

static void bridge_KeDisconnectInterrupt(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t i;
    LONG was_connected = 0;

    for (i = 0; i < g_guest_interrupt_count; ++i) {
        if (g_guest_interrupts[i].interrupt_va == interrupt_va) {
            was_connected = InterlockedExchange(
                &g_guest_interrupts[i].connected, 0);
            break;
        }
    }
    g_eax = was_connected ? TRUE : FALSE;
}

/*
 *                       PVOID DeferredContext)
 *
 * Initializes a DPC object. The Xbox KDPC structure is 32 bytes.
 * We zero it and set the routine and context pointers.
 */
static void bridge_KeInitializeDpc(void)
{
    uint32_t dpc_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);

    /* Zero the structure (32 bytes) */
    memset(XBOX_TO_NATIVE(dpc_va), 0, 32);

    /* Set Type (0x13 = DpcObject) and fields */
    BRIDGE_MEM16(dpc_va + 0) = 0x13;   /* Type */
    BRIDGE_MEM32(dpc_va + 12) = routine; /* DeferredRoutine */
    BRIDGE_MEM32(dpc_va + 16) = context; /* DeferredContext */
    g_eax = 0;
}

static BOOLEAN bridge_invoke_guest_dpc(uint32_t dpc_va,
                                       uint32_t system_argument1,
                                       uint32_t system_argument2)
{
    uint32_t routine = dpc_va ? BRIDGE_MEM32(dpc_va + 12u) : 0u;
    uint32_t context = dpc_va ? BRIDGE_MEM32(dpc_va + 16u) : 0u;
    bridge_guest_cpu_context saved_context;
    recomp_func_t fn;
    uint32_t saved_esp;

    if (!dpc_va || !routine)
        return FALSE;

    BRIDGE_MEM32(dpc_va + 20u) = system_argument1;
    BRIDGE_MEM32(dpc_va + 24u) = system_argument2;
    fn = bridge_lookup_guest_function(routine);
    if (!fn) {
        fprintf(stderr, "[GUEST-DPC] unresolved routine=%08X dpc=%08X\n",
                routine, dpc_va);
        return FALSE;
    }

    bridge_capture_guest_context(&saved_context);
    saved_esp = saved_context.esp;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = system_argument2;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = system_argument1;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = dpc_va;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0u;
    fn();
    if (g_esp != saved_esp && bridge_trace_interrupts_enabled()) {
        fprintf(stderr,
                "[GUEST-DPC] stack mismatch routine=%08X expected=%08X got=%08X\n",
                routine, saved_esp, g_esp);
    }
    bridge_restore_guest_context(&saved_context);
    if (bridge_trace_interrupts_enabled() && g_guest_interrupt_log_count < 64u) {
        fprintf(stderr,
                "[GUEST-DPC] delivered dpc=%08X routine=%08X context=%08X\n",
                dpc_va, routine, context);
        ++g_guest_interrupt_log_count;
    }
    return TRUE;
}

static void bridge_KeInsertQueueDpc(void)
{
    const uint32_t dpc_va = STACK_ARG(0);
    const uint32_t system_argument1 = STACK_ARG(1);
    const uint32_t system_argument2 = STACK_ARG(2);

    g_eax = bridge_queue_guest_dpc(
        dpc_va, system_argument1, system_argument2);
}

static BOOLEAN bridge_queue_guest_dpc(uint32_t dpc_va, uint32_t argument1,
                                      uint32_t argument2)
{
    uint32_t i;
    if (!dpc_va || !BRIDGE_MEM32(dpc_va + 12u))
        return FALSE;
    for (i = 0; i < g_guest_dpc_count; ++i)
        if (g_guest_dpcs[i].dpc_va == dpc_va)
            return FALSE; /* Preserve the original queued arguments. */
    if (g_guest_dpc_count == BRIDGE_GUEST_DPC_CAPACITY) {
        fprintf(stderr, "[GUEST-DPC] queue capacity exhausted dpc=%08X\n", dpc_va);
        return FALSE;
    }
    g_guest_dpcs[g_guest_dpc_count].dpc_va = dpc_va;
    g_guest_dpcs[g_guest_dpc_count].argument1 = argument1;
    g_guest_dpcs[g_guest_dpc_count].argument2 = argument2;
    ++g_guest_dpc_count;
    BRIDGE_MEM32(dpc_va + 20u) = argument1;
    BRIDGE_MEM32(dpc_va + 24u) = argument2;
    g_kernel_pending_guest_dpcs = 1;
    return TRUE;
}

static BOOLEAN bridge_remove_guest_dpc(uint32_t dpc_va)
{
    uint32_t i;
    for (i = 0; i < g_guest_dpc_count; ++i) {
        if (g_guest_dpcs[i].dpc_va == dpc_va) {
            --g_guest_dpc_count;
            memmove(&g_guest_dpcs[i], &g_guest_dpcs[i + 1u],
                    (g_guest_dpc_count - i) * sizeof(g_guest_dpcs[0]));
            g_kernel_pending_guest_dpcs = g_guest_dpc_count != 0u;
            return TRUE;
        }
    }
    return FALSE;
}

static void bridge_KeRemoveQueueDpc(void)
{
    g_eax = bridge_remove_guest_dpc(STACK_ARG(0));
}

static int bridge_deliver_guest_dpcs(void)
{
    uint32_t delivered = 0u;
    if (g_delivering_guest_dpcs || g_delivering_hardware_interrupt ||
        g_delivering_guest_timers || BRIDGE_MEM8(0x24u) >= DISPATCH_LEVEL)
        return 0;
    g_delivering_guest_dpcs = 1;
    while (g_guest_dpc_count && delivered < BRIDGE_GUEST_DPC_CAPACITY) {
        const bridge_guest_dpc work = g_guest_dpcs[0];
        KIRQL old_irql;
        /* Remove before invocation: the running DPC may queue itself again. */
        bridge_remove_guest_dpc(work.dpc_va);
        old_irql = xbox_KfRaiseIrql(DISPATCH_LEVEL);
        BRIDGE_MEM8(0x24u) = DISPATCH_LEVEL;
        bridge_invoke_guest_dpc(work.dpc_va, work.argument1, work.argument2);
        xbox_KfLowerIrql(old_irql);
        BRIDGE_MEM8(0x24u) = old_irql;
        ++delivered;
    }
    g_delivering_guest_dpcs = 0;
    return delivered != 0u;
}

void xbox_kernel_service_guest_dpcs(void)
{
    if (g_kernel_pending_guest_dpcs)
        bridge_deliver_guest_dpcs();
}

static void bridge_KeSynchronizeExecution(void)
{
    uint32_t interrupt_va = STACK_ARG(0);
    uint32_t routine = STACK_ARG(1);
    uint32_t context = STACK_ARG(2);
    recomp_func_t fn = bridge_lookup_guest_function(routine);
    uint32_t saved_esp = g_esp;
    KIRQL old_irql = BRIDGE_MEM8(0x24u);
    KIRQL sync_irql = old_irql;
    uint32_t i;

    if (!fn) {
        fprintf(stderr, "[GUEST-IRQ] unresolved synchronize routine=%08X\n",
                routine);
        g_eax = FALSE;
        return;
    }
    for (i = 0; i < g_guest_interrupt_count; ++i)
        if (g_guest_interrupts[i].interrupt_va == interrupt_va &&
            g_guest_interrupts[i].irql > sync_irql)
            sync_irql = (KIRQL)g_guest_interrupts[i].irql;
    xbox_KfRaiseIrql(sync_irql);
    BRIDGE_MEM8(0x24u) = sync_irql;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = context;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0u;
    fn();
    xbox_KfLowerIrql(old_irql);
    BRIDGE_MEM8(0x24u) = old_irql;
    if (g_esp != saved_esp && bridge_trace_interrupts_enabled()) {
        fprintf(stderr,
                "[GUEST-IRQ] synchronize stack mismatch routine=%08X expected=%08X got=%08X\n",
                routine, saved_esp, g_esp);
    }
}
/* Guest KTIMER layout matches the 32-bit Xbox dispatcher object:
 *   +0x03 Inserted, +0x04 SignalState, +0x10 DueTime,
 *   +0x20 Dpc, +0x24 Period.
 *
 * Host timer threads must never invoke a guest DPC directly because the
 * recompiled CPU register file is process-global. The 1 ms kernel tick only
 * marks an expired deadline pending; the game thread performs delivery at a
 * translated-function or kernel-thunk safe point.
 */
static void bridge_recompute_guest_timer_deadline(void)
{
    int64_t earliest = 0;
    uint32_t i;

    for (i = 0; i < BRIDGE_GUEST_TIMER_CAPACITY; ++i) {
        if (g_guest_timers[i].active &&
            (!earliest || g_guest_timers[i].due_time_100ns < earliest))
            earliest = g_guest_timers[i].due_time_100ns;
    }
    InterlockedExchange64(
        &g_guest_timer_next_due_100ns, (LONG64)earliest);
}

static bridge_guest_timer *bridge_find_guest_timer(uint32_t timer_va,
                                                    int create)
{
    bridge_guest_timer *free_slot = NULL;
    uint32_t i;

    for (i = 0; i < BRIDGE_GUEST_TIMER_CAPACITY; ++i) {
        if (g_guest_timers[i].timer_va == timer_va)
            return &g_guest_timers[i];
        if (!g_guest_timers[i].active && !free_slot)
            free_slot = &g_guest_timers[i];
    }
    if (create && free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->timer_va = timer_va;
        return free_slot;
    }
    return NULL;
}

static BOOLEAN bridge_set_guest_timer(uint32_t timer_va, int64_t due_time,
                                      uint32_t period_ms, uint32_t dpc_va)
{
    bridge_guest_timer *timer;
    LARGE_INTEGER now;
    BOOLEAN was_active;

    if (!timer_va)
        return FALSE;
    timer = bridge_find_guest_timer(timer_va, 1);
    if (!timer) {
        fprintf(stderr, "[GUEST-TIMER] timer table full; timer=%08X\n",
                timer_va);
        return FALSE;
    }

    was_active = timer->active ? TRUE : FALSE;
    xbox_KeQuerySystemTime(&now);
    if (due_time < 0) {
        const uint64_t relative =
            (uint64_t)(-(due_time + 1)) + UINT64_C(1);
        timer->due_time_100ns =
            now.QuadPart + (int64_t)relative;
    } else {
        timer->due_time_100ns =
            due_time ? due_time : now.QuadPart;
    }
    timer->dpc_va = dpc_va;
    timer->period_ms = period_ms;
    timer->active = 1;

    BRIDGE_MEM8(timer_va + 3u) = 1u;
    BRIDGE_MEM32(timer_va + 4u) = 0u;
    BRIDGE_MEM32(timer_va + 16u) =
        (uint32_t)timer->due_time_100ns;
    BRIDGE_MEM32(timer_va + 20u) =
        (uint32_t)((uint64_t)timer->due_time_100ns >> 32);
    BRIDGE_MEM32(timer_va + 32u) = dpc_va;
    BRIDGE_MEM32(timer_va + 36u) = period_ms;

    bridge_recompute_guest_timer_deadline();
    if (timer->due_time_100ns <= now.QuadPart)
        InterlockedExchange(
            (volatile LONG *)&g_kernel_pending_guest_timers, 1);
    return was_active;
}

/* KeInitializeTimerEx (ordinal 113). */
static void bridge_KeInitializeTimerEx(void)
{
    const uint32_t timer_va = STACK_ARG(0);
    const uint32_t type = STACK_ARG(1);
    bridge_guest_timer *timer =
        bridge_find_guest_timer(timer_va, 0);

    if (timer)
        memset(timer, 0, sizeof(*timer));
    memset(XBOX_TO_NATIVE(timer_va), 0, 40);
    BRIDGE_MEM16(timer_va) =
        (uint16_t)(0x08u + (type & 1u));
    bridge_recompute_guest_timer_deadline();
    g_eax = 0;
}

/* KeSetTimer (ordinal 149). */
static void bridge_KeSetTimer(void)
{
    const uint64_t due_bits =
        (uint64_t)STACK_ARG(1) |
        ((uint64_t)STACK_ARG(2) << 32);
    g_eax = bridge_set_guest_timer(
        STACK_ARG(0), (int64_t)due_bits, 0u, STACK_ARG(3));
}

/* KeSetTimerEx (ordinal 150). */
static void bridge_KeSetTimerEx(void)
{
    const uint64_t due_bits =
        (uint64_t)STACK_ARG(1) |
        ((uint64_t)STACK_ARG(2) << 32);
    g_eax = bridge_set_guest_timer(
        STACK_ARG(0), (int64_t)due_bits, STACK_ARG(3), STACK_ARG(4));
}

/* KeCancelTimer (ordinal 97). */
static void bridge_KeCancelTimer(void)
{
    const uint32_t timer_va = STACK_ARG(0);
    bridge_guest_timer *timer =
        bridge_find_guest_timer(timer_va, 0);

    g_eax = timer && timer->active ? TRUE : FALSE;
    if (timer) {
        timer->active = 0;
        timer->dpc_va = 0;
        timer->period_ms = 0;
    }
    if (timer_va)
        BRIDGE_MEM8(timer_va + 3u) = 0u;
    bridge_recompute_guest_timer_deadline();
}

static int bridge_deliver_guest_timers(void)
{
    LARGE_INTEGER now;
    uint32_t i;
    int delivered = 0;

    if (g_delivering_guest_timers)
        return 0;

    InterlockedExchange(
        (volatile LONG *)&g_kernel_pending_guest_timers, 0);
    xbox_KeQuerySystemTime(&now);
    g_delivering_guest_timers = 1;

    for (i = 0; i < BRIDGE_GUEST_TIMER_CAPACITY; ++i) {
        bridge_guest_timer *timer = &g_guest_timers[i];
        uint32_t timer_va;
        uint32_t dpc_va;

        if (!timer->active ||
            timer->due_time_100ns > now.QuadPart)
            continue;

        timer_va = timer->timer_va;
        dpc_va = timer->dpc_va;
        BRIDGE_MEM32(timer_va + 4u) = 1u;

        if (timer->period_ms) {
            const int64_t period_100ns =
                (int64_t)timer->period_ms * INT64_C(10000);
            do {
                timer->due_time_100ns += period_100ns;
            } while (timer->due_time_100ns <= now.QuadPart);
            BRIDGE_MEM32(timer_va + 16u) =
                (uint32_t)timer->due_time_100ns;
            BRIDGE_MEM32(timer_va + 20u) =
                (uint32_t)((uint64_t)timer->due_time_100ns >> 32);
        } else {
            timer->active = 0;
            BRIDGE_MEM8(timer_va + 3u) = 0u;
        }

        if (dpc_va)
            bridge_queue_guest_dpc(dpc_va, 0u, 0u);
        delivered = 1;
    }

    g_delivering_guest_timers = 0;
    bridge_recompute_guest_timer_deadline();
    bridge_deliver_guest_dpcs();
    return delivered;
}

void xbox_kernel_service_guest_timers(void)
{
    if (InterlockedCompareExchange(
            (volatile LONG *)&g_kernel_pending_guest_timers, 0, 0) != 0)
        bridge_deliver_guest_timers();
}

/* -- ExQueryPoolBlockSize (ordinal 24) --------------------
 * ULONG ExQueryPoolBlockSize(PVOID PoolBlock)
 *
 * Returns the size of a pool memory block.
 * Since we use HeapAlloc, we can query the Windows heap.
 */
static void bridge_ExQueryNonVolatileSetting(void)
{
    uint32_t value_index = STACK_ARG(0);
    uint32_t type_va = STACK_ARG(1);
    uint32_t value_va = STACK_ARG(2);
    uint32_t value_length = STACK_ARG(3);
    uint32_t result_length_va = STACK_ARG(4);

    g_eax = (uint32_t)xbox_ExQueryNonVolatileSetting(
        value_index,
        (PULONG)XBOX_TO_NATIVE(type_va),
        XBOX_TO_NATIVE(value_va),
        value_length,
        (PULONG)XBOX_TO_NATIVE(result_length_va));
    fprintf(stderr,
            "  [EEPROM] index=0x%08X len=%u status=0x%08X type=%u result=%u value=0x%08X\n",
            value_index, value_length, g_eax,
            type_va ? BRIDGE_MEM32(type_va) : 0,
            result_length_va ? BRIDGE_MEM32(result_length_va) : 0,
            (value_va && value_length >= 4) ? BRIDGE_MEM32(value_va) : 0);
}

/* -- RtlNtStatusToDosError (ordinal 301) -----------------
 * ULONG RtlNtStatusToDosError(NTSTATUS Status)
 *
 * Converts an NTSTATUS to a Win32 error code.
 */
static void bridge_RtlNtStatusToDosError(void)
{
    uint32_t status = STACK_ARG(0);

    /* Simple mapping of common status codes */
    switch (status) {
    case 0x00000000: g_eax = 0; break;          /* STATUS_SUCCESS -> ERROR_SUCCESS */
    case 0xC0000034: g_eax = 2; break;          /* STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND */
    case 0xC000003A: g_eax = 3; break;          /* STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND */
    case 0xC0000022: g_eax = 5; break;          /* STATUS_ACCESS_DENIED -> ERROR_ACCESS_DENIED */
    case 0xC0000008: g_eax = 6; break;          /* STATUS_INVALID_HANDLE -> ERROR_INVALID_HANDLE */
    case 0xC0000017: g_eax = 8; break;          /* STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY */
    case 0xC000000D: g_eax = 87; break;         /* STATUS_INVALID_PARAMETER -> ERROR_INVALID_PARAMETER */
    default:         g_eax = 317; break;         /* ERROR_MR_MID_NOT_FOUND (generic) */
    }
}

/* -- File I/O bridge helpers ------------------------------- */

/*
 * Xbox structures use 32-bit pointers. On Win64, the C structs
 * (XBOX_OBJECT_ATTRIBUTES, etc.) have 64-bit pointers, so we can't
 * cast Xbox memory to them directly. Instead, parse the 32-bit
 * Xbox layout manually:
 *
 * XBOX_OBJECT_ATTRIBUTES (12 bytes):
 *   offset 0: RootDirectory  (uint32_t)
 *   offset 4: ObjectName     (uint32_t, Xbox VA to ANSI_STRING)
 *   offset 8: Attributes     (uint32_t)
 *
 * XBOX_ANSI_STRING (8 bytes):
 *   offset 0: Length          (uint16_t)
 *   offset 2: MaximumLength   (uint16_t)
 *   offset 4: Buffer          (uint32_t, Xbox VA to char[])
 *
 * XBOX_IO_STATUS_BLOCK (8 bytes):
 *   offset 0: Status          (uint32_t)
 *   offset 4: Information     (uint32_t)
 */

/* Extract the ANSI path string from an Xbox OBJECT_ATTRIBUTES */
static const char* bridge_get_xbox_path(uint32_t obj_attrs_va)
{
    uint32_t ansi_str_va, buf_va;
    if (!obj_attrs_va) return NULL;
    ansi_str_va = BRIDGE_MEM32(obj_attrs_va + 4);
    if (!ansi_str_va) return NULL;
    buf_va = BRIDGE_MEM32(ansi_str_va + 4);
    if (!buf_va) return NULL;
    return (const char*)XBOX_TO_NATIVE(buf_va);
}

/* Write NTSTATUS + Information into Xbox IO_STATUS_BLOCK */
static void bridge_write_iostatus(uint32_t ios_va, NTSTATUS status, uint32_t info)
{
    if (ios_va) {
        BRIDGE_MEM32(ios_va + 0) = (uint32_t)status;
        BRIDGE_MEM32(ios_va + 4) = info;
    }
}

/*
 * Handle table.
 *
 * Xbox memory only has 32-bit handle slots, but native HANDLEs are 64-bit
 * pointers (win32_compat objects, or real Win32 handles on Windows). Map
 * 32-bit tokens <-> native HANDLEs so a handle survives a round-trip through
 * Xbox memory. Tokens carry a tag in the high byte so they never collide
 * with the synthetic handles (0xDEAD0001 / 0xBEEF0010) used elsewhere.
 */
#define BRIDGE_HANDLE_TAG  0x48000000u
#define BRIDGE_HANDLE_MASK 0x00FFFFFFu
#define BRIDGE_HANDLE_MAX  16384
static HANDLE s_handle_table[BRIDGE_HANDLE_MAX];

static int bridge_trace_save_io_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_SAVE_IO") != NULL;
    return enabled;
}

static uint32_t bridge_handle_token(HANDLE h)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == h) return BRIDGE_HANDLE_TAG | (uint32_t)i;
    for (i = 1; i < BRIDGE_HANDLE_MAX; i++)
        if (s_handle_table[i] == NULL) {
            s_handle_table[i] = h;
            return BRIDGE_HANDLE_TAG | (uint32_t)i;
        }
    fprintf(stderr, "  [BRIDGE] handle table full\n");
    return 0;
}

/* Store a native HANDLE into a 32-bit Xbox memory slot (as a token). */
static void bridge_write_handle(uint32_t handle_va, HANDLE h)
{
    if (handle_va)
        BRIDGE_MEM32(handle_va) = bridge_handle_token(h);
}

/* Resolve a by-value 32-bit Xbox handle token back to a native HANDLE. */
static HANDLE bridge_read_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        return (i > 0 && i < BRIDGE_HANDLE_MAX) ? s_handle_table[i] : NULL;
    }
    /* Untagged value: synthetic/dummy handle -- pass through unchanged. */
    return (HANDLE)(uintptr_t)token;
}

/* Resolve a token to a HANDLE and release its table slot (for NtClose). */
static HANDLE bridge_take_handle(uint32_t token)
{
    if ((token & 0xFF000000u) == BRIDGE_HANDLE_TAG) {
        uint32_t i = token & BRIDGE_HANDLE_MASK;
        if (i > 0 && i < BRIDGE_HANDLE_MAX) {
            HANDLE h = s_handle_table[i];
            s_handle_table[i] = NULL;
            return h;
        }
    }
    return NULL;   /* untagged -> not a table handle, do not close */
}

/* Build a native OBJECT_ATTRIBUTES wrapping the translated Xbox path. */
static void bridge_build_oa(uint32_t obj_attrs_va,
                            XBOX_OBJECT_ATTRIBUTES* oa, XBOX_ANSI_STRING* name)
{
    uint32_t ansi_str_va = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va + 4) : 0;
    const char* path = bridge_get_xbox_path(obj_attrs_va);
    name->Buffer        = (PCHAR)path;
    name->Length        = ansi_str_va ? BRIDGE_MEM16(ansi_str_va) : 0;
    name->MaximumLength = ansi_str_va ? BRIDGE_MEM16(ansi_str_va + 2) : 0;
    oa->RootDirectory = NULL;
    oa->ObjectName    = name;
    oa->Attributes    = 0;
}

static int s_cache_io_trace_armed;
static unsigned s_cache_volume_trace_count;

/* Open a file by delegating to the ported xbox_NtCreateFile kernel HLE. */
static NTSTATUS bridge_create_file_impl(
    uint32_t handle_va, ACCESS_MASK access, uint32_t obj_attrs_va,
    uint32_t iostatus_va, ULONG file_attrs, ULONG share,
    ULONG disposition, ULONG options)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;
    XBOX_IO_STATUS_BLOCK   ios;
    HANDLE   h  = NULL;
    NTSTATUS st;

    bridge_build_oa(obj_attrs_va, &oa, &name);
    int trace_cache_io = 0;
    if (getenv("MERCENARIES_TRACE_CACHE_IO") != NULL) {
        if (name.Buffer != NULL && name.Length >= 2 &&
            (name.Buffer[0] == 'z' || name.Buffer[0] == 'Z') && name.Buffer[1] == ':')
            s_cache_io_trace_armed = 1;
        trace_cache_io = s_cache_io_trace_armed;
    }
    {
        static unsigned file_open_count;
        static int trace_file_open_all = -1;
        static uint32_t trace_file_open_dump_at;
        static int trace_file_open_dump_initialized;
        uint32_t root = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va + 0) : 0;
        uint32_t ansi = obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va + 4) : 0;
        if (trace_file_open_all < 0)
            trace_file_open_all = getenv("MERCENARIES_TRACE_FILE_OPEN_ALL") != NULL;
        if (!trace_file_open_dump_initialized) {
            const char *value = getenv("MERCENARIES_TRACE_FILE_OPEN_DUMP_AT");
            if (value != NULL && value[0] != '\0') {
                const unsigned long parsed = strtoul(value, NULL, 0);
                if (parsed > 0u && parsed <= UINT32_MAX)
                    trace_file_open_dump_at = (uint32_t)parsed;
            }
            trace_file_open_dump_initialized = 1;
        }
        file_open_count++;
        if (file_open_count <= 32 ||
            (trace_file_open_all && file_open_count <= 512)) {
            fprintf(stderr, "  [FILE] #%u oa=%08X root=%08X ansi=%08X path=%s disp=%u opts=%08X\n",
                    file_open_count, obj_attrs_va, root, ansi,
                    name.Buffer ? name.Buffer : "<null>", disposition, options);
            fflush(stderr);
        }
        if (trace_file_open_dump_at != 0u &&
            file_open_count == trace_file_open_dump_at) {
            fprintf(stderr,
                    "[FILE-OPEN-TRACE] requesting translated-function dump "
                    "at open #%u path=%s esp=%08X\n",
                    file_open_count, name.Buffer ? name.Buffer : "<null>", g_esp);
            {
                const uint32_t recent_index = g_recomp_recent_game_func_idx;
                const uint32_t recent_count = recent_index < 256u ? recent_index : 256u;
                fprintf(stderr,
                        "[FILE-OPEN-TRACE-STATE] current=%08X eax=%08X ebx=%08X "
                        "ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X "
                        "recent_index=%u count=%u\n",
                        (uint32_t)g_recomp_current_func, g_eax, g_ebx, g_ecx,
                        g_edx, g_esi, g_edi, g_seh_ebp, g_esp,
                        recent_index, recent_count);
                for (uint32_t i = 0u; i < recent_count; ++i) {
                    const uint32_t index =
                        (recent_index - recent_count + i) & 255u;
                    fprintf(stderr, "  [F%03u] %08X\n", i,
                            (uint32_t)g_recomp_recent_game_funcs[index]);
                }
            }
            fflush(stderr);
            exit(90);
        }
    }
    if (!name.Buffer) {
        bridge_write_iostatus(iostatus_va, STATUS_OBJECT_PATH_NOT_FOUND, 0);
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    memset(&ios, 0, sizeof(ios));

    st = xbox_NtCreateFile(&h, access, &oa, &ios, NULL,
                           file_attrs, share, disposition, options);
    if (getenv("MERCENARIES_TRACE_ASYNC_IO") != NULL) {
        uint32_t recent_end = g_recomp_recent_func_idx;
        fprintf(stderr,
                "[ASYNC-OPEN] func=%08X handle_va=%08X path=%.*s "
                "status=%08X host=%p\n",
                g_recomp_current_func, handle_va, (int)name.Length,
                name.Buffer, (uint32_t)st, h);
        if (strstr(name.Buffer, "shl_streams.xwb") != NULL) {
            uint32_t recent_count = recent_end < 64u ? recent_end : 64u;
            fprintf(stderr, "[ASYNC-OPEN CALLERS] count=%u\n", recent_count);
            for (uint32_t i = 0; i < recent_count; ++i) {
                uint32_t index = (recent_end - recent_count + i) & 63u;
                fprintf(stderr, "  [%02u] %08X\n", i,
                        g_recomp_recent_funcs[index]);
            }
            {
                uint32_t game_end = g_recomp_recent_game_func_idx;
                uint32_t game_count = game_end < 256u ? game_end : 256u;
                fprintf(stderr, "[ASYNC-OPEN GAME CALLERS] count=%u\n",
                        game_count);
                for (uint32_t i = 0; i < game_count; ++i) {
                    uint32_t index = (game_end - game_count + i) & 255u;
                    fprintf(stderr, "  [%03u] %08X\n", i,
                            g_recomp_recent_game_funcs[index]);
                }
            }
        }
    }
    if (trace_cache_io) {
        fprintf(stderr,
                "  [CACHE IO] open path=%.*s root=%08X access=%08X share=%X disp=%u opts=%08X status=%08X handle=%p\n",
                (int)name.Length, name.Buffer,
                obj_attrs_va ? BRIDGE_MEM32(obj_attrs_va + 0) : 0,
                (unsigned)access, (unsigned)share,
                (unsigned)disposition, (unsigned)options, (unsigned)st, h);
        fflush(stderr);
    }

    if (NT_SUCCESS(st)) {
        bridge_write_handle(handle_va, h);
        bridge_write_iostatus(iostatus_va, ios.Status, (uint32_t)ios.Information);
        if (bridge_trace_save_io_enabled() && name.Buffer != NULL &&
            name.Length == sizeof("U:\\9AA9F19E10CF\\Mercenaries Saves") - 1u &&
            memcmp(name.Buffer, "U:\\9AA9F19E10CF\\Mercenaries Saves",
                   sizeof("U:\\9AA9F19E10CF\\Mercenaries Saves") - 1u) == 0) {
            s_save_trace_token = handle_va ? BRIDGE_MEM32(handle_va) : 0u;
            fprintf(stderr,
                    "[SAVE-IO] open func=%08X token=%08X host=%p "
                    "access=%08X share=%X disp=%u opts=%08X iosb=%08X\n",
                    g_recomp_current_func, s_save_trace_token, h,
                    (unsigned)access, (unsigned)share,
                    (unsigned)disposition, (unsigned)options, iostatus_va);
            fflush(stderr);
        }
        if (getenv("MERCENARIES_TRACE_ASYNC_IO") != NULL) {
            fprintf(stderr,
                    "[ASYNC-OPEN] guest_handle=%08X iosb=%08X/%08X\n",
                    handle_va ? BRIDGE_MEM32(handle_va) : 0u,
                    iostatus_va ? BRIDGE_MEM32(iostatus_va) : 0u,
                    iostatus_va ? BRIDGE_MEM32(iostatus_va + 4u) : 0u);
        }
    } else {
        bridge_write_iostatus(iostatus_va, st, 0);
    }
    return st;
}

/* -- NtCreateFile (ordinal 190, 9 args = 36 bytes) ------- */
static void bridge_NtCreateFile(void)
{
    uint32_t handle_va   = STACK_ARG(0);  /* PHANDLE */
    uint32_t access      = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs   = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus    = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    /* arg4: AllocationSize - ignored */
    uint32_t file_attrs  = STACK_ARG(5);  /* FileAttributes */
    uint32_t share       = STACK_ARG(6);  /* ShareAccess */
    uint32_t disposition = STACK_ARG(7);  /* CreateDisposition */
    uint32_t options     = STACK_ARG(8);  /* CreateOptions */

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* -- NtOpenFile (ordinal 202, 6 args = 24 bytes) -------- */
static void bridge_NtOpenFile(void)
{
    uint32_t handle_va = STACK_ARG(0);  /* PHANDLE */
    uint32_t access    = STACK_ARG(1);  /* ACCESS_MASK */
    uint32_t obj_attrs = STACK_ARG(2);  /* POBJECT_ATTRIBUTES */
    uint32_t iostatus  = STACK_ARG(3);  /* PIO_STATUS_BLOCK */
    uint32_t share     = STACK_ARG(4);  /* ShareAccess */
    uint32_t options   = STACK_ARG(5);  /* OpenOptions */

    /* NtOpenFile = NtCreateFile with FILE_OPEN disposition */
    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        0, share, 1 /* FILE_OPEN */, options);
}

/* -- NtReadFile (ordinal 219, 8 args = 32 bytes) -------- */
static void bridge_NtReadFile(void)
{
    static uint32_t trace_count;
    const char *trace_async = getenv("MERCENARIES_TRACE_ASYNC_IO");
    const int trace_this = trace_async != NULL &&
        (trace_count < 128u || strcmp(trace_async, "all") == 0);
    const uint32_t trace_id = trace_count++;
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t raw_handle = STACK_ARG(0);
    uint32_t apc_va    = STACK_ARG(2);
    uint32_t context_va = STACK_ARG(3);
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    if (trace_this) {
        fprintf(stderr,
                "[ASYNC-IO] #%u func=%08X handle=%08X host=%p apc=%08X "
                "context=%08X iosb=%08X buffer=%08X length=%u offset=%08X\n",
                trace_id, g_recomp_current_func, raw_handle, handle, apc_va,
                context_va, iostatus, buffer_va, length, offset_va);
    }
    g_eax = (uint32_t)xbox_NtReadFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    if (bridge_trace_save_io_enabled() && raw_handle == s_save_trace_token) {
        fprintf(stderr,
                "[SAVE-IO] read func=%08X token=%08X buffer=%08X offset=%lld length=%u "
                "result=%08X ios=%08X/%u apc=%08X\n",
                g_recomp_current_func, raw_handle, buffer_va,
                poff ? (long long)poff->QuadPart : -1ll, length, g_eax,
                (uint32_t)ios.Status, (uint32_t)ios.Information, apc_va);
        fflush(stderr);
    }
    if (trace_this) {
        fprintf(stderr,
                "[ASYNC-IO] #%u result=%08X status=%08X transferred=%u\n",
                trace_id, g_eax, (uint32_t)ios.Status,
                (uint32_t)ios.Information);
    }
    if (apc_va)
        bridge_queue_guest_apc(apc_va, context_va, iostatus);
}

/* -- NtWriteFile (ordinal 236, 8 args = 32 bytes) ------- */
static void bridge_NtWriteFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t raw_handle = STACK_ARG(0);
    uint32_t apc_va    = STACK_ARG(2);
    uint32_t context_va = STACK_ARG(3);
    uint32_t iostatus  = STACK_ARG(4);
    uint32_t buffer_va = STACK_ARG(5);
    uint32_t length    = STACK_ARG(6);
    uint32_t offset_va = STACK_ARG(7);
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER  off;
    PLARGE_INTEGER poff = NULL;

    memset(&ios, 0, sizeof(ios));
    if (offset_va) {
        off.LowPart  = BRIDGE_MEM32(offset_va);
        off.HighPart = (LONG)BRIDGE_MEM32(offset_va + 4);
        poff = &off;
    }
    g_eax = (uint32_t)xbox_NtWriteFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(buffer_va), length, poff);
    bridge_write_iostatus(iostatus, ios.Status, (uint32_t)ios.Information);
    if (bridge_trace_save_io_enabled() && raw_handle == s_save_trace_token) {
        fprintf(stderr,
                "[SAVE-IO] write func=%08X token=%08X offset=%lld length=%u "
                "result=%08X ios=%08X/%u apc=%08X\n",
                g_recomp_current_func, raw_handle,
                poff ? (long long)poff->QuadPart : -1ll, length, g_eax,
                (uint32_t)ios.Status, (uint32_t)ios.Information, apc_va);
        fflush(stderr);
    }
    if (apc_va)
        bridge_queue_guest_apc(apc_va, context_va, iostatus);
}

/* NtUserIoApcDispatcher (ordinal 232).
 *
 * Xbox ReadFileEx passes this kernel dispatcher as NtReadFile's APC routine
 * and the title's LPOVERLAPPED_COMPLETION_ROUTINE as ApcContext. Match the
 * retail kernel/XAPI behavior: translate the IOSB result into Win32 callback
 * arguments and invoke the guest completion routine.
 */
static void bridge_NtUserIoApcDispatcher(void)
{
    const uint32_t completion_va = STACK_ARG(0);
    const uint32_t iostatus_va = STACK_ARG(1);
    const uint32_t status = iostatus_va ? BRIDGE_MEM32(iostatus_va) :
                                             (uint32_t)STATUS_INVALID_PARAMETER;
    const uint32_t transferred = iostatus_va ? BRIDGE_MEM32(iostatus_va + 4u) : 0u;
    const uint32_t error_code = ((int32_t)status >= 0)
                                    ? 0u
                                    : xbox_RtlNtStatusToDosError((NTSTATUS)status);
    recomp_func_t completion = recomp_lookup_manual(completion_va);
    uint32_t saved_esp;

    if (!completion) completion = recomp_lookup(completion_va);
    if (!completion) {
        fprintf(stderr,
                "[GUEST-APC] unresolved I/O completion routine=%08X\n",
                completion_va);
        g_eax = 0;
        return;
    }

    saved_esp = g_esp;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = iostatus_va;  /* lpOverlapped */
    g_esp -= 4; BRIDGE_MEM32(g_esp) = transferred;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = error_code;
    g_esp -= 4; BRIDGE_MEM32(g_esp) = 0u;           /* return address */
    completion();
    if (g_esp != saved_esp) {
        fprintf(stderr,
                "[GUEST-APC] completion stack mismatch routine=%08X "
                "expected=%08X got=%08X\n",
                completion_va, saved_esp, g_esp);
        g_esp = saved_esp;
    }
    g_eax = 0;
}
/* -- NtQueryInformationFile (ordinal 211, 5 args = 20 bytes) */
static void bridge_NtQueryInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* -- NtSetInformationFile (ordinal 226, 5 args = 20 bytes) - */
static void bridge_NtSetInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtSetInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)infoclass);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* -- NtQueryVolumeInformationFile (ordinal 218, 5 args = 20 bytes) */
static void bridge_NtQueryVolumeInformationFile(void)
{
    HANDLE   handle    = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va    = STACK_ARG(1);
    uint32_t info_va   = STACK_ARG(2);
    uint32_t length    = STACK_ARG(3);
    uint32_t infoclass = STACK_ARG(4);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtQueryVolumeInformationFile(handle, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FS_INFORMATION_CLASS)infoclass);
    if (getenv("MERCENARIES_TRACE_CACHE_IO") != NULL &&
        s_cache_io_trace_armed && s_cache_volume_trace_count++ < 16) {
        fprintf(stderr,
                "  [CACHE IO] volume handle=%p class=%u len=%u status=%08X info=%u\n",
                handle, (unsigned)infoclass, (unsigned)length,
                (unsigned)g_eax, (unsigned)ios.Information);
        if (g_eax == 0 && infoclass == XboxFileFsSizeInformation && info_va != 0) {
            PXBOX_FILE_FS_SIZE_INFORMATION info =
                (PXBOX_FILE_FS_SIZE_INFORMATION)XBOX_TO_NATIVE(info_va);
            fprintf(stderr,
                    "  [CACHE IO] volume total_units=%lld avail_units=%lld sectors_per_unit=%u bytes_per_sector=%u\n",
                    (long long)info->TotalAllocationUnits.QuadPart,
                    (long long)info->AvailableAllocationUnits.QuadPart,
                    (unsigned)info->SectorsPerAllocationUnit,
                    (unsigned)info->BytesPerSector);
        }
        fflush(stderr);
    }
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* -- NtQueryFullAttributesFile (ordinal 210, 2 args = 8 bytes) */
static void bridge_NtQueryFullAttributesFile(void)
{
    uint32_t obj_attrs = STACK_ARG(0);
    uint32_t info_va   = STACK_ARG(1);
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(obj_attrs, &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtQueryFullAttributesFile(&oa,
                (PXBOX_FILE_NETWORK_OPEN_INFORMATION)XBOX_TO_NATIVE(info_va));
}

/* -- NtFlushBuffersFile (ordinal 198, 2 args = 8 bytes) --- */
static void bridge_NtFlushBuffersFile(void)
{
    HANDLE   handle = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va = STACK_ARG(1);
    XBOX_IO_STATUS_BLOCK ios;

    memset(&ios, 0, sizeof(ios));
    g_eax = (uint32_t)xbox_NtFlushBuffersFile(handle, &ios);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* -- NtDeleteFile (ordinal 195, 1 arg = 4 bytes) ------- */
static void bridge_NtDeleteFile(void)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_ANSI_STRING       name;

    bridge_build_oa(STACK_ARG(0), &oa, &name);
    if (!name.Buffer) { g_eax = STATUS_OBJECT_PATH_NOT_FOUND; return; }
    g_eax = (uint32_t)xbox_NtDeleteFile(&oa);
}

/* -- NtQueryDirectoryFile (ordinal 207, 10 args = 40 bytes) - */
static void bridge_NtQueryDirectoryFile(void)
{
    HANDLE   handle      = bridge_read_handle(STACK_ARG(0));
    uint32_t ios_va      = STACK_ARG(4);
    uint32_t info_va     = STACK_ARG(5);
    uint32_t length      = STACK_ARG(6);
    uint32_t info_class  = STACK_ARG(7);
    uint32_t filename_va = STACK_ARG(8);  /* PXBOX_ANSI_STRING */
    uint32_t restart     = STACK_ARG(9);  /* BOOLEAN */
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING     fn;
    PXBOX_ANSI_STRING    pfn = NULL;

    memset(&ios, 0, sizeof(ios));
    if (filename_va) {
        /* Xbox ANSI_STRING: 0=Length(u16), 2=MaximumLength(u16), 4=Buffer(u32) */
        uint32_t fn_buf  = BRIDGE_MEM32(filename_va + 4);
        fn.Length        = BRIDGE_MEM16(filename_va);
        fn.MaximumLength = BRIDGE_MEM16(filename_va + 2);
        fn.Buffer        = fn_buf ? (PCHAR)XBOX_TO_NATIVE(fn_buf) : NULL;
        if (fn.Buffer) pfn = &fn;
    }
    g_eax = (uint32_t)xbox_NtQueryDirectoryFile(handle, NULL, NULL, NULL, &ios,
                XBOX_TO_NATIVE(info_va), length,
                (XBOX_FILE_INFORMATION_CLASS)info_class, pfn, (BOOLEAN)restart);
    bridge_write_iostatus(ios_va, ios.Status, (uint32_t)ios.Information);
}

/* -- NtOpenSymbolicLinkObject (ordinal 203, 2 args = 8 bytes) */
static void bridge_NtOpenSymbolicLinkObject(void)
{
    uint32_t handle_va = STACK_ARG(0);
    /* arg1: POBJECT_ATTRIBUTES - ignored, we return a synthetic handle.
     * Written raw (untagged) so NtClose recognises it and skips it. */
    if (handle_va) BRIDGE_MEM32(handle_va) = 0xDEAD0001u;
    g_eax = STATUS_SUCCESS;
}

/* -- NtQuerySymbolicLinkObject (ordinal 215, 3 args = 12 bytes) */
static void bridge_NtQuerySymbolicLinkObject(void)
{
    /* uint32_t handle = STACK_ARG(0); */
    uint32_t target_va = STACK_ARG(1);
    uint32_t retlen_va = STACK_ARG(2);
    const char* target = "\\Device\\CdRom0";
    USHORT len = (USHORT)strlen(target);

    if (target_va) {
        uint16_t max_len = BRIDGE_MEM16(target_va + 2);
        uint32_t buf_va  = BRIDGE_MEM32(target_va + 4);
        if (buf_va && len < max_len) {
            memcpy(XBOX_TO_NATIVE(buf_va), target, len + 1);
            BRIDGE_MEM16(target_va) = len;
        }
    }
    if (retlen_va) BRIDGE_MEM32(retlen_va) = (uint32_t)len;
    g_eax = STATUS_SUCCESS;
}

/* -- IoCreateFile (ordinal 67, 10 args = 40 bytes) ------ */
static void bridge_IoCreateFile(void)
{
    /* Same as NtCreateFile with an extra Options arg at the end */
    uint32_t handle_va   = STACK_ARG(0);
    uint32_t access      = STACK_ARG(1);
    uint32_t obj_attrs   = STACK_ARG(2);
    uint32_t iostatus    = STACK_ARG(3);
    uint32_t file_attrs  = STACK_ARG(5);
    uint32_t share       = STACK_ARG(6);
    uint32_t disposition = STACK_ARG(7);
    uint32_t options     = STACK_ARG(8);

    g_eax = (uint32_t)bridge_create_file_impl(
        handle_va, access, obj_attrs, iostatus,
        file_attrs, share, disposition, options);
}

/* -- NtDeviceIoControlFile (ordinal 196, 10 args = 40 bytes) */
static void bridge_NtDeviceIoControlFile(void)
{
    uint32_t ioctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    uint32_t output_va = STACK_ARG(8);
    uint32_t output_length = STACK_ARG(9);

    /* IOCTL_DISK_GET_DRIVE_GEOMETRY.  A fresh Xbox cache partition asks
     * for this before formatting/mounting Partition5.  Returning NOT_IMPLEMENTED
     * makes Xapi take its fatal reboot path, so report the stock 10 GB Xbox HDD
     * geometry used by Cxbx/Xemu.  DISK_GEOMETRY is 24 bytes on Xbox. */
    /* Keep the incomplete fresh-partition formatting path opt-in. Geometry
     * success makes Xapi immediately issue format FSCTL 0x74004; until that
     * operation is implemented, enabling this during ordinary gameplay makes
     * retail deliberately reboot. */
    if (getenv("MERCENARIES_ENABLE_DISK_GEOMETRY") != NULL &&
        ioctl == 0x00070000u && output_va != 0 && output_length >= 24u) {
        BRIDGE_MEM32(output_va + 0) = 0x01400000u; /* Cylinders.LowPart */
        BRIDGE_MEM32(output_va + 4) = 0u;          /* Cylinders.HighPart */
        BRIDGE_MEM32(output_va + 8) = 12u;         /* FixedMedia */
        BRIDGE_MEM32(output_va + 12) = 1u;         /* TracksPerCylinder */
        BRIDGE_MEM32(output_va + 16) = 1u;         /* SectorsPerTrack */
        BRIDGE_MEM32(output_va + 20) = 512u;       /* BytesPerSector */
        bridge_write_iostatus(ios_va, 0u, 24u);
        fprintf(stderr,
                "  [FILE] NtDeviceIoControlFile(0x70000) - Xbox HDD geometry\n");
        g_eax = 0u;
        return;
    }

    fprintf(stderr, "  [FILE] NtDeviceIoControlFile(0x%X) - stub\n", ioctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu; /* STATUS_NOT_IMPLEMENTED */
}

/* -- NtFsControlFile (ordinal 200, 10 args = 40 bytes) ---- */
static void bridge_NtFsControlFile(void)
{
    uint32_t fsctl = STACK_ARG(5);
    uint32_t ios_va = STACK_ARG(4);
    fprintf(stderr, "  [FILE] NtFsControlFile(0x%X) - stub\n", fsctl);
    bridge_write_iostatus(ios_va, 0xC00000BBu, 0);
    g_eax = 0xC00000BBu;
}

/* -- NtCreateDirectoryObject (ordinal 188) ---------------- */
static void bridge_NtCreateDirectoryObject(void)
{
    /* Return STATUS_SUCCESS with a fake handle */
    uint32_t handle_ptr = STACK_ARG(0);
    if (handle_ptr) BRIDGE_MEM32(handle_ptr) = 0xBEEF0010;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* -- IoCreateSymbolicLink (ordinal 63) --------------------- */
static void bridge_IoCreateSymbolicLink(void)
{
    g_eax = 0;  /* STATUS_SUCCESS */
}

/* -- ObReferenceObjectByHandle (ordinal 246) --------------- */
static void bridge_ObReferenceObjectByHandle(void)
{
    /* Xbox: NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, PVOID ObjectType, PVOID* Object)
     * 3 args (not 6 like Windows NT) */
    uint32_t handle = STACK_ARG(0);
    uint32_t obj_type = STACK_ARG(1);
    uint32_t object_ptr = STACK_ARG(2);
    if (object_ptr) BRIDGE_MEM32(object_ptr) = 0;
    g_eax = 0;  /* STATUS_SUCCESS */
}

/*  ObfDereferenceObject (ordinal 250, fastcall)  */
static void bridge_ObfDereferenceObject(void)
{
    xbox_ObfDereferenceObject((PVOID)(uintptr_t)g_ecx);
    g_eax = 0;
}

/* -- RtlRaiseException (ordinal 302) ---------------------
 * VOID RtlRaiseException(PEXCEPTION_RECORD ExceptionRecord)
 *
 * Called by CRT / SEH code to raise structured exceptions.
 * On Xbox this triggers the kernel exception dispatcher.
 * For recompilation, we log and continue (no real SEH dispatch yet).
 */
static void bridge_RtlInitAnsiString(void)
{
    uint32_t destination_va = STACK_ARG(0);
    uint32_t source_va = STACK_ARG(1);

    if (destination_va) {
        if (source_va) {
            size_t length = strlen((const char*)XBOX_TO_NATIVE(source_va));
            if (length > 0xFFFEu) length = 0xFFFEu;
            BRIDGE_MEM16(destination_va + 0) = (uint16_t)length;
            BRIDGE_MEM16(destination_va + 2) = (uint16_t)(length + 1);
            BRIDGE_MEM32(destination_va + 4) = source_va;
        } else {
            BRIDGE_MEM16(destination_va + 0) = 0;
            BRIDGE_MEM16(destination_va + 2) = 0;
            BRIDGE_MEM32(destination_va + 4) = 0;
        }
    }
    g_eax = 0;
}

static void bridge_RtlEqualString(void)
{
    uint32_t string1_va = STACK_ARG(0);
    uint32_t string2_va = STACK_ARG(1);
    uint32_t case_insensitive = STACK_ARG(2);
    uint16_t length1, length2;
    uint32_t buffer1_va, buffer2_va;

    if (!string1_va || !string2_va) {
        g_eax = FALSE;
        return;
    }
    length1 = BRIDGE_MEM16(string1_va + 0);
    length2 = BRIDGE_MEM16(string2_va + 0);
    if (length1 != length2) {
        g_eax = FALSE;
        return;
    }
    if (length1 == 0) {
        g_eax = TRUE;
        return;
    }
    buffer1_va = BRIDGE_MEM32(string1_va + 4);
    buffer2_va = BRIDGE_MEM32(string2_va + 4);
    if (!buffer1_va || !buffer2_va) {
        g_eax = FALSE;
        return;
    }
    g_eax = case_insensitive
        ? (_strnicmp((const char*)XBOX_TO_NATIVE(buffer1_va),
                     (const char*)XBOX_TO_NATIVE(buffer2_va), length1) == 0)
        : (memcmp(XBOX_TO_NATIVE(buffer1_va), XBOX_TO_NATIVE(buffer2_va), length1) == 0);
}

static void bridge_RtlRaiseException(void)
{
    uint32_t record_ptr = STACK_ARG(0);
    uint32_t code = record_ptr ? BRIDGE_MEM32(record_ptr) : 0;

    static int raise_count = 0;
    raise_count++;
    if (raise_count <= 10) {
        fprintf(stderr, "  [KERNEL] RtlRaiseException: record=0x%08X code=0x%08X (#%d)\n",
                record_ptr, code, raise_count);
        fflush(stderr);
    }
    if (code == 0xC000008Fu &&
        getenv("MERCENARIES_TRACE_FP_EXCEPTION") != NULL) {
        g_recomp_trace_dump_requested = 1u;
    }
    /* Handle float exceptions by clearing the FPU status.
     *
     * On the real Xbox, RtlRaiseException dispatches through the SEH chain.
     * For float exceptions (0xC000008D-0xC0000093), the CRT exception handler
     * clears the x87/SSE status word and continues execution. Without clearing,
     * the caller re-checks the FPU status, sees the exception still pending,
     * and re-raises in an infinite loop.
     *
     * _clearfp() clears both x87 and SSE exception flags on Windows x64.
     */
    if (code >= 0xC000008Du && code <= 0xC0000093u) {
        _clearfp();
    }

    g_eax = 0;
}

/* Guest-side exception-unwind diagnostic. */
static void bridge_RtlUnwind(void)
{
    static uint32_t reports;
    const uint32_t target_frame = STACK_ARG(0);
    const uint32_t target_ip = STACK_ARG(1);
    const uint32_t exception_record = STACK_ARG(2);
    const uint32_t return_value = STACK_ARG(3);

    if (reports++ < 8u) {
        const uint32_t recent_index = g_recomp_recent_func_idx;
        fprintf(stderr,
                "  [KERNEL-RTL-UNWIND] current=%08X frame=%08X ip=%08X "
                "record=%08X return=%08X esp=%08X ebp=%08X eax=%08X "
                "ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n",
                (uint32_t)g_recomp_current_func, target_frame, target_ip,
                exception_record, return_value, g_esp, g_seh_ebp, g_eax,
                g_ebx, g_ecx, g_edx, g_esi, g_edi);
        for (uint32_t i = 0; i < 24u && i < recent_index; ++i) {
            const uint32_t index = (recent_index - 1u - i) & 63u;
            fprintf(stderr, "    rtl-unwind-recent[-%u]=%08X\n", i,
                    (uint32_t)g_recomp_recent_funcs[index]);
        }
        fflush(stderr);
    }

    /* Guest SEH frames cannot be passed to the host's 64-bit RtlUnwind.
     * Preserve the bridge's historical no-op semantics while making the
     * exceptional control path observable and correctly consuming stdcall
     * arguments in kernel_thunk_dispatch. */
    g_eax = return_value;
}
/* MmMapIoSpace (ordinal 177): map Xbox physical I/O memory. */
static void bridge_MmMapIoSpace(void)
{
    uint32_t phys_addr = STACK_ARG(0);
    uint32_t num_bytes = STACK_ARG(1);
    uint32_t protect = STACK_ARG(2);
    uint32_t xbox_va = xbox_HeapAlloc(num_bytes, 4096);

    fprintf(stderr, "  [KERNEL] MmMapIoSpace: phys=0x%08X size=%u -> Xbox VA 0x%08X\n",
            phys_addr, num_bytes, xbox_va);
    fflush(stderr);

    g_eax = xbox_va;
}

/* -- MmPersistContiguousMemory (ordinal 178) -------------
 * VOID MmPersistContiguousMemory(PVOID BaseAddress, ULONG NumberOfBytes, BOOLEAN Persist)
 *
 * Marks contiguous memory as persistent across reboots (for save data).
 * No-op for recompilation.
 */
static void bridge_MmPersistContiguousMemory(void)
{
    /* No-op stub */
    g_eax = 0;
}

/* -- Generic fallback for simple value-only functions ------ */
static void bridge_generic_stub(void)
{
    /* Success-returning stub for functions whose callers only check for 0.
     * Deliberately silent: the caller (kernel_thunk_dispatch) warns for
     * ordinals with no bridge at all, which is the case worth hearing about. */
    g_eax = 0;
}

/* -- Dispatch table: ordinal -> bridge function + stack arg bytes -- */

typedef void (*bridge_func_t)(void);

/**
 * stdcall arg byte count for each kernel ordinal.
 * On x86 stdcall, the callee cleans (ret N). Our bridges must do the same
 * via g_esp += N after execution so the simulated stack stays balanced.
 *
 * Special cases:
 *   - KfRaiseIrql/KfLowerIrql: fastcall (arg in ecx), 0 stack bytes
 *   - KeSetTimer: DueTime is LARGE_INTEGER (8 bytes on stack) + Timer + Dpc
 */
static int stdcall_args_for_ordinal(ULONG ordinal)
{
    if (ordinal == 46) return 24; /* HalReadWritePCISpace(6) */
    if (ordinal == 99) return 12; /* KeDelayExecutionThread(3) */
    switch (ordinal) {
    /* -- Display / AV -- */
    case   1: return  0;  /* AvGetSavedDataAddress(void) */
    case   2: return 16;  /* AvSendTVEncoderOption(4) */
    case   3: return 24;  /* AvSetDisplayMode(6) */
    case   4: return  4;  /* AvSetSavedDataAddress(1) */

    /* -- Unknown stubs -- */
    case   8: return  0;  /* Unknown_8(void) */
    case  42: return  0;  /* Unknown_42(void) */

    /* -- Pool Allocator -- */
    case  14: return  4;  /* ExAllocatePool(1) */
    case  15: return  8;  /* ExAllocatePoolWithTag(2) */
    /* case 16: DATA export - ExEventObjectType */
    case  17: return  4;  /* ExFreePool(1) */
    case  23: return  4;  /* ExQueryPoolBlockSize(1) */
    case  24: return 20;  /* ExQueryNonVolatileSetting(5) */

    /* -- HAL -- */
    case  40: return  4;  /* HalClearSoftwareInterrupt(1) */
    case  41: return  8;  /* HalDisableSystemInterrupt(2) */
    case  44: return  8;  /* HalGetInterruptVector(2) */
    case  46: return  8;  /* HalReadSMCTrayState(2) */
    case  47: return  8;  /* HalRegisterShutdownNotification(2) */
    case  49: return  4;  /* HalRequestSoftwareInterrupt(1) */
    case 358: return  0;  /* HalIsResetOrShutdownPending(void) */

    /* -- I/O Manager -- */
    case  62: return 36;  /* IoBuildDeviceIoControlRequest(9) */
    /* case  64: DATA export - IoCompletionObjectType */
    case  66: return 40;  /* IoCreateFile(10) */
    case  67: return  8;  /* IoCreateSymbolicLink(2) */
    case  69: return  4;  /* IoDeleteDevice(1) */
    /* case  71: DATA export - IoDeviceObjectType */
    case  74: return 12;  /* IoInitializeIrp(3) */
    case  81: return 20;  /* IoSetIoCompletion(5) */
    case  83: return  8;  /* IoStartNextPacket(2) */
    case  84: return 12;  /* IoStartNextPacketByKey(3) */
    case  85: return 16;  /* IoStartPacket(4) */
    case  86: return 32;  /* IoSynchronousDeviceIoControlRequest(8) */
    case  87: return 20;  /* IoSynchronousFsdRequest(5) */
    case 359: return  4;  /* IoMarkIrpMustComplete(1) */

    /* -- Kernel Synchronization -- */
    case  95: return  8;  /* KeAlertThread(2) */
    case  97: return  4;  /* KeCancelTimer(1) */
    case  98: return  4;  /* KeConnectInterrupt(1) */
    case  99: return 12;  /* KeDelayExecutionThread(3) */
    case 100: return  4;  /* KeDisconnectInterrupt(1) */
    case 107: return 12;  /* KeInitializeDpc(3) */
    case 109: return 28;  /* KeInitializeInterrupt(7) */
    case 113: return  8;  /* KeInitializeTimerEx(2) */
    case 119: return 12;  /* KeInsertQueueDpc(3) */
    case 124: return  4;  /* KeQueryBasePriorityThread(1) */
    case 126: return  0;  /* KeQueryPerformanceCounter(void) */
    case 127: return  0;  /* KeQueryPerformanceFrequency(void) */
    case 128: return  4;  /* KeQuerySystemTime(1) */
    case 129: return  0;  /* KeRaiseIrqlToDpcLevel(void) */
    case 137: return  4;  /* KeRemoveQueueDpc(1) */
    case 139: return  4;  /* KeRestoreFloatingPointState(1) */
    case 142: return  4;  /* KeSaveFloatingPointState(1) */
    case 143: return  8;  /* KeSetBasePriorityThread(2) */
    case 145: return 12;  /* KeSetEvent(3) */
    case 149: return 16;  /* KeSetTimer(Timer+DueTime[8]+Dpc) */
    case 150: return 20;  /* KeSetTimerEx(Timer+DueTime[8]+Period+Dpc) */
    case 151: return  4;  /* KeStallExecutionProcessor(1) */
    case 153: return 12;  /* KeSynchronizeExecution(3) */
    /* case 156: DATA export - KeTickCount */
    case 158: return 32;  /* KeWaitForMultipleObjects(8) */
    case 159: return 20;  /* KeWaitForSingleObject(5) */
    case 160: return  0;  /* KfRaiseIrql (fastcall: arg in ecx) */
    case 161: return  0;  /* KfLowerIrql (fastcall: arg in ecx) */

    /* -- Launch Data -- */
    /* case 164: DATA export - LaunchDataPage */

    /* -- Memory Management -- */
    case 165: return  4;  /* MmAllocateContiguousMemory(1) */
    case 166: return 20;  /* MmAllocateContiguousMemoryEx(5) */
    case 167: return  8;  /* MmAllocateSystemMemory(2) */
    case 168: return  8;  /* MmClaimGpuInstanceMemory(2) */
    case 169: return  8;  /* MmCreateKernelStack(2) */
    case 170: return  8;  /* MmDeleteKernelStack(2) */
    case 171: return  4;  /* MmFreeContiguousMemory(1) */
    case 172: return  8;  /* MmFreeSystemMemory(2) */
    case 173: return  4;  /* MmGetPhysicalAddress(1) */
    case 175: return 12;  /* MmLockUnlockBufferPages(3) */
    case 176: return  8;  /* MmLockUnlockPhysicalPage(2) */
    case 177: return 12;  /* MmMapIoSpace(3) */
    case 178: return 12;  /* MmPersistContiguousMemory(3) */
    case 179: return  4;  /* MmQueryAddressProtect(1) */
    case 180: return  4;  /* MmQueryAllocationSize(1) */
    case 181: return  4;  /* MmQueryStatistics(1) */
    case 182: return 12;  /* MmSetAddressProtect(3) */

    /* -- NT Virtual Memory -- */
    case 184: return 20;  /* NtAllocateVirtualMemory(5) */

    /* -- NT File I/O & Handle -- */
    case 187: return  4;  /* NtClose(1) */
    case 189: return 16;  /* NtCreateEvent(4) */
    case 190: return 36;  /* NtCreateFile(9) */
    case 193: return 16;  /* NtCreateSemaphore(4) */
    case 195: return  4;  /* NtDeleteFile(1) */
    case 196: return 40;  /* NtDeviceIoControlFile(10) */
    case 197: return 12;  /* NtDuplicateObject(3) */
    case 198: return  8;  /* NtFlushBuffersFile(2) */
    case 199: return 12;  /* NtFreeVirtualMemory(3) */
    case 200: return 40;  /* NtFsControlFile(10) */
    case 202: return 24;  /* NtOpenFile(6) */
    case 203: return  8;  /* NtOpenSymbolicLinkObject(2) */
    case 207: return 40;  /* NtQueryDirectoryFile(10) */
    case 210: return  8;  /* NtQueryFullAttributesFile(2) */
    case 211: return 20;  /* NtQueryInformationFile(5) */
    case 215: return 12;  /* NtQuerySymbolicLinkObject(3) */
    case 217: return 16;  /* NtQueryVirtualMemory(4) */
    case 218: return 20;  /* NtQueryVolumeInformationFile(5) */
    case 219: return 32;  /* NtReadFile(8) */
    case 222: return 12;  /* NtReleaseSemaphore(3) */
    case 225: return  8;  /* NtSetEvent(2) */
    case 226: return 20;  /* NtSetInformationFile(5) */
    case 228: return  8;  /* NtSetSystemTime(2) */
    case 232: return 12;  /* NtUserIoApcDispatcher(3) */
    case 233: return 12;  /* NtWaitForSingleObject(3) */
    case 234: return 16;  /* NtWaitForSingleObjectEx(4), includes WaitMode */
    case 235: return 24;  /* NtWaitForMultipleObjectsEx(6), includes WaitMode */
    case 236: return 32;  /* NtWriteFile(8) */
    case 238: return  0;  /* NtYieldExecution(void) */

    /* -- Object Manager -- */
    case 246: return 12;  /* ObReferenceObjectByHandle(3) - Xbox: Handle,Type,Object* */
    case 247: return 20;  /* ObReferenceObjectByName(5) */
    case 250: return  0;  /* ObfDereferenceObject (fastcall: arg in ecx) */

    /* -- Network / PHY -- */
    case 252: return  4;  /* PhyGetLinkState(1) */
    case 253: return  8;  /* PhyInitialize(2) */

    /* -- Threading -- */
    case 255: return 40;  /* PsCreateSystemThreadEx(10) */
    case 256: return 12;  /* KeDelayExecutionThread(3) */
    case 258: return  4;  /* PsTerminateSystemThread(1) */
    /* case 259: DATA export - PsThreadObjectType */

    /* -- Runtime Library -- */
    case 260: return 12;  /* RtlAnsiStringToUnicodeString(3) */
    case 269: return 12;  /* RtlCompareMemoryUlong(3) */
    case 277: return  4;  /* RtlEnterCriticalSection(1) */
    case 279: return 12;  /* RtlEqualString(3) */
    case 289: return  8;  /* RtlInitAnsiString(2) */
    case 291: return  4;  /* RtlInitializeCriticalSection(1) */
    case 294: return  4;  /* RtlLeaveCriticalSection(1) */
    case 301: return  4;  /* RtlNtStatusToDosError(1) */
    case 302: return  4;  /* RtlRaiseException(1) */
    case 304: return  8;  /* RtlTimeFieldsToTime(2) */
    case 305: return  8;  /* RtlTimeToTimeFields(2) */
    case 308: return 12;  /* RtlUnicodeStringToAnsiString(3) */
    case 312: return 16;  /* RtlUnwind(4) */
    case 354: return 12;  /* RtlRip(3) */

    /* -- Xbox Identity (data exports) -- */
    /* cases 322-328, 355-357: DATA exports */

    /* -- Port I/O -- */
    case 335: return 12;  /* WRITE_PORT_BUFFER_USHORT(3) */
    case 336: return 12;  /* WRITE_PORT_BUFFER_ULONG(3) */

    /* -- Crypto -- */
    case 337: return  4;  /* XcSHAInit(1) */
    case 338: return 12;  /* XcSHAUpdate(3) */
    case 339: return  8;  /* XcSHAFinal(2) */
    case 340: return 12;  /* XcRC4Key(3) */
    case 344: return 12;  /* XcPKDecPrivate(3) */
    case 345: return  4;  /* XcPKGetKeyLen(1) */
    case 346: return 12;  /* XcVerifyPKCS1Signature(3) */
    case 347: return 20;  /* XcModExp(5) */
    case 349: return 12;  /* XcKeyTable(3) */
    case 353: return  8;  /* XcUpdateCrypto(2) */

    default:  return  0;  /* DATA exports or truly unknown */
    }
}

static bridge_func_t bridge_for_ordinal(ULONG ordinal)
{
    if (ordinal == 46) return bridge_HalReadWritePCISpace;
    switch (ordinal) {
    /* Threading */
    case 255: return bridge_PsCreateSystemThreadEx;
    case 258: return bridge_PsTerminateSystemThread;

    /* File/Handle */
    case 187: return bridge_NtClose;
    case 190: return bridge_NtCreateFile;
    case 195: return bridge_NtDeleteFile;
    case 196: return bridge_NtDeviceIoControlFile;
    case 198: return bridge_NtFlushBuffersFile;
    case 200: return bridge_NtFsControlFile;
    case 202: return bridge_NtOpenFile;
    case 203: return bridge_NtOpenSymbolicLinkObject;
    case 207: return bridge_NtQueryDirectoryFile;
    case 210: return bridge_NtQueryFullAttributesFile;
    case 211: return bridge_NtQueryInformationFile;
    case 215: return bridge_NtQuerySymbolicLinkObject;
    case 218: return bridge_NtQueryVolumeInformationFile;
    case 219: return bridge_NtReadFile;
    case 232: return bridge_NtUserIoApcDispatcher;
    case 226: return bridge_NtSetInformationFile;
    case 236: return bridge_NtWriteFile;

    /* Memory - contiguous */
    case 165: return bridge_MmAllocateContiguousMemory;
    case 166: return bridge_MmAllocateContiguousMemoryEx;
    case 167: return bridge_MmAllocateSystemMemory;
    case 180: return bridge_MmQueryAllocationSize;
    case 171: return bridge_MmFreeContiguousMemory;
    case 172: return bridge_MmFreeSystemMemory;
    case 168: return bridge_MmClaimGpuInstanceMemory;
    case 173: return bridge_MmGetPhysicalAddress;
    case 175: return bridge_MmLockUnlockBufferPages;
    case 182: return bridge_MmSetAddressProtect;
    case 181: return bridge_MmQueryStatistics;

    /* Memory - virtual */
    case 184: return bridge_NtAllocateVirtualMemory;
    case 199: return bridge_NtFreeVirtualMemory;

    /* Pool */
    case  14: return bridge_ExAllocatePool;
    case  15: return bridge_ExAllocatePoolWithTag;
    case  17: return bridge_ExFreePool;
    case  23: return bridge_ExQueryPoolBlockSize;
    case  24: return bridge_ExQueryNonVolatileSetting;

    /* IRQL */
    case 160: return bridge_KfRaiseIrql;
    case 161: return bridge_KfLowerIrql;
    case 129: return bridge_KeRaiseIrqlToDpcLevel;

    /* Critical sections */
    case 291: return bridge_RtlInitializeCriticalSection;
    case 277: return bridge_RtlEnterCriticalSection;
    case 294: return bridge_RtlLeaveCriticalSection;

    /* Timing */
    case  97: return bridge_KeCancelTimer;
    case  99: return bridge_KeDelayExecutionThread;
    case 126: return bridge_KeQueryPerformanceCounter;
    case 127: return bridge_KeQueryPerformanceFrequency;
    case 128: return bridge_KeQuerySystemTime;
    case 149: return bridge_KeSetTimer;
    case 150: return bridge_KeSetTimerEx;
    case 151: return bridge_KeStallExecutionProcessor;

    /* DPC / Timer init */
    case 107: return bridge_KeInitializeDpc;
    case 113: return bridge_KeInitializeTimerEx;
    case 119: return bridge_KeInsertQueueDpc;
    case 137: return bridge_KeRemoveQueueDpc;

    /* Synchronization */
    case 189: return bridge_NtCreateEvent;
    case 145: return bridge_KeSetEvent;
    case 159: return bridge_KeWaitForSingleObject;
    case 153: return bridge_KeSynchronizeExecution;
    case 238: return bridge_NtYieldExecution;

    /* Hardware */
    case  44: return bridge_HalGetInterruptVector;
    case  47: return bridge_HalRegisterShutdownNotification;
    case  49: return bridge_HalReturnToFirmware;
    case  98: return bridge_KeConnectInterrupt;
    case 100: return bridge_KeDisconnectInterrupt;
    case 109: return bridge_KeInitializeInterrupt;

    /* Display */
    case   2: return bridge_AvSendTVEncoderOption;
    case   1: return bridge_AvGetSavedDataAddress;
    case   3: return bridge_AvSetDisplayMode;
    case   4: return bridge_AvSetSavedDataAddress;

    /* I/O */
    case  66: return bridge_IoCreateFile;
    case  67: return bridge_IoCreateSymbolicLink;
    case 188: return bridge_NtCreateDirectoryObject;
    case 246: return bridge_ObReferenceObjectByHandle;

    case 250: return bridge_ObfDereferenceObject;
    /* Memory - I/O mapping */
    case 177: return bridge_MmMapIoSpace;
    case 178: return bridge_MmPersistContiguousMemory;

    /* RTL */
    case 279: return bridge_RtlEqualString;
    case 289: return bridge_RtlInitAnsiString;
    case 301: return bridge_RtlNtStatusToDosError;
    case 302: return bridge_RtlRaiseException;
    case 312: return bridge_RtlUnwind;
    case 304: return bridge_RtlTimeFieldsToTime;
    case 305: return bridge_RtlTimeToTimeFields;

    default:  return NULL;
    }
}

/* -- Per-slot bridge functions (resolved at init) ---------- */

static bridge_func_t g_slot_bridges[XBOX_KERNEL_THUNK_TABLE_SIZE];
static int g_slot_arg_bytes[XBOX_KERNEL_THUNK_TABLE_SIZE];

/* Current dispatching slot */
static int g_kernel_dispatch_slot = -1;

static void kernel_thunk_dispatch(void)
{
    int slot = g_kernel_dispatch_slot;
    bridge_func_t bridge;
    ULONG ordinal;

    if (slot < 0 || slot >= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr, "  [KERNEL] bad slot %d\n", slot);
        g_eax = 0;
        g_esp += 4;  /* pop dummy return address */
        return;
    }

    ordinal = g_slot_ordinals[slot];
    bridge = g_slot_bridges[slot];

    g_kernel_call_count++;
    {
        static uint32_t trace_kernel_dump_at;
        static int trace_kernel_dump_initialized;
        if (!trace_kernel_dump_initialized) {
            const char *value = getenv("MERCENARIES_TRACE_KERNEL_DUMP_AT");
            if (value != NULL && value[0] != '\0') {
                const unsigned long parsed = strtoul(value, NULL, 0);
                if (parsed > 0u && parsed <= UINT32_MAX)
                    trace_kernel_dump_at = (uint32_t)parsed;
            }
            trace_kernel_dump_initialized = 1;
        }
        if (trace_kernel_dump_at != 0u &&
            (uint32_t)g_kernel_call_count == trace_kernel_dump_at) {
            const uint32_t recent_index = g_recomp_recent_game_func_idx;
            const uint32_t recent_count = recent_index < 256u ? recent_index : 256u;
            const uint32_t all_recent_index = g_recomp_recent_func_idx;
            const uint32_t all_recent_count =
                all_recent_index < 64u ? all_recent_index : 64u;
            fprintf(stderr,
                    "[KERNEL-BOUNDARY-TRACE] call=%d ordinal=%u slot=%d "
                    "current=%08X eax=%08X ebx=%08X ecx=%08X edx=%08X "
                    "esi=%08X edi=%08X ebp=%08X esp=%08X recent_index=%u count=%u\n",
                    g_kernel_call_count, ordinal, slot,
                    (uint32_t)g_recomp_current_func, g_eax, g_ebx, g_ecx,
                    g_edx, g_esi, g_edi, g_seh_ebp, g_esp,
                    recent_index, recent_count);
            for (uint32_t i = 0u; i < recent_count; ++i) {
                const uint32_t index = (recent_index - recent_count + i) & 255u;
                fprintf(stderr, "  [K%03u] %08X\n", i,
                        (uint32_t)g_recomp_recent_game_funcs[index]);
            }
            fprintf(stderr,
                    "[KERNEL-BOUNDARY-ALL-RECENT] recent_index=%u count=%u\n",
                    all_recent_index, all_recent_count);
            for (uint32_t i = 0u; i < all_recent_count; ++i) {
                const uint32_t index =
                    (all_recent_index - all_recent_count + i) & 63u;
                fprintf(stderr, "  [R%02u] %08X\n", i,
                        (uint32_t)g_recomp_recent_funcs[index]);
            }
            fflush(stderr);
            exit(91);
        }
    }
    if (g_kernel_call_count == 5000 &&
        getenv("MERCENARIES_TRACE_KERNEL_STORM") != NULL) {
        g_recomp_trace_dump_requested = 1u;
    }

    if (bridge_trace_kernel_calls_enabled() && g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] #%d: ordinal %u (slot %d) esp=0x%08X\n",
                g_kernel_call_count, ordinal, slot, g_esp);
        fflush(stderr);
    }

    {
        static DWORD last_summary_tick = 0;
        DWORD now = GetTickCount();
        if (last_summary_tick == 0) last_summary_tick = now;
        if (bridge_trace_kernel_calls_enabled() &&
            now - last_summary_tick >= 2000 && g_kernel_call_count > 200) {
            fprintf(stderr, "  [KERNEL] summary: %d total calls, latest ordinal %u (slot %d) esp=0x%08X\n",
                    g_kernel_call_count, ordinal, slot, g_esp);
            fflush(stderr);
            last_summary_tick = now;
        }
    }

    /* Hardware backends raise on their worker threads. Invoke the registered
     * guest ISR only here, at a safe boundary on the thread that owns the
     * recompiled CPU register state. */
    bridge_deliver_hardware_interrupts();
    xbox_kernel_service_guest_timers();
    xbox_kernel_service_guest_dpcs();

    /* Pop the dummy return address that PUSH32(esp, 0) pushed before RECOMP_ICALL.
     * On real x86, "call [thunk]" pushes a real return address and "ret" pops it.
     * In our model, the bridge is called directly (not via the simulated stack),
     * so we must manually consume the dummy return address. */
    g_esp += 4;

    if (bridge) {
        bridge();
    } else {
        /* No specific bridge - return 0. Warn once per ordinal rather than
         * gating on g_kernel_call_count: a missing bridge is rare and is
         * usually the reason a game misbehaves, so it must not be swallowed
         * by the general call-trace throttle. Bounded to one line per slot. */
        static uint8_t warned[XBOX_KERNEL_THUNK_TABLE_SIZE];
        if (!warned[slot]) {
            warned[slot] = 1;
            fprintf(stderr, "  [KERNEL] WARNING: no bridge for ordinal %u (slot %d), returning 0\n",
                    ordinal, slot);
            fflush(stderr);
        }
        g_eax = 0;
    }

    /* Clean stdcall args from the simulated stack.
     * On real x86, stdcall callee does "ret N" to pop the return address
     * and N bytes of arguments. We already popped the dummy return address
     * above; now pop the args. */
    g_esp += g_slot_arg_bytes[slot];

    if (bridge_trace_kernel_calls_enabled() && g_kernel_call_count <= 200) {
        fprintf(stderr, "  [KERNEL] -> returned 0x%08X\n", g_eax);
        fflush(stderr);
    }
}

/* -- Dispatch lookup -------------------------------------- */

/**
 * Look up a kernel thunk by synthetic VA.
 * Called as a fallback when recomp_lookup() returns NULL.
 */
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va)
{
    if (xbox_va >= KERNEL_VA_BASE && xbox_va < KERNEL_VA_END) {
        int slot = (xbox_va - KERNEL_VA_BASE) / 4;
        if (slot >= 0 && slot < XBOX_KERNEL_THUNK_TABLE_SIZE) {
            g_kernel_dispatch_slot = slot;
            return kernel_thunk_dispatch;
        }
    }
    return NULL;
}

/* -- Initialization --------------------------------------- */

/*
 * Where this title's kernel thunk table lives. Defaults to the compile-time
 * constant, but every XBE puts it somewhere different (it comes from the
 * header's KernelImageThunkAddress), so xbox_MemoryLayoutInit() parses the
 * real address out of the binary and overrides it here.
 *
 * Halo build 2276 puts it at 0x00253090 against the default's 0x0036B7C0 --
 * without the override the bridge patches ordinals into whatever happens to
 * live at the wrong address and every kernel call goes somewhere arbitrary.
 */
static uint32_t g_thunk_table_base  = XBOX_KERNEL_THUNK_TABLE_BASE;
static uint32_t g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;

void xbox_kernel_set_thunk_address(uint32_t xbox_va, uint32_t count)
{
    if (!xbox_va) {
        return;
    }

    g_thunk_table_base = xbox_va;

    /* count indexes g_slot_* arrays, which are sized by the macro. A title
     * importing more slots than the real kernel exports would run off them. */
    if (count && count <= XBOX_KERNEL_THUNK_TABLE_SIZE) {
        g_thunk_table_count = count;
    } else if (count > XBOX_KERNEL_THUNK_TABLE_SIZE) {
        fprintf(stderr,
                "  Kernel thunk bridge: XBE declares %u thunk slots, clamping to %d\n",
                count, XBOX_KERNEL_THUNK_TABLE_SIZE);
        g_thunk_table_count = XBOX_KERNEL_THUNK_TABLE_SIZE;
    }
}

void xbox_kernel_bridge_update_tick_count(void)
{
    const ULONG ticks = GetTickCount();
    FILETIME file_time;
    ULARGE_INTEGER now;
    const LONG64 next_due = InterlockedCompareExchange64(
        &g_guest_timer_next_due_100ns, 0, 0);

    xbox_KeTickCount = ticks;
    if (g_xbox_mem_offset != 0) {
        BRIDGE_MEM32(XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT) = ticks;
    }

    if (next_due > 0) {
        GetSystemTimeAsFileTime(&file_time);
        now.LowPart = file_time.dwLowDateTime;
        now.HighPart = file_time.dwHighDateTime;
        if ((LONG64)now.QuadPart >= next_due)
            InterlockedExchange(
                (volatile LONG *)&g_kernel_pending_guest_timers, 1);
    }
}

/**
 * Resolve the kernel thunk table in Xbox memory.
 *
 * Must be called AFTER xbox_MemoryLayoutInit() so Xbox memory is mapped.
 *
 * Reads the actual ordinals from the XBE memory thunk table (0x80000000|ordinal),
 * resolves each to a per-ordinal bridge function, and replaces the entry
 * with a synthetic VA for dispatch.
 */
void xbox_kernel_bridge_init(void)
{
    int i;
    int resolved = 0;
    int bridged = 0;
    int unbridged = 0;
    DWORD old_protect;

    fprintf(stderr, "  Kernel thunk bridge: resolving %d entries at 0x%08X\n",
            g_thunk_table_count, g_thunk_table_base);

    memset(g_guest_timers, 0, sizeof(g_guest_timers));
    memset(g_guest_dpcs, 0, sizeof(g_guest_dpcs));
    g_guest_dpc_count = 0;
    g_delivering_guest_dpcs = 0;
    g_kernel_pending_guest_dpcs = 0;
    g_delivering_guest_timers = 0;
    InterlockedExchange64(&g_guest_timer_next_due_100ns, 0);
    InterlockedExchange(
        (volatile LONG *)&g_kernel_pending_guest_timers, 0);

    /* Request writable thunk-table pages and restore their prior protection
     * below. The current layout already leaves the shared .rdata/.data
     * boundary page writable. */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        PAGE_READWRITE,
        &old_protect
    );

    /* Initialize kernel data export values first */
    kernel_data_init();

    for (i = 0; i < g_thunk_table_count; i++) {
        uint32_t va = g_thunk_table_base + i * 4;
        uint32_t current = BRIDGE_MEM32(va);

        if (current & 0x80000000) {
            /* Read the actual ordinal from Xbox memory */
            ULONG ordinal = current & 0x7FFFFFFF;
            g_slot_ordinals[i] = ordinal;

            /* Check if this is a data export */
            uint32_t data_va = kernel_data_va_for_ordinal(ordinal);
            if (data_va) {
                /* DATA export: point thunk to actual data in mapped memory.
                 * This allows the game to dereference the thunk entry. */
                BRIDGE_MEM32(va) = data_va;
                resolved++;
                bridged++;
                continue;
            }

            /* FUNCTION export: use synthetic VA for dispatch */
            g_slot_bridges[i] = bridge_for_ordinal(ordinal);
            g_slot_arg_bytes[i] = stdcall_args_for_ordinal(ordinal);
            if (g_slot_bridges[i]) {
                bridged++;
            } else {
                unbridged++;
            }

            /* Replace Xbox memory entry with synthetic VA */
            uint32_t synthetic = KERNEL_VA_BASE + i * 4;
            BRIDGE_MEM32(va) = synthetic;
            resolved++;
        }
    }

    /* Restore original protection */
    VirtualProtect(
        (LPVOID)((uintptr_t)g_thunk_table_base + g_xbox_mem_offset),
        g_thunk_table_count * 4,
        old_protect,
        &old_protect
    );

    fprintf(stderr, "  Kernel thunk bridge: %d/%d resolved (%d bridged, %d stub)\n",
            resolved, g_thunk_table_count, bridged, unbridged);
    fprintf(stderr, "  Synthetic VA range: 0x%08X-0x%08X\n",
            KERNEL_VA_BASE, KERNEL_VA_BASE + (resolved - 1) * 4);

}

/**
 * Xbox memory layout for the recompiled guest.
 *
 * A pagefile-backed section supplies 64 MiB of retail RAM, or 128 MiB
 * with the optional graphics bank. XBE sections (including executable
 * bytes) are copied to guest VAs within that mapping and zero-filled
 * to their virtual sizes. Native C executes the translated instructions;
 * guest code can still read the original bytes.
 *
 * Address translation, RAM aliases, stack/heap setup, and synthetic
 * kernel data share this layout. See the shared-page protection note
 * in xbox_MemoryLayoutInit before changing section permissions.
 */

#include "xbox_memory_layout.h"
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* XBE header field offsets (per xboxdevwiki.net/Xbe) */
#define XBE_MAGIC_OFFSET        0x0000
#define XBE_BASE_ADDR_OFFSET    0x0104
#define XBE_TLS_ADDR_OFFSET     0x012C
#define XBE_HEADER_SIZE_OFFSET  0x0108
#define XBE_SECTION_COUNT_OFFSET 0x011C
#define XBE_SECTION_HEADERS_OFFSET 0x0120

/* XBE section header layout (56 bytes each) */
#define SECTHDR_FLAGS       0x00
#define SECTHDR_VA          0x04
#define SECTHDR_VSIZE       0x08
#define SECTHDR_RAW_OFFSET  0x0C
#define SECTHDR_RAW_SIZE    0x10
#define SECTHDR_NAME_ADDR   0x14
#define SECTHDR_SIZE        56

static void *g_memory_base = NULL;
static size_t g_memory_size = 0;
static BOOL g_extended_graphics_memory;

void xbox_SetExtendedGraphicsMemory(BOOL enabled)
{
    /* The layout is immutable once the CPU/GPU mappings exist. */
    if (!g_memory_base) g_extended_graphics_memory = enabled;
}

uint32_t xbox_GetGraphicsMemorySize(void)
{
    return XBOX_TOTAL_RAM * (g_extended_graphics_memory ? 2u : 1u);
}

uint32_t xbox_GetExtendedGraphicsPool(void)
{
    return g_memory_base && g_extended_graphics_memory ? XBOX_TOTAL_RAM : 0u;
}
static ptrdiff_t g_memory_offset = 0;  /* Host mapping base minus XBOX_MAP_START */

/* File mapping handle for the Xbox memory region.
 * Using CreateFileMapping + MapViewOfFileEx allows mirror views to alias
 * the same physical pages as the base region, so writes to mirror addresses
 * (which wrap modulo 64 MB on real Xbox hardware) correctly modify the
 * underlying data. */
static HANDLE g_mapping_handle = NULL;

/* Mirror view pointers for cleanup */
static void *g_mirror_views[XBOX_NUM_MIRRORS] = {0};

/* Demand-mapped direct physical-memory (KSEG0/KSEG1) views. Windows requires
 * file-view offsets and target addresses to use 64 KiB granularity.
 *
 * Track each CPU alias block, rather than only each 64 MiB physical block.
 * Addresses such as 0x812D0000 and 0x852D0000 name the same RAM but need two
 * distinct host views. Collapsing the tracking index to physical RAM made
 * the second fault look mapped when only the first host address existed. */
#define XBOX_ALIAS_BLOCK_SIZE 0x10000u
#define XBOX_CPU_ALIAS_BASE 0x80000000u
#define XBOX_CPU_ALIAS_END  0xC0000000u
#define XBOX_CPU_ALIAS_BLOCK_COUNT \
    ((XBOX_CPU_ALIAS_END - XBOX_CPU_ALIAS_BASE) / XBOX_ALIAS_BLOCK_SIZE)
static void *g_cpu_alias_views[XBOX_CPU_ALIAS_BLOCK_COUNT] = {0};


/* Separate allocation for Xbox kernel address space (0x80010000+).
 * Some RenderWare code reads the kernel PE header to detect features. */
static void *g_kernel_memory = NULL;

/* D3D's Xbox cache-flush path writes a fence value at 0x80000000 before and
 * after WBINVD. This address is separate from the kernel image at 0x80010000
 * and needs its own writable synthetic page on the host. */
static void *g_d3d_fence_memory = NULL;

/* Global offset accessible by recompiled code (via recomp_types.h) */
ptrdiff_t g_xbox_mem_offset = 0;

/* Global registers for recompiled code (via recomp_types.h) */
uint32_t g_eax = 0, g_ecx = 0, g_edx = 0, g_esp = 0;
uint32_t g_ebx = 0, g_esi = 0, g_edi = 0;
volatile uint32_t g_recomp_current_func = 0;
volatile uint32_t g_recomp_entry_trace_enabled = 0;
volatile uint32_t g_recomp_watchdog_heartbeat_enabled = 0;
volatile uint32_t g_recomp_watchdog_heartbeat = 0;
volatile uint32_t g_recomp_irq_entry_safepoint_enabled = 0;
volatile uint32_t g_recomp_target_call_trace_enabled = 0;
volatile uint32_t g_recomp_recent_funcs[64] = {0};
volatile uint32_t g_recomp_recent_func_idx = 0;
volatile uint32_t g_recomp_recent_game_funcs[256] = {0};
volatile uint32_t g_recomp_recent_game_func_idx = 0;
volatile uint32_t g_recomp_recent_asset_requests[512] = {0};
volatile uint32_t g_recomp_recent_asset_request_idx = 0;
volatile uint32_t g_recomp_trace_dump_requested = 0;

/* Shared x87 register stack. Floating-point values may cross translated
 * function boundaries through ST(0), just as integer returns cross in EAX. */
double g_fp_stack[8] = {0};
uint32_t g_fp_top = 0;
uint16_t g_x87_control_word = 0x027Fu;
uint16_t g_x87_status_word = 0u;

/* Shared SSE and MMX architectural registers. */
float g_xmm0[4] = {0}, g_xmm1[4] = {0}, g_xmm2[4] = {0}, g_xmm3[4] = {0};
float g_xmm4[4] = {0}, g_xmm5[4] = {0}, g_xmm6[4] = {0}, g_xmm7[4] = {0};
uint64_t g_mm0 = 0, g_mm1 = 0, g_mm2 = 0, g_mm3 = 0;
uint64_t g_mm4 = 0, g_mm5 = 0, g_mm6 = 0, g_mm7 = 0;

/* SEH frame pointer bridge (see recomp_types.h for explanation) */
uint32_t g_seh_ebp = 0;

/* ICALL trace ring buffer */
volatile uint32_t g_icall_trace[16] = {0};
volatile uint32_t g_icall_trace_idx = 0;
volatile uint64_t g_icall_count = 0;
static void xbox_HeapReset(void);


#if defined(_WIN64)
/* Check the entire guest address space, including physical aliases and MMIO,
 * before selecting a Wine arena. Reserving RAM alone misses module collisions
 * in the later VRAM aperture. No host mappings are removed or overwritten. */
static BOOL xbox_guest_arena_available(uintptr_t base)
{
    uintptr_t cursor = base;
    const uintptr_t size = 0x100000000ull;
    if (base > UINTPTR_MAX - size) return FALSE;
    while (cursor < base + size) {
        MEMORY_BASIC_INFORMATION info;
        uintptr_t next;
        if (!VirtualQuery((LPCVOID)cursor, &info, sizeof(info)) ||
            info.State != MEM_FREE) return FALSE;
        next = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (next <= cursor) return FALSE;
        cursor = next;
    }
    return TRUE;
}
#endif

/* Keep the failure available even when diagnostic logging is disabled. */
static char g_memory_layout_error[512];
const char *xbox_GetMemoryLayoutError(void)
{
    return g_memory_layout_error;
}

static BOOL xbox_memory_layout_failure(const char *stage, DWORD error)
{
    char detail[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, error, 0, detail, sizeof(detail), NULL);
    snprintf(g_memory_layout_error, sizeof(g_memory_layout_error),
             "%s (Windows error %lu).\n%s", stage, (unsigned long)error, detail);
    fprintf(stderr, "xbox_MemoryLayoutInit: %s\n", g_memory_layout_error);
    SetLastError(error);
    return FALSE;
}

static void *xbox_map_guest_at(HANDLE mapping, size_t size, uintptr_t base, DWORD *error)
{
    void *view = MapViewOfFileEx(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size, (void *)base);
    if (!view) {
        *error = GetLastError();
        return NULL;
    }
    if ((uintptr_t)view != base) {
        UnmapViewOfFile(view);
        *error = ERROR_INVALID_ADDRESS;
        return NULL;
    }
    return view;
}

static void *xbox_map_guest_memory(HANDLE mapping, size_t size, DWORD *error)
{
    static const uintptr_t native_bases[] = {
        XBOX_BASE_ADDRESS, 0x00800000, 0x01000000, 0x02000000, 0x10000000
    };
    int wine = 0;
    void *view;
#if defined(_WIN64)
    wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != NULL;
#endif
    if (!wine) {
        for (size_t i = 0; i < sizeof(native_bases)/sizeof(native_bases[0]); ++i) {
            view = xbox_map_guest_at(mapping, size, native_bases[i], error);
            if (view) return view;
        }
    }
#if defined(_WIN64)
    static const uintptr_t high_bases[] = {
        0x1000000000ull, 0x2000000000ull, 0x3000000000ull
    };
    for (size_t i = 0; i < sizeof(high_bases)/sizeof(high_bases[0]); ++i) {
        if (!xbox_guest_arena_available(high_bases[i])) continue;
        view = xbox_map_guest_at(mapping, size, high_bases[i], error);
        if (view) return view;
    }

    /* Query free regions rather than stopping at a finite list of hints.
     * The 4 GiB is virtual address space, not a 4 GiB RAM commitment. Only
     * the 64/128 MiB backing section is committed. Never replace a host view.
     * A concurrent allocation may invalidate a query; failed maps are safe
     * and retried at another arena. Bound retries on resource failures. */
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    const uintptr_t arena_size = 0x100000000ull;
    const uintptr_t limit = (uintptr_t)system.lpMaximumApplicationAddress;
    const uintptr_t alignment = system.dwAllocationGranularity;
    uintptr_t cursor = high_bases[0];
    unsigned attempts = 0;
    while (cursor <= limit && limit - cursor >= arena_size - 1 && attempts < 64) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)cursor, &info, sizeof(info))) break;
        uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (end <= cursor) break;
        uintptr_t candidate = (cursor + alignment - 1) & ~(alignment - 1);
        if (info.State == MEM_FREE && end >= candidate && end - candidate >= arena_size) {
            ++attempts;
            view = xbox_map_guest_at(mapping, size, candidate, error);
            if (view) return view;
            cursor = candidate + arena_size;
        } else {
            cursor = end;
        }
    }
#endif
    return NULL;
}

BOOL xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size)
{
    DWORD old_protect;
    const uint8_t *xbe = (const uint8_t *)xbe_data;

    g_memory_layout_error[0] = '\0';
    if (g_memory_base)
        return xbox_memory_layout_failure("Xbox memory is already initialized", ERROR_ALREADY_EXISTS);

    xbox_HeapReset();
    /*
     * Calculate the full range we need to map.
     * From XBOX_MAP_START (0x0) to the end of the furthest section.
     * This includes low memory (KPCR at 0x0-0xFF) which game code reads
     * from, the XBE sections, and the simulated stack.
     */
    /* Mod graphics occupy a separate upper bank. The CPU heap and game
     * objects retain their retail 64 MiB bounds. */
    g_memory_size = xbox_GetGraphicsMemorySize();

    /*
     * Create a file mapping backed by the page file.
     *
     * Using file mapping instead of VirtualAlloc allows us to map the same
     * physical pages at multiple virtual addresses via MapViewOfFileEx.
     * This is critical for the Xbox RAM mirror: the Xbox memory controller
     * uses a 26-bit address bus, so ALL addresses wrap modulo 64 MB.
     * Code that writes to address 0x20000448 is really writing to 0x00000448.
     * With file mapping views, we create aliased mappings at 64 MB intervals
     * that all point to the same physical memory.
     */
    g_mapping_handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,   /* page file backed */
        NULL,                   /* default security */
        PAGE_READWRITE,         /* read-write access */
        0,                      /* high DWORD of size */
        (DWORD)g_memory_size,   /* low DWORD of configured backing size */
        NULL                    /* unnamed mapping */
    );
    if (!g_mapping_handle) {
        return xbox_memory_layout_failure("Could not allocate Xbox backing memory", GetLastError());
    }

    /* Preserve working native placements, then search a complete high arena
     * if the fixed locations collide. A RAM-only OS-selected view can leave
     * the later CPU aliases or VRAM aperture occupied by an unrelated module. */
    DWORD map_error = ERROR_INVALID_ADDRESS;
    g_memory_base = xbox_map_guest_memory(g_mapping_handle, g_memory_size, &map_error);
    if (!g_memory_base) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
        g_memory_size = 0;
        return xbox_memory_layout_failure("Could not map the Xbox virtual address range", map_error);
    }

    g_memory_offset = (uintptr_t)g_memory_base - XBOX_MAP_START;

    if (g_memory_offset == 0) {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%08X (original Xbox address)\n",
                g_memory_size / 1024, XBOX_MAP_START);
    } else {
        fprintf(stderr, "xbox_MemoryLayoutInit: mapped %zu KB at 0x%p (offset %+td from Xbox base)\n",
                g_memory_size / 1024, g_memory_base, g_memory_offset);
    }

    /*
     * Helper macro: convert Xbox VA to actual mapped address.
     * When g_memory_offset == 0 (ideal case), this is identity.
     */
    #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))

    /*
     * Copy XBE header to base address.
     * The Xbox kernel maps the XBE image header at 0x00010000.
     * Game code reads kernel thunk table, certificate data, and
     * section info from this region.
     */
    {
        /* XBE header size is at file offset 0x0108 (SizeOfImageHeader) */
        DWORD header_size = 0;
        if (xbe_size >= 0x10C) {
            header_size = *(const DWORD *)(xbe + 0x0108);
        }
        if (header_size == 0 || header_size > 0x10000)
            header_size = 0x1000;  /* fallback: 4KB */
        if (header_size > xbe_size)
            header_size = (DWORD)xbe_size;
        memcpy(XBOX_VA(XBOX_BASE_ADDRESS), xbe, header_size);
        fprintf(stderr, "  XBE header: %u bytes at %p (Xbox VA 0x%08X)\n",
                header_size, XBOX_VA(XBOX_BASE_ADDRESS), XBOX_BASE_ADDRESS);
    }

    /*
     * Dynamically load ALL XBE sections by parsing the section headers.
     *
     * This replaces the old approach of hardcoding section addresses for
     * a specific game (Burnout 3). By reading the section table from the
     * XBE header, any game's sections are loaded automatically.
     *
     * Every section is copied to its original Xbox VA:
     * - .text: needed because memory walkers may scan code pages
     * - .rdata: constants, vtables, kernel thunk table
     * - .data: global variables (initialized portion from XBE, BSS zeroed)
     * - XDK library sections (D3D, DSOUND, WMADEC, XPP, etc.)
     * - DOLBY, BINK, XTIMAGE, etc.
     */
    {
        DWORD base_addr = *(const DWORD *)(xbe + XBE_BASE_ADDR_OFFSET);
        DWORD num_sections = *(const DWORD *)(xbe + XBE_SECTION_COUNT_OFFSET);
        DWORD sect_headers_va = *(const DWORD *)(xbe + XBE_SECTION_HEADERS_OFFSET);
        DWORD sect_headers_off = sect_headers_va - base_addr;
        int sections_loaded = 0;
        size_t total_bytes = 0;

        if (num_sections > 64) num_sections = 64;  /* sanity cap */

        fprintf(stderr, "  XBE sections: %u (headers at file offset 0x%08X)\n",
                num_sections, sect_headers_off);

        for (DWORD si = 0; si < num_sections; si++) {
            if (sect_headers_off + (si + 1) * SECTHDR_SIZE > xbe_size) break;

            const uint8_t *sh = xbe + sect_headers_off + si * SECTHDR_SIZE;
            DWORD sec_va       = *(const DWORD *)(sh + SECTHDR_VA);
            DWORD sec_vsize    = *(const DWORD *)(sh + SECTHDR_VSIZE);
            DWORD sec_raw_off  = *(const DWORD *)(sh + SECTHDR_RAW_OFFSET);
            DWORD sec_raw_size = *(const DWORD *)(sh + SECTHDR_RAW_SIZE);
            DWORD sec_name_va  = *(const DWORD *)(sh + SECTHDR_NAME_ADDR);

            /* Read section name from XBE header */
            const char *sec_name = "?";
            DWORD name_off = sec_name_va - base_addr;
            if (name_off < xbe_size && name_off + 8 <= xbe_size)
                sec_name = (const char *)(xbe + name_off);

            /* XBE sections must fit within the retail lower 64 MiB bank */
            if (sec_va < XBOX_BASE_ADDRESS || sec_va + sec_vsize > XBOX_TOTAL_RAM)
                continue;

            /* Determine copy size (raw_size may exceed vsize due to alignment) */
            DWORD copy_size = (sec_raw_size < sec_vsize) ? sec_raw_size : sec_vsize;

            /* Zero the full virtual size first (handles BSS) */
            memset(XBOX_VA(sec_va), 0, sec_vsize);

            /* Copy initialized data from XBE */
            if (copy_size > 0 && sec_raw_off + copy_size <= xbe_size) {
                memcpy(XBOX_VA(sec_va), xbe + sec_raw_off, copy_size);
            }

            sections_loaded++;
            total_bytes += copy_size;

            fprintf(stderr, "  [%2u] %-12s VA=0x%08X vsize=%-8u raw=0x%08X rsize=%-8u%s\n",
                    si, sec_name, sec_va, sec_vsize, sec_raw_off, sec_raw_size,
                    (sec_raw_size < sec_vsize) ? " (BSS)" : "");
        }

        fprintf(stderr, "  Loaded %d/%u sections (%zu bytes total)\n",
                sections_loaded, num_sections, total_bytes);
    }

    /*
     * Parse the kernel thunk table address from the XBE header.
     * The XBE stores KernelImageThunkAddress at offset 0x0158, XOR-encrypted.
     * The key differs between retail and debug XBEs, and there is no flag
     * saying which was used -- decode with both and keep whichever lands in
     * the mapped address range (this is what tools/xbe_parser does).
     *
     * Debug XBEs are not an edge case here: they are the builds most worth
     * recompiling, since they still carry assert strings and symbols. Halo's
     * cachebeta.xbe is one, and assuming the retail key decoded its thunk
     * table to 0xB4F98174 instead of 0x00253090, which silently fell back to
     * the compile-time default and resolved 0 of 378 kernel imports.
     */
    if (xbe_size >= 0x015C) {
        uint32_t thunk_raw = *(const uint32_t *)(xbe + 0x0158);
        uint32_t thunk_retail = thunk_raw ^ 0x5B6D40B6;  /* retail XOR key */
        uint32_t thunk_debug  = thunk_raw ^ 0xEFB1F152;  /* debug XOR key  */
        uint32_t thunk_va;

        if (thunk_retail >= XBOX_BASE_ADDRESS && thunk_retail < XBOX_TOTAL_RAM) {
            thunk_va = thunk_retail;
        } else {
            thunk_va = thunk_debug;
        }

        /* Validate: thunk VA should be within our mapped region */
        if (thunk_va >= XBOX_BASE_ADDRESS && thunk_va < XBOX_TOTAL_RAM) {
            /* Count thunk entries by scanning until we hit 0 */
            uint32_t thunk_count = 0;
            /* XBOX_KERNEL_THUNK_TABLE_SIZE, not 366: the kernel exports 378
             * slots, and kernel.h notes 366 is short by 12. A title importing
             * a high ordinal would have had its table truncated here. */
            for (uint32_t t = 0; t < XBOX_KERNEL_THUNK_TABLE_SIZE; t++) {
                uint32_t entry = *(volatile uint32_t *)((uintptr_t)(thunk_va + t * 4) + g_memory_offset);
                if (entry == 0) break;
                thunk_count++;
            }
            xbox_kernel_set_thunk_address(thunk_va, thunk_count);
            fprintf(stderr, "  Kernel thunks: %u entries at Xbox VA 0x%08X\n",
                    thunk_count, thunk_va);
        } else {
            fprintf(stderr, "  WARNING: kernel thunk VA 0x%08X out of range (raw=0x%08X)\n",
                    thunk_va, thunk_raw);
        }
    }

    /*
     * NOTE: .rdata is NOT set read-only.
     * VirtualProtect rounds to page boundaries, and the .rdata end (0x003B2454)
     * and .data start (0x003B2360) share the same 4KB page (0x003B2000-0x003B2FFF).
     * Making .rdata read-only also makes the first ~0xCA0 bytes of .data read-only,
     * which causes game initialization code to fault when writing to .data globals
     * in that overlap range.
     */
    (void)old_protect;

    #undef XBOX_VA

    /* Set the global offset for recompiled code MEM macros */
    g_xbox_mem_offset = g_memory_offset;

    /*
     * Initialize the Xbox stack for recompiled code.
     * The stack area lives at XBOX_STACK_BASE in Xbox address space.
     * g_esp is the global stack pointer shared by all translated functions.
     */
    g_esp = XBOX_STACK_TOP;
    fprintf(stderr, "  Stack: %u KB at Xbox VA 0x%08X (ESP = 0x%08X)\n",
            XBOX_STACK_SIZE / 1024, XBOX_STACK_BASE, g_esp);

    /*
     * Populate the fake Thread Information Block (TIB) at Xbox VA 0x0.
     *
     * The original Xbox code uses fs:[offset] to read per-thread data,
     * but the recompiler drops the fs: segment prefix and generates
     * MEM32(offset) instead. Since we mapped low memory (0x0-0xFFFF),
     * we populate the TIB fields that game code accesses:
     *
     *   fs:[0x00] = SEH exception list (-1 = end of chain)
     *   fs:[0x04] = stack base used for negative Xbox TLS indices
     *   fs:[0x08] = stack limit (bottom of stack)
     *   fs:[0x18] = self pointer (TIB address)
     *   fs:[0x20] = KPCR Prcb pointer (-> fake structure)
     *   fs:[0x28] = KPRCB.CurrentThread (a KTHREAD pointer)
     *
     * We use free space in the BSS area for the fake structures.
     */
    {
        #define XBOX_VA(va) ((void *)((uintptr_t)(va) + g_memory_offset))
        #define MEM32_INIT(va, val) (*(uint32_t *)XBOX_VA(va) = (uint32_t)(val))

        /* Fake TIB at address 0x0 */
        MEM32_INIT(0x00, 0xFFFFFFFF);       /* SEH: end of chain */
        MEM32_INIT(0x08, XBOX_STACK_BASE);  /* Stack limit (low address) */
        MEM32_INIT(0x18, 0x00000000);       /* Self pointer (TIB at VA 0) */

        /*
         * fs:[0x20] - On Xbox KPCR, this is the Prcb pointer.
         * Game code reads [fs:[0x20] + 0x250] which on the real Xbox
         * accesses a D3D cache structure. We set it to 0 so the read
         * at offset 0x250 returns 0, causing the cache init to be skipped.
         */
        MEM32_INIT(0x20, 0x00000000);

        /*
         * Xbox titles address XBE TLS as fs:[4] + tls_index * 4. The XAPI
         * entry point stores a negative index derived from the TLS block size,
         * because the block normally lives immediately below StackBase.
         * KPRCB.CurrentThread is exposed directly at fs:[0x28], and the
         * KTHREAD TlsData member is at offset 0x28.
         *
         * Keep the synthetic TLS block out of the actively growing guest stack
         * while preserving that exact address equation. Its first dword points
         * at the payload, matching the Xbox kernel/Cxbx thread layout.
         */
        #define FAKE_KTHREAD_VA 0x00760000
        #define FAKE_TLS_VA     0x00700000
        uint32_t tls_block_size = 4;
        uint32_t tls_raw_start = 0;
        uint32_t tls_raw_size = 0;
        uint32_t tls_zero_size = 0;
        uint32_t tls_index_addr = 0;

        if (xbe_size >= XBE_TLS_ADDR_OFFSET + sizeof(uint32_t)) {
            const uint32_t tls_directory =
                *(const uint32_t *)(xbe + XBE_TLS_ADDR_OFFSET);

            if (tls_directory >= XBOX_BASE_ADDRESS &&
                tls_directory + 24u <= XBOX_TOTAL_RAM) {
                const uint32_t tls_raw_end =
                    *(const uint32_t *)XBOX_VA(tls_directory + 4u);
                tls_raw_start = *(const uint32_t *)XBOX_VA(tls_directory);
                tls_index_addr =
                    *(const uint32_t *)XBOX_VA(tls_directory + 8u);
                tls_zero_size =
                    *(const uint32_t *)XBOX_VA(tls_directory + 16u);
                if (tls_raw_end >= tls_raw_start)
                    tls_raw_size = tls_raw_end - tls_raw_start;
            }
        }

        tls_block_size += (tls_raw_size + tls_zero_size + 15u) & ~15u;
        if (tls_block_size > 0x10000u)
            tls_block_size = 4;

        memset(XBOX_VA(FAKE_TLS_VA), 0, tls_block_size);
        MEM32_INIT(FAKE_TLS_VA, FAKE_TLS_VA + 4u);
        if (tls_raw_size != 0 && tls_raw_start >= XBOX_BASE_ADDRESS &&
            tls_raw_start + tls_raw_size <= XBOX_TOTAL_RAM) {
            memcpy(XBOX_VA(FAKE_TLS_VA + 4u), XBOX_VA(tls_raw_start),
                   tls_raw_size);
        }

        MEM32_INIT(0x04, FAKE_TLS_VA + tls_block_size);
        MEM32_INIT(0x28, FAKE_KTHREAD_VA);
        MEM32_INIT(FAKE_KTHREAD_VA + 0x28, FAKE_TLS_VA);
        if (tls_index_addr >= XBOX_BASE_ADDRESS &&
            tls_index_addr + 4u <= XBOX_TOTAL_RAM) {
            MEM32_INIT(tls_index_addr,
                       (uint32_t)(-(int32_t)(tls_block_size / 4u)));
        }

        fprintf(stderr,
                "  TIB: TLS block=0x%08X size=%u KTHREAD=0x%08X index=0x%08X\n",
                FAKE_TLS_VA, tls_block_size, FAKE_KTHREAD_VA, tls_index_addr);

        #undef FAKE_KTHREAD_VA
        #undef FAKE_TLS_VA
        #undef MEM32_INIT
        #undef XBOX_VA
    }

    /*
     * Map the D3D cache-flush fence at Xbox VA 0x80000000. Xbox graphics
     * code writes a sequence number and then 0xDEADBEEF here around WBINVD;
     * the native recomp treats WBINVD as a no-op, but the sentinel writes
     * must still land on valid memory.
     */
    {
        #define XBOX_D3D_FENCE_BASE 0x80000000u
        #define D3D_FENCE_PAGE_SIZE 4096
        uintptr_t fence_native = XBOX_D3D_FENCE_BASE + g_memory_offset;
        g_d3d_fence_memory = VirtualAlloc(
            (LPVOID)fence_native,
            D3D_FENCE_PAGE_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_d3d_fence_memory) {
            memset(g_d3d_fence_memory, 0, D3D_FENCE_PAGE_SIZE);
            fprintf(stderr, "  D3D: cache fence at Xbox VA 0x%08X (native %p)\n",
                    XBOX_D3D_FENCE_BASE, g_d3d_fence_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map D3D fence VA 0x%08X\n",
                    XBOX_D3D_FENCE_BASE);
        }
        #undef XBOX_D3D_FENCE_BASE
        #undef D3D_FENCE_PAGE_SIZE
    }

    /*
     * Allocate a page at Xbox kernel address space (0x80010000).
     *
     * RenderWare's Xbox driver code (xbcache.c) reads MEM32(0x8001003C)
     * to parse the Xbox kernel's PE header and find the INIT section for
     * CPU cache line sizing. On PC, we provide a minimal fake PE header
     * with 0 sections so the function gracefully skips the cache init.
     *
     * The actual native address is 0x80010000 + g_memory_offset.
     */
    {
        #define XBOX_KERNEL_BASE 0x80010000u
        #define KERNEL_PAGE_SIZE 4096
        uintptr_t kernel_native = XBOX_KERNEL_BASE + g_memory_offset;
        g_kernel_memory = VirtualAlloc(
            (LPVOID)kernel_native,
            KERNEL_PAGE_SIZE,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE
        );
        if (g_kernel_memory) {
            /* Zero-fill then set e_lfanew = 0x80 (offset to PE header).
             * With the rest zeroed, NumberOfSections = 0 and the INIT
             * section search finds nothing, which is the safe path. */
            memset(g_kernel_memory, 0, KERNEL_PAGE_SIZE);
            *(uint32_t *)((uint8_t *)g_kernel_memory + 0x3C) = 0x80;  /* e_lfanew */
            fprintf(stderr, "  Kernel: fake PE header at Xbox VA 0x%08X (native %p)\n",
                    XBOX_KERNEL_BASE, g_kernel_memory);
        } else {
            fprintf(stderr, "  WARNING: could not map Xbox kernel VA 0x%08X\n",
                    XBOX_KERNEL_BASE);
        }
        #undef XBOX_KERNEL_BASE
        #undef KERNEL_PAGE_SIZE
    }

    /* Initialize the dynamic heap. */
    fprintf(stderr, "  Heap: %u MB at Xbox VA 0x%08X-0x%08X\n",
            XBOX_HEAP_SIZE / (1024 * 1024), XBOX_HEAP_BASE,
            XBOX_HEAP_BASE + XBOX_HEAP_SIZE);

    /*
     * Map mirror views of physical memory (64 MiB retail, 128 MiB extended).
     *
     * On retail Xbox, physical RAM wraps at 64 MB due to the 26-bit
     * address bus. Address 0x04070000 reads the same data as 0x00070000.
     * The RenderWare engine's memory walker crosses 64 MB and accesses
     * mirrored data for an extended walk covering 256+ MB of virtual
     * addresses. Game init code also writes large data structures past
     * 64 MB that on real hardware wrap into physical RAM.
     *
     * We map additional views of the SAME file mapping section at 64 MB
     * intervals. All views alias the same physical pages, so reads and
     * writes at any mirror address correctly access the base data.
     */
    {
        int mirrors_ok = 0;
        const int mirror_count = XBOX_NUM_MIRRORS /
                                 (g_extended_graphics_memory ? 2 : 1);
        for (int m = 0; m < mirror_count; m++) {
            uintptr_t mirror_base = (uintptr_t)g_memory_base +
                                    (uintptr_t)(m + 1) * g_memory_size;
            g_mirror_views[m] = MapViewOfFileEx(
                g_mapping_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                g_memory_size,
                (LPVOID)mirror_base
            );
            if (g_mirror_views[m]) {
                mirrors_ok++;
            } else {
                fprintf(stderr, "  Mirror %d: FAILED at %p (error %lu)\n",
                        m + 1, (void *)mirror_base, GetLastError());
            }
        }
        fprintf(stderr, "  RAM mirror: %d/%d views mapped (covers %d MB)\n",
                mirrors_ok, mirror_count,
                (int)((mirrors_ok + 1) * g_memory_size / (1024 * 1024)));
    }

    if (g_extended_graphics_memory)
        fprintf(stderr, "  Graphics: extended bank enabled; CPU heap remains 64 MiB\n");
    fprintf(stderr, "xbox_MemoryLayoutInit: complete\n");
    return TRUE;
}

void xbox_MemoryLayoutShutdown(void)
{
    /* Unmap demand-mapped CPU aliases before closing their backing store. */
    for (unsigned int i = 0; i < XBOX_CPU_ALIAS_BLOCK_COUNT; i++) {
        if (g_cpu_alias_views[i]) {
            UnmapViewOfFile(g_cpu_alias_views[i]);
            g_cpu_alias_views[i] = NULL;
        }
    }
    if (g_d3d_fence_memory) {
        VirtualFree(g_d3d_fence_memory, 0, MEM_RELEASE);
        g_d3d_fence_memory = NULL;
    }
    if (g_kernel_memory) {
        VirtualFree(g_kernel_memory, 0, MEM_RELEASE);
        g_kernel_memory = NULL;
    }
    /* Unmap mirror views first */
    for (int m = 0; m < XBOX_NUM_MIRRORS; m++) {
        if (g_mirror_views[m]) {
            UnmapViewOfFile(g_mirror_views[m]);
            g_mirror_views[m] = NULL;
        }
    }
    /* Unmap base view */
    if (g_memory_base) {
        UnmapViewOfFile(g_memory_base);
        g_memory_base = NULL;
        g_memory_size = 0;
    }
    /* Close file mapping handle */
    if (g_mapping_handle) {
        CloseHandle(g_mapping_handle);
        g_mapping_handle = NULL;
    }
    fprintf(stderr, "xbox_MemoryLayoutShutdown: released\n");
}

BOOL xbox_IsXboxAddress(uintptr_t address)
{
    return (address >= XBOX_BASE_ADDRESS &&
            address < XBOX_BASE_ADDRESS + g_memory_size);
}

void *xbox_GetMemoryBase(void)
{
    return g_memory_base;
}

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return g_memory_offset;
}

/* -- Dynamic heap allocator --------------------------------
 *
 * Monotonic Xbox-VA arena with tracked block reuse. All returned addresses
 * remain inside the mapped 64 MiB guest RAM region so MEM32() is valid.
 */
#define XBOX_HEAP_MAX_ALLOCS 16384

enum {
    XBOX_HEAP_CLASS_NORMAL = 0,
    XBOX_HEAP_CLASS_PAGE_BACKED = 1
};

typedef struct xbox_heap_allocation {
    uint32_t xbox_va;
    uint32_t requested_size;
    uint32_t allocation_size;
    uint32_t owner_ring[8];
    uint8_t in_use;
    uint8_t allocation_class;
} xbox_heap_allocation;

/* Xbox pool/system allocations and physically contiguous allocations use
 * different kernel allocators even though they ultimately share 64 MiB of
 * RAM. Grow their virgin ranges toward each other so small pool churn cannot
 * strand the large aligned extents needed by render targets. */
static uint32_t g_heap_next = XBOX_HEAP_BASE;
static uint32_t g_heap_page_next = XBOX_HEAP_BASE + XBOX_HEAP_SIZE;
static int g_heap_alloc_count = 0;
static int g_heap_free_count = 0;
static uint32_t g_heap_oom_count = 0u;
static xbox_heap_allocation g_heap_allocations[XBOX_HEAP_MAX_ALLOCS];

static void xbox_HeapCaptureOwner(xbox_heap_allocation *allocation)
{
    const uint32_t end = g_recomp_recent_game_func_idx;
    uint32_t i;

    for (i = 0u; i < 8u; ++i)
        allocation->owner_ring[i] =
            end > i ? g_recomp_recent_game_funcs[(end - 1u - i) & 255u] : 0u;
}

static int xbox_HeapTraceEnabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        enabled = getenv("XBOXRECOMP_TRACE_HEAP") != NULL ||
                  getenv("MERCENARIES_TRACE_HEAP") != NULL;
        initialized = 1;
    }
    return enabled;
}

static int xbox_HeapCensusEnabled(void)
{
    static int initialized;
    static int enabled;
    if (!initialized) {
        enabled = getenv("MERCENARIES_TRACE_HEAP_CENSUS") != NULL;
        initialized = 1;
    }
    return enabled;
}

static void xbox_HeapDumpCensus(void)
{
    static int dumped;
    const xbox_heap_allocation *top[96] = {0};
    uint64_t live_bytes = 0u;
    int live_count = 0;
    int top_count = 0;
    int i;

    if (dumped || !xbox_HeapCensusEnabled())
        return;
    dumped = 1;

    for (i = 0; i < g_heap_alloc_count; ++i) {
        const xbox_heap_allocation *allocation = &g_heap_allocations[i];
        int pos;

        if (!allocation->in_use || allocation->allocation_size == 0u)
            continue;
        live_bytes += allocation->allocation_size;
        ++live_count;

        if (top_count < 96) {
            pos = top_count++;
        } else {
            if (allocation->allocation_size <= top[95]->allocation_size)
                continue;
            pos = 95;
        }
        while (pos > 0 &&
               top[pos - 1]->allocation_size < allocation->allocation_size) {
            top[pos] = top[pos - 1];
            --pos;
        }
        top[pos] = allocation;
    }

    fprintf(stderr,
            "[HEAP-CENSUS] live=%d bytes=%llu tracked=%d top=%d\n",
            live_count, (unsigned long long)live_bytes,
            g_heap_alloc_count, top_count);
    for (i = 0; i < top_count; ++i) {
        const xbox_heap_allocation *allocation = top[i];
        uint32_t ring;

        fprintf(stderr,
                "[HEAP-CENSUS] #%d va=%08X size=%u requested=%u owner=",
                i, allocation->xbox_va, allocation->allocation_size,
                allocation->requested_size);
        for (ring = 0u; ring < 8u; ++ring)
            fprintf(stderr, "%s%08X", ring != 0u ? "," : "",
                    allocation->owner_ring[ring]);
        fputc('\n', stderr);
    }
    fflush(stderr);
}
static xbox_heap_allocation *xbox_HeapAppendAllocation(
    uint32_t xbox_va, uint32_t requested_size, uint32_t allocation_size,
    uint8_t in_use, uint8_t allocation_class)
{
    xbox_heap_allocation *allocation;
    int i;

    if (allocation_size == 0u)
        return NULL;

    allocation = NULL;
    for (i = 0; i < g_heap_alloc_count; ++i) {
        if (g_heap_allocations[i].allocation_size == 0u) {
            allocation = &g_heap_allocations[i];
            break;
        }
    }
    if (allocation == NULL) {
        if (g_heap_alloc_count >= XBOX_HEAP_MAX_ALLOCS)
            return NULL;
        allocation = &g_heap_allocations[g_heap_alloc_count++];
    }
    allocation->xbox_va = xbox_va;
    allocation->requested_size = requested_size;
    allocation->allocation_size = allocation_size;
    allocation->in_use = in_use;
    allocation->allocation_class = allocation_class;
    if (in_use)
        xbox_HeapCaptureOwner(allocation);
    else {
        memset(allocation->owner_ring, 0, sizeof(allocation->owner_ring));
        ++g_heap_free_count;
    }
    return allocation;
}

static int xbox_HeapAvailableRecordSlots(void)
{
    int available = XBOX_HEAP_MAX_ALLOCS - g_heap_alloc_count;
    int i;

    for (i = 0; i < g_heap_alloc_count; ++i) {
        if (g_heap_allocations[i].allocation_size == 0u)
            ++available;
    }
    return available;
}
static void xbox_HeapCoalesceFree(xbox_heap_allocation *block)
{
    xbox_heap_allocation *left = NULL;
    xbox_heap_allocation *right = NULL;
    int i;

    if (block == NULL || block->in_use || block->allocation_size == 0u)
        return;

    /* All older free blocks are already coalesced, so a newly freed block can
     * have at most one free neighbour on either side. Find both in one pass
     * instead of repeatedly comparing every tracked allocation with every
     * other allocation. This path is hot once the streamed world is loaded. */
    for (i = 0; i < g_heap_alloc_count; ++i) {
        xbox_heap_allocation *candidate = &g_heap_allocations[i];

        if (candidate == block || candidate->in_use ||
            candidate->allocation_size == 0u)
            continue;
        if (candidate->xbox_va + candidate->allocation_size == block->xbox_va)
            left = candidate;
        else if (block->xbox_va + block->allocation_size == candidate->xbox_va)
            right = candidate;
    }

    if (left != NULL) {
        /* allocation_class is a reuse preference, not a physical boundary.
         * A last-resort allocation may consume part of a free extent from the
         * other class after the bump frontier is exhausted.  Once that
         * allocation is released, refusing to join the differently tagged
         * pieces permanently fragments what was one contiguous Xbox range.
         * Preserve the stricter page-backed preference on the merged extent
         * so ordinary pool traffic still cannot carve it up while frontier
         * space remains. */
        if (block->allocation_class == XBOX_HEAP_CLASS_PAGE_BACKED)
            left->allocation_class = XBOX_HEAP_CLASS_PAGE_BACKED;
        left->allocation_size += block->allocation_size;
        memset(block, 0, sizeof(*block));
        --g_heap_free_count;
        block = left;
    }
    if (right != NULL) {
        if (right->allocation_class == XBOX_HEAP_CLASS_PAGE_BACKED)
            block->allocation_class = XBOX_HEAP_CLASS_PAGE_BACKED;
        block->allocation_size += right->allocation_size;
        memset(right, 0, sizeof(*right));
        --g_heap_free_count;
    }
}

static void xbox_HeapTrimTail(void)
{
    int i;
    int trimmed;

    do {
        trimmed = 0;
        for (i = 0; i < g_heap_alloc_count; ++i) {
            xbox_heap_allocation *allocation = &g_heap_allocations[i];

            if (!allocation->in_use && allocation->allocation_size != 0u &&
                allocation->allocation_class == XBOX_HEAP_CLASS_NORMAL &&
                allocation->xbox_va + allocation->allocation_size == g_heap_next) {
                g_heap_next = allocation->xbox_va;
                allocation->xbox_va = 0u;
                allocation->requested_size = 0u;
                allocation->allocation_size = 0u;
                --g_heap_free_count;
                trimmed = 1;
                break;
            }
            if (!allocation->in_use && allocation->allocation_size != 0u &&
                allocation->allocation_class == XBOX_HEAP_CLASS_PAGE_BACKED &&
                allocation->xbox_va == g_heap_page_next) {
                g_heap_page_next += allocation->allocation_size;
                allocation->xbox_va = 0u;
                allocation->requested_size = 0u;
                allocation->allocation_size = 0u;
                --g_heap_free_count;
                trimmed = 1;
                break;
            }
        }
    } while (trimmed);
}

static void xbox_HeapReset(void)
{
    g_heap_next = XBOX_HEAP_BASE;
    g_heap_page_next = XBOX_HEAP_BASE + XBOX_HEAP_SIZE;
    g_heap_alloc_count = 0;
    g_heap_free_count = 0;
    g_heap_oom_count = 0u;
    memset(g_heap_allocations, 0, sizeof(g_heap_allocations));
}

static uint32_t xbox_HeapTryReuse(
    uint32_t size, uint32_t alignment, uint32_t requested_size,
    uint8_t allocation_class, int require_same_class, int exact_size_only)
{
    int best_index = -1;
    uint32_t best_waste = UINT32_MAX;
    uint32_t best_aligned_va = 0u;
    int i;

    for (i = 0; g_heap_free_count != 0 &&
         i < g_heap_alloc_count && i < XBOX_HEAP_MAX_ALLOCS; ++i) {
        xbox_heap_allocation *allocation = &g_heap_allocations[i];
        uint32_t block_end;
        uint32_t aligned_va;
        uint32_t waste;

        if (allocation->in_use || allocation->allocation_size == 0u)
            continue;
        if (require_same_class &&
            allocation->allocation_class != allocation_class)
            continue;
        block_end = allocation->xbox_va + allocation->allocation_size;
        aligned_va =
            (allocation->xbox_va + alignment - 1u) & ~(alignment - 1u);
        if (aligned_va < allocation->xbox_va || aligned_va > block_end ||
            size > block_end - aligned_va)
            continue;
        /* Coalescing can attach at most one alignment gap to an otherwise
         * exact released range. Treat that padding as an exact fit, but do not
         * split a materially larger page extent while frontier space remains. */
        if (exact_size_only && allocation->allocation_size - size >= alignment)
            continue;
        waste = allocation->allocation_size - size;
        if (waste < best_waste) {
            best_index = i;
            best_waste = waste;
            best_aligned_va = aligned_va;
        }
    }

    if (best_index >= 0) {
        xbox_heap_allocation *allocation = &g_heap_allocations[best_index];
        const uint32_t original_va = allocation->xbox_va;
        const uint32_t block_end =
            allocation->xbox_va + allocation->allocation_size;
        const uint32_t prefix_size = best_aligned_va - original_va;
        const uint32_t suffix_size = block_end - (best_aligned_va + size);
        const int extra_blocks =
            (prefix_size != 0u ? 1 : 0) + (suffix_size != 0u ? 1 : 0);
        const uint8_t free_class = allocation->allocation_class;

        if (extra_blocks <= xbox_HeapAvailableRecordSlots()) {
            allocation->xbox_va = best_aligned_va;
            allocation->requested_size = requested_size;
            allocation->allocation_size = size;
            allocation->in_use = 1u;
            allocation->allocation_class = allocation_class;
            --g_heap_free_count;
            xbox_HeapCaptureOwner(allocation);
            if (prefix_size != 0u)
                (void)xbox_HeapAppendAllocation(
                    original_va, 0u, prefix_size, 0u, free_class);
            if (suffix_size != 0u)
                (void)xbox_HeapAppendAllocation(
                    best_aligned_va + size, 0u, suffix_size, 0u, free_class);
            memset((void *)((uintptr_t)allocation->xbox_va + g_memory_offset),
                   0, allocation->allocation_size);
            if (xbox_HeapTraceEnabled()) {
                fprintf(stderr,
                        "  [HEAP] reuse: size=%u align=%u class=%u -> 0x%08X "
                        "(prefix=%u suffix=%u, guest=%08X)\n",
                        size, alignment, allocation_class, allocation->xbox_va,
                        prefix_size, suffix_size, g_recomp_current_func);
                fflush(stderr);
            }
            return allocation->xbox_va;
        }
    }

    return 0u;
}

static uint32_t xbox_HeapAllocClass(
    uint32_t size, uint32_t alignment, uint8_t allocation_class)
{
    uint32_t result;
    uint32_t requested_size = size;
    int i;

    if (alignment < 4u) alignment = 4u;

    /* Zero-byte Xbox allocations still need unique storage, but small pool
     * objects must not consume the 4 KiB minimum used by the old bump-only
     * workaround. Contiguous callers retain page alignment explicitly. */
    if (size < 4u) size = 4u;

    /* Keep released physical/page ranges available for later physical
     * allocations while the unused frontier can satisfy an ordinary pool
     * request (and vice versa). Xbox exposes these as distinct allocation
     * APIs; mixing their free extents lets small streamed objects carve up
     * render-target ranges across scene reloads. If the frontier is exhausted,
     * fall back to any suitable extent before reporting OOM. */
    result = xbox_HeapTryReuse(
        size, alignment, requested_size, allocation_class, 1,
        allocation_class == XBOX_HEAP_CLASS_PAGE_BACKED);
    if (result != 0u)
        return result;

    if (allocation_class == XBOX_HEAP_CLASS_PAGE_BACKED) {
        if (size > g_heap_page_next - XBOX_HEAP_BASE)
            result = 0u;
        else
            result = (g_heap_page_next - size) & ~(alignment - 1u);
        if (result < g_heap_next)
            result = 0u;
    } else {
        result = (g_heap_next + alignment - 1u) & ~(alignment - 1u);
        if (result > g_heap_page_next || size > g_heap_page_next - result)
            result = 0u;
    }

    if (result == 0u) {
        /* Once the frontier is exhausted, choose the globally smallest
         * suitable extent. Preferring class at this point can split a large
         * physical range even when a tighter ordinary hole exists, defeating
         * the segregation policy and increasing fragmentation. */
        result = xbox_HeapTryReuse(
            size, alignment, requested_size, allocation_class, 0, 0);
        if (result != 0u)
            return result;

        uint64_t reusable_bytes = 0;
        uint32_t largest_reusable = 0;
        int reusable_blocks = 0;
        int tracked_count = g_heap_alloc_count;

        if (tracked_count > XBOX_HEAP_MAX_ALLOCS)
            tracked_count = XBOX_HEAP_MAX_ALLOCS;
        for (i = 0; i < tracked_count; ++i) {
            const xbox_heap_allocation *allocation = &g_heap_allocations[i];
            if (!allocation->in_use) {
                reusable_bytes += allocation->allocation_size;
                if (allocation->allocation_size > largest_reusable)
                    largest_reusable = allocation->allocation_size;
                ++reusable_blocks;
            }
        }
        ++g_heap_oom_count;
        if (xbox_HeapTraceEnabled() || g_heap_oom_count <= 8u ||
            (g_heap_oom_count & (g_heap_oom_count - 1u)) == 0u) {
            const uint32_t recent_end = g_recomp_recent_game_func_idx;
            const uint32_t recent_count = recent_end < 256u ? recent_end : 256u;
            fprintf(stderr,
                    "xbox_HeapAlloc: out of memory #%u "
                    "(requested %u, used %u/%u, guest=%08X)\n",
                    g_heap_oom_count, size,
                    (g_heap_next - XBOX_HEAP_BASE) +
                        (XBOX_HEAP_BASE + XBOX_HEAP_SIZE - g_heap_page_next),
                    XBOX_HEAP_SIZE,
                    g_recomp_current_func);
            fprintf(stderr,
                    "  [HEAP] reusable=%llu bytes in %d blocks, largest=%u\n",
                    (unsigned long long)reusable_bytes, reusable_blocks,
                    largest_reusable);
            if (xbox_HeapTraceEnabled()) {
                fprintf(stderr, "  [HEAP] recent guest functions at OOM:");
                for (i = (int)recent_count; i > 0; --i)
                    fprintf(stderr, " %08X",
                            g_recomp_recent_game_funcs[
                                (recent_end - (uint32_t)i) & 255u]);
                fputc('\n', stderr);
            }
            fflush(stderr);
        }
        xbox_HeapDumpCensus();
        return 0;
    }
    if (allocation_class == XBOX_HEAP_CLASS_PAGE_BACKED) {
        const uint32_t old_page_next = g_heap_page_next;
        if (result + size < old_page_next)
            (void)xbox_HeapAppendAllocation(
                result + size, 0u, old_page_next - (result + size), 0u,
                XBOX_HEAP_CLASS_PAGE_BACKED);
        g_heap_page_next = result;
    } else {
        if (result > g_heap_next)
            (void)xbox_HeapAppendAllocation(
                g_heap_next, 0u, result - g_heap_next, 0u,
                XBOX_HEAP_CLASS_NORMAL);
        g_heap_next = result + size;
    }
    memset((void *)((uintptr_t)result + g_memory_offset), 0, size);

    (void)xbox_HeapAppendAllocation(
        result, requested_size, size, 1u, allocation_class);
    if (xbox_HeapTraceEnabled()) {
        fprintf(stderr, "  [HEAP] #%d: size=%u align=%u class=%u -> 0x%08X..0x%08X "
                "(used %u/%u, guest=%08X)\n",
                g_heap_alloc_count, size, alignment, allocation_class,
                result, result + size,
                (g_heap_next - XBOX_HEAP_BASE) +
                    (XBOX_HEAP_BASE + XBOX_HEAP_SIZE - g_heap_page_next),
                XBOX_HEAP_SIZE,
                g_recomp_current_func);
        fflush(stderr);
    }

    return result;
}

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment)
{
    return xbox_HeapAllocClass(
        size, alignment, XBOX_HEAP_CLASS_NORMAL);
}

uint32_t xbox_HeapAllocPageRounded(uint32_t size, uint32_t alignment)
{
    uint32_t allocation_size;

    if (size == 0u || size > UINT32_MAX - 4095u)
        return 0u;
    allocation_size = (size + 4095u) & ~4095u;
    return xbox_HeapAllocClass(
        allocation_size, alignment, XBOX_HEAP_CLASS_PAGE_BACKED);
}

static const xbox_heap_allocation *xbox_HeapFindAllocation(uint32_t xbox_va)
{
    int count = g_heap_alloc_count;
    int i;

    if (count > XBOX_HEAP_MAX_ALLOCS) count = XBOX_HEAP_MAX_ALLOCS;
    for (i = count - 1; i >= 0; --i) {
        if (g_heap_allocations[i].in_use &&
            g_heap_allocations[i].xbox_va == xbox_va)
            return &g_heap_allocations[i];
    }
    return NULL;
}

uint32_t xbox_HeapGetRequestedSize(uint32_t xbox_va)
{
    const xbox_heap_allocation *allocation =
        xbox_HeapFindAllocation(xbox_va);
    return allocation ? allocation->requested_size : 0;
}

uint32_t xbox_HeapGetAllocationSize(uint32_t xbox_va)
{
    const xbox_heap_allocation *allocation =
        xbox_HeapFindAllocation(xbox_va);
    return allocation ? allocation->allocation_size : 0;
}

void xbox_HeapFree(uint32_t xbox_va)
{
    int count = g_heap_alloc_count;
    int i;

    /* KSEG0/KSEG1 pointers name the same 64 MiB physical pages as low RAM
     * VAs.  Normalize the full direct-mapped CPU aperture, not only its
     * first 64 MiB repetition: retail pointers such as 0x852D8804 are valid
     * aliases of 0x012D8804. */
    if (xbox_va >= XBOX_CPU_ALIAS_BASE && xbox_va < XBOX_CPU_ALIAS_END)
        xbox_va &= xbox_GetGraphicsMemorySize() - 1u;

    if (count > XBOX_HEAP_MAX_ALLOCS) count = XBOX_HEAP_MAX_ALLOCS;
    for (i = count - 1; i >= 0; --i) {
        if (g_heap_allocations[i].in_use &&
            g_heap_allocations[i].xbox_va == xbox_va) {
            g_heap_allocations[i].in_use = 0u;
            g_heap_allocations[i].requested_size = 0u;
            ++g_heap_free_count;
            if (xbox_HeapTraceEnabled()) {
                fprintf(stderr,
                        "  [HEAP] free: 0x%08X size=%u (slot=%d)\n",
                        xbox_va, g_heap_allocations[i].allocation_size, i);
                fflush(stderr);
            }
            xbox_HeapCoalesceFree(&g_heap_allocations[i]);
            xbox_HeapTrimTail();
            return;
        }
    }
    fprintf(stderr, "  [HEAP] free ignored: untracked 0x%08X\n", xbox_va);
    fflush(stderr);
}
HANDLE xbox_GetMappingHandle(void)
{
    return g_mapping_handle;
}

BOOL xbox_MapPhysicalAlias(uint32_t xbox_va)
{
    uint32_t physical_base;
    uint32_t alias_base;
    unsigned int block_index;
    uintptr_t native_base;
    void *view;

    if (!g_mapping_handle || xbox_va < XBOX_CPU_ALIAS_BASE ||
        xbox_va >= XBOX_CPU_ALIAS_END) {
        return FALSE;
    }

    physical_base = (xbox_va & (xbox_GetGraphicsMemorySize() - 1u)) &
                    ~(XBOX_ALIAS_BLOCK_SIZE - 1u);
    alias_base = xbox_va & ~(XBOX_ALIAS_BLOCK_SIZE - 1u);
    block_index = (alias_base - XBOX_CPU_ALIAS_BASE) / XBOX_ALIAS_BLOCK_SIZE;

    if (g_cpu_alias_views[block_index]) {
        return TRUE;
    }

    native_base = (uintptr_t)alias_base + (uintptr_t)g_memory_offset;
    view = MapViewOfFileEx(
        g_mapping_handle,
        FILE_MAP_ALL_ACCESS,
        0,
        physical_base,
        XBOX_ALIAS_BLOCK_SIZE,
        (LPVOID)native_base);
    if (view != (void *)native_base) {
        DWORD error = GetLastError();
        if (view) {
            UnmapViewOfFile(view);
        }
        fprintf(stderr,
                "  KSEG0: failed to alias Xbox VA 0x%08X to physical "
                "0x%08X (error %lu)\n",
                alias_base, physical_base, error);
        return FALSE;
    }

    g_cpu_alias_views[block_index] = view;
    if (xbox_HeapTraceEnabled()) {
        fprintf(stderr,
                "  CPU alias: Xbox VA 0x%08X aliases physical 0x%08X "
                "(native %p)\n",
                alias_base, physical_base, view);
    }
    return TRUE;
}

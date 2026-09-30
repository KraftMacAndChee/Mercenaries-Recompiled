/*
 * NV2A GPU Core - Standalone register handlers
 *
 * Adapted from xemu (Copyright (c) 2012 espes, 2015 Jannik Vogel,
 * 2018-2025 Matt Borgerson) - LGPL v2+
 *
 * Contains: PMC, PBUS, PTIMER, PFB, PCRTC, PRAMDAC register handlers,
 * nv2a_update_irq, DMA helpers, block dispatch table, and standalone init.
 */

#include "nv2a_state.h"
#include "nv2a_pgraph_d3d11.h"

/* ============================================================
 * Global state
 * ============================================================ */

static NV2AState *g_nv2a = NULL;
static MemoryRegion g_vram_region;
static MemoryRegion g_ramin_region;

volatile uint32_t g_nv2a_pfifo_diag_submission;
volatile uint32_t g_nv2a_pfifo_diag_get;
volatile uint32_t g_nv2a_pfifo_diag_put;
volatile uint32_t g_nv2a_pfifo_diag_word;
volatile uint32_t g_nv2a_pfifo_diag_method;
volatile uint32_t g_nv2a_pfifo_diag_subchannel;
volatile uint32_t g_nv2a_pfifo_diag_stage;

NV2AState *nv2a_get_state(void) {
    return g_nv2a;
}

void nv2a_set_scanout_framebuffer(uint32_t framebuffer)
{
    if (g_nv2a)
        g_nv2a->pcrtc.start = framebuffer & 0x07FFFFFFu;
}

/* ============================================================
 * IRQ aggregation (from xemu nv2a.c)
 * ============================================================ */

void nv2a_update_irq(NV2AState *d)
{
    /* PFIFO */
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;
    }

    /* PCRTC */
    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;
    }

    /* PGRAPH */
    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;
    }

    if (d->pmc.pending_interrupts && d->pmc.enabled_interrupts) {
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

/* ============================================================
 * DMA helpers (from xemu nv2a.c)
 * ============================================================ */

DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);
    dma.address &= 0x07FFFFFF;

    if (dma.address >= memory_region_size(d->vram)) {
        fprintf(stderr, "[NV2A] DMA map address 0x%llx out of VRAM range\n",
                (unsigned long long)dma.address);
        *len = 0;
        return NULL;
    }

    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

typedef struct RAMHTEntry {
    uint32_t instance;
    uint8_t valid;
} RAMHTEntry;

static uint32_t pfifo_ramht_hash(NV2AState *d, uint32_t handle)
{
    const uint32_t ramht_size =
        1u << (GET_MASK(d->pfifo.regs[NV_PFIFO_RAMHT],
                        NV_PFIFO_RAMHT_SIZE) + 12u);
    const unsigned int bits = ctz32(ramht_size) - 1u;
    const uint32_t chunk_mask = (1u << bits) - 1u;
    uint32_t hash = 0;

    while (handle) {
        hash ^= handle & chunk_mask;
        handle >>= bits;
    }
    hash ^= GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_PUSH1],
                     NV_PFIFO_CACHE1_PUSH1_CHID) << (bits - 4u);
    return hash;
}

static RAMHTEntry pfifo_ramht_lookup(NV2AState *d, uint32_t handle)
{
    const uint32_t ramht_size =
        1u << (GET_MASK(d->pfifo.regs[NV_PFIFO_RAMHT],
                        NV_PFIFO_RAMHT_SIZE) + 12u);
    const uint32_t ramht_address =
        GET_MASK(d->pfifo.regs[NV_PFIFO_RAMHT],
                 NV_PFIFO_RAMHT_BASE_ADDRESS) << 12;
    const uint32_t hash = pfifo_ramht_hash(d, handle);
    const uint32_t entry_address = ramht_address + hash * 8u;
    RAMHTEntry result = { 0, 0 };
    static uint32_t lookup_samples;

    if (hash * 8u >= ramht_size ||
        entry_address + 8u > memory_region_size(&d->ramin)) {
        return result;
    }

    const uint8_t *entry = d->ramin_ptr + entry_address;
    const uint32_t entry_handle = ldl_le_p((const uint32_t *)entry);
    const uint32_t entry_context =
        ldl_le_p((const uint32_t *)(entry + 4u));

    result.instance = (entry_context & NV_RAMHT_INSTANCE) << 4;
    result.valid = (entry_handle == handle) &&
                   ((entry_context & NV_RAMHT_STATUS) != 0) &&
                   (result.instance + 4u <= memory_region_size(&d->ramin));
    if (lookup_samples < 16u) {
        fprintf(stderr,
                "[RAMHT] handle=%08X cfg=%08X hash=%08X addr=%08X "
                "entry_handle=%08X context=%08X instance=%08X valid=%u\n",
                handle, d->pfifo.regs[NV_PFIFO_RAMHT], hash, entry_address,
                entry_handle, entry_context, result.instance, result.valid);
        ++lookup_samples;
    }
    return result;
}

/* ============================================================
 * PMC - card master control (from xemu pmc.c)
 * ============================================================ */

uint64_t pmc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PMC_BOOT_0:
        /* NV2A, A03, Rev 0 */
        r = 0x02A000A3;
        break;
    case NV_PMC_INTR_0:
        r = d->pmc.pending_interrupts;
        break;
    case NV_PMC_INTR_EN_0:
        r = d->pmc.enabled_interrupts;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PMC, addr, size, r);
    return r;
}

void pmc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PMC, addr, size, val);

    switch (addr) {
    case NV_PMC_INTR_0:
        d->pmc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PMC_INTR_EN_0:
        d->pmc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PBUS - bus control (from xemu pbus.c)
 * ============================================================ */

uint64_t pbus_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    PCIDevice *d = PCI_DEVICE(s);

    uint64_t r = 0;
    switch (addr) {
    case NV_PBUS_PCI_NV_0:
        r = pci_get_long(d->config + PCI_VENDOR_ID);
        break;
    case NV_PBUS_PCI_NV_1:
        r = pci_get_long(d->config + PCI_COMMAND);
        break;
    case NV_PBUS_PCI_NV_2:
        r = pci_get_long(d->config + PCI_CLASS_REVISION);
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PBUS, addr, size, r);
    return r;
}

void pbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *s = (NV2AState *)opaque;
    PCIDevice *d = PCI_DEVICE(s);

    nv2a_reg_log_write(NV_PBUS, addr, size, val);

    switch (addr) {
    case NV_PBUS_PCI_NV_1:
        pci_set_long(d->config + PCI_COMMAND, val);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PTIMER - time measurement (from xemu ptimer.c)
 * ============================================================ */

static uint64_t ptimer_get_clock(NV2AState *d)
{
    if (d->ptimer.numerator == 0) return 0;
    return muldiv64(muldiv64(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                             d->pramdac.core_clock_freq,
                             NANOSECONDS_PER_SECOND),
                    d->ptimer.denominator,
                    d->ptimer.numerator);
}

uint64_t ptimer_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PTIMER_INTR_0:
        r = d->ptimer.pending_interrupts;
        break;
    case NV_PTIMER_INTR_EN_0:
        r = d->ptimer.enabled_interrupts;
        break;
    case NV_PTIMER_NUMERATOR:
        r = d->ptimer.numerator;
        break;
    case NV_PTIMER_DENOMINATOR:
        r = d->ptimer.denominator;
        break;
    case NV_PTIMER_TIME_0:
        r = (ptimer_get_clock(d) & 0x7ffffff) << 5;
        break;
    case NV_PTIMER_TIME_1:
        r = (ptimer_get_clock(d) >> 27) & 0x1fffffff;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PTIMER, addr, size, r);
    return r;
}

void ptimer_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PTIMER, addr, size, val);

    switch (addr) {
    case NV_PTIMER_INTR_0:
        d->ptimer.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_INTR_EN_0:
        d->ptimer.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PTIMER_DENOMINATOR:
        d->ptimer.denominator = val;
        break;
    case NV_PTIMER_NUMERATOR:
        d->ptimer.numerator = val;
        break;
    case NV_PTIMER_ALARM_0:
        d->ptimer.alarm_time = val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * PFB - framebuffer / memory control (from xemu pfb.c)
 * ============================================================ */

uint64_t pfb_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFB_CSTATUS:
        r = memory_region_size(d->vram);
        break;
    case NV_PFB_WBC:
        r = 0; /* Flush not pending */
        break;
    default:
        r = d->pfb.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFB, addr, size, r);
    return r;
}

void pfb_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFB, addr, size, val);

    switch (addr) {
    default:
        d->pfb.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * PCRTC - CRT controller (from xemu pcrtc.c)
 * ============================================================ */

uint64_t pcrtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PCRTC_INTR_0:
        r = d->pcrtc.pending_interrupts;
        break;
    case NV_PCRTC_INTR_EN_0:
        r = d->pcrtc.enabled_interrupts;
        break;
    case NV_PCRTC_START:
        r = d->pcrtc.start;
        break;
    case NV_PCRTC_RASTER:
        r = d->pcrtc.raster++;
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PCRTC, addr, size, r);
    return r;
}

void pcrtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PCRTC, addr, size, val);

    switch (addr) {
    case NV_PCRTC_INTR_0:
        d->pcrtc.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_INTR_EN_0:
        d->pcrtc.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    case NV_PCRTC_START:
        val &= 0x07FFFFFF;
        d->pcrtc.start = val;
        NV2A_DPRINTF("PCRTC_START - %x %x %x %x\n",
                d->vram_ptr[val+64], d->vram_ptr[val+64+1],
                d->vram_ptr[val+64+2], d->vram_ptr[val+64+3]);
        break;
    default:
        break;
    }
}

/* ============================================================
 * PRAMDAC - RAMDAC / PLL control (from xemu pramdac.c)
 * ============================================================ */

uint64_t pramdac_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr & ~3) {
    case NV_PRAMDAC_NVPLL_COEFF:
        r = d->pramdac.core_clock_coeff;
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        r = d->pramdac.memory_clock_coeff;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        r = d->pramdac.video_clock_coeff;
        break;
    case NV_PRAMDAC_PLL_TEST_COUNTER:
        /* emulated PLLs locked instantly */
        r = NV_PRAMDAC_PLL_TEST_COUNTER_VPLL2_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_NVPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_MPLL_LOCK
             | NV_PRAMDAC_PLL_TEST_COUNTER_VPLL_LOCK;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        r = d->pramdac.general_control;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        r = d->pramdac.fp_vdisplay_end;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        r = d->pramdac.fp_vcrtc;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        r = d->pramdac.fp_vsync_end;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        r = d->pramdac.fp_vvalid_end;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        r = d->pramdac.fp_hdisplay_end;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        r = d->pramdac.fp_hcrtc;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        r = d->pramdac.fp_hvalid_end;
        break;
    default:
        break;
    }

    /* Handle unaligned access */
    r >>= 32 - 8 * size - 8 * (addr & 3);

    nv2a_reg_log_read(NV_PRAMDAC, addr, size, r);
    return r;
}

void pramdac_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint32_t m, n, p;

    nv2a_reg_log_write(NV_PRAMDAC, addr, size, val);

    switch (addr) {
    case NV_PRAMDAC_NVPLL_COEFF:
        d->pramdac.core_clock_coeff = val;

        m = val & NV_PRAMDAC_NVPLL_COEFF_MDIV;
        n = (val & NV_PRAMDAC_NVPLL_COEFF_NDIV) >> 8;
        p = (val & NV_PRAMDAC_NVPLL_COEFF_PDIV) >> 16;

        if (m == 0) {
            d->pramdac.core_clock_freq = 0;
        } else {
            d->pramdac.core_clock_freq = (NV2A_CRYSTAL_FREQ * n)
                                          / (1 << p) / m;
        }
        break;
    case NV_PRAMDAC_MPLL_COEFF:
        d->pramdac.memory_clock_coeff = val;
        break;
    case NV_PRAMDAC_VPLL_COEFF:
        d->pramdac.video_clock_coeff = val;
        break;
    case NV_PRAMDAC_GENERAL_CONTROL:
        d->pramdac.general_control = val;
        break;
    case NV_PRAMDAC_FP_VDISPLAY_END:
        d->pramdac.fp_vdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_VCRTC:
        d->pramdac.fp_vcrtc = val;
        break;
    case NV_PRAMDAC_FP_VSYNC_END:
        d->pramdac.fp_vsync_end = val;
        break;
    case NV_PRAMDAC_FP_VVALID_END:
        d->pramdac.fp_vvalid_end = val;
        break;
    case NV_PRAMDAC_FP_HDISPLAY_END:
        d->pramdac.fp_hdisplay_end = val;
        break;
    case NV_PRAMDAC_FP_HCRTC:
        d->pramdac.fp_hcrtc = val;
        break;
    case NV_PRAMDAC_FP_HVALID_END:
        d->pramdac.fp_hvalid_end = val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * PVIDEO - video overlay (stub)
 * ============================================================ */

uint64_t pvideo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r;

    if (addr == NV_PVIDEO_STOP) {
        r = 0;
    } else {
        r = d->pvideo.regs[addr];
    }
    nv2a_reg_log_read(NV_PVIDEO, addr, size, r);
    return r;
}

void pvideo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    static uint32_t trace_count;

    nv2a_reg_log_write(NV_PVIDEO, addr, size, val);
    if (getenv("MERCENARIES_TRACE_PVIDEO") != NULL && trace_count < 256u) {
        fprintf(stderr, "[PVIDEO-MMIO] addr=%04llX value=%08llX size=%u\n",
                (unsigned long long)addr, (unsigned long long)val, size);
        ++trace_count;
    }
    switch (addr) {
    case NV_PVIDEO_BUFFER:
        d->pvideo.regs[addr] = val;
        break;
    case NV_PVIDEO_STOP:
        if (val & 1u) {
            d->pvideo.regs[NV_PVIDEO_BUFFER] = 0;
        }
        break;
    default:
        d->pvideo.regs[addr] = val;
        break;
    }
}

/* ============================================================
 * PGRAPH - graphics register interface
 * ============================================================ */

uint64_t pgraph_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = d->pgraph.regs[addr];
    nv2a_reg_log_read(NV_PGRAPH, addr, size, r);
    return r;
}

void pgraph_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PGRAPH, addr, size, val);

    /* NV_PGRAPH_INTR is write-one-to-clear. XDK interrupt handlers write the
     * pending-bit mask back to this register; storing that mask makes the same
     * interrupt appear permanently pending and recursively re-enters the
     * handler until the host stack overflows. */
    if (addr == NV_PGRAPH_INTR)
        d->pgraph.regs[addr] &= ~(uint32_t)val;
    else
        d->pgraph.regs[addr] = (uint32_t)val;
}

/* ============================================================
 * PGRAPH method dispatch
 * Called when push buffer commands are parsed.
 * Updates method state and dispatches rendering to the host backend.
 * ============================================================ */

static uint32_t g_pgraph_method_count = 0;
static uint32_t g_pgraph_draw_count = 0;
static uint32_t g_pgraph_clear_count = 0;
static uint32_t g_pgraph_flip_count = 0;
static uint32_t g_pgraph_inline_verts = 0;
static int g_pgraph_in_begin = 0;
static uint32_t g_pgraph_trace_count = 0;
static uint32_t g_pgraph_draw_trace_count = 0;
static uint16_t g_pgraph_unhandled_seen[128];
static uint32_t g_pgraph_unhandled_seen_count = 0;
static int g_pgraph_trace_enabled = -1;
static DWORD g_pgraph_trace_start_tick;
static DWORD g_pgraph_trace_delay_ms;

/* NV097 method constants for dispatch */
#define M_NO_OPERATION          0x0100
#define M_SET_SURFACE_FORMAT    0x0208
#define M_SET_SURFACE_PITCH     0x020C
#define M_SET_SURFACE_COLOR_OFF 0x0210
#define M_SET_SURFACE_ZETA_OFF  0x0214
#define M_SET_SURFACE_CLIP_H    0x0200
#define M_SET_SURFACE_CLIP_V    0x0204
#define M_CLEAR_SURFACE         0x01D0
#define M_SET_COLOR_CLEAR_VALUE 0x01D4
#define M_SET_BEGIN_END         0x17FC
#define M_INLINE_ARRAY          0x1818
#define M_FLIP_INCREMENT_WRITE  NV097_FLIP_INCREMENT_WRITE
#define M_FLIP_STALL            NV097_FLIP_STALL
#define M_SET_VIEWPORT_OFFSET   0x0A20
#define M_SET_VIEWPORT_SCALE    0x0AF0

void pgraph_method(NV2AState *d, uint32_t subchannel,
                   uint32_t method, uint32_t param)
{
    uint8_t graphics_class = 0;
    int method_handled = 0;
    int trace_active;

    if (subchannel < 8u) {
        RAMHTEntry entry;
        graphics_class = d->pgraph.subchannel_class[subchannel];

        if (method == NV097_SET_OBJECT) {
            entry = pfifo_ramht_lookup(d, param);
            if (entry.valid &&
                entry.instance + 16u <= memory_region_size(&d->ramin)) {
                const uint8_t *obj = d->ramin_ptr + entry.instance;

                /* Match xemu's SET_OBJECT path: load the object's five-word
                 * PGRAPH context into the selected subchannel cache. */
                d->pgraph.regs[NV_PGRAPH_CTX_CACHE1 + subchannel * 4u] =
                    ldl_le_p((const uint32_t *)(obj + 0u));
                d->pgraph.regs[NV_PGRAPH_CTX_CACHE2 + subchannel * 4u] =
                    ldl_le_p((const uint32_t *)(obj + 4u));
                d->pgraph.regs[NV_PGRAPH_CTX_CACHE3 + subchannel * 4u] =
                    ldl_le_p((const uint32_t *)(obj + 8u));
                d->pgraph.regs[NV_PGRAPH_CTX_CACHE4 + subchannel * 4u] =
                    ldl_le_p((const uint32_t *)(obj + 12u));
                d->pgraph.regs[NV_PGRAPH_CTX_CACHE5 + subchannel * 4u] =
                    entry.instance;
                d->pgraph.subchannel_object[subchannel] = entry.instance;
                param = entry.instance;
                graphics_class = (uint8_t)(
                    d->pgraph.regs[NV_PGRAPH_CTX_CACHE1 + subchannel * 4u] &
                    NV_PGRAPH_CTX_SWITCH1_GRCLASS);
                d->pgraph.subchannel_class[subchannel] = graphics_class;
                method_handled = 1;
            }
        } else if (method >= 0x180u && method < 0x200u) {
            /* xemu resolves methods in this range through RAMHT in PFIFO
             * before handing the object instance to PGRAPH. */
            entry = pfifo_ramht_lookup(d, param);
            if (entry.valid) {
                param = entry.instance;
                d->pgraph.subchannel_context[subchannel]
                    [(method - 0x180u) / 4u] = param;
                method_handled = 1;
            }
        }

        /* Select this subchannel's cached context for every method, matching
         * xemu's pgraph_method context-switch sequence. */
        d->pgraph.regs[NV_PGRAPH_CTX_SWITCH1] =
            d->pgraph.regs[NV_PGRAPH_CTX_CACHE1 + subchannel * 4u];
        d->pgraph.regs[NV_PGRAPH_CTX_SWITCH2] =
            d->pgraph.regs[NV_PGRAPH_CTX_CACHE2 + subchannel * 4u];
        d->pgraph.regs[NV_PGRAPH_CTX_SWITCH3] =
            d->pgraph.regs[NV_PGRAPH_CTX_CACHE3 + subchannel * 4u];
        d->pgraph.regs[NV_PGRAPH_CTX_SWITCH4] =
            d->pgraph.regs[NV_PGRAPH_CTX_CACHE4 + subchannel * 4u];
        d->pgraph.regs[NV_PGRAPH_CTX_SWITCH5] =
            d->pgraph.regs[NV_PGRAPH_CTX_CACHE5 + subchannel * 4u];
        graphics_class = (uint8_t)(
            d->pgraph.regs[NV_PGRAPH_CTX_SWITCH1] &
            NV_PGRAPH_CTX_SWITCH1_GRCLASS);

        if (graphics_class == NV_CONTEXT_SURFACES_2D) {
            if (method == NV062_SET_OBJECT) {
                d->pgraph.context_surfaces_2d_object = param;
                method_handled = 1;
            } else if (method == NV062_SET_COLOR_FORMAT) {
                d->pgraph.context_surfaces_2d_color_format = param;
                method_handled = 1;
            } else if (method == NV062_SET_PITCH) {
                d->pgraph.context_surfaces_2d_source_pitch = param & 0xFFFFu;
                d->pgraph.context_surfaces_2d_dest_pitch = param >> 16;
                method_handled = 1;
            } else if (method == NV062_SET_OFFSET_SOURCE) {
                d->pgraph.context_surfaces_2d_source_offset = param & 0x07FFFFFFu;
                method_handled = 1;
            } else if (method == NV062_SET_OFFSET_DESTIN) {
                d->pgraph.context_surfaces_2d_dest_offset = param & 0x07FFFFFFu;
                method_handled = 1;
            } else if (method == NV062_SET_CONTEXT_DMA_IMAGE_SOURCE) {
                d->pgraph.context_surfaces_2d_dma_source = param;
                method_handled = 1;
            } else if (method == NV062_SET_CONTEXT_DMA_IMAGE_DESTIN) {
                d->pgraph.context_surfaces_2d_dma_dest = param;
                method_handled = 1;
            }
        } else if (graphics_class == NV_IMAGE_BLIT) {
            if (method == NV09F_SET_CONTEXT_SURFACES) {
                d->pgraph.image_blit_context_surfaces = param;
                method_handled = 1;
            } else if (method == NV09F_SET_OPERATION) {
                d->pgraph.image_blit_operation = param;
                method_handled = 1;
            } else if (method == NV09F_CONTROL_POINT_IN) {
                d->pgraph.image_blit_point_in = param;
                method_handled = 1;
            } else if (method == NV09F_CONTROL_POINT_OUT) {
                d->pgraph.image_blit_point_out = param;
                method_handled = 1;
            } else if (method == NV09F_SIZE) {
                d->pgraph.image_blit_size = param;
                /* NV09F SIZE submits the copy, independently of Kelvin draws. */
                pgraph_d3d11_image_blit();
                method_handled = 1;
            }
        }

        if (graphics_class == NV_CONTEXT_PATTERN &&
            method == NV044_SET_MONOCHROME_COLOR0) {
            d->pgraph.regs[NV_PGRAPH_PATT_COLOR0] = param;
            method_handled = 1;
        }

        if (graphics_class == NV_KELVIN_PRIMITIVE) {
            if (method == NV097_SET_CONTEXT_DMA_SEMAPHORE) {
                d->pgraph.dma_semaphore = param;
                method_handled = 1;
            } else if (method == NV097_SET_SEMAPHORE_OFFSET) {
                d->pgraph.regs[NV_PGRAPH_SEMAPHOREOFFSET] = param;
                method_handled = 1;
            } else if (method == NV097_BACK_END_WRITE_SEMAPHORE_RELEASE) {
                hwaddr semaphore_len = 0;
                uint8_t *semaphore = (uint8_t *)nv_dma_map(
                    d, d->pgraph.dma_semaphore, &semaphore_len);
                const uint32_t semaphore_offset =
                    d->pgraph.regs[NV_PGRAPH_SEMAPHOREOFFSET];

                if (semaphore && semaphore_offset + 4u <= semaphore_len) {
                    stl_le_p((uint32_t *)(semaphore + semaphore_offset), param);
                }
                method_handled = 1;
            }
        }
    }
    g_pgraph_method_count++;

    if (g_pgraph_trace_enabled < 0) {
        const char *trace = getenv("MERCENARIES_TRACE_PGRAPH" );
        const char *delay = getenv("MERCENARIES_TRACE_PGRAPH_DELAY_MS");
        g_pgraph_trace_enabled = trace && trace[0] && trace[0] != '0';
        g_pgraph_trace_delay_ms = delay ? (DWORD)strtoul(delay, NULL, 0) : 0u;
        g_pgraph_trace_start_tick = GetTickCount();
    }
    trace_active = g_pgraph_trace_enabled &&
        GetTickCount() - g_pgraph_trace_start_tick >= g_pgraph_trace_delay_ms;
    if (trace_active && g_pgraph_trace_count < 1200u) {
        fprintf(stderr,
                "[PGRAPH-TRACE] #%u sub=%u class=0x%02X method=0x%04X param=0x%08X\n",
                g_pgraph_method_count, subchannel, graphics_class, method, param);
        ++g_pgraph_trace_count;
    }
    if (trace_active && g_pgraph_draw_trace_count < 400u &&
        (method == NV097_SET_BEGIN_END ||
         method == NV097_ARRAY_ELEMENT16 ||
         method == NV097_ARRAY_ELEMENT32 ||
         method == NV097_DRAW_ARRAYS ||
         method == NV097_INLINE_ARRAY ||
         method == NV097_FLIP_INCREMENT_WRITE ||
         method == NV097_FLIP_STALL)) {
        fprintf(stderr,
                "[PGRAPH-DRAW-TRACE] #%u sub=%u class=0x%02X method=0x%04X param=0x%08X\n",
                g_pgraph_method_count, subchannel, graphics_class, method, param);
        ++g_pgraph_draw_trace_count;
    }

    /* Route Kelvin methods through the D3D11 translator after PFIFO/PGRAPH
     * object resolution, just as xemu dispatches after RAMHT lookup. */
    if (graphics_class == NV_KELVIN_PRIMITIVE &&
        pgraph_d3d11_method(subchannel, method, param)) {
        return;
    }

    if (method_handled) {
        return;
    }

    if (trace_active) {
        uint32_t i;
        int seen = 0;
        for (i = 0; i < g_pgraph_unhandled_seen_count; ++i) {
            if (g_pgraph_unhandled_seen[i] == method) {
                seen = 1;
                break;
            }
        }
        if (!seen && g_pgraph_unhandled_seen_count < 128u) {
            g_pgraph_unhandled_seen[g_pgraph_unhandled_seen_count++] =
                (uint16_t)method;
            fprintf(stderr,
                    "[PGRAPH-UNHANDLED-UNIQUE] #%u sub=%u class=0x%02X "
                    "method=0x%04X param=0x%08X\n",
                    g_pgraph_method_count, subchannel, graphics_class,
                    method, param);
        }
    }

    /* NV097 methods and PGRAPH MMIO registers are distinct address spaces.
     * Xemu updates internal PGRAPH registers only in explicit method handlers;
     * treating method / 4 as a register offset aliases unrelated state (for
     * example method 0x1C00 corrupts the XDK idle-status read at 0x700). */

    switch (method) {
    case M_CLEAR_SURFACE:
        g_pgraph_clear_count++;
        break;

    case M_SET_BEGIN_END:
        if (param != 0) {
            g_pgraph_in_begin = 1;
            g_pgraph_draw_count++;
        } else {
            g_pgraph_in_begin = 0;
        }
        break;

    case M_INLINE_ARRAY:
        if (g_pgraph_in_begin) {
            g_pgraph_inline_verts++;
        }
        break;

    case M_FLIP_INCREMENT_WRITE:
        g_pgraph_flip_count++;
        if (trace_active &&
            (g_pgraph_flip_count <= 5 ||
             (g_pgraph_flip_count % 300) == 0)) {
            fprintf(stderr, "[PGRAPH] Frame %u: %u methods, %u draws, %u clears, %u inline verts\n",
                    g_pgraph_flip_count, g_pgraph_method_count,
                    g_pgraph_draw_count, g_pgraph_clear_count,
                    g_pgraph_inline_verts);
        }
        break;

    default:
        break;
    }
}

/* ============================================================
 * PFIFO - command FIFO
 *
 * NV_USER DMA PUT writes invoke the synchronous push-buffer processor.
 * ============================================================ */

uint64_t pfifo_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    uint64_t r = 0;
    switch (addr) {
    case NV_PFIFO_INTR_0:
        r = d->pfifo.pending_interrupts;
        break;
    case NV_PFIFO_INTR_EN_0:
        r = d->pfifo.enabled_interrupts;
        break;
    case NV_PFIFO_RUNOUT_STATUS:
        /* No runout queue is modeled. */
        r = NV_PFIFO_RUNOUT_STATUS_LOW_MARK;
        break;
    case NV_PFIFO_CACHE1_STATUS:
        /* Report cache1 empty; the DMA pusher processes commands synchronously. */
        r = NV_PFIFO_CACHE1_STATUS_LOW_MARK;
        break;
    default:
        r = d->pfifo.regs[addr];
        break;
    }

    nv2a_reg_log_read(NV_PFIFO, addr, size, r);
    return r;
}

void pfifo_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_PFIFO, addr, size, val);

    switch (addr) {
    case NV_PFIFO_INTR_0:
        d->pfifo.pending_interrupts &= ~val;
        nv2a_update_irq(d);
        break;
    case NV_PFIFO_INTR_EN_0:
        d->pfifo.enabled_interrupts = val;
        nv2a_update_irq(d);
        break;
    default:
        d->pfifo.regs[addr] = val;
        break;
    }
}

/*
 * Synchronous PFIFO DMA pusher.
 *
 * This follows xemu's packet-state and jump/call/return rules, but executes on
 * NV_USER PUT because this standalone backend has no QEMU GPU worker thread.
 */
static void pfifo_process_pushbuffer(NV2AState *d, uint32_t put)
{
    static int trace_target_initialized;
    static int trace_progress_initialized;
    static int trace_progress_enabled;
    static uint32_t submission_sequence;
    static uint32_t trace_target;
    uint32_t get = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
    const uint32_t start_get = get;
    uint32_t *dma_state = &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_STATE];
    uint32_t *dma_subroutine =
        &d->pfifo.regs[NV_PFIFO_CACHE1_DMA_SUBROUTINE];
    uint32_t budget = 1024u * 1024u;
    uint32_t methods = 0;
    const uint32_t dma_instance =
        GET_MASK(d->pfifo.regs[NV_PFIFO_CACHE1_DMA_INSTANCE],
                 NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS) << 4;
    hwaddr dma_len = 0;
    uint8_t *dma = nv_dma_map(d, dma_instance, &dma_len);
    const uint32_t dma_base = dma ? (uint32_t)(dma - d->vram_ptr) : 0u;
    const uint32_t initial_method_count =
        GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT);
    static uint32_t submission_samples;
    const uint32_t submission_index = ++submission_sequence;
    bool trace_submission;
    ULONGLONG progress_tick = GetTickCount64();
    uint32_t progress_methods = 0u;

    g_nv2a_pfifo_diag_submission = submission_index;
    g_nv2a_pfifo_diag_get = get;
    g_nv2a_pfifo_diag_put = put;
    g_nv2a_pfifo_diag_stage = 1u;

    if (!trace_target_initialized) {
        const char *value = getenv("MERCENARIES_TRACE_PFIFO_SUBMISSION");
        trace_target = value ? (uint32_t)strtoul(value, NULL, 0) : 0u;
        trace_target_initialized = 1;
    }
    if (!trace_progress_initialized) {
        const char *value = getenv("MERCENARIES_TRACE_PFIFO_PROGRESS");
        trace_progress_enabled = value && value[0] && value[0] != '0';
        trace_progress_initialized = 1;
    }
    trace_submission = submission_index == trace_target;
    if (trace_submission) {
        fprintf(stderr,
                "[PFIFO-TRACE] begin submission=%u get=%08X put=%08X "
                "state=%08X subroutine=%08X\n",
                submission_index, get, put, *dma_state, *dma_subroutine);
    }

    if (!dma || put >= dma_len) {
        fprintf(stderr,
                "[PFIFO] invalid DMA range instance=%08X base=%08X "
                "len=%08llX get=%08X put=%08X\n",
                dma_instance, dma_base, (unsigned long long)dma_len,
                get, put);
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = put;
        g_nv2a_pfifo_diag_get = put;
        g_nv2a_pfifo_diag_stage = 0u;
        return;
    }

    while (get != put && budget != 0u) {
        --budget;
        uint32_t word;
        uint32_t method_count;

        if (get + 4u > dma_len) {
            fprintf(stderr,
                    "[PFIFO] GET outside DMA object: instance=%08X "
                    "base=%08X len=%08llX get=%08X put=%08X\n",
                    dma_instance, dma_base, (unsigned long long)dma_len,
                    get, put);
            get = put;
            break;
        }

        word = ldl_le_p((const uint32_t *)(dma + get));
        g_nv2a_pfifo_diag_get = get;
        g_nv2a_pfifo_diag_word = word;
        if (trace_submission) {
            fprintf(stderr,
                    "[PFIFO-TRACE] word submission=%u at=%08X value=%08X "
                    "state=%08X\n",
                    submission_index, get, word, *dma_state);
        }
        get += 4u;
        method_count = GET_MASK(*dma_state,
                                NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT);

        if (method_count != 0u) {
            uint32_t method =
                GET_MASK(*dma_state,
                         NV_PFIFO_CACHE1_DMA_STATE_METHOD) << 2;
            const uint32_t subchannel =
                GET_MASK(*dma_state,
                         NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL);
            const uint32_t method_type =
                GET_MASK(*dma_state,
                         NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE);

            g_nv2a_pfifo_diag_method = method;
            g_nv2a_pfifo_diag_subchannel = subchannel;
            g_nv2a_pfifo_diag_stage = 2u;

            if (trace_progress_enabled) {
                const ULONGLONG now = GetTickCount64();
                if (now - progress_tick >= 1000u) {
                    fprintf(stderr,
                            "[PFIFO-PROGRESS] submission=%u get=%08X "
                            "put=%08X methods=%u next=%04X/%08X\n",
                            submission_index, get - 4u, put,
                            progress_methods, method, word);
                    fflush(stderr);
                    progress_tick = now;
                    progress_methods = 0u;
                }
            }

            if (trace_submission) {
                fprintf(stderr,
                        "[PFIFO-TRACE] method-enter submission=%u sub=%u "
                        "method=%04X param=%08X\n",
                        submission_index, subchannel, method, word);
            }
            pgraph_method(d, subchannel, method, word);
            g_nv2a_pfifo_diag_stage = 1u;
            ++progress_methods;
            if (trace_submission) {
                fprintf(stderr,
                        "[PFIFO-TRACE] method-exit submission=%u sub=%u "
                        "method=%04X\n",
                        submission_index, subchannel, method);
            }
            ++methods;
            /* Indexed meshes send long runs of non-incrementing data. Keep
             * the first word on the normal context/trace path, then append
             * only words already present in this DMA submission. */
            if (method_type != NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC &&
                method_count > 1u && put > get && dma_len > get && budget &&
                subchannel < 8u &&
                d->pgraph.subchannel_class[subchannel] == NV_KELVIN_PRIMITIVE &&
                !g_pgraph_trace_enabled && !trace_submission &&
                !trace_progress_enabled &&
                (method == NV097_ARRAY_ELEMENT16 ||
                 method == NV097_ARRAY_ELEMENT32 || method == NV097_INLINE_ARRAY)) {
                uint32_t available = (put - get) / 4u;
                const uint32_t dma_available = (uint32_t)((dma_len - get) / 4u);
                if (available > dma_available) available = dma_available;
                if (available > method_count - 1u) available = method_count - 1u;
                if (available > budget) available = budget;
                const uint32_t consumed = pgraph_d3d11_inline_batch(
                    method, (const uint32_t *)(dma + get), available);
                if (consumed) {
                    get += consumed * 4u;
                    budget -= consumed;
                    method_count -= consumed;
                    methods += consumed;
                    progress_methods += consumed;
                    g_pgraph_method_count += consumed;
                    g_nv2a_pfifo_diag_get = get - 4u;
                    g_nv2a_pfifo_diag_word = ldl_le_p((const uint32_t *)(dma + get - 4u));
                }
            }
            if (method_type ==
                NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC) {
                SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                         (method + 4u) >> 2);
            }
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                     method_count - 1u);
            continue;
        }

        if ((word & 0xE0000003u) == 0x20000000u) {
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET_JMP_SHADOW] = get;
            get = word & 0x1FFFFFFFu;
        } else if ((word & 3u) == 1u) {
            d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET_JMP_SHADOW] = get;
            get = word & 0xFFFFFFFCu;
        } else if ((word & 3u) == 2u) {
            if (GET_MASK(*dma_subroutine,
                         NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE) != 0u) {
                fprintf(stderr, "[PFIFO] nested push-buffer CALL\n");
                get = put;
            } else {
                *dma_subroutine = get;
                SET_MASK(*dma_subroutine,
                         NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE, 1u);
                get = word & 0xFFFFFFFCu;
            }
        } else if (word == 0x00020000u) {
            if (GET_MASK(*dma_subroutine,
                         NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE) == 0u) {
                fprintf(stderr, "[PFIFO] push-buffer RETURN without CALL\n");
                get = put;
            } else {
                get = *dma_subroutine & 0xFFFFFFFCu;
                SET_MASK(*dma_subroutine,
                         NV_PFIFO_CACHE1_DMA_SUBROUTINE_STATE, 0u);
            }
        } else if ((word & 0xE0030003u) == 0u) {
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                     (word & 0x1FFFu) >> 2);
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,
                     (word >> 13) & 7u);
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                     (word >> 18) & 0x7FFu);
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,
                     NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_INC);
        } else if ((word & 0xE0030003u) == 0x40000000u) {
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD,
                     (word & 0x1FFFu) >> 2);
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_SUBCHANNEL,
                     (word >> 13) & 7u);
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT,
                     (word >> 18) & 0x7FFu);
            SET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE,
                     NV_PFIFO_CACHE1_DMA_STATE_METHOD_TYPE_NON_INC);
        } else {
            fprintf(stderr,
                    "[PFIFO] reserved command %08X at %08X (put=%08X)\n",
                    word, get - 4u, put);
            get = put;
        }
    }

    if (budget == 0u && get != put) {
        fprintf(stderr,
                "[PFIFO] synchronous processing budget exhausted "
                "(get=%08X put=%08X)\n", get, put);
        get = put;
    }
    d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = get;
    g_nv2a_pfifo_diag_get = get;
    g_nv2a_pfifo_diag_stage = 0u;

    if (submission_samples < 12u) {
        fprintf(stderr,
                "[PFIFO] submit get=%08X put=%08X methods=%u "
                "state=%u->%u patt=%08X\n",
                get, put, methods, initial_method_count,
                GET_MASK(*dma_state, NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT),
                d->pgraph.regs[NV_PGRAPH_PATT_COLOR0]);
        if (start_get < put && start_get + 24u <= dma_len) {
            fprintf(stderr,
                    "[PFIFO-RAW] %08X: %08X %08X %08X %08X %08X %08X\n",
                    start_get,
                    ldl_le_p((const uint32_t *)(dma + start_get + 0u)),
                    ldl_le_p((const uint32_t *)(dma + start_get + 4u)),
                    ldl_le_p((const uint32_t *)(dma + start_get + 8u)),
                    ldl_le_p((const uint32_t *)(dma + start_get + 12u)),
                    ldl_le_p((const uint32_t *)(dma + start_get + 16u)),
                    ldl_le_p((const uint32_t *)(dma + start_get + 20u)));
        }
        ++submission_samples;
    }
}

/* ============================================================
 * USER - per-channel PFIFO DMA register aperture
 *
 * NV_USER_DMA_PUT, NV_USER_DMA_GET, and NV_USER_REF alias the active
 * CACHE1 DMA registers. A PUT write invokes pfifo_process_pushbuffer(),
 * which parses commands, dispatches methods, and advances GET.
 * ============================================================ */

uint64_t user_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t r = 0;

    switch (addr & 0xFFFF) {
    case NV_USER_DMA_PUT:
        r = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT];
        break;
    case NV_USER_DMA_GET:
        r = d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET];
        break;
    case NV_USER_REF:
        r = d->pfifo.regs[NV_PFIFO_CACHE1_REF];
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_USER, addr, size, r);
    return r;
}

void user_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;

    nv2a_reg_log_write(NV_USER, addr, size, val);

    switch (addr & 0xFFFF) {
    case NV_USER_DMA_PUT:
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT] = (uint32_t)val;
        pfifo_process_pushbuffer(d, (uint32_t)val);
        break;
    case NV_USER_DMA_GET:
        d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET] = (uint32_t)val;
        break;
    case NV_USER_REF:
        d->pfifo.regs[NV_PFIFO_CACHE1_REF] = (uint32_t)val;
        break;
    default:
        break;
    }
}

/* ============================================================
 * Stub handler for unimplemented blocks
 * ============================================================ */

uint64_t nv2a_stub_read(void *opaque, hwaddr addr, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub read: addr=0x%llx size=%d\n",
                 (unsigned long long)addr, size);
    return 0;
}

void nv2a_stub_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    (void)opaque; (void)size;
    NV2A_DPRINTF("stub write: addr=0x%llx val=0x%llx size=%d\n",
                 (unsigned long long)addr, (unsigned long long)val, size);
}

/* VGA-compatible DAC palette. Xbox D3D uses this as the display gamma LUT. */
uint64_t prmdio_read(void *opaque, hwaddr addr, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    uint64_t result = 0;
    if (addr == NV_USER_DAC_WRITE_MODE_ADDRESS)
        result = d->puserdac.write_mode_address / 3u;
    nv2a_reg_log_read(NV_PRMDIO, addr, size, result);
    return result;
}

void prmdio_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    NV2AState *d = (NV2AState *)opaque;
    nv2a_reg_log_write(NV_PRMDIO, addr, size, val);
    if (addr == NV_USER_DAC_WRITE_MODE_ADDRESS) {
        d->puserdac.write_mode_address = (uint16_t)((val & 0xffu) * 3u);
    } else if (addr == NV_USER_DAC_PALETTE_DATA) {
        unsigned int index = d->puserdac.write_mode_address++ % (256u * 3u);
        d->puserdac.palette[index] = (uint8_t)val;
        if (getenv("MERCENARIES_TRACE_GAMMA") &&
            d->puserdac.write_mode_address != 0u &&
            d->puserdac.write_mode_address % (256u * 3u) == 0u) {
            const uint8_t *p = d->puserdac.palette;
            fprintf(stderr,
                    "NV2A: DAC gamma r={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u} "
                    "g={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u} "
                    "b={%u,%u,%u,%u,%u,%u,%u,%u,%u,%u}\n",
                    p[0], p[16*3], p[32*3], p[64*3], p[96*3],
                    p[128*3], p[160*3], p[192*3], p[224*3], p[255*3],
                    p[1], p[16*3+1], p[32*3+1], p[64*3+1], p[96*3+1],
                    p[128*3+1], p[160*3+1], p[192*3+1], p[224*3+1], p[255*3+1],
                    p[2], p[16*3+2], p[32*3+2], p[64*3+2], p[96*3+2],
                    p[128*3+2], p[160*3+2], p[192*3+2], p[224*3+2], p[255*3+2]);
            fflush(stderr);
        }
    }
}
/* ============================================================
 * Block dispatch table (from xemu nv2a.c)
 * ============================================================ */

#define ENTRY(NAME, LNAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                      \
    .offset = OFFSET,                                     \
    .size   = SIZE,                                       \
    .ops    = { .read = LNAME##_read, .write = LNAME##_write }, \
}
#define STUB_ENTRY(NAME, OFFSET, SIZE) [NV_##NAME] = { \
    .name   = #NAME,                                    \
    .offset = OFFSET,                                   \
    .size   = SIZE,                                     \
    .ops    = { .read = nv2a_stub_read, .write = nv2a_stub_write }, \
}

const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    STUB_ENTRY(PFIFO_CACHE,   0x003000, 0x001000),
    STUB_ENTRY(PRMA,          0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    STUB_ENTRY(PCOUNTER,      0x00a000, 0x001000),
    STUB_ENTRY(PVPE,          0x00b000, 0x001000),
    STUB_ENTRY(PTV,           0x00d000, 0x001000),
    STUB_ENTRY(PRMFB,         0x0a0000, 0x020000),
    STUB_ENTRY(PRMVIO,        0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    STUB_ENTRY(PSTRAPS,       0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    STUB_ENTRY(PRMCIO,        0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    ENTRY(PRMDIO,    prmdio,  0x681000, 0x001000),
    /* NV_PRAMIN = 19 */
    { .name = NULL },
    /* NV_USER = 20 */
    ENTRY(USER, user,         0x800000, 0x800000),
};

#undef ENTRY
#undef STUB_ENTRY

/* ============================================================
 * MMIO dispatch (for VEH handler integration)
 * ============================================================ */

uint64_t nv2a_mmio_read(NV2AState *d, hwaddr addr, unsigned int size)
{
    static int trace_sync = -1;
    static uint32_t trace_sync_samples;
    uint64_t result;

    /* xemu maps the 1 MiB RAMIN object memory directly at BAR0+0x700000. */
    if (addr >= 0x700000u && addr < 0x800000u &&
        size <= sizeof(uint64_t) &&
        addr - 0x700000u + size <= memory_region_size(&d->ramin)) {
        uint64_t r = 0;
        memcpy(&r, d->ramin_ptr + (addr - 0x700000u), size);
        nv2a_reg_log_read(NV_PRAMIN, addr - 0x700000u, size, r);
        return r;
    }

    /* Find which block handles this address */
    for (int i = 0; i < NV_NUM_BLOCKS; i++) {
        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            result = blocktable[i].ops.read(d, block_addr, size);
            if (trace_sync < 0) {
                const char *value = getenv("MERCENARIES_TRACE_NV2A_SYNC");
                trace_sync = value && value[0] != '\0' && value[0] != '0';
            }
            if (trace_sync && trace_sync_samples < 64u &&
                (addr == 0x100410u || addr == 0x400700u ||
                 (addr >= 0x800000u &&
                  ((addr & 0xFFFFu) == NV_USER_DMA_PUT ||
                   (addr & 0xFFFFu) == NV_USER_DMA_GET)))) {
                fprintf(stderr,
                        "[NV2A-SYNC] read addr=%08llX block=%s "
                        "offset=%08llX size=%u value=%08llX\n",
                        (unsigned long long)addr, blocktable[i].name,
                        (unsigned long long)block_addr, size,
                        (unsigned long long)result);
                ++trace_sync_samples;
            }
            return result;
        }
    }
    NV2A_DPRINTF("MMIO read unmapped: addr=0x%llx\n", (unsigned long long)addr);
    return 0;
}

void nv2a_mmio_write(NV2AState *d, hwaddr addr, uint64_t val, unsigned int size)
{
    static uint32_t pramin_write_samples;
    static int trace_sync = -1;
    static uint32_t trace_sync_samples;

    if (addr >= 0x700000u && addr < 0x800000u &&
        size <= sizeof(uint64_t) &&
        addr - 0x700000u + size <= memory_region_size(&d->ramin)) {
        memcpy(d->ramin_ptr + (addr - 0x700000u), &val, size);
        if (pramin_write_samples < 32u) {
            fprintf(stderr,
                    "[PRAMIN] write addr=%08llX offset=%08llX size=%u "
                    "value=%08llX\n",
                    (unsigned long long)addr,
                    (unsigned long long)(addr - 0x700000u), size,
                    (unsigned long long)val);
            ++pramin_write_samples;
        }
        nv2a_reg_log_write(NV_PRAMIN, addr - 0x700000u, size, val);
        return;
    }

    for (int i = 0; i < NV_NUM_BLOCKS; i++) {

        if (!blocktable[i].name) continue;
        if (addr >= blocktable[i].offset &&
            addr < blocktable[i].offset + blocktable[i].size) {
            hwaddr block_addr = addr - blocktable[i].offset;
            blocktable[i].ops.write(d, block_addr, val, size);
            if (trace_sync < 0) {
                const char *value = getenv("MERCENARIES_TRACE_NV2A_SYNC");
                trace_sync = value && value[0] != '\0' && value[0] != '0';
            }
            if (trace_sync && trace_sync_samples < 64u &&
                (addr == 0x100410u || addr == 0x400700u ||
                 (addr >= 0x800000u &&
                  ((addr & 0xFFFFu) == NV_USER_DMA_PUT ||
                   (addr & 0xFFFFu) == NV_USER_DMA_GET)))) {
                fprintf(stderr,
                        "[NV2A-SYNC] write addr=%08llX block=%s "
                        "offset=%08llX size=%u value=%08llX\n",
                        (unsigned long long)addr, blocktable[i].name,
                        (unsigned long long)block_addr, size,
                        (unsigned long long)val);
                ++trace_sync_samples;
            }
            return;
        }
    }
    NV2A_DPRINTF("MMIO write unmapped: addr=0x%llx val=0x%llx\n",
                 (unsigned long long)addr, (unsigned long long)val);
}

/* ============================================================
 * Standalone initialization
 * ============================================================ */

NV2AState *nv2a_init_standalone(uint8_t *vram_ptr, uint32_t vram_size,
                                 uint8_t *ramin_ptr, uint32_t ramin_size)
{
    if (g_nv2a) return g_nv2a;

    NV2AState *d = (NV2AState *)calloc(1, sizeof(NV2AState));
    if (!d) return NULL;

    /* Set up VRAM */
    g_vram_region.size = vram_size;
    d->vram = &g_vram_region;
    d->vram_ptr = vram_ptr;
    d->vram_pci.size = vram_size;

    /* Set up RAMIN */
    g_ramin_region.size = ramin_size;
    d->ramin.size = ramin_size;
    d->ramin_ptr = ramin_ptr;

    /* PCI config space: NV2A vendor/device */
    pci_set_long(d->parent_obj.config + PCI_VENDOR_ID, 0x02A010DE); /* NVIDIA NV2A */
    pci_set_long(d->parent_obj.config + PCI_CLASS_REVISION, 0x030000A1);

    /* Default PLL: 233 MHz core clock (Xbox default) */
    d->pramdac.core_clock_coeff = 0x00011C01; /* n=0x1C, m=1, p=0 */
    d->pramdac.core_clock_freq = NV2A_CRYSTAL_FREQ * 0x1C; /* ~233 MHz */

    /* Default timer divisors */
    d->ptimer.numerator = 1;
    d->ptimer.denominator = 1;

    /* Xemu initializes the VGA DAC LUT to an identity curve. */
    for (unsigned int i = 0; i < 256u; ++i) {
        d->puserdac.palette[i * 3u] = (uint8_t)i;
        d->puserdac.palette[i * 3u + 1u] = (uint8_t)i;
        d->puserdac.palette[i * 3u + 2u] = (uint8_t)i;
    }

    /* Initialize PFIFO mutex */
    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    g_nv2a = d;

    fprintf(stderr, "[NV2A] Standalone GPU initialized: VRAM=%uMB RAMIN=%uKB\n",
            vram_size / (1024*1024), ramin_size / 1024);

    return d;
}

void nv2a_set_system_ram(uint8_t *system_ram_ptr)
{
    if (g_nv2a && system_ram_ptr) {
        g_nv2a->vram_ptr = system_ram_ptr;
    }
}

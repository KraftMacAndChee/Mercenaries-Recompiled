/*
 * MCPX APU DSP (GP/EP)
 *
 * The GP and EP execute the title-provided DSP56300 programs.  DMA/FIFO and
 * peripheral behavior is adapted from Xemu's LGPL MCPX APU implementation;
 * instruction execution is provided by the pinned MIT dsp56300 v0.1.3 JIT.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2026 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#ifdef XBOX_APU_DSP56300
#include <dsp56300.h>
#endif

#define DSP_SPACE_X 0
#define DSP_SPACE_Y 1
#define DSP_SPACE_P 2

#define DMA_CONFIGURATION_AUTOSTART (1u << 0)
#define DMA_CONFIGURATION_AUTOREADY (1u << 1)
#define DMA_CONFIGURATION_IOC_CLEAR (1u << 2)
#define DMA_CONFIGURATION_EOL_CLEAR (1u << 3)
#define DMA_CONFIGURATION_ERR_CLEAR (1u << 4)
#define DMA_CONTROL_ACTION 0x7u
#define DMA_CONTROL_ACTION_NOP 0u
#define DMA_CONTROL_ACTION_START 1u
#define DMA_CONTROL_ACTION_STOP 2u
#define DMA_CONTROL_ACTION_FREEZE 3u
#define DMA_CONTROL_ACTION_UNFREEZE 4u
#define DMA_CONTROL_ACTION_ABORT 5u
#define DMA_CONTROL_FROZEN (1u << 3)
#define DMA_CONTROL_RUNNING (1u << 4)
#define DMA_CONTROL_STOPPED (1u << 5)
#define NODE_POINTER_VAL 0x3fffu
#define NODE_POINTER_EOL (1u << 14)
#define NODE_CONTROL_DIRECTION (1u << 1)
#define INTERRUPT_START_FRAME (1u << 1)
#define INTERRUPT_DMA_EOL (1u << 7)
#define XBOX_RAM_SIZE 0x04000000u
#define DSP_FRAME_CYCLE_LIMIT 2000000u
#define DSP_DMA_NODE_LIMIT 0x4000u

static const int16_t ep_silence[256][2] = { 0 };

static int speaker_downmix_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        /*
         * When the title DSP is unavailable, preserve the voice processor's
         * authored routing into the six Xbox speaker mixbins and perform the
         * normal 5.1-to-stereo fold-down.  The old VP monitor selected one
         * strongest route from each voice, bypassing multipass recombination;
         * that made voices, weapons, and vehicle effects arbitrarily quiet.
         * Keep that monitor available only as a diagnostic comparison.
         */
        enabled = getenv("MERCENARIES_DISABLE_APU_SPEAKER_DOWNMIX") == NULL;
    }
    return enabled;
}

static void fallback_speaker_sample(
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
    unsigned int sample, float *left, float *right)
{
    /* Xbox DirectSound's required 3D buses are not ordered as two adjacent
     * stereo pairs in route space: mixbins 6/7 are 3D front L/R and 8/9 are
     * 3D back L/R. The GP/EP image normally folds these into the physical
     * speaker buses. When that DSP is unavailable, omitting them makes a
     * positional source vanish whenever its direct center send is reduced
     * (for example a weapon muzzle directly below the listener). */
    float l = mixbins[0][sample] +
              0.70710678f * mixbins[2][sample] +
              0.5f * mixbins[3][sample] +
              0.70710678f * mixbins[4][sample] +
              mixbins[6][sample] +
              0.70710678f * mixbins[8][sample];
    float r = mixbins[1][sample] +
              0.70710678f * mixbins[2][sample] +
              0.5f * mixbins[3][sample] +
              0.70710678f * mixbins[5][sample] +
              mixbins[7][sample] +
              0.70710678f * mixbins[9][sample];

    *left = l;
    *right = r;
}

static int dsp_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        /*
         * Match Xemu's stable default: render the voice processor directly
         * unless full GP/EP DSP execution is explicitly requested. A GP-only
         * monitor is an intermediate effects bus, not the final Xbox speaker
         * mix; making it the default suppresses cues routed through the EP
         * (notably weapons and positional voices). Keep the authored DSP
         * path available for differential testing until both processors can
         * complete a frame correctly.
         */
        enabled = getenv("MERCENARIES_ENABLE_APU_DSP") != NULL &&
                  getenv("MERCENARIES_DISABLE_APU_DSP") == NULL;
    }
    return enabled;
}

static int ep_dsp_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        /*
         * The bundled DSP56300 core currently completes Mercenaries' GP
         * program, but the EP program can remain inside its convolution loop
         * past a complete hardware frame. Keep EP execution opt-in until
         * that instruction-level discrepancy is resolved. GP output is the
         * title-authored dry/effects-send mix and is a faithful, deterministic
         * fallback; it also avoids a one-time two-million-cycle startup hitch.
         */
        enabled = getenv("MERCENARIES_ENABLE_APU_EP_DSP") != NULL &&
                  getenv("MERCENARIES_DISABLE_APU_EP_DSP") == NULL;
    }
    return enabled;
}

#ifdef XBOX_APU_DSP56300

static Dsp56300MemSpace dsp_space(char space)
{
    return space == 'X' ? DSP56300_MEM_SPACE_X :
           space == 'Y' ? DSP56300_MEM_SPACE_Y : DSP56300_MEM_SPACE_P;
}

uint32_t mcpx_apu_dsp_read_memory(DSPState *dsp, char space,
                                  uint32_t address)
{
    if (!dsp || !dsp->jit) return 0;
    return dsp56300_read_memory(dsp->jit, dsp_space(space), address) &
           0x00ffffffu;
}

void mcpx_apu_dsp_write_memory(DSPState *dsp, char space,
                               uint32_t address, uint32_t value)
{
    if (!dsp || !dsp->jit) return;
    dsp56300_write_memory(dsp->jit, dsp_space(space), address,
                          value & 0x00ffffffu);
}

static uint32_t dsp_dma_mem_read(void *opaque, int space, uint32_t address)
{
    DSPState *dsp = (DSPState *)opaque;
    return dsp56300_read_memory(dsp->jit, (Dsp56300MemSpace)space, address);
}

static void dsp_dma_mem_write(void *opaque, int space, uint32_t address,
                              uint32_t value)
{
    DSPState *dsp = (DSPState *)opaque;
    dsp56300_write_memory(dsp->jit, (Dsp56300MemSpace)space, address,
                          value & 0x00ffffffu);
}

static bool guest_range_valid(uint32_t address, size_t length)
{
    return address <= XBOX_RAM_SIZE && length <= XBOX_RAM_SIZE - address;
}

static void dsp_dma_fail(DSPDMAState *dma, const char *reason)
{
    DSPState *owner = (DSPState *)dma->mem_opaque;
    if (!dma->error) {
        fprintf(stderr,
                "[APU-DSP-DMA] %s failure=%s node=%06X next=%06X "
                "control=%06X count=%06X dsp=%06X scratch_offset=%06X "
                "scratch_base=%06X scratch_size=%06X dma_control=%06X "
                "start=%06X\n",
                owner && owner->is_gp ? "GP" : "EP", reason,
                dma->diagnostic_node, dma->diagnostic_next,
                dma->diagnostic_control, dma->diagnostic_count,
                dma->diagnostic_dsp_offset, dma->diagnostic_scratch_offset,
                dma->diagnostic_scratch_base, dma->diagnostic_scratch_size,
                dma->control, dma->start_block);
        fflush(stderr);
    }
    dma->error = true;
}

static bool scatter_gather_rw(MCPXAPUState *d, uint32_t sge_base,
                              unsigned int max_sge, uint8_t *ptr,
                              uint32_t address, size_t length, bool dir)
{
    unsigned int page_entry = address / TARGET_PAGE_SIZE;
    unsigned int offset_in_page = address % TARGET_PAGE_SIZE;

    while (length > 0) {
        size_t bytes_to_copy = TARGET_PAGE_SIZE - offset_in_page;
        uint32_t descriptor_address;
        uint32_t physical_address;

        if (page_entry > max_sge ||
            !guest_range_valid(sge_base + page_entry * 8u, 4)) {
            fprintf(stderr,
                    "[APU-DSP-SG] table-range failure sge_base=%08X "
                    "max_sge=%08X page=%08X table_addr=%08X "
                    "offset=%08X length=%zu direction=%u\n",
                    sge_base, max_sge, page_entry,
                    sge_base + page_entry * 8u, offset_in_page, length,
                    dir ? 1u : 0u);
            fflush(stderr);
            return false;
        }
        memcpy(&descriptor_address,
               d->ram_ptr + sge_base + page_entry * 8u, sizeof(uint32_t));
        physical_address = descriptor_address + offset_in_page;
        if (bytes_to_copy > length) bytes_to_copy = length;
        if (!guest_range_valid(physical_address, bytes_to_copy)) {
            fprintf(stderr,
                    "[APU-DSP-SG] physical-range failure sge_base=%08X "
                    "max_sge=%08X page=%08X descriptor=%08X "
                    "physical=%08X bytes=%zu offset=%08X length=%zu "
                    "direction=%u\n",
                    sge_base, max_sge, page_entry, descriptor_address,
                    physical_address, bytes_to_copy, offset_in_page, length,
                    dir ? 1u : 0u);
            fflush(stderr);
            return false;
        }

        if (dir) memcpy(d->ram_ptr + physical_address, ptr, bytes_to_copy);
        else memcpy(ptr, d->ram_ptr + physical_address, bytes_to_copy);

        ptr += bytes_to_copy;
        length -= bytes_to_copy;
        page_entry++;
        offset_in_page = 0;
    }
    return true;
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t address,
                          size_t length, bool dir)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    if (!scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR],
                           d->regs[NV_PAPU_GPSMAXSGE], ptr, address,
                           length, dir)) {
        dsp_dma_fail(&d->gp.dsp->dma, "gp-scratch-scatter-gather");
    }
}

static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t address,
                          size_t length, bool dir)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    if (!scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR],
                           d->regs[NV_PAPU_EPSMAXSGE], ptr, address,
                           length, dir)) {
        dsp_dma_fail(&d->ep.dsp->dma, "ep-scratch-scatter-gather");
    }
}

static bool circular_scatter_gather_rw(MCPXAPUState *d, uint32_t sge_base,
                                       unsigned int max_sge, uint8_t *ptr,
                                       uint32_t base, uint32_t end,
                                       uint32_t *current, size_t length,
                                       bool dir)
{
    if (end <= base) return false;
    if (*current < base || *current >= end) *current = base;

    while (length > 0) {
        size_t bytes_to_copy = end - *current;
        if (bytes_to_copy > length) bytes_to_copy = length;
        if (!scatter_gather_rw(d, sge_base, max_sge, ptr, *current,
                               bytes_to_copy, dir)) {
            return false;
        }
        ptr += bytes_to_copy;
        length -= bytes_to_copy;
        *current += (uint32_t)bytes_to_copy;
        if (*current == end) *current = base;
    }
    return true;
}

static bool fifo_parameters(MCPXAPUState *d, bool gp, bool dir,
                            unsigned int index, uint32_t *base,
                            uint32_t *end, uint32_t *current_reg,
                            uint32_t *sge_base, uint32_t *max_sge)
{
    unsigned int limit = dir ? 4u : 2u;
    uint32_t first;
    if (index >= limit) return false;

    if (gp) {
        first = dir ? NV_PAPU_GPOFBASE0 : NV_PAPU_GPIFBASE0;
        *sge_base = d->regs[NV_PAPU_GPFADDR];
        *max_sge = d->regs[NV_PAPU_GPFMAXSGE];
    } else {
        first = dir ? NV_PAPU_EPOFBASE0 : NV_PAPU_EPIFBASE0;
        *sge_base = d->regs[NV_PAPU_EPFADDR];
        *max_sge = d->regs[NV_PAPU_EPFMAXSGE];
    }
    *base = GET_MASK(d->regs[first + 0x10u * index],
                     NV_PAPU_GPOFBASE0_VALUE);
    *end = GET_MASK(d->regs[first + 4u + 0x10u * index],
                    NV_PAPU_GPOFEND0_VALUE);
    *current_reg = first + 8u + 0x10u * index;
    return *end > *base;
}

static bool ep_sink_samples(MCPXAPUState *d, uint8_t *ptr, size_t length)
{
    if (d->monitor.point == MCPX_APU_DEBUG_MON_AC97) return false;
    if (d->monitor.point == MCPX_APU_DEBUG_MON_EP ||
        d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP) {
        if (length != sizeof(d->monitor.frame_buf)) return false;
        memcpy(d->monitor.frame_buf, ptr, length);
    }
    return true;
}

static void dsp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                        size_t length, bool dir, bool gp)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    DSPState *dsp = gp ? d->gp.dsp : d->ep.dsp;
    uint32_t base, end, current_reg, sge_base, max_sge, current;

    if (!fifo_parameters(d, gp, dir, index, &base, &end, &current_reg,
                         &sge_base, &max_sge)) {
        dsp_dma_fail(&dsp->dma, "invalid-fifo-parameters");
        return;
    }

    if (!gp && dir && index == 0 && ep_sink_samples(d, ptr, length)) {
        if (length > sizeof(ep_silence)) {
            dsp_dma_fail(&dsp->dma, "ep-monitor-frame-too-large");
            return;
        }
        ptr = (uint8_t *)ep_silence;
    }

    current = GET_MASK(d->regs[current_reg], NV_PAPU_GPOFCUR0_VALUE);
    if (!circular_scatter_gather_rw(d, sge_base, max_sge, ptr, base, end,
                                    &current, length, dir)) {
        dsp_dma_fail(&dsp->dma, "fifo-scatter-gather");
        return;
    }
    SET_MASK(d->regs[current_reg], NV_PAPU_GPOFCUR0_VALUE, current);
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t length, bool dir)
{
    dsp_fifo_rw(opaque, ptr, index, length, dir, true);
}

static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                       size_t length, bool dir)
{
    dsp_fifo_rw(opaque, ptr, index, length, dir, false);
}

static bool ensure_dma_buffer(DSPDMAState *dma, size_t size)
{
    uint8_t *replacement;
    if (size <= dma->scratch_buf_size) return true;
    replacement = (uint8_t *)realloc(dma->scratch_buf, size);
    if (!replacement) return false;
    dma->scratch_buf = replacement;
    dma->scratch_buf_size = size;
    return true;
}

static bool scratch_circular_copy(DSPDMAState *dma, uint32_t scratch_base,
                                  uint32_t *scratch_offset,
                                  uint32_t scratch_size,
                                  uint32_t transfer_size, bool direction)
{
    uint32_t buffer_offset = 0;
    if (scratch_size == 0) return false;
    if (*scratch_offset >= scratch_size) *scratch_offset = 0;

    while (transfer_size > 0) {
        size_t until_wrap = scratch_size - *scratch_offset;
        size_t chunk = MIN(transfer_size, until_wrap);
        dma->scratch_rw(dma->rw_opaque, dma->scratch_buf + buffer_offset,
                        scratch_base + *scratch_offset, chunk, direction);
        if (dma->error) return false;
        *scratch_offset += (uint32_t)chunk;
        if (*scratch_offset == scratch_size) *scratch_offset = 0;
        transfer_size -= (uint32_t)chunk;
        buffer_offset += (uint32_t)chunk;
    }
    return true;
}

static bool decode_dsp_address(uint32_t encoded, uint32_t words,
                               int *space, uint32_t *address)
{
    uint32_t limit;
    if (encoded < 0x1800u) {
        *space = DSP_SPACE_X;
        *address = encoded;
        limit = 0x1800u;
    } else if (encoded < 0x2000u) {
        *space = DSP_SPACE_Y;
        *address = encoded - 0x1800u;
        limit = 0x0800u;
    } else if (encoded >= 0x2800u && encoded < 0x3800u) {
        *space = DSP_SPACE_P;
        *address = encoded - 0x2800u;
        limit = 0x1000u;
    } else {
        return false;
    }
    return *address <= limit && words <= limit - *address;
}

static void dsp_dma_run(DSPDMAState *dma)
{
    unsigned int nodes = 0;
    if (!(dma->control & DMA_CONTROL_RUNNING) ||
        (dma->control & DMA_CONTROL_FROZEN)) return;

    while (!(dma->next_block & NODE_POINTER_EOL)) {
        uint32_t node = dma->next_block & NODE_POINTER_VAL;
        uint32_t next, control, count, dsp_offset, scratch_offset;
        uint32_t scratch_base, scratch_size, channel_count, block_count;
        uint32_t item_size, item_mask, buffer_id, transfer_size;
        uint32_t memory_address, node_address;
        int memory_space, node_space;
        bool interleave, direction, offset_writeback;
        unsigned int i;

        if (++nodes > DSP_DMA_NODE_LIMIT ||
            !decode_dsp_address(node, 7, &node_space, &node_address)) {
            dma->diagnostic_node = node;
            dsp_dma_fail(dma, nodes > DSP_DMA_NODE_LIMIT ?
                         "descriptor-chain-limit" : "descriptor-address");
            break;
        }
        next = dma->mem_read(dma->mem_opaque, node_space, node_address);
        control = dma->mem_read(dma->mem_opaque, node_space, node_address + 1);
        count = dma->mem_read(dma->mem_opaque, node_space, node_address + 2);
        dsp_offset = dma->mem_read(dma->mem_opaque, node_space, node_address + 3);
        scratch_offset = dma->mem_read(dma->mem_opaque, node_space, node_address + 4);
        scratch_base = dma->mem_read(dma->mem_opaque, node_space, node_address + 5);
        scratch_size = dma->mem_read(dma->mem_opaque, node_space, node_address + 6) + 1u;
        dma->diagnostic_node = node;
        dma->diagnostic_next = next;
        dma->diagnostic_control = control;
        dma->diagnostic_count = count;
        dma->diagnostic_dsp_offset = dsp_offset;
        dma->diagnostic_scratch_offset = scratch_offset;
        dma->diagnostic_scratch_base = scratch_base;
        dma->diagnostic_scratch_size = scratch_size;
        dma->next_block = next;
        if (next & NODE_POINTER_EOL) dma->eol = true;

        interleave = (control & 1u) != 0;
        direction = (control & NODE_CONTROL_DIRECTION) != 0;
        offset_writeback = (control & (1u << 4)) != 0;
        buffer_id = (control >> 5) & 0xfu;
        if (((control >> 2) & 3u) != 0 || (control & (1u << 13))) {
            dsp_dma_fail(dma, "unsupported-control-bits");
            break;
        }
        channel_count = (count & 0xfu) + 1u;
        block_count = count >> 4;

        switch ((control >> 10) & 7u) {
        case 0: item_size = 1; item_mask = 0xffu; break;
        case 1: item_size = 2; item_mask = 0xffffu; break;
        case 2:
        case 6: item_size = 4; item_mask = 0x00ffffffu; break;
        default:
            dsp_dma_fail(dma, "unsupported-sample-format");
            break;
        }
        if (dma->error) break;
        if (!decode_dsp_address(dsp_offset, count, &memory_space,
                                &memory_address)) {
            dsp_dma_fail(dma, "dsp-transfer-address");
            break;
        }
        transfer_size = count * item_size;
        if (interleave && direction) {
            transfer_size = block_count * item_size * channel_count;
        }
        if (!ensure_dma_buffer(dma, transfer_size ? transfer_size : 1)) {
            dsp_dma_fail(dma, "host-transfer-buffer-allocation");
            break;
        }

        if (direction) {
            if (interleave) {
                for (i = 0; i < block_count; i++) {
                    unsigned int channel;
                    for (channel = 0; channel < channel_count; channel++) {
                        uint32_t value = dma->mem_read(
                            dma->mem_opaque, memory_space,
                            memory_address + channel * block_count + i);
                        size_t out = ((size_t)i * channel_count + channel) *
                                     item_size;
                        if (item_size == 2) {
                            uint16_t v16 = (uint16_t)(value >> 8);
                            memcpy(dma->scratch_buf + out, &v16, 2);
                        } else if (item_size == 4) {
                            memcpy(dma->scratch_buf + out, &value, 4);
                        } else {
                            dma->scratch_buf[out] = (uint8_t)value;
                        }
                    }
                }
            } else {
                for (i = 0; i < count; i++) {
                    uint32_t value = dma->mem_read(dma->mem_opaque,
                                                   memory_space,
                                                   memory_address + i);
                    size_t out = (size_t)i * item_size;
                    if (item_size == 1) dma->scratch_buf[out] = (uint8_t)value;
                    else if (item_size == 2) {
                        uint16_t v16 = (uint16_t)(value >> 8);
                        memcpy(dma->scratch_buf + out, &v16, 2);
                    } else memcpy(dma->scratch_buf + out, &value, 4);
                }
            }

            if (buffer_id <= 3u) {
                dma->fifo_rw(dma->rw_opaque, dma->scratch_buf, buffer_id,
                             transfer_size, true);
            } else if (buffer_id == 0xeu) {
                if (!scratch_circular_copy(dma, scratch_base,
                                           &scratch_offset, scratch_size,
                                           transfer_size, true)) break;
            } else if (buffer_id == 0xfu) {
                dma->scratch_rw(dma->rw_opaque, dma->scratch_buf,
                                scratch_base + scratch_offset,
                                transfer_size, true);
            } else {
                dsp_dma_fail(dma, "unsupported-output-buffer");
            }
        } else {
            if (interleave || (buffer_id != 0xeu && buffer_id != 0xfu)) {
                dsp_dma_fail(dma, interleave ?
                             "interleaved-input-transfer" :
                             "unsupported-input-buffer");
                break;
            }
            if (buffer_id == 0xeu) {
                if (!scratch_circular_copy(dma, scratch_base,
                                           &scratch_offset, scratch_size,
                                           transfer_size, false)) break;
            } else {
                dma->scratch_rw(dma->rw_opaque, dma->scratch_buf,
                                scratch_base + scratch_offset,
                                transfer_size, false);
            }
            if (dma->error) break;
            for (i = 0; i < count; i++) {
                uint32_t value = 0;
                size_t in = (size_t)i * item_size;
                if (item_size == 1) value = dma->scratch_buf[in];
                else if (item_size == 2) {
                    uint16_t v16;
                    memcpy(&v16, dma->scratch_buf + in, 2);
                    /* MCPX expands 16-bit DMA input into the high 16 bits of
                     * its 24-bit word.  Do not subsequently apply the
                     * 0x00ffff transfer-size mask: that would discard the
                     * newly shifted high byte and corrupt authored DSP
                     * coefficients.  This matches Xemu's dsp_dma_run(). */
                    value = (uint32_t)v16 << 8;
                } else {
                    memcpy(&value, dma->scratch_buf + in, 4);
                    value &= item_mask;
                }
                dma->mem_write(dma->mem_opaque, memory_space,
                               memory_address + i, value);
            }
        }

        if (dma->error) break;
        if (offset_writeback) {
            dma->mem_write(dma->mem_opaque, node_space, node_address + 4,
                           scratch_offset);
        }
    }
    if (dma->error) {
        dma->control &= ~DMA_CONTROL_RUNNING;
        dma->control |= DMA_CONTROL_STOPPED;
    }
}

static uint32_t dsp_dma_read(DSPDMAState *dma, unsigned int reg)
{
    switch (reg) {
    case 0: return dma->next_block;
    case 1: return dma->start_block;
    case 2:
        if (dma->control & DMA_CONTROL_RUNNING) {
            if (++dma->dma_read_count > 2) {
                dma->control &= ~DMA_CONTROL_RUNNING;
                dma->control |= DMA_CONTROL_STOPPED;
                dma->dma_read_count = 0;
            }
        }
        return dma->control;
    case 3: return dma->configuration;
    default: return 0;
    }
}

static void dsp_dma_write(DSPDMAState *dma, unsigned int reg, uint32_t value)
{
    if (reg == 0) dma->next_block = value;
    else if (reg == 1) dma->start_block = value;
    else if (reg == 3) {
        dma->configuration = value;
        if (value & DMA_CONFIGURATION_EOL_CLEAR) dma->eol = false;
        if (value & DMA_CONFIGURATION_ERR_CLEAR) dma->error = false;
    } else if (reg == 2) {
        switch (value & DMA_CONTROL_ACTION) {
        case DMA_CONTROL_ACTION_NOP: break;
        case DMA_CONTROL_ACTION_START:
            dma->control |= DMA_CONTROL_RUNNING;
            dma->control &= ~DMA_CONTROL_STOPPED;
            dma->dma_read_count = 0;
            break;
        case DMA_CONTROL_ACTION_STOP:
        case DMA_CONTROL_ACTION_ABORT:
            dma->control |= DMA_CONTROL_STOPPED;
            dma->control &= ~DMA_CONTROL_RUNNING;
            break;
        case DMA_CONTROL_ACTION_FREEZE:
            dma->control |= DMA_CONTROL_FROZEN;
            break;
        case DMA_CONTROL_ACTION_UNFREEZE:
            dma->control &= ~DMA_CONTROL_FROZEN;
            break;
        default:
            dsp_dma_fail(dma, "invalid-control-action");
            break;
        }
        dsp_dma_run(dma);
    }
}

static uint32_t dsp_read_peripheral(void *opaque, uint32_t address)
{
    DSPState *dsp = (DSPState *)opaque;
    switch (address) {
    case 0xffffb3u:
        /* Match MCPX/Xemu hardware behavior.  This register is not the host
         * emulator's execution counter: Xbox DSP firmware samples it before
         * and after a frame and interprets a large delta as an EP overrun.
         * Exposing the JIT's synthetic cycle count here sent Mercenaries down
         * that error path on its first authored EP frame. */
        return 0u;
    case 0xffffc5u:
        return dsp->interrupts | (dsp->dma.eol ? INTERRUPT_DMA_EOL : 0);
    case 0xffffd4u: return dsp_dma_read(&dsp->dma, 0);
    case 0xffffd5u: return dsp_dma_read(&dsp->dma, 1);
    case 0xffffd6u: return dsp_dma_read(&dsp->dma, 2);
    case 0xffffd7u: return dsp_dma_read(&dsp->dma, 3);
    default: return 0x0ababau;
    }
}

static void dsp_write_peripheral(void *opaque, uint32_t address,
                                 uint32_t value)
{
    DSPState *dsp = (DSPState *)opaque;
    switch (address) {
    case 0xffffc4u:
        if (value & 1u) dsp56300_set_halt_requested(dsp->jit, true);
        break;
    case 0xffffc5u:
        dsp->interrupts &= ~value;
        if (value & INTERRUPT_DMA_EOL) dsp->dma.eol = false;
        break;
    case 0xffffd4u: dsp_dma_write(&dsp->dma, 0, value); break;
    case 0xffffd5u: dsp_dma_write(&dsp->dma, 1, value); break;
    case 0xffffd6u: dsp_dma_write(&dsp->dma, 2, value); break;
    case 0xffffd7u: dsp_dma_write(&dsp->dma, 3, value); break;
    default: break;
    }
}

/* The pinned MIT core models a generic DSP56300 hardware reset. MCPX titles
 * have historically run from the reset state exposed by Xemu's production
 * interpreter instead: OMR=2, 16-bit-linear modulo registers (widened to the
 * JIT's 24-bit representation), and the four architectural exception slots at
 * IPL 3. Normalize through the public state API so firmware execution is
 * reproducible regardless of which instruction engine supplies the reset. */
static void dsp_reset_mcpx(Dsp56300Jit *jit)
{
    static const unsigned int interrupt_slots[4] = { 0u, 2u, 1u, 4u };
    Dsp56300State state;
    unsigned int i;

    dsp56300_reset(jit);
    memset(&state, 0, sizeof(state));
    state.registers[DSP56300_REG_OMR] = 0x000002u;
    for (i = 0; i < 8u; i++)
        state.registers[DSP56300_REG_M0 + i] = 0x0000ffffu;
    state.interrupts.vector_addr = 0x0000ffffu;
    state.interrupts.saved_pc = 0x0000ffffu;
    for (i = 0; i < 4u; i++)
        state.interrupts.ipl[interrupt_slots[i]] = 3;
    dsp56300_set_state(jit, &state);
    dsp56300_invalidate_cache(jit);
}

static DSPState *dsp_create(MCPXAPUState *apu, bool is_gp)
{
    DSPState *dsp = (DSPState *)calloc(1, sizeof(*dsp));
    Dsp56300MemoryRegion x_regions[3];
    Dsp56300MemoryRegion y_regions[2];
    Dsp56300MemoryRegion p_regions[1];
    Dsp56300CreateInfo info;
    if (!dsp) return NULL;

    /* Match the MCPX backend power-on state used by Xemu for both its
     * interpreter and JIT engines.  Bootstrap replaces the loaded half of
     * PRAM, but the title's firmware can observe untouched X/Y/PRAM words
     * during initialization; calloc-zeroing these arrays changes control
     * flow before the first authored audio frame. */
    memset(dsp->core.xram, 0xCA, sizeof(dsp->core.xram));
    memset(dsp->core.yram, 0xCA, sizeof(dsp->core.yram));
    memset(dsp->core.pram, 0xCA, sizeof(dsp->core.pram));

    dsp->is_gp = is_gp;
    dsp->core.is_gp = is_gp;
    dsp->dma.core = &dsp->core;
    dsp->dma.rw_opaque = apu;
    dsp->dma.scratch_rw = is_gp ? gp_scratch_rw : ep_scratch_rw;
    dsp->dma.fifo_rw = is_gp ? gp_fifo_rw : ep_fifo_rw;
    dsp->dma.mem_opaque = dsp;
    dsp->dma.mem_read = dsp_dma_mem_read;
    dsp->dma.mem_write = dsp_dma_mem_write;

    memset(x_regions, 0, sizeof(x_regions));
    x_regions[0].start = 0x0000; x_regions[0].end = 0x1000;
    x_regions[0].kind = DSP56300_REGION_BUFFER;
    x_regions[0].data.buffer.base = dsp->core.xram;
    x_regions[0].data.buffer.offset = 0;
    x_regions[1].start = 0x1400; x_regions[1].end = 0x1800;
    x_regions[1].kind = DSP56300_REGION_BUFFER;
    x_regions[1].data.buffer.base = dsp->core.xram;
    x_regions[1].data.buffer.offset = 0x0c00;
    x_regions[2].start = 0xffff80; x_regions[2].end = 0x1000000;
    x_regions[2].kind = DSP56300_REGION_CALLBACK;
    x_regions[2].data.callback.opaque = dsp;
    x_regions[2].data.callback.read = dsp_read_peripheral;
    x_regions[2].data.callback.write = dsp_write_peripheral;

    memset(y_regions, 0, sizeof(y_regions));
    y_regions[0].start = 0; y_regions[0].end = 0x0800;
    y_regions[0].kind = DSP56300_REGION_BUFFER;
    y_regions[0].data.buffer.base = dsp->core.yram;
    y_regions[0].data.buffer.offset = 0;
    /* MCPX firmware addresses a second Y window at $0800..$17ff.  On the
     * production Xbox DSP model used by Xemu this window is backed by the
     * 4096-word program store: for example, retail Mercenaries deliberately
     * reads Y:$fc9 and receives P:$7c9.  Model that device-specific alias
     * explicitly instead of depending on the interpreter's adjacent host
     * arrays (which would be undefined behavior in this runtime). */
    y_regions[1].start = 0x0800; y_regions[1].end = 0x1800;
    y_regions[1].kind = DSP56300_REGION_BUFFER;
    y_regions[1].data.buffer.base = dsp->core.pram;
    y_regions[1].data.buffer.offset = 0;

    memset(p_regions, 0, sizeof(p_regions));
    p_regions[0].start = 0; p_regions[0].end = 0x1000;
    p_regions[0].kind = DSP56300_REGION_BUFFER;
    p_regions[0].data.buffer.base = dsp->core.pram;

    memset(&info, 0, sizeof(info));
    info.address_register_mask = 0x0000ffffu;
    info.memory_map.x_regions = x_regions;
    info.memory_map.x_count = 3;
    info.memory_map.y_regions = y_regions;
    info.memory_map.y_count = 2;
    info.memory_map.p_regions = p_regions;
    info.memory_map.p_count = 1;
    dsp->jit = dsp56300_create(&info);
    if (!dsp->jit) {
        free(dsp);
        return NULL;
    }
    dsp_reset_mcpx(dsp->jit);
    return dsp;
}

static void dsp_destroy(DSPState *dsp)
{
    if (!dsp) return;
    if (dsp->jit) dsp56300_destroy(dsp->jit);
    free(dsp->dma.scratch_buf);
    free(dsp);
}

static void dsp_bootstrap(DSPState *dsp)
{
    unsigned int i;
    dsp->dma.error = false;
    dsp->dma.scratch_rw(dsp->dma.rw_opaque, (uint8_t *)dsp->core.pram,
                        0, 0x800u * sizeof(uint32_t), false);
    if (dsp->dma.error) {
        fprintf(stderr, "[APU-DSP] %s bootstrap scratch read failed\n",
                dsp->is_gp ? "GP" : "EP");
        return;
    }
    for (i = 0; i < 0x800u; i++) dsp->core.pram[i] &= 0x00ffffffu;
    dsp56300_invalidate_cache(dsp->jit);
    fprintf(stderr,
            "[APU-DSP] %s bootstrap P=%06X/%06X/%06X/%06X\n",
            dsp->is_gp ? "GP" : "EP", dsp->core.pram[0],
            dsp->core.pram[1], dsp->core.pram[2], dsp->core.pram[3]);
}

void mcpx_apu_dsp_reset_write(DSPState *dsp, uint32_t old_value,
                              uint32_t new_value)
{
    bool old_running, new_running;
    if (!dsp || !dsp->jit) return;
    old_running = (old_value & (NV_PAPU_GPRST_GPRST |
                                NV_PAPU_GPRST_GPDSPRST)) ==
                  (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST);
    new_running = (new_value & (NV_PAPU_GPRST_GPRST |
                                NV_PAPU_GPRST_GPDSPRST)) ==
                  (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST);
    if (!new_running) dsp_reset_mcpx(dsp->jit);
    else if (!old_running) dsp_bootstrap(dsp);
}

static bool dsp_step_diagnostic_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TEST_APU_DSP_STEP") != NULL;
    return enabled;
}

static bool dsp_n5_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("MERCENARIES_TRACE_APU_DSP_N5") != NULL;
    return enabled;
}
static uint32_t dsp_frame_cycle_limit(void)
{
    const char *value = getenv("MERCENARIES_TEST_APU_DSP_CYCLE_LIMIT");
    char *end = NULL;
    unsigned long parsed;
    if (!value || !*value) return DSP_FRAME_CYCLE_LIMIT;
    parsed = strtoul(value, &end, 10);
    if (!end || *end || parsed < 1000ul || parsed > 100000000ul)
        return DSP_FRAME_CYCLE_LIMIT;
    return (uint32_t)parsed;
}

static void dsp_dump_pram_if_requested(DSPState *dsp)
{
    const char *request = getenv("MERCENARIES_DUMP_APU_DSP_PRAM");
    const char *name;
    FILE *file;
    uint32_t address;

    if (!request || !dsp || !dsp->jit) return;
    name = dsp->is_gp ? "mercenaries_gp_pram.u24be" :
                        "mercenaries_ep_pram.u24be";
    file = fopen(name, "wb");
    if (!file) return;
    for (address = 0; address < DSP_PRAM_SIZE; address++) {
        uint32_t word = mcpx_apu_dsp_read_memory(dsp, 'P', address);
        unsigned char bytes[3] = {
            (unsigned char)(word >> 16),
            (unsigned char)(word >> 8),
            (unsigned char)word,
        };
        if (fwrite(bytes, sizeof(bytes), 1, file) != 1) break;
    }
    fclose(file);
}

static bool g_ep_trace_completed;
static bool g_ep_trace_active;
static Dsp56300State g_ep_trace_state;

typedef struct DSPDiagnosticSnapshot {
    char magic[8];
    Dsp56300State state;
    uint32_t xram[DSP_XRAM_SIZE];
    uint32_t yram[DSP_YRAM_SIZE];
    uint32_t pram[DSP_PRAM_SIZE];
    uint32_t interrupts;
    uint32_t dma_configuration;
    uint32_t dma_control;
    uint32_t dma_start_block;
    uint32_t dma_next_block;
    uint32_t dma_eol;
    uint32_t dma_read_count;
} DSPDiagnosticSnapshot;

static bool dsp_dump_state_to_path(DSPState *dsp,
                                   const Dsp56300State *state,
                                   const char *path)
{
    DSPDiagnosticSnapshot *snapshot;
    FILE *file;
    bool written = false;

    if (!path || !*path || !dsp || !state || dsp->is_gp)
        return false;
    snapshot = (DSPDiagnosticSnapshot *)calloc(1, sizeof(*snapshot));
    if (!snapshot) return false;
    memcpy(snapshot->magic, "DSPSS001", sizeof(snapshot->magic));
    snapshot->state = *state;
    memcpy(snapshot->xram, dsp->core.xram, sizeof(snapshot->xram));
    memcpy(snapshot->yram, dsp->core.yram, sizeof(snapshot->yram));
    memcpy(snapshot->pram, dsp->core.pram, sizeof(snapshot->pram));
    snapshot->interrupts = dsp->interrupts;
    snapshot->dma_configuration = dsp->dma.configuration;
    snapshot->dma_control = dsp->dma.control;
    snapshot->dma_start_block = dsp->dma.start_block;
    snapshot->dma_next_block = dsp->dma.next_block;
    snapshot->dma_eol = dsp->dma.eol ? 1u : 0u;
    snapshot->dma_read_count = dsp->dma.dma_read_count;
    file = fopen(path, "wb");
    if (file) {
        if (fwrite(snapshot, sizeof(*snapshot), 1, file) == 1) {
            written = true;
            fprintf(stderr,
                    "[APU-DSP] EP diagnostic snapshot pc=%06X path=%s\n",
                    state->pc, path);
        }
        fclose(file);
    }
    free(snapshot);
    return written;
}

static void dsp_dump_state_if_requested(DSPState *dsp,
                                        const Dsp56300State *state)
{
    static bool dumped;
    if (!dumped) {
        dumped = dsp_dump_state_to_path(
            dsp, state, getenv("MERCENARIES_DUMP_APU_DSP_STATE_PATH"));
    }
}

static bool dsp_run_frame(DSPState *dsp)
{
    uint32_t trace_n5 = 0;
    uint32_t trace_x63c = 0;
    uint32_t trace_dma_descriptor[7] = { 0 };
    uint32_t trace_previous_cycles = 0;
    uint32_t trace_previous_pc = 0;
    uint32_t cycles;
    if (dsp_n5_trace_enabled() && !dsp->is_gp && !g_ep_trace_completed) {
        g_ep_trace_active = true;
        memset(&g_ep_trace_state, 0, sizeof(g_ep_trace_state));
        dsp56300_get_state(dsp->jit, &g_ep_trace_state);
        trace_n5 = g_ep_trace_state.registers[DSP56300_REG_N5];
        trace_x63c = mcpx_apu_dsp_read_memory(dsp, 'X', 0x063cu);
        {
            unsigned int word;
            for (word = 0; word < 7u; word++)
                trace_dma_descriptor[word] = mcpx_apu_dsp_read_memory(
                    dsp, 'P', 0x01a2u + word);
        }
        fprintf(stderr,
                "[APU-DSP] EP N5 trace start pc=%06X n5=%06X "
                "r1=%06X r5=%06X n4=%06X y0=%06X y1=%06X "
                "x63c=%06X\n",
                g_ep_trace_state.pc, trace_n5,
                g_ep_trace_state.registers[DSP56300_REG_R1],
                g_ep_trace_state.registers[DSP56300_REG_R5],
                g_ep_trace_state.registers[DSP56300_REG_N4],
                g_ep_trace_state.registers[DSP56300_REG_Y0],
                g_ep_trace_state.registers[DSP56300_REG_Y1], trace_x63c);
    }
    dsp->interrupts |= INTERRUPT_START_FRAME;
    dsp56300_set_halt_requested(dsp->jit, false);
    dsp56300_set_cycle_count(dsp->jit, 0);
    while (!dsp56300_halt_requested(dsp->jit)) {
        if (g_ep_trace_active) {
            trace_previous_cycles = g_ep_trace_state.cycle_count;
            trace_previous_pc = g_ep_trace_state.pc;
        }
        if ((dsp_step_diagnostic_enabled() || g_ep_trace_active) && !dsp->is_gp)
            dsp56300_step(dsp->jit);
        else
            dsp56300_run(dsp->jit, 1000);
        cycles = dsp56300_cycle_count(dsp->jit);
        if (g_ep_trace_active) {
            dsp56300_get_state(dsp->jit, &g_ep_trace_state);
            if (trace_previous_pc == 0x03eau &&
                g_ep_trace_state.registers[DSP56300_REG_N1] > 0x000100u) {
                uint32_t r2 = g_ep_trace_state.registers[DSP56300_REG_R2];
                uint32_t n2 = g_ep_trace_state.registers[DSP56300_REG_N2];
                int32_t signed_n2 = (int16_t)n2;
                fprintf(stderr,
                        "[APU-DSP] EP suspicious command-table read "
                        "cycles=%u writer_pc=%06X next_pc=%06X "
                        "r2=%06X n2=%06X signed_n2=%d "
                        "linear_ea=%06X n1=%06X\n",
                        cycles, trace_previous_pc, g_ep_trace_state.pc,
                        r2, n2, signed_n2,
                        (r2 + (uint32_t)signed_n2) & 0x0000ffffu,
                        g_ep_trace_state.registers[DSP56300_REG_N1]);
            }
            if (mcpx_apu_dsp_read_memory(dsp, 'P', 0x05c3u) == 0x0597d6u)
                dsp_dump_state_if_requested(dsp, &g_ep_trace_state);
            if ((uint32_t)(g_ep_trace_state.cycle_count -
                           trace_previous_cycles) > 100u) {
                fprintf(stderr,
                        "[APU-DSP] EP anomalous instruction cycles=%u "
                        "pc=%06X opcode=%06X next=%06X\n",
                        (uint32_t)(g_ep_trace_state.cycle_count -
                                   trace_previous_cycles),
                        trace_previous_pc,
                        mcpx_apu_dsp_read_memory(dsp, 'P', trace_previous_pc),
                        mcpx_apu_dsp_read_memory(
                            dsp, 'P', (trace_previous_pc + 1u) & 0x0fffu));
            }
            if (g_ep_trace_state.registers[DSP56300_REG_N5] != trace_n5) {
                trace_n5 = g_ep_trace_state.registers[DSP56300_REG_N5];
                fprintf(stderr,
                        "[APU-DSP] EP N5 change cycles=%u pc=%06X n5=%06X "
                        "r1=%06X r5=%06X n4=%06X y0=%06X y1=%06X\n",
                        cycles, g_ep_trace_state.pc, trace_n5,
                        g_ep_trace_state.registers[DSP56300_REG_R1],
                        g_ep_trace_state.registers[DSP56300_REG_R5],
                        g_ep_trace_state.registers[DSP56300_REG_N4],
                        g_ep_trace_state.registers[DSP56300_REG_Y0],
                        g_ep_trace_state.registers[DSP56300_REG_Y1]);
            }
            {
                uint32_t current_x63c =
                    mcpx_apu_dsp_read_memory(dsp, 'X', 0x063cu);
                if (current_x63c != trace_x63c) {
                    fprintf(stderr,
                            "[APU-DSP] EP X:063C change cycles=%u "
                            "writer_pc=%06X next_pc=%06X old=%06X "
                            "new=%06X opcode=%06X next=%06X\n",
                            cycles, trace_previous_pc,
                            g_ep_trace_state.pc, trace_x63c, current_x63c,
                            mcpx_apu_dsp_read_memory(
                                dsp, 'P', trace_previous_pc),
                            mcpx_apu_dsp_read_memory(
                                dsp, 'P',
                                (trace_previous_pc + 1u) & 0x0fffu));
                    trace_x63c = current_x63c;
                }
            }
            {
                unsigned int word;
                for (word = 0; word < 7u; word++) {
                    uint32_t current = mcpx_apu_dsp_read_memory(
                        dsp, 'P', 0x01a2u + word);
                    if (current != trace_dma_descriptor[word]) {
                        fprintf(stderr,
                                "[APU-DSP] EP DMA descriptor P:%04X change "
                                "cycles=%u writer_pc=%06X next_pc=%06X "
                                "old=%06X new=%06X opcode=%06X next=%06X "
                                "r0=%06X r1=%06X r4=%06X n0=%06X "
                                "n1=%06X n4=%06X\n",
                                0x01a2u + word, cycles, trace_previous_pc,
                                g_ep_trace_state.pc,
                                trace_dma_descriptor[word], current,
                                mcpx_apu_dsp_read_memory(
                                    dsp, 'P', trace_previous_pc),
                                mcpx_apu_dsp_read_memory(
                                    dsp, 'P',
                                    (trace_previous_pc + 1u) & 0x0fffu),
                                g_ep_trace_state.registers[DSP56300_REG_R0],
                                g_ep_trace_state.registers[DSP56300_REG_R1],
                                g_ep_trace_state.registers[DSP56300_REG_R4],
                                g_ep_trace_state.registers[DSP56300_REG_N0],
                                g_ep_trace_state.registers[DSP56300_REG_N1],
                                g_ep_trace_state.registers[DSP56300_REG_N4]);
                        trace_dma_descriptor[word] = current;
                    }
                }
            }
        }
        if (cycles >= dsp_frame_cycle_limit() || dsp->dma.error) {
            Dsp56300State state;
            const char *failure_path;
            memset(&state, 0, sizeof(state));
            dsp56300_get_state(dsp->jit, &state);
            failure_path = getenv(
                "MERCENARIES_DUMP_APU_DSP_FAILURE_STATE_PATH");
            dsp_dump_state_to_path(dsp, &state, failure_path);
            fprintf(stderr,
                    "[APU-DSP] %s frame failed cycles=%u state_cycles=%u "
                    "trace=%u pc=%06X sr=%06X lc=%06X dma_error=%u "
                    "dma_next=%06X dma_control=%06X dma_reads=%u "
                    "dma_config=%06X step=%u\n",
                    dsp->is_gp ? "GP" : "EP", cycles, state.cycle_count,
                    g_ep_trace_active ? 1u : 0u, state.pc,
                    state.registers[DSP56300_REG_SR],
                    state.registers[DSP56300_REG_LC],
                    dsp->dma.error ? 1u : 0u, dsp->dma.next_block,
                    dsp->dma.control, dsp->dma.dma_read_count,
                    dsp->dma.configuration,
                    dsp_step_diagnostic_enabled() ? 1u : 0u);
            {
                uint32_t pc = state.pc;
                int delta;
                fprintf(stderr,
                        "[APU-DSP] %s regs x1=%06X a=%02X:%06X "
                        "b=%02X:%06X r5=%06X n5=%06X m5=%06X\n",
                        dsp->is_gp ? "GP" : "EP",
                        state.registers[DSP56300_REG_X1],
                        state.registers[DSP56300_REG_A2],
                        state.registers[DSP56300_REG_A1],
                        state.registers[DSP56300_REG_B2],
                        state.registers[DSP56300_REG_B1],
                        state.registers[DSP56300_REG_R5],
                        state.registers[DSP56300_REG_N5],
                        state.registers[DSP56300_REG_M5]);
                fprintf(stderr,
                        "[APU-DSP] %s loop regs r0=%06X r1=%06X "
                        "r2=%06X r4=%06X r6=%06X n0=%06X n1=%06X "
                        "n2=%06X n4=%06X n6=%06X la=%06X\n",
                        dsp->is_gp ? "GP" : "EP",
                        state.registers[DSP56300_REG_R0],
                        state.registers[DSP56300_REG_R1],
                        state.registers[DSP56300_REG_R2],
                        state.registers[DSP56300_REG_R4],
                        state.registers[DSP56300_REG_R6],
                        state.registers[DSP56300_REG_N0],
                        state.registers[DSP56300_REG_N1],
                        state.registers[DSP56300_REG_N2],
                        state.registers[DSP56300_REG_N4],
                        state.registers[DSP56300_REG_N6],
                        state.registers[DSP56300_REG_LA]);
                fprintf(stderr, "[APU-DSP] %s X kernel config:",
                        dsp->is_gp ? "GP" : "EP");
                for (delta = 0; delta <= 0x59; delta++) {
                    uint32_t address = 0x0633u + (uint32_t)delta;
                    fprintf(stderr, " %04X:%06X", address,
                            mcpx_apu_dsp_read_memory(dsp, 'X', address));
                }
                fputc('\n', stderr);
                fprintf(stderr,
                        "[APU-DSP] %s stack sp=%06X sc=%06X ep=%06X "
                        "sz=%06X omr=%06X ssh=%06X ssl=%06X\n",
                        dsp->is_gp ? "GP" : "EP",
                        state.registers[DSP56300_REG_SP],
                        state.registers[DSP56300_REG_SC],
                        state.registers[DSP56300_REG_EP],
                        state.registers[DSP56300_REG_SZ],
                        state.registers[DSP56300_REG_OMR],
                        state.registers[DSP56300_REG_SSH],
                        state.registers[DSP56300_REG_SSL]);
                fprintf(stderr, "[APU-DSP] %s X stack extension:",
                        dsp->is_gp ? "GP" : "EP");
                for (delta = 0; delta < 32; delta++) {
                    uint32_t address = (0x0be0u + (uint32_t)delta) & 0x0fffu;
                    fprintf(stderr, " %04X:%06X", address,
                            mcpx_apu_dsp_read_memory(dsp, 'X', address));
                }
                fputc('\n', stderr);
                fprintf(stderr, "[APU-DSP] %s P context:",
                        dsp->is_gp ? "GP" : "EP");
                for (delta = -16; delta <= 24; delta++) {
                    uint32_t address = (pc + (uint32_t)delta) & 0x0fffu;
                    fprintf(stderr, " %04X:%06X", address,
                            mcpx_apu_dsp_read_memory(dsp, 'P', address));
                }
                fputc('\n', stderr);
            }
            g_ep_trace_completed = g_ep_trace_active;
            g_ep_trace_active = false;
            dsp_dump_pram_if_requested(dsp);
            dsp->execution_faulted = true;
            dsp56300_set_halt_requested(dsp->jit, true);
            return false;
        }
    }
    g_ep_trace_completed = g_ep_trace_active;
    g_ep_trace_active = false;
    return true;
}

static void dsp_use_fallback(MCPXAPUState *d, const char *reason)
{
    static int warned;
    if (!warned) {
        fprintf(stderr, "[APU] DSP disabled: %s; using %s fallback\n",
                reason, speaker_downmix_enabled() ?
                "authored speaker-mixbin" : "diagnostic VP-monitor");
        warned = 1;
    }
    d->gp.realtime = false;
    d->ep.realtime = false;
    d->monitor.point = speaker_downmix_enabled() ?
        MCPX_APU_DEBUG_MON_AC97 : MCPX_APU_DEBUG_MON_VP;
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    d->gp.dsp = dsp_create(d, true);
    d->ep.dsp = dsp_create(d, false);
    if (!d->gp.dsp || !dsp_enabled()) {
        dsp_use_fallback(d, !dsp_enabled() ? "full DSP execution not enabled" :
                                           "DSP56300 initialization failed");
    } else {
        d->gp.realtime = true;
        d->ep.realtime = d->ep.dsp && ep_dsp_enabled();
        d->monitor.point = d->ep.realtime ? MCPX_APU_DEBUG_MON_GP_OR_EP :
                                            MCPX_APU_DEBUG_MON_GP;
        fprintf(stderr,
                "[APU] DSP GP initialized%s (DSP56300 JIT v0.1.3)\n",
                d->ep.realtime ? " with experimental EP" : "");
    }
}

void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    dsp_destroy(d->gp.dsp);
    dsp_destroy(d->ep.dsp);
    d->gp.dsp = NULL;
    d->ep.dsp = NULL;
}

void mcpx_apu_dsp_reset_all(MCPXAPUState *d)
{
    if (d->gp.dsp && d->gp.dsp->jit) dsp_reset_mcpx(d->gp.dsp->jit);
    if (d->ep.dsp && d->ep.dsp->jit) dsp_reset_mcpx(d->ep.dsp->jit);
    d->ep_frame_div = 0;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    if (!dsp_enabled() || !d->gp.dsp || !d->gp.dsp->jit ||
        d->gp.dsp->dma.error || d->gp.dsp->execution_faulted) {
        dsp_use_fallback(d, "GP DSP execution unavailable");
    } else if (!ep_dsp_enabled() || !d->ep.dsp || !d->ep.dsp->jit ||
               d->ep.dsp->dma.error || d->ep.dsp->execution_faulted) {
        d->monitor.point = MCPX_APU_DEBUG_MON_GP;
        d->gp.realtime = true;
        d->ep.realtime = false;
    } else {
        d->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
        d->gp.realtime = true;
        d->ep.realtime = true;
    }
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    bool gp_enabled, ep_enabled;
    unsigned int mixbin, sample;
    if (!d->gp.realtime || !d->gp.dsp) {
        if (speaker_downmix_enabled()) {
            int offset = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            for (sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
                float left, right;
                fallback_speaker_sample(mixbins, sample, &left, &right);
                if (left > 1.0f) left = 1.0f;
                if (left < -1.0f) left = -1.0f;
                if (right > 1.0f) right = 1.0f;
                if (right < -1.0f) right = -1.0f;
                d->monitor.frame_buf[offset + sample][0] =
                    (int16_t)(left * 32767.0f);
                d->monitor.frame_buf[offset + sample][1] =
                    (int16_t)(right * 32767.0f);
            }
        }
        g_dbg.gp.cycles = 0;
        g_dbg.ep.cycles = 0;
        return;
    }

    for (mixbin = 0; mixbin < NUM_MIXBINS; mixbin++) {
        uint32_t base = GP_DSP_MIXBUF_BASE +
                        mixbin * NUM_SAMPLES_PER_FRAME;
        for (sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
            mcpx_apu_dsp_write_memory(d->gp.dsp, 'X', base + sample,
                                      float_to_24b(mixbins[mixbin][sample]));
        }
    }

    gp_enabled = (d->gp.regs[NV_PAPU_GPRST] &
                  (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST)) ==
                 (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST);
    ep_enabled = d->ep.realtime &&
                 (d->ep.regs[NV_PAPU_EPRST] &
                  (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST)) ==
                 (NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST);

    if (gp_enabled) {
        if (!dsp_run_frame(d->gp.dsp)) {
            dsp_use_fallback(d, "GP did not complete a frame");
            return;
        }
        g_dbg.gp.cycles = (int)dsp56300_cycle_count(d->gp.dsp->jit);
        if (d->monitor.point == MCPX_APU_DEBUG_MON_GP ||
            (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP && !ep_enabled)) {
            int offset = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            for (sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
                uint32_t left = mcpx_apu_dsp_read_memory(
                    d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + sample);
                uint32_t right = mcpx_apu_dsp_read_memory(
                    d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE +
                    NUM_SAMPLES_PER_FRAME + sample);
                d->monitor.frame_buf[offset + sample][0] =
                    (int16_t)(left >> 8);
                d->monitor.frame_buf[offset + sample][1] =
                    (int16_t)(right >> 8);
            }
        }
    } else g_dbg.gp.cycles = 0;

    if (ep_enabled && d->ep_frame_div % 8 == 0) {
        if (!dsp_run_frame(d->ep.dsp)) {
            fprintf(stderr,
                    "[APU] EP DSP unavailable; retaining GP effects output\n");
            d->monitor.point = MCPX_APU_DEBUG_MON_GP;
            d->ep.realtime = false;
            return;
        }
        g_dbg.ep.cycles = (int)dsp56300_cycle_count(d->ep.dsp->jit);
    } else g_dbg.ep.cycles = 0;
}

#else

uint32_t mcpx_apu_dsp_read_memory(DSPState *dsp, char space,
                                  uint32_t address)
{
    uint32_t *memory = space == 'Y' ? dsp->core.yram :
                       space == 'P' ? dsp->core.pram : dsp->core.xram;
    return memory[address] & 0x00ffffffu;
}

void mcpx_apu_dsp_write_memory(DSPState *dsp, char space,
                               uint32_t address, uint32_t value)
{
    uint32_t *memory = space == 'Y' ? dsp->core.yram :
                       space == 'P' ? dsp->core.pram : dsp->core.xram;
    memory[address] = value & 0x00ffffffu;
}

void mcpx_apu_dsp_reset_write(DSPState *dsp, uint32_t old_value,
                              uint32_t new_value)
{
    (void)dsp; (void)old_value; (void)new_value;
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->monitor.point = speaker_downmix_enabled() ?
        MCPX_APU_DEBUG_MON_AC97 : MCPX_APU_DEBUG_MON_VP;
}

void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    free(d->gp.dsp); free(d->ep.dsp);
}

void mcpx_apu_dsp_reset_all(MCPXAPUState *d) { d->ep_frame_div = 0; }
void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    d->monitor.point = speaker_downmix_enabled() ?
        MCPX_APU_DEBUG_MON_AC97 : MCPX_APU_DEBUG_MON_VP;
}
void mcpx_apu_dsp_frame(MCPXAPUState *d,
                        float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    if (speaker_downmix_enabled()) {
        int offset = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
        for (unsigned int sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
            float left, right;
            fallback_speaker_sample(mixbins, sample, &left, &right);
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;
            d->monitor.frame_buf[offset + sample][0] =
                (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[offset + sample][1] =
                (int16_t)(right * 32767.0f);
        }
    }
}

#endif

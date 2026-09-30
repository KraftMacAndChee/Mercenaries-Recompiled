/*
 * NV2A MMIO Hook - VEH instruction decoder for GPU register access
 *
 * When recompiled code accesses NV2A MMIO registers (0xFD000000+),
 * the access faults because no physical page is mapped. This module
 * decodes the faulting x86-64 instruction, extracts the read/write
 * operation, routes it through the NV2A register handlers, and
 * advances RIP past the instruction.
 *
 * This is the key bridge between recompiled Xbox D3D8 code and
 * the xemu NV2A GPU emulation.
 */

#ifndef BURNOUT3_NV2A_MMIO_HOOK_H
#define BURNOUT3_NV2A_MMIO_HOOK_H

#include "platform/xbox_winnt.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Initialize the NV2A GPU subsystem.
 * Call this during startup before the game code runs.
 * Borrows guest RAM as VRAM, maps its aperture, allocates RAMIN, and
 * initializes register state.
 */
void nv2a_hook_init(ptrdiff_t xbox_mem_offset,
                    HANDLE system_memory_mapping);

/* Service any completed PGRAPH resolve before the host refreshes scanout.
 * Returns true only when a distinct pending guest frame was consumed. */
bool nv2a_hook_service_scanout(void);

/*
 * Handle an NV2A register access fault (guest 0xFD000000-0xFDFFFFFF).
 * Decodes the host instruction, routes the read/write through register
 * handlers, and updates CPU context. Returns true only when handled.
 * False leaves the fault unhandled for the caller's exception path;
 * allocating a zero page is not a substitute for register emulation.
 */
bool nv2a_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write);

/* Handle faults in the framebuffer/push-buffer range selected by the caller.
 * The physical VRAM aperture is mapped eagerly; faults within it return
 * false rather than creating incoherent private pages. Outside that aperture,
 * this handler attempts to allocate a zero-filled 64 KiB demand page.
 * Returns true only when the demand-page allocation succeeds. */
bool nv2a_hook_handle_vram(uintptr_t fault_addr, uint32_t fault_xbox_va);

#endif /* BURNOUT3_NV2A_MMIO_HOOK_H */

/*
 * MCPX APU MMIO fault bridge for the Windows recomp host.
 */
#pragma once

#include "apu.h"

#if defined(_WIN32)
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern MCPXAPUState *g_apu_state;

/* Direct lifted accesses share the exact handlers used by the fault bridge. */
int apu_hook_try_read32(uint32_t address, uint32_t *value);
int apu_hook_try_write32(uint32_t address, uint32_t value);

void ac97_hook_init(void);
bool ac97_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                           uint32_t fault_xbox_va, int is_write);

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                          uint32_t fault_xbox_va, int is_write);

#ifdef __cplusplus
}
#endif
#endif

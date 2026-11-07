/* Plugin Interface 2 guest calls (included by recomp_manual.c after the guest
 * CPU context helpers). A call saves the complete translated CPU state,
 * reserves scratch memory below the live guest stack, runs the translated
 * function and puts everything back on end, so the interrupted guest code
 * continues exactly as before. Nesting is bounded. */
#ifndef MERCENARIES_PLUGIN_GUEST_CALLS_H
#define MERCENARIES_PLUGIN_GUEST_CALLS_H
#include "plugin_hooks.h"

#define PLUGIN_GUEST_CALL_DEPTH 4u
static recomp_saved_guest_cpu_context g_plugin_call_saved[PLUGIN_GUEST_CALL_DEPTH];
static uint32_t g_plugin_call_base[PLUGIN_GUEST_CALL_DEPTH];
static uint32_t g_plugin_call_depth;

int recomp_plugin_guest_call_begin(uint32_t scratch_bytes, uint32_t *scratch_address)
{
    const uint32_t bytes = (scratch_bytes + 15u) & ~15u;
    if (g_plugin_call_depth >= PLUGIN_GUEST_CALL_DEPTH || g_xbox_mem_offset == 0 ||
        bytes > 4096u || g_esp < 0x00012000u + bytes + 0x1000u)
        return 0;
    recomp_save_guest_cpu_context(&g_plugin_call_saved[g_plugin_call_depth]);
    /* Leave a red zone below the interrupted function's stack top. */
    g_esp = (g_esp - 0x100u - bytes) & ~0xFu;
    g_plugin_call_base[g_plugin_call_depth] = g_esp;
    *scratch_address = g_esp;
    ++g_plugin_call_depth;
    return 1;
}

int recomp_plugin_guest_call(uint32_t function, uint32_t this_ptr, const uint32_t *args,
                             uint32_t arg_count, uint32_t *eax)
{
    if (g_plugin_call_depth == 0u || arg_count > 16u) return 0;
    recomp_func_t fn = recomp_lookup(function);
    if (fn == NULL) return 0;
    const uint32_t base = g_plugin_call_base[g_plugin_call_depth - 1u];
    g_esp = base;
    for (uint32_t i = arg_count; i-- > 0u;) recomp_guest_push_u32(args[i]);
    recomp_guest_push_u32(0u); /* translated call return slot */
    g_ecx = this_ptr;
    fn();
    if (eax) *eax = g_eax;
    g_esp = base; /* undo callee or caller cleanup alike */
    return 1;
}

void recomp_plugin_guest_call_end(void)
{
    if (g_plugin_call_depth == 0u) return;
    --g_plugin_call_depth;
    recomp_restore_guest_cpu_context(&g_plugin_call_saved[g_plugin_call_depth]);
}
#endif

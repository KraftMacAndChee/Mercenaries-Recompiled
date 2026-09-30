/* Included after dev_spawn.h in recomp_manual.c. The master Lua state is
 * owned by RsLuaMission (00111F20), distinct from the temporary briefing VM.
 * ResetToBaseLayerOnly sets a deferred flag; it does not destroy this VM here. */
#include "dev_missions.h"
#include "free_cam.h"
extern void recomp_dev_missions_set_province(int province);
extern unsigned recomp_dev_missions_take(void);

void recomp_dev_missions_tick(void)
{
    static int executing;
    if (executing) return;
    uint32_t map = g_xbox_mem_offset ? guest_u32(0x403970u) : 0;
    int province = map == 0x4A5220AFu ? 0 : map == 0x4A364A32u ? 1 : -1;
    recomp_dev_missions_set_province(province);
    unsigned command = recomp_dev_missions_take();
    if (!command) return;
    char script[768];
    if (!recomp_dev_menu_allowed() || !recomp_dev_mission_script(command, script, sizeof(script)) ||
        !g_xbox_mem_offset || province < 0 || g_esp < 0x20000u || g_esp >= 0x4000000u ||
        guest_u32(0x413F6Cu) != 0x4249D707u || guest_u32(0x413F68u) != 0xC2CBD863u) {
        recomp_dev_transition_result(0, "Enter normal North/South province gameplay first.");
        return;
    }
    uint32_t vm = guest_u32(0x3717BCu);
    if (!dev_guest_address(vm, 0xA0u) || *(uint8_t *)guest_ptr(0x3717D6u)) {
        recomp_dev_transition_result(0, "The master script is not ready or a transition is pending.");
        return;
    }
    const uint32_t calls[] = {0x001DC970u, 0x001DC980u, 0x001DC480u, 0x001DD5A0u};
    for (unsigned i = 0; i < sizeof(calls)/sizeof(calls[0]); ++i)
        if (!recomp_lookup(calls[i])) {
            recomp_dev_transition_result(0, "Required retail script functions are unavailable.");
            return;
        }
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    executing = 1;
    uint32_t stack = (g_esp - 0x4000u) & ~15u, text = stack + 0x100u;
    uint32_t name = stack + 0x500u;
    memcpy(guest_ptr(text), script, strlen(script) + 1);
    strcpy(guest_ptr(name), "developer transition");
    uint32_t top = dev_call(stack, 0x001DC970u, 0, 1, &vm);
    uint32_t load[] = {vm, text, (uint32_t)strlen(script), name};
    uint32_t error = dev_call(stack, 0x001DC480u, 0, 4, load); /* luaL_loadbuffer */
    if (!error) {
        uint32_t call[] = {vm, 0, 0, 0};
        error = dev_call(stack, 0x001DD5A0u, 0, 4, call); /* lua_pcall */
    }
    if (error) recomp_lua_pcall_checkpoint(vm, error, name);
    uint32_t restore[] = {vm, top};
    dev_call(stack, 0x001DC980u, 0, 2, restore);
    int accepted = !error && *(uint8_t *)guest_ptr(0x3717D6u) != 0;
    recomp_restore_guest_cpu_context(&saved);
    executing = 0;
    if (accepted) recomp_freecam_set(0);
    xbox_preview_log_event("dev-transition", "command=%04X map=%08X status=%u accepted=%d", command, map, error, accepted);
    recomp_dev_transition_result(accepted, accepted ? "Transition requested through the original game script." :
        "Transition rejected. Travel requires no active contract; see the preview log for details.");
}

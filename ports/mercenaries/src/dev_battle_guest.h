/* Runtime-only targeting policy. Faction standings and save data are untouched. */
#include "dev_battle.h"
static uint32_t dev_battle_player;
void recomp_dev_battle_refresh_player(void)
{
    dev_battle_player=0;
    if(!recomp_dev_battle_flags() || !g_xbox_mem_offset ||
       guest_u32(0x413F6C)!=0x4249D707u || g_esp<0x20000u || g_esp>=0x4000000u ||
       !recomp_lookup(0x8C7E0))return;
    recomp_saved_guest_cpu_context saved;recomp_save_guest_cpu_context(&saved);
    uint32_t name=0x660E4490u;
    uint32_t actor=dev_call((g_esp-0x4000u)&~15u,0x8C7E0,0,1,&name);
    if(dev_guest_address(actor,0x76C) && guest_u32(actor+4)==name)dev_battle_player=actor;
    recomp_restore_guest_cpu_context(&saved);
}
int recomp_dev_battle_protected(uint32_t ai, uint32_t target)
{
    unsigned flags = recomp_dev_battle_flags();
    if (!flags || !g_xbox_mem_offset || !dev_guest_address(ai,0x4A8) ||
        !dev_guest_address(target,0x100) || guest_u32(0x413F6C)!=0x4249D707u)
        return 0;
    uint32_t human=dev_battle_player;
    if(dev_guest_address(human,0x76C) && guest_u32(human+4)==0x660E4490u &&
       ai==guest_u32(human+0x200))return 0;
    uint32_t owner=guest_u32(ai+0x10);
    if (dev_guest_address(owner,8) && guest_u32(owner+4)==0x660E4490u) return 0; /* The camera operator can still fire. */
    if (flags & DEV_BATTLE_PASSIVE) return 1;
    if (guest_u32(target+4)==0x660E4490u) return 1;
    if (!dev_guest_address(human,0x76C) || guest_u32(human+4)!=0x660E4490u) return 0;
    uint32_t seat=guest_u32(human+0x768);
    if (!dev_guest_address(seat,4)) return 0;
    uint32_t manager=guest_u32(seat);
    return dev_guest_address(manager,4) && target==guest_u32(manager);
}

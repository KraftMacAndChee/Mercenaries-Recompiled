#include "dev_factions.h"

/* A one-shot edit through the retail setter, not a permanent AI override. */
void recomp_dev_relations_tick(void)
{
    unsigned command=recomp_dev_take_relation();
    if(!command)return;
    unsigned a=command&7u,b=(command>>3u)&7u,relation=(command>>6u)&3u;
    if(command!=dev_relation_command(a,b,relation) || !recomp_dev_menu_allowed() ||
       !g_xbox_mem_offset || g_esp<0x20000u || g_esp>=0x4000000u ||
       guest_u32(0x413F6Cu)!=0x4249D707u || guest_u32(0x413F68u)!=0xC2CBD863u ||
       !recomp_lookup(0x00095B50u)){
        recomp_dev_relation_result(0,"Enter normal gameplay before changing faction relations.");return;
    }
    recomp_saved_guest_cpu_context saved;recomp_save_guest_cpu_context(&saved);
    uint32_t stack=(g_esp-0x4000u)&~15u;
    uint32_t args[]={a+1u,b+1u,recomp_float_bits(dev_relation_values[relation])};
    dev_call(stack,0x00095B50u,0x00323338u,3,args);
    args[0]=b+1u;args[1]=a+1u;dev_call(stack,0x00095B50u,0x00323338u,3,args);
    recomp_restore_guest_cpu_context(&saved);
    char message[192];snprintf(message,sizeof(message),"%s and %s are now %s.",
        dev_faction_names[a],dev_faction_names[b],dev_relation_names[relation]);
    xbox_preview_log_event("dev-relations","first=%u second=%u standing=%.3f",a+1u,b+1u,dev_relation_values[relation]);
    recomp_dev_relation_result(1,message);
}

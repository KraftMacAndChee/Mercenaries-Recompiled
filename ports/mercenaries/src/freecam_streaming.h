/* Free Cam needs the retail world activation pass at both observers. Keep the
 * mercenary's collision/actors resident and use the same authored distances at
 * the remote camera. Only the base RedWorld pass is repeated: actor simulation,
 * traffic spawning, mission timers and population queue maintenance run once. */
#ifndef MERC_FREECAM_STREAMING_H
#define MERC_FREECAM_STREAMING_H
#include "free_cam.h"
static unsigned freecam_world_pass;
static float freecam_world_player_focus[3],freecam_world_view_focus[3];
int recomp_freecam_world_secondary(void) { return freecam_world_pass==2; }
int recomp_freecam_stream_keep(uint32_t spore,int preloading) {
    if(!freecam_world_pass || !dev_guest_address(spore,0x34))return 0;
    const float *focus=freecam_world_pass==1?freecam_world_view_focus:freecam_world_player_focus;
    const float *p=(const float*)guest_ptr(spore+8);
    float dx=focus[0]-p[0],dz=focus[2]-p[2];
    uint16_t flags=*(const uint16_t*)guest_ptr(spore+0x30);
    int32_t distance=*(const int16_t*)guest_ptr(spore+(preloading?0x2E:0x2C));
    /* Match retail 001EC2A0/001EC320, including hysteresis and strictness. */
    uint32_t factor=preloading ? ((flags&0x100)?0x2E4060u:0x2DC08Cu)
                              : ((flags&8)?0x2DC08Cu:0x2E4060u);
    float limit=(float)(distance*distance) * *(const float*)guest_ptr(factor);
    float squared=dx*dx+dz*dz;
    return preloading ? squared<limit : squared<=limit;
}
int recomp_freecam_world_update(uint32_t world,uint32_t delta,
                                uint32_t position_arg,uint32_t direction_arg) {
    if(freecam_world_pass || !g_xbox_mem_offset || g_esp<0x20000u ||
       g_esp>=0x4000000u || !dev_guest_address(position_arg,12) ||
       !dev_guest_address(direction_arg,12))return 0;
    float direction[3];
    if(!recomp_freecam_focus(freecam_world_view_focus,direction))return 0;
    memcpy(freecam_world_player_focus,guest_ptr(position_arg),12);
    for(unsigned i=0;i<3;i++)if(!isfinite(freecam_world_player_focus[i]))return 0;
    if(!recomp_lookup(0x1EE610u))return 0;
    recomp_saved_guest_cpu_context saved;recomp_save_guest_cpu_context(&saved);
    uint32_t stack=(g_esp-0x4000u)&~15u,remote_pos=stack+0x100u,remote_dir=stack+0x110u;
    memcpy(guest_ptr(remote_pos),freecam_world_view_focus,12);
    memcpy(guest_ptr(remote_dir),direction,12);
    uint32_t args[3]={delta,position_arg,direction_arg};
    freecam_world_pass=1;dev_call(stack,0x1EE610u,world,3,args);
    args[1]=remote_pos;args[2]=remote_dir;
    freecam_world_pass=2;dev_call(stack,0x1EE610u,world,3,args);
    freecam_world_pass=0;
    recomp_restore_guest_cpu_context(&saved);
    g_esp+=16u; /* RedWorld::Update is thiscall, ret 12. */
    return 1;
}
#endif

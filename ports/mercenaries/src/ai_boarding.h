/* Normal NPC boarding uses horizontal pathfinding. A helicopter dock can be
 * directly overhead while still unreachable. Enforce the existing 1.1m dock
 * proximity vertically too, without touching scripted immediate transfers,
 * player action hijacks, or non-helicopter boarding animations. OG Bugs restores
 * retail horizontal-only boarding, including the Embedded journalist exploit. */
int recomp_ai_helicopter_dock_reachable(uint32_t seat,uint32_t rider,float dock_y) {
    if(recomp_options_og_bugs())return 1; /* Leave both retail boarding paths intact. */
    if(!dev_guest_address(seat,4) || !dev_guest_address(rider,0xEC))return 0;
    uint32_t manager=guest_u32(seat);
    if(!dev_guest_address(manager,4))return 0;
    uint32_t vehicle=guest_u32(manager);
    if(!dev_guest_address(vehicle,4))return 0;
    if(guest_u32(vehicle)!=0x002E21A8u)return 1; /* retail helicopter class */
    float rider_y=*(float*)guest_ptr(rider+0xE4);
    return isfinite(dock_y) && isfinite(rider_y) && fabsf(dock_y-rider_y)<=1.1f;
}

/* Included by recomp_manual.c after dev_spawn.h. Expose the transports
 * authored seats and turrets, retaining the ordinary rider/weapon state machines. */
static uint32_t cargo_pilot_dock(uint32_t seat) {
    if(!dev_guest_address(seat,0x12Cu) || guest_u32(seat+0x114u)!=1u)return 0;
    uint32_t manager=guest_u32(seat);
    if(!dev_guest_address(manager,4))return 0;
    uint32_t vehicle=guest_u32(manager);
    if(!dev_guest_address(vehicle,0x5Cu))return 0;
    switch(guest_u32(vehicle+0x58u)) {
    case 0x12E1A504u: return 0xD9E53C2Au; /* sk_veh_mh-53pavelow: hp_dock_rf */
    case 0x6BF5631Fu: return 0xD9E53C2Au; /* allies_veh_chinook: hp_dock_rf */
    case 0x10C5327Au: return 0xE9B835BCu; /* mafia_veh_mi26halo: hp_dock_lf */
    default:return 0;
    }
}
static uint32_t cargo_seat_model(uint32_t seat) {
    if(!dev_guest_address(seat,0x12Cu))return 0;
    uint32_t manager=guest_u32(seat);
    if(!dev_guest_address(manager,4))return 0;
    uint32_t vehicle=guest_u32(manager);
    return dev_guest_address(vehicle,0x5Cu)?guest_u32(vehicle+0x58u):0;
}
static uint32_t cargo_access_dock(uint32_t seat) {
    uint32_t pilot=cargo_pilot_dock(seat);
    if(pilot)return pilot;
    uint32_t model=cargo_seat_model(seat);
    if(!model)return 0;
    unsigned index=*(const uint8_t*)guest_ptr(seat+0x111u);
    unsigned type=guest_u32(seat+0x114u);
    if((model==0x12E1A504u || model==0x6BF5631Fu) &&
       (type==2u || (model==0x12E1A504u && type==3u))) {
        if(index==1u)return 0xD5B81640u; /* hp_dock_lr */
        if(index==2u)return 0xE5E54F0Eu; /* hp_dock_rr */
    }
    if(model==0x10C5327Au && type==3u && index==1u)return 0xD9E53C2Au;
    return 0;
}
uint32_t recomp_cargo_dock(uint32_t seat,uint32_t authored) {
    return authored?authored:cargo_access_dock(seat);
}
static int cargo_boarding_player(uint32_t seat,uint32_t rider) {
    if(!cargo_access_dock(seat) || !dev_guest_address(rider,0x76Cu))return 0;
    uint32_t table=guest_u32(rider);
    if(!dev_guest_address(table,0xF4u))return 0;
    uint32_t predicate=guest_u32(table+0xF0u);
    if(predicate<0x10000u || predicate>=0x2DC000u)return 0;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    int player=(dev_call(g_esp-0x100u,predicate,rider,0,NULL)&255u)!=0;
    recomp_restore_guest_cpu_context(&saved);
    return player;
}
int recomp_cargo_player(uint32_t seat,uint32_t rider) {
    return cargo_pilot_dock(seat) && cargo_boarding_player(seat,rider);
}
static int cargo_gunner(uint32_t seat) {
    return cargo_access_dock(seat) && guest_u32(seat+0x114u)==2u;
}
uint32_t recomp_cargo_entry_type(uint32_t seat,uint32_t rider,uint32_t type) {
    if(type==7u || type==8u)return type;
    /* Black Hawk gunner get-in motion cannot span the transport ramp-to-gun
     * distance for either players or NPCs. Use retail immediate entry only
     * for these incompatible seats; leave ordinary NPC boarding intact. */
    if(cargo_gunner(seat) && dev_guest_address(rider,0x76Cu))return 7u;
    if(!cargo_boarding_player(seat,rider))return type;
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    uint32_t arg[]={type};
    int exists=(dev_call(g_esp-0x100u,0x001625A0u,seat,1,arg)&255u)!=0;
    recomp_restore_guest_cpu_context(&saved);
    xbox_preview_log_event("cargo-boarding","seat=%08X type=%u animation=%d",seat,type,exists);
    return exists?type:7u;
}
int recomp_cargo_skip_door(uint32_t state,uint32_t rider) {
    if(!dev_guest_address(state,12) || !dev_guest_address(rider,0x76Cu))return 0;
    uint32_t seat=guest_u32(state+4);
    if(!cargo_gunner(seat) && !cargo_boarding_player(seat,rider))return 0;
    recomp_saved_guest_cpu_context check;
    recomp_save_guest_cpu_context(&check);
    uint32_t args[]={guest_u32(state+8),guest_u32(seat+0xE4),guest_u32(seat+0xE8)};
    int exists=(dev_call(g_esp-0x100u,0x00063D70u,0,3,args)&255u)!=0;
    recomp_restore_guest_cpu_context(&check);
    if(exists && !cargo_gunner(seat))return 0;
    /* StateUseDoor normally snaps to the dock after a successful animation.
     * These cockpits have no matching entry/exit animation. Snap explicitly,
     * then use the original missing-animation cleanup and disconnection. */
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    dev_call(g_esp-0x100u,0x00165260u,state,0,NULL);
    recomp_restore_guest_cpu_context(&saved);
    return 1;
}

/* The Mi-26's unused cockpit dock is above its landing gear contact plane.
 * Keep its authored horizontal position/orientation, but make the interaction
 * and normal exit reachable on the supporting surface. Never extend an air
 * boarding radius or change forced action-hijack docks. */
void recomp_cargo_ground_dock(uint32_t seat,uint32_t matrix) {
    if(cargo_seat_model(seat)!=0x10C5327Au || !cargo_access_dock(seat) || !dev_guest_address(matrix,64) || g_esp<0x20000u)return;
    float *m=(float*)guest_ptr(matrix);
    if(!isfinite(m[12]) || !isfinite(m[13]) || !isfinite(m[14]))return;
    float start[3]={m[12],m[13]+.25f,m[14]},end[3]={m[12],m[13]-6.f,m[14]},hit[3];
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);
    uint32_t stack=(g_esp-0x1000u)&~15u;
    int found=dev_ray(stack,stack+0x100u,start,end,hit);
    recomp_restore_guest_cpu_context(&saved);
    if(found && isfinite(hit[1]) && hit[1]<=start[1] && hit[1]>=end[1])m[13]=hit[1];
}

/* Pave Low passenger hardpoints are children of its two minigun turrets:
 * B/hp_seat_lr is turret B; C/hp_seat_rr is turret A. Enable the existing
 * stations rather than inventing overlapping seats or duplicating weapons. */
void recomp_cargo_configure_seat(uint32_t seat) {
    if(cargo_seat_model(seat)!=0x12E1A504u)return;
    unsigned index=*(const uint8_t*)guest_ptr(seat+0x111u);
    if((index!=1u && index!=2u) || guest_u32(seat+0x114u)!=3u)return;
    uint32_t vehicle=guest_u32(guest_u32(seat)),table=guest_u32(vehicle);
    if(!dev_guest_address(table,0x258u))return;
    uint32_t fn=guest_u32(table+0x254u);
    if(fn<0x10000u || fn>=0x2DC000u)return;
    recomp_saved_guest_cpu_context saved;recomp_save_guest_cpu_context(&saved);
    uint32_t args[]={2u-index};
    uint32_t turret=dev_call(g_esp-0x100u,fn,vehicle,1,args);
    recomp_restore_guest_cpu_context(&saved);
    if(!dev_guest_address(turret,0x2DCu))return;
    uint32_t anim=index==1u?0x1B66A546u:0x246D66FBu;
    *(uint32_t*)guest_ptr(seat+0x114u)=2u;
    *(uint32_t*)guest_ptr(seat+0x124u)=2u-index;
    *(uint32_t*)guest_ptr(seat+0x104u)=10u;
    *(uint32_t*)guest_ptr(seat+0x100u)=anim;
    *(uint32_t*)guest_ptr(seat+0xE8u)=anim;
    *(uint32_t*)guest_ptr(seat+0x128u)=2u; /* rear automatic ramp */
    if(guest_u32(turret+0x2D8u)!=1u)*(uint32_t*)guest_ptr(turret+0x2D8u)=2u;
}

int recomp_cargo_defer_gunner_completion(uint32_t seat,uint32_t rider) {
    if(!cargo_gunner(seat) || guest_u32(seat+0x11Cu)!=7u || !cargo_boarding_player(seat,rider))return 0;
    /* Complete through next tick's retail GETIN_DONE_HACK state, after the
     * caller has set WAIT_FOR_ENTER. A synchronous callback makes Exit treat
     * boarding as cancelled and then overwrites the turret camera. */
    *(uint32_t*)guest_ptr(seat+0x118u)=5u;
    return 1;
}

void recomp_cargo_hide_gunner(uint32_t seat,uint32_t rider) {
    if(!cargo_gunner(seat) || !cargo_boarding_player(seat,rider))return;
    /* These previously AI-only gun stations put the turret camera inside the
     * seated player mesh. Hide that mesh; retail gunner Exit calls ShowRider. */
    recomp_saved_guest_cpu_context saved;recomp_save_guest_cpu_context(&saved);
    dev_call(g_esp-0x1000u,0x00162540u,seat,0,NULL);
    recomp_restore_guest_cpu_context(&saved);
}

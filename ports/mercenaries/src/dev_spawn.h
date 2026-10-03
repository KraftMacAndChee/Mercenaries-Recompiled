/* Included by recomp_manual.c: all guest calls preserve the interrupted CPU
 * context. F9 only queues commands; this runs at the retail world-update checkpoint. */
#include "dev_battle.h"
#include "dev_weapons.h"
#include "dev_factions.h"
#include "free_cam.h"
static uint32_t dev_spawn_serial;
static uint32_t dev_last_spawn_guid;
static int dev_region_import_active;
int recomp_dev_region_import_active(void){return dev_region_import_active;}

static int dev_guest_address(uint32_t p, uint32_t bytes)
{
    return p >= 0x10000u && bytes < 0x4000000u && p <= 0x4000000u-bytes;
}
#include "dev_player_actor.h"
/* The model registry is populated from the current retail load. Never use its
 * fallback model to authorize a missing regional dependency. */
static uint32_t dev_resident_model(uint32_t hash)
{
    uint32_t count=guest_u32(0x6438A8),keys=guest_u32(0x6438B4),values=guest_u32(0x6438B0);
    if(!hash||count>8192||!dev_guest_address(keys,count*4)||!dev_guest_address(values,count*4))return 0;
    for(uint32_t i=0;i<count;i++)if(guest_u32(keys+i*4)==hash){
        uint32_t model=guest_u32(values+i*4);
        return dev_guest_address(model,16)?model:0;
    }
    return 0;
}
static float dev_occupied_radius(uint32_t human)
{
    if(!dev_guest_address(human,0x76C))return 1.5f;
    uint32_t seat=guest_u32(human+0x768);
    if(!dev_guest_address(seat,4))return 1.5f;
    uint32_t manager=guest_u32(seat);
    if(!dev_guest_address(manager,4))return 1.5f;
    uint32_t actor=guest_u32(manager),hash=dev_guest_address(actor,0x5C)?guest_u32(actor+0x58):0;
    float radius=1.5f,maximum=1.5f;
    for(unsigned i=0;i<recomp_dev_vehicle_count();i++){
        const DevVehicle *v=recomp_dev_vehicle_at(i);
        maximum=fmaxf(maximum,v->radius);
        if(v->model_hash==hash)radius=fmaxf(radius,v->radius);
    }
    return radius>1.5f?radius:maximum;
}
static uint32_t dev_call(uint32_t stack, uint32_t va, uint32_t object,
                         unsigned count, const uint32_t *args)
{
    recomp_func_t fn = recomp_lookup(va);
    g_esp=stack;
    for(unsigned i=count;i>0;--i) recomp_guest_push_u32(args[i-1]);
    recomp_guest_push_u32(0);
    g_ecx=object;
    fn();
    g_esp=stack;
    return g_eax;
}
static int dev_ray(uint32_t stack,uint32_t scratch,const float *start,
                   const float *end,float *hit)
{
    uint32_t ray=scratch+32;
    memcpy(guest_ptr(scratch),start,12);memcpy(guest_ptr(scratch+16),end,12);
    memset(guest_ptr(ray),0,160);
    uint32_t ctor[]={scratch,scratch+16};
    dev_call(stack,0x000187A0,ray,2,ctor);
    uint32_t args[]={ray,2,0x12F,1,1};
    int found=dev_call(stack,0x0012C650,0,5,args)!=0;
    if(found&&hit)memcpy(hit,guest_ptr(ray+0x18),12);
    /* RsRayCollider::~RsRayCollider releases its four obstacle references. */
    for(unsigned i=0;i<4;i++){
        uint32_t obstacle=guest_u32(ray+4+4*i);
        if(obstacle)dev_call(stack,0x0012BE40,obstacle,0,NULL);
    }
    return found;
}
static void dev_property(uint32_t stack,uint32_t scratch,uint32_t list,
                        const char *key,const char *value)
{
    strcpy(guest_ptr(scratch+16),key);strcpy(guest_ptr(scratch+96),value);
    uint32_t ctor[]={scratch+16,scratch+96};
    dev_call(stack,0x001EAFB0,scratch,2,ctor);
    uint32_t arg[]={scratch};dev_call(stack,0x001EB080,list,1,arg);
}
/* Use the retail list loader, which queues dependencies in their original
 * order and calls RedWorld::ReadData to bracket the temporary typed-PPD table.
 * AddPermanent alone is not valid after world startup. Only catalogue layers
 * shipped in the supported ISO are accepted here. */
static uint32_t dev_vehicle_asset_list(const char *name)
{
    static const char *allowed[]={"template_vehicles","template_vehicles_ch1",
        "template_vehicles_ch2","template_vehicles_ch3","template_vehicles_ch4","template_humans","template_weapons"};
    if(!name)return 0;
    for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++)if(!strcmp(name,allowed[i])){
        uint32_t hash=2166136261u;
        for(const unsigned char *p=(const unsigned char*)name;*p;p++){
            hash^=(*p|32u);hash*=16777619u;
        }
        return hash;
    }
    return 0;
}
static uint32_t dev_hash(const char *text)
{
    uint32_t value=2166136261u;
    for(;*text;++text)value=(value^((unsigned char)*text|32u))*16777619u;
    return value;
}
static int dev_load_layer(uint32_t stack,const char *name)
{
    uint32_t layer=dev_vehicle_asset_list(name);
    if(!layer || !recomp_lookup(0x180AE0) || !recomp_lookup(0x17F490))return 0;
    int importing=dev_region_import_active;
    dev_region_import_active=1;
    dev_call(stack,0x180AE0,0,1,&layer);
    if(*(uint8_t*)guest_ptr(0x413FC9))dev_call(stack,0x17F490,0,0,NULL);
    dev_region_import_active=importing;
    return 1;
}
#include "dev_troop_variants.h"

static int dev_human_available(uint32_t stack,const char *name)
{
    uint32_t args[]={dev_hash(name),0x134603D7u};
    uint32_t model=dev_call(stack,0x1ED340,0,2,args);
    return model && dev_resident_model(model);
}
static int dev_prepare_crew(uint32_t stack,unsigned faction_index,unsigned mode,
                            const char **driver,const char **gunner,const char **passenger)
{
    static const char *drivers[]={"template_allies_driver","template_china_driver","template_mafia_driver","template_nk_driver","template_sk_driver"};
    static const char *gunners[]={"template_allies_gunner","template_china_gunner","template_mafia_gunner","template_nk_gunner","template_sk_gunner"};
    static const char *soldiers[]={"template_allies_soldier","template_china_soldier","template_mafia_soldier","template_nk_soldier","template_sk_soldier"};
    if(faction_index>=5 || mode>2)return 0;
    *driver=drivers[faction_index];*gunner=gunners[faction_index];*passenger=soldiers[faction_index];
    if(!dev_human_available(stack,*driver) || (mode==2 && !dev_human_available(stack,*gunner)))
        dev_load_layer(stack,guest_u32(0x403970)==0x4A364A32u?"template_vehicles_ch3":"template_vehicles_ch1");
    if(mode==2 && !dev_human_available(stack,*passenger))dev_load_layer(stack,"template_humans");
    return dev_human_available(stack,*driver) && (mode!=2 ||
        (dev_human_available(stack,*gunner) && dev_human_available(stack,*passenger)));
}
static void dev_transient_actor(uint32_t actor)
{
    if(!dev_guest_address(actor,12))return;
    uint32_t spore=guest_u32(actor+8);
    if(dev_guest_address(spore,0x34))*(uint16_t*)guest_ptr(spore+0x30)&=(uint16_t)~0xE00u;
}
static void dev_inspect_spawn(void)
{
    const char *message="No spawned vehicle is currently available.";
    static char report[256];
    if(!g_xbox_mem_offset||g_esp<0x20000u||g_esp>=0x4000000u||!recomp_lookup(0x1EC220)||!recomp_lookup(0x8C7E0)||!dev_last_spawn_guid||guest_u32(0x413F6C)!=0x4249D707u||guest_u32(0x413F68)!=0xC2CBD863u){recomp_dev_spawn_result(0,message);return;}
    recomp_saved_guest_cpu_context saved;recomp_save_guest_cpu_context(&saved);
    uint32_t stack=(g_esp-0x4000u)&~15u,args[]={dev_last_spawn_guid};
    uint32_t actor=dev_call(stack,0x1EC220,0,1,args);
    args[0]=0x660E4490u;uint32_t player=dev_call(stack,0x8C7E0,0,1,args);
    uint32_t human=dev_player_human(player);
    if(dev_guest_address(actor,0x300)&&human){
        uint32_t vt=guest_u32(actor),info=stack+0x200;
        uint32_t use_args[]={human,8,info};
        if(dev_guest_address(human,0x100)&&dev_guest_address(vt,0x178)&&recomp_lookup(guest_u32(vt+0x174))){
            unsigned count=dev_call(stack,guest_u32(vt+0x174),actor,3,use_args);
            snprintf(report,sizeof(report),"%u entry actions. Vehicle diagnostics written to the preview log.",count);message=report;
            xbox_preview_log_event("dev-inspect","guid=%08X actor=%08X player=%08X actions=%u model=%08X",dev_last_spawn_guid,actor,human,count,guest_u32(actor+0x58));
            for(unsigned i=0;i<count&&i<8;i++){
                const float *v=(const float*)guest_ptr(info+i*24);
                xbox_preview_log_event("dev-inspect","action=%u flags=%08X pos=(%.4f,%.4f,%.4f) radius=%.4f player=(%.4f,%.4f,%.4f)",i,guest_u32(info+i*24+20),v[0],v[1],v[2],v[3],*(float*)guest_ptr(human+0xE0),*(float*)guest_ptr(human+0xE4),*(float*)guest_ptr(human+0xE8));
            }
        }
    }
    /* Explicit Inspect command only: expose authored seat clearance without
     * changing recruitment, occupancy, collision, or player access. */
    if(dev_guest_address(actor,0x1470u) && guest_u32(actor)==0x002E21A8u){
        uint32_t manager=actor+0xBA4u;
        if(guest_u32(manager)==actor && guest_u32(manager+8u)==manager){
            uint32_t choose_args[]={actor+0xE0u,0u,0u,0x41200000u};
            uint32_t chosen=dev_call(stack,0x00168200u,manager,4,choose_args);
            int landed=dev_call(stack,0x00049CD0u,actor,0,NULL)&255u;
            xbox_preview_log_sample("dev-seats","actor=%08X landed=%d available_non_driver=%08X riders=%u max=%u",actor,landed,chosen,*(const uint8_t*)guest_ptr(manager+0x8C8u),*(const uint8_t*)guest_ptr(manager+0x8C9u));
            unsigned seats=*(const uint8_t*)guest_ptr(manager+0x8C9u);
            for(unsigned i=0;i<seats && i<7u;i++){
                uint32_t seat=manager+8u+i*0x12Cu,out=stack+0x600u;
                uint32_t pos_args[]={out,0u},clear_args[]={1u};
                int found=dev_call(stack,0x001665B0u,seat,2,pos_args)&255u;
                int clear=dev_call(stack,0x00167D70u,seat,1,clear_args)&255u;
                char tag[32];snprintf(tag,sizeof(tag),"dev-seat-%u",i);
                const float *v=(const float*)guest_ptr(out);
                xbox_preview_log_sample(tag,"seat=%08X type=%u rider=%08X dock=%08X found=%d clear=%d pos=(%.4f,%.4f,%.4f)",seat,guest_u32(seat+0x114u),guest_u32(seat+0xBCu),guest_u32(seat+0xD8u),found,clear,found?v[0]:0.f,found?v[1]:0.f,found?v[2]:0.f);
            }
        }
    }
    recomp_restore_guest_cpu_context(&saved);recomp_dev_spawn_result(0,message);
}
/* A private route can request the same checked spawn used by F9 without
 * desktop input. Both environment switches are required; normal previews do
 * not poll a file. The sequence prevents repeated spawns from one request. */
static int dev_test_take_spawn(unsigned *index)
{
    static int initialized;
    static const char *path;
    static unsigned long last_sequence;
    static ULONGLONG next_poll;
    if (!initialized) {
        initialized = 1;
        if (getenv("MERCENARIES_TEST_GAMEPAD_FILE"))
            path = getenv("MERCENARIES_TEST_SPAWN_FILE");
        if(path && *path)xbox_preview_log_event("dev-test-enabled","Spawn command file: %s",path);
    }
    if (!path || !*path) return 0;
    ULONGLONG now = GetTickCount64();
    if (now < next_poll) return 0;
    next_poll = now + 100;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    char line[256], name[160], trailing;
    unsigned long sequence;
    int complete = fgets(line, sizeof(line), file) != NULL;
    int extra = fgetc(file);
    fclose(file);
    if (!complete || extra != EOF || line[0] < '0' || line[0] > '9' ||
        sscanf(line, "%lu %159s %c", &sequence, name, &trailing) != 2 ||
        !sequence || sequence <= last_sequence) return 0;
    last_sequence = sequence;
    if (!strcmp(name, "boids")) {
        recomp_dev_request_boids();
        return 0;
    }
    if (!strcmp(name,"board-test")) {
        if (guest_u32(0x413F6Cu)==0x4249D707u && g_esp>0x20000u && g_esp<0x4000000u && dev_last_spawn_guid) {
            recomp_saved_guest_cpu_context saved;recomp_save_guest_cpu_context(&saved);
            uint32_t stack=(g_esp-0x4000u)&~15u,args[]={dev_last_spawn_guid};
            uint32_t actor=dev_call(stack,0x1EC220u,0u,1,args);
            args[0]=0x660E4490u;uint32_t ai=dev_call(stack,0x8C7E0u,0u,1,args);
            uint32_t human=dev_guest_address(ai,0x99Cu)?guest_u32(ai+0x998u):0;
            if(dev_guest_address(actor,0x2000u) && dev_guest_address(human,0x76Cu) && !guest_u32(human+0x768u)) {
                for(uint32_t off=0x300u;off<0x1500u;off+=4u) {
                    uint32_t manager=actor+off;
                    if(guest_u32(manager)!=actor || guest_u32(manager+8u)!=manager || guest_u8(manager+0x8C9u)<1u || guest_u8(manager+0x8C9u)>7u)continue;
                    uint32_t seat=manager+8u,enter[]={human,7u,1u};
                    if(!guest_u32(seat+0xBCu))dev_call(stack,0x166130u,seat,3,enter);
                    xbox_preview_log_event("hair-private-test","actor=%08X human=%08X manager=%08X seat=%08X attached=%08X",actor,human,manager,seat,guest_u32(human+0x768u));
                    break;
                }
            }
            recomp_restore_guest_cpu_context(&saved);
        }
        return 0;
    }
    if (!strcmp(name, "audio-yes") || !strcmp(name, "audio-lz")) {
        if (guest_u32(0x413F6Cu)==0x4249D707u && g_esp>0x20000u && g_esp<0x4000000u) {
            recomp_saved_guest_cpu_context saved;recomp_save_guest_cpu_context(&saved);
            uint32_t args[]={0u,!strcmp(name,"audio-yes")?0x23F08077u:0xD24E4223u};
            uint32_t handle=dev_call((g_esp-0x4000u)&~15u,0x001FFA60u,0u,2,args);
            recomp_restore_guest_cpu_context(&saved);
            xbox_preview_log_event("audio-private-test","cue=%08X managed=%08X",args[1],handle);
        }
        return 0;
    }
    if (!strcmp(name, "inspect")) {
        *index = 0x7fffffffu;
        return 1;
    }
    if (!strncmp(name,"request:",8)) {
        char *end=NULL;unsigned long command=strtoul(name+8,&end,10);
        if(end && *end==0 && command<0x2000000u){*index=(unsigned)command;return 1;}
        return 0;
    }
    if (!strncmp(name,"policy:",7)) {
        if(name[7]>='0' && name[7]<='3' && !name[8])recomp_dev_battle_set((unsigned)(name[7]-'0'));
        xbox_preview_log_event("dev-policy","flags=%u",recomp_dev_battle_flags());return 0;
    }
    for (unsigned i=0;i<recomp_dev_troop_count();i++){
        if(!strcmp(name,recomp_dev_troop_at(i)->template_name)){*index=i|DEV_SPAWN_TROOP;return 1;}
    }
    for (unsigned i = 0; i < recomp_dev_vehicle_count(); ++i) {
        const DevVehicle *vehicle = recomp_dev_vehicle_at(i);
        if (!strcmp(name, vehicle->template_name)) {
            *index = i;
            xbox_preview_log_event("dev-test-request", "sequence=%lu template=%s", sequence, name);
            return 1;
        }
    }
    xbox_preview_log_event("dev-test-request", "sequence=%lu rejected unknown template=%s", sequence, name);
    return 0;
}
void recomp_dev_spawn_tick(void)
{
    static int executing;
    unsigned index;
    if(executing)return;
    recomp_dev_battle_refresh_player();
    recomp_dev_relations_tick();
    if(!recomp_dev_take_spawn(&index)&&!dev_test_take_spawn(&index))return;
    if(index==0x7fffffffu){dev_inspect_spawn();return;}
    unsigned troop=(index&DEV_SPAWN_TROOP)!=0, crew=(index>>DEV_SPAWN_CREW_SHIFT)&3u;
    unsigned crew_faction=(index>>DEV_SPAWN_FACTION_SHIFT)&7u;
    unsigned weapon_index=(index>>DEV_SPAWN_WEAPON_SHIFT)&DEV_SPAWN_WEAPON_MASK;
    unsigned requested=troop?1u+((index>>DEV_SPAWN_COUNT_SHIFT)&15u):1u;
    const DevVehicle *vehicle=troop?recomp_dev_troop_at(index&DEV_SPAWN_INDEX_MASK):recomp_dev_vehicle_at(index&DEV_SPAWN_INDEX_MASK);
    if(!vehicle || weapon_index>=DEV_WEAPON_COUNT || (!troop && weapon_index) || requested>12 || crew>2 || (crew && crew_faction>=5)){recomp_dev_spawn_result(0,"Invalid spawn request.");return;}
    if(!g_xbox_mem_offset||guest_u32(0x413F6C)!=0x4249D707u||
       guest_u32(0x413F68)!=0xC2CBD863u||g_esp<0x20000u||g_esp>=0x4000000u){
        recomp_dev_spawn_result(0,"Enter normal gameplay before spawning units.");return;
    }
    const uint32_t calls[]={0x8C7E0,0x1ED340,0x187A0,0x12C650,0x12BE40,
                           0x1EB010,0x1EAFB0,0x1EB080,0x174480};
    for(unsigned i=0;i<sizeof(calls)/sizeof(calls[0]);i++)if(!recomp_lookup(calls[i])){
        recomp_dev_spawn_result(0,"Required retail spawn function is unavailable.");return;
    }
    recomp_saved_guest_cpu_context saved;
    recomp_save_guest_cpu_context(&saved);executing=1;
    uint32_t stack=(g_esp-0x4000u)&~15u, pos=stack+0x100, matrix=stack+0x140;
    uint32_t ray=stack+0x200, prop=stack+0x400, list=stack+0x600;
    const char *result="No safe ground ahead. Move to an open, level area and try again.";
    int success=0;unsigned created=0;
    const char *driver="none",*gunner="none",*passenger="none";
    uint32_t actor=0;
    float origin[3]={0},destination[3]={0},direction[3]={0};
    uint32_t availability[]={vehicle->template_hash,0x8D39BDE6u};
    if(!dev_call(stack,0x1ED340,0,2,availability)){
        uint32_t layer=dev_vehicle_asset_list(vehicle->asset_list);
        if(layer&&
           recomp_lookup(0x180AE0)&&recomp_lookup(0x17F490)){
            recomp_dev_spawn_progress("Loading this vehicle's original chapter dependencies...");
            xbox_preview_log_event("dev-import","begin template=%s list=%s",vehicle->template_name,vehicle->asset_list);
            dev_region_import_active=1;
            dev_call(stack,0x180AE0,0,1,&layer); /* RsMain::LoadList */
            /* Some map scripts deliberately leave deferred loading enabled.
             * Complete the queued chapter dependencies before using its PPDs. */
            if(*(uint8_t*)guest_ptr(0x413FC9))dev_call(stack,0x17F490,0,0,NULL);
            dev_region_import_active=0;
            xbox_preview_log_event("dev-import","end template=%s list=%s",vehicle->template_name,vehicle->asset_list);
        }
        if(!dev_call(stack,0x1ED340,0,2,availability)){
            result="This variant is not available in the current map/chapter.";goto done;
        }
    }
    const DevTroopVariant *variant=troop?dev_troop_variant(vehicle):NULL;
    availability[1]=0x134603D7u; /* geometryfile, resolved from the live template */
    uint32_t model_hash=dev_call(stack,0x1ED340,0,2,availability);
    if(variant){
        if(model_hash!=dev_hash(variant->base_model) || !dev_prepare_troop_variant(stack,variant)){
            result="Troop model dependencies are unavailable. Spawn cancelled.";goto done;
        }
        model_hash=dev_hash(variant->model);
    }
    if(model_hash==vehicle->model_hash && !dev_resident_model(model_hash))dev_load_layer(stack,vehicle->asset_list);
    if(model_hash!=vehicle->model_hash||!dev_resident_model(model_hash)){
        result="Vehicle model dependencies are unavailable in this map. Spawn cancelled.";goto done;
    }
    if(crew && !troop && !dev_prepare_crew(stack,crew_faction,crew,&driver,&gunner,&passenger)){result="Crew dependencies are unavailable. Spawn cancelled.";goto done;}
    if(troop && weapon_index){
        uint32_t args[]={dev_hash(dev_weapons[weapon_index].template_name),dev_hash("geometryfile")};
        uint32_t weapon_model=dev_call(stack,0x1ED340,0,2,args);
        if(!weapon_model || !dev_resident_model(weapon_model)){
            dev_load_layer(stack,"template_weapons");
            weapon_model=dev_call(stack,0x1ED340,0,2,args);
        }
        if(!weapon_model || !dev_resident_model(weapon_model)){
            result="Selected weapon dependencies are unavailable. Spawn cancelled.";goto done;
        }
    }
    uint32_t player_arg[]={0x660E4490u};
    uint32_t player=dev_call(stack,0x8C7E0,0,1,player_arg);
    if(!dev_guest_address(player,0x76C)) {result="Player is not ready.";goto done;}
    int camera_spawn=recomp_freecam_enabled();
    if(camera_spawn){
        if(!recomp_freecam_focus(origin,direction)){result="Free camera is not ready.";goto done;}
    }else{
        uint32_t vtable=guest_u32(player);
        if(!dev_guest_address(vtable,0x40)||!recomp_lookup(guest_u32(vtable+0x3C))){result="Player position is unavailable.";goto done;}
        uint32_t position_arg[]={pos};dev_call(stack,guest_u32(vtable+0x3C),player,1,position_arg);
        memcpy(origin,guest_ptr(pos),12);
        uint32_t camera=guest_u32(0x41410C);
        if(!dev_guest_address(camera,0x40)) {result="Gameplay camera is unavailable.";goto done;}
        direction[0]=-*(float*)guest_ptr(camera+0x30);direction[2]=-*(float*)guest_ptr(camera+0x38);
    }
    if(!isfinite(origin[0])||!isfinite(origin[1])||!isfinite(origin[2])){result="Spawn origin is invalid.";goto done;}
    float length=sqrtf(direction[0]*direction[0]+direction[2]*direction[2]);
    if(!isfinite(length)||length<0.01f){result="Look toward the horizon before spawning.";goto done;}
    direction[0]/=length;direction[2]/=length;
    float occupied_radius=camera_spawn?1.5f:dev_occupied_radius(dev_player_human(player));
    for(unsigned unit=0;unit<requested;unit++){
    success=0;
    for(unsigned attempt=0;attempt<4&&!success;attempt++){
        float distance=vehicle->radius+occupied_radius+2.0f+attempt*4.0f+(float)(unit/4u)*3.0f;
        float start[3]={origin[0]+direction[0]*distance,origin[1]+15.0f,origin[2]+direction[2]*distance};
        if(troop){float side=((float)(unit%4u)-1.5f)*2.0f;start[0]+=direction[2]*side;start[2]-=direction[0]*side;}
        float end[3]={start[0],origin[1]-(camera_spawn?300.0f:100.0f),start[2]},hit[3];
        if(!dev_ray(stack,ray,start,end,hit)||!isfinite(hit[1])||(!camera_spawn && fabsf(hit[1]-origin[1])>30.0f))continue;
        /* Sample the authored footprint before constructing any actor. Tall
         * obstacles and uneven support reject the placement instead of embedding
         * a chassis, rotor, or large cargo helicopter into nearby geometry. */
        float lowest=hit[1],highest=hit[1];int supported=1;
        for(int x=-1;x<=1&&supported;x++)for(int z=-1;z<=1;z++){
            if(!x&&!z)continue;
            float side=x*vehicle->half_width,along=z*vehicle->half_length;
            float top[3]={hit[0]+side*direction[2]+along*direction[0],hit[1]+vehicle->height+1,
                          hit[2]-side*direction[0]+along*direction[2]};
            float bottom[3]={top[0],hit[1]-2,top[2]},support[3];
            if(!dev_ray(stack,ray,top,bottom,support)||!isfinite(support[1])){supported=0;break;}
            lowest=fminf(lowest,support[1]);highest=fmaxf(highest,support[1]);
            if(highest-lowest>0.75f){supported=0;break;}
        }
        if(!supported)continue;
        memcpy(destination,hit,12);destination[1]=highest-vehicle->bottom+0.25f;
        /* Check the approach at body height: don't place the vehicle through a wall. */
        float from[3]={origin[0]+direction[0]*(occupied_radius+0.5f),origin[1]+1.0f,origin[2]+direction[2]*(occupied_radius+0.5f)};
        float to[3]={hit[0],hit[1]+1.0f,hit[2]};
        if(dev_ray(stack,ray,from,to,NULL))continue;
        float mat[16]={direction[2],0,-direction[0],0, 0,1,0,0,
                       direction[0],0,direction[2],0,
                       destination[0],destination[1],destination[2],1};
        memcpy(guest_ptr(matrix),mat,sizeof(mat));
        dev_call(stack,0x1EB010,list,0,NULL);
        char spawn_name[48];snprintf(spawn_name,sizeof(spawn_name),"recomp_spawn_%u",++dev_spawn_serial);
        dev_property(stack,prop,list,"name",spawn_name);
        dev_property(stack,prop,list,"dieuponhibernation","true");
        if(troop){
            if(variant){
                DevTroopVariant selected=*variant;
                if(weapon_index)selected.weapon=dev_weapons[weapon_index].template_name;
                dev_apply_troop_variant(stack,prop,list,&selected);
            }else if(weapon_index)dev_property(stack,prop,list,"weapon_A_template",dev_weapons[weapon_index].template_name);
            dev_property(stack,prop,list,"path","none");
            dev_property(stack,prop,list,"encounter","none");
            dev_property(stack,prop,list,"squad","none");
        }else{
        dev_property(stack,prop,list,"DriverSeatOccupant","none");
        dev_property(stack,prop,list,"aiType","none");
        if(crew){static const char *factions[]={"allies","china","mafia","nk","sk"};dev_property(stack,prop,list,"faction",factions[crew_faction]);}
        for(unsigned i=0;i<7;i++){
            char key[32];snprintf(key,sizeof(key),"RiderOccupant_%c",'a'+i);
            const char *occupant="none";
            if(crew){
                char type_key[32];snprintf(type_key,sizeof(type_key),"RiderType_%c",'a'+i);
                uint32_t args[]={vehicle->template_hash,dev_hash(type_key)};
                uint32_t type=dev_call(stack,0x1ED340,0,2,args);
                if(type==dev_hash("driver"))occupant=driver;
                else if(crew==2 && type==dev_hash("gunner"))occupant=gunner;
                else if(crew==2 && type==dev_hash("passenger"))occupant=passenger;
            }
            dev_property(stack,prop,list,key,occupant);
        }
        }
        strcpy(guest_ptr(pos),spawn_name);
        uint32_t spawn_args[]={vehicle->template_hash,pos,matrix,list,0};
        actor=dev_call(stack,0x174480,0x4031B0,5,spawn_args);
        success=dev_guest_address(actor,16);
        if(success&&dev_guest_address(guest_u32(actor+8),0x2C))dev_last_spawn_guid=guest_u32(guest_u32(actor+8)+0x28);
        if(success){
            created++;dev_transient_actor(actor);
            xbox_preview_log_event("dev-unit","name=%s actor=%08X template=%s crew=%u faction=%u weapon=%s",spawn_name,actor,vehicle->template_name,crew,crew_faction,dev_weapons[weapon_index].name);
            if(crew){
                for(unsigned seat=0;seat<8;seat++){
                    char rider_name[64];
                    if(seat==0)snprintf(rider_name,sizeof(rider_name),"%s_driver",spawn_name);
                    else snprintf(rider_name,sizeof(rider_name),"%s_pass%c",spawn_name,'a'+seat-1);
                    uint32_t hash=dev_hash(rider_name);uint32_t rider=dev_call(stack,0x8C7E0,0,1,&hash);
                    dev_transient_actor(rider);
                    if(dev_guest_address(rider,0x76C))xbox_preview_log_event("dev-crew","name=%s actor=%08X seat=%08X",rider_name,rider,guest_u32(rider+0x768));
                }
            }
        }
        result=success?"Spawned ahead. Close F9 to return to the game.":"The game could not create this vehicle.";
        break;
    }
    if(!success)break;
    }
    success=created>0;
    char batch_result[160];
    if(troop){snprintf(batch_result,sizeof(batch_result),"Spawned %u / %u soldiers. Use open ground for larger groups.",created,requested);result=batch_result;}
done:
    recomp_restore_guest_cpu_context(&saved);executing=0;
    xbox_preview_log_event("dev-spawn","template=%s actor=%08X success=%d origin=(%.3f,%.3f,%.3f) spawn=(%.3f,%.3f,%.3f) radius=%.3f result=%s",vehicle->template_name,actor,success,origin[0],origin[1],origin[2],destination[0],destination[1],destination[2],vehicle->radius,result);
    if(getenv("MERCENARIES_TEST_GAMEPAD_FILE")){
        const char *request_path=getenv("MERCENARIES_TEST_SPAWN_FILE");
        if(request_path && strlen(request_path)<900){
            char report_path[1024];snprintf(report_path,sizeof(report_path),"%s.result",request_path);
            FILE *report=fopen(report_path,"wb");
            if(report){fprintf(report,"success=%d created=%u template=%s actor=%08X\n%s\n",success,created,vehicle->template_name,actor,result);fclose(report);}
        }
    }
    recomp_dev_spawn_result(success,result);
}

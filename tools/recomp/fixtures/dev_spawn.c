/* Production dev-spawn command exercised against a bounded retail-call fixture. */
#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dev_menu.h"
#ifndef _MSC_VER
int _putenv_s(const char *, const char *);
#endif
static unsigned char memory[0x4000000];
static uint32_t g_esp,g_ecx,g_eax,g_xbox_mem_offset=0x10000,current_va;
typedef void (*recomp_func_t)(void);
typedef struct {uint32_t esp,ecx,eax;} recomp_saved_guest_cpu_context;
static void *guest_ptr(uint32_t p){assert(p<sizeof(memory));return memory+p;}
static uint8_t guest_u8(uint32_t p){assert(p<sizeof(memory));return memory[p];}
static uint32_t guest_u32(uint32_t p){uint32_t v;memcpy(&v,guest_ptr(p),4);return v;}
static void put(uint32_t p,uint32_t v){memcpy(guest_ptr(p),&v,4);}
static void recomp_guest_push_u32(uint32_t v){g_esp-=4;put(g_esp,v);}
static void recomp_save_guest_cpu_context(recomp_saved_guest_cpu_context *s){*s=(recomp_saved_guest_cpu_context){g_esp,g_ecx,g_eax};}
static void recomp_restore_guest_cpu_context(const recomp_saved_guest_cpu_context *s){g_esp=s->esp;g_ecx=s->ecx;g_eax=s->eax;}
static const DevVehicle catalog[]={
 {"small","small","test","jeep",100,200,4,-1,3,2,3,"template_vehicles_ch4"},
 {"large","large","test","helicopter",101,201,40,-2,8,15,35,"template_vehicles_ch1"}
};
unsigned recomp_dev_vehicle_count(void){return 2;}
const DevVehicle *recomp_dev_vehicle_at(unsigned i){return i<2?catalog+i:NULL;}
static DevVehicle variant_troop;
unsigned recomp_dev_troop_count(void){return 2;}
const DevVehicle *recomp_dev_troop_at(unsigned i){return i==0?catalog:i==1?&variant_troop:NULL;}
void recomp_dev_battle_refresh_player(void){}
void recomp_dev_relations_tick(void){}
void recomp_dev_request_boids(void){}
static unsigned policy;
unsigned recomp_dev_battle_flags(void){return policy;}
void recomp_dev_battle_set(unsigned v){policy=v;}
static int pending,spawns,success,missing_template,missing_model,ground_missing,obstruction,uneven,ray_count,imports,import_succeeds,big_loads;
static float player_position[3],spawn_position[3];static const char *message;
int recomp_dev_take_spawn(unsigned *i){if(pending<0)return 0;*i=pending;pending=-1;return 1;}
void recomp_dev_spawn_result(int ok,const char *m){success=ok;message=m;}
void recomp_dev_spawn_progress(const char *m){message=m;}
static void xbox_preview_log_event(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
static void xbox_preview_log_sample(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
static uint32_t dev_hash(const char *);
static char keys[64][48],values[64][160];static unsigned properties;
static const char *property(const char *key){for(unsigned i=0;i<properties;i++)if(!strcmp(keys[i],key))return values[i];return "";}
static int freecam,freecam_ready;
static float freecam_position[3],freecam_direction[3];
int recomp_freecam_enabled(void){return freecam;}
int recomp_freecam_focus(float *p,float *d){if(!freecam_ready)return 0;memcpy(p,freecam_position,12);memcpy(d,freecam_direction,12);return 1;}
static void retail_call(void){
 uint32_t arg=guest_u32(g_esp+4);
 switch(current_va){
 case 0x180AE0:imports++;assert(arg==0x196FE72Bu);if(import_succeeds&&!memory[0x413fc9])missing_template=0;break;
 case 0x17F490:big_loads++;if(import_succeeds)missing_template=0;break;
 case 0x1ED340:{
  uint32_t key=guest_u32(g_esp+8);g_eax=missing_template?0:key==0x134603d7?200:100;
  if(arg==variant_troop.template_hash && key==0x134603d7)g_eax=dev_hash("mafia_hum_soldier");
  if(key==dev_hash("RiderType_a"))g_eax=dev_hash("driver");
  if(key==dev_hash("RiderType_b"))g_eax=dev_hash("gunner");
  if(key==dev_hash("RiderType_c"))g_eax=dev_hash("passenger");
  if(key==dev_hash("RiderType_d"))g_eax=dev_hash("extraction");break;}
 case 0x1EB010:properties=0;break;
 case 0x1EAFB0:assert(properties<64);strcpy(keys[properties],guest_ptr(arg));strcpy(values[properties++],guest_ptr(guest_u32(g_esp+8)));break;
 case 0x8C7E0:g_eax=0x200000;break;
 case 0x1EC220:g_eax=0x300000;break;
 case 0x1120:assert(arg==0x210000);assert(guest_u32(g_esp+8)==8);g_eax=0;break;
 case 0x1110:memcpy(guest_ptr(arg),player_position,12);break;
 case 0x187A0:{float ends[6];memcpy(ends,guest_ptr(arg),12);memcpy(ends+3,guest_ptr(guest_u32(g_esp+8)),12);memcpy(guest_ptr(g_ecx+64),ends,24);break;}
 case 0x12C650:{
  float ends[6],hit[3];memcpy(ends,guest_ptr(arg+64),24);ray_count++;
  int vertical=fabsf(ends[0]-ends[3])<.0001f&&fabsf(ends[2]-ends[5])<.0001f;
  g_eax=vertical?!ground_missing:obstruction;
  hit[0]=ends[0];hit[1]=uneven&&ray_count%9!=1?5:0;hit[2]=ends[2];memcpy(guest_ptr(arg+24),hit,12);break;
 }
 case 0x174480:{uint32_t matrix=guest_u32(g_esp+12);memcpy(spawn_position,guest_ptr(matrix+48),12);spawns++;g_eax=0x300000;put(0x300008,0x301000);put(0x301028,123);break;}
 default:g_eax=0;break;
 }
}
static recomp_func_t recomp_lookup(uint32_t va){current_va=va;return retail_call;}
/* The fixture does not enable timed test requests. */
#define GetTickCount64 dev_fixture_tick_count64
static unsigned long long dev_fixture_tick_count64(void){return 0;}
#include "dev_spawn.h"
static void reset(void){
 freecam=freecam_ready=0;
 memset(memory,0,sizeof(memory));pending=0;spawns=success=missing_template=missing_model=ground_missing=obstruction=uneven=ray_count=imports=import_succeeds=big_loads=0;
 
 g_esp=0x800000;g_ecx=1234;g_eax=5678;message="";
 put(0x413F6C,0x4249D707);put(0x413F68,0xC2CBD863);
 put(0x200000,0x201000);put(0x20103C,0x1110);put(0x200998,0x210000);
 put(0x41410C,0x220000);float back=-1;memcpy(guest_ptr(0x220038),&back,4);
 put(0x6438A8,2);put(0x6438B4,0x230000);put(0x6438B0,0x231000);put(0x230000,200);put(0x230004,201);put(0x231000,0x240000);put(0x231004,0x241000);
 player_position[0]=10;player_position[1]=1;player_position[2]=20;
}
static void run(void){recomp_dev_spawn_tick();assert(g_esp==0x800000&&g_ecx==1234&&g_eax==5678);}
int main(void){
 reset();dev_last_spawn_guid=123;put(0x300000,0x310000);put(0x310174,0x1120);
 dev_inspect_spawn();assert(strstr(message,"0 entry actions"));
 assert(g_esp==0x800000&&g_ecx==1234&&g_eax==5678);
 reset();put(0x200998,0);dev_inspect_spawn();assert(strstr(message,"No spawned vehicle"));
 reset();missing_template=1;run();assert(!spawns&&!success&&strstr(message,"map/chapter"));
 reset();missing_template=1;import_succeeds=1;run();assert(imports==1&&spawns==1&&success);
 reset();missing_template=1;run();assert(imports==1&&!spawns&&!success);
 reset();missing_template=1;put(0x230000,999);run();assert(imports==1&&!spawns&&!success);
 reset();memory[0x413fc9]=1;missing_template=1;import_succeeds=1;run();assert(imports==1&&big_loads==1&&spawns==1&&success&&memory[0x413fc9]==1);
 assert(!dev_vehicle_asset_list("unrelated_world"));
 reset();put(0x230000,999);run();assert(!spawns&&!success&&strstr(message,"dependencies"));
 reset();put(0x413F68,0);run();assert(!spawns&&!success);
 reset();player_position[0]=NAN;run();assert(!spawns&&!success);
 reset();memset(guest_ptr(0x220030),0,12);run();assert(!spawns&&!success);
 reset();ground_missing=1;run();assert(!spawns&&!success);
 reset();uneven=1;run();assert(!spawns&&!success);
 reset();obstruction=1;run();assert(!spawns&&!success);
 reset();run();assert(spawns==1&&success&&ray_count==10);assert(fabsf(spawn_position[2]-27.5f)<.001f);assert(fabsf(spawn_position[1]-1.25f)<.001f);
 reset();put(0x210768,0x250000);put(0x250000,0x251000);put(0x251000,0x252000);put(0x252058,201);run();assert(success&&fabsf(spawn_position[2]-66)<.001f);
 reset();put(0x210768,0x250000);put(0x250000,0x251000);put(0x251000,0x252000);put(0x252058,999);run();assert(success&&fabsf(spawn_position[2]-66)<.001f);
 reset();put(0x200768,0x250000);put(0x250000,0x251000);put(0x251000,0x252000);put(0x252058,201);run();assert(success&&fabsf(spawn_position[2]-27.5f)<.001f); /* Unrelated controller memory must not count as a seat. */
 reset();pending=(1u<<DEV_SPAWN_CREW_SHIFT)|(3u<<DEV_SPAWN_FACTION_SHIFT);run();assert(success);
 assert(!strcmp(property("RiderOccupant_a"),"template_nk_driver"));assert(!strcmp(property("RiderOccupant_b"),"none"));
 reset();pending=(2u<<DEV_SPAWN_CREW_SHIFT)|(4u<<DEV_SPAWN_FACTION_SHIFT);run();assert(success);
 assert(!strcmp(property("faction"),"sk"));assert(!strcmp(property("RiderOccupant_a"),"template_sk_driver"));
 assert(!strcmp(property("RiderOccupant_b"),"template_sk_gunner"));assert(!strcmp(property("RiderOccupant_c"),"template_sk_soldier"));
 assert(!strcmp(property("RiderOccupant_d"),"none"));assert(!strcmp(property("aiType"),"none"));
 reset();pending=DEV_SPAWN_TROOP|(3u<<DEV_SPAWN_COUNT_SHIFT);run();assert(success&&spawns==4);
 assert(!*property("aiType") && !strcmp(property("encounter"),"none"));
 reset();pending=DEV_SPAWN_TROOP|(8u<<DEV_SPAWN_WEAPON_SHIFT)|(3u<<DEV_SPAWN_COUNT_SHIFT);run();assert(success&&spawns==4);
 assert(!strcmp(property("weapon_A_template"),"template_pic_sniperrifle"));
 reset();pending=DEV_SPAWN_TROOP;run();assert(success && !*property("weapon_A_template"));
 variant_troop=catalog[0];variant_troop.template_hash=dev_hash("template_mafia_soldier");
 variant_troop.model_hash=dev_hash("mafia_hum_heavysoldier");
 reset();put(0x6438A8,3);put(0x230008,variant_troop.model_hash);put(0x231008,0x242000);
 pending=DEV_SPAWN_TROOP|1u|(8u<<DEV_SPAWN_WEAPON_SHIFT);run();assert(success);
 unsigned weapon_properties=0;for(unsigned i=0;i<properties;i++)if(!strcmp(keys[i],"weapon_A_template"))weapon_properties++;
 assert(weapon_properties==1 && !strcmp(property("weapon_A_template"),"template_pic_sniperrifle"));
 assert(!strcmp(property("geometryfile"),"mafia_hum_heavysoldier"));
 assert(!strcmp(dev_troop_variants[0].weapon,"template_pic_lmg"));
 reset();pending=DEV_SPAWN_TROOP|(31u<<DEV_SPAWN_WEAPON_SHIFT);run();assert(!success&&!spawns);
 reset();pending=1u<<DEV_SPAWN_WEAPON_SHIFT;run();assert(!success&&!spawns);
 reset();pending=DEV_SPAWN_TROOP|(12u<<DEV_SPAWN_COUNT_SHIFT);run();assert(!success&&!spawns);
 reset();pending=(2u<<DEV_SPAWN_CREW_SHIFT)|(7u<<DEV_SPAWN_FACTION_SHIFT);run();assert(!success&&!spawns);
 reset();freecam=1;run();assert(!success&&!spawns&&strstr(message,"camera is not ready"));
 reset();freecam=freecam_ready=1;freecam_position[0]=1000;freecam_position[1]=150;freecam_position[2]=2000;
 freecam_direction[0]=1;freecam_direction[1]=-.5f;freecam_direction[2]=0;
 run();assert(success&&fabsf(spawn_position[0]-1007.5f)<.001f&&spawn_position[2]==2000);
 reset();freecam=freecam_ready=1;pending=DEV_SPAWN_TROOP;run();assert(success&&spawn_position[0]>1000&&spawn_position[2]>1900);
 reset();freecam=freecam_ready=1;obstruction=1;run();assert(!success&&!spawns);
 reset();freecam=freecam_ready=1;ground_missing=1;run();assert(!success&&!spawns);
 puts("PASS: troop batch bounds, driver/full crew roles, extraction-seat exclusion, regional/model gates, invalid state, placement support, obstacles, large-vehicle/player clearance, and CPU-context restoration");return 0;
}

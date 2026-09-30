/* Exercise the production boid bridge with bounded guest memory and retail calls. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned char memory[0x800000];
static uint32_t g_esp=0x700000, g_xbox_mem_offset=1, cpu_marker=123;
static int pending, reports, success, created, removed, live[15], missing_assets, dev_region_import_active;
static float player_position[3]={10,20,30};
typedef struct {uint32_t esp, marker;} recomp_saved_guest_cpu_context;
static void *guest_ptr(uint32_t p){assert(p<sizeof(memory));return memory+p;}
static uint32_t guest_u32(uint32_t p){uint32_t v;memcpy(&v,guest_ptr(p),4);return v;}
static void put(uint32_t p,uint32_t v){memcpy(guest_ptr(p),&v,4);}
static int dev_guest_address(uint32_t p,unsigned n){return p>=0x10000&&p<sizeof(memory)&&n<=sizeof(memory)-p;}
static int recomp_lookup(uint32_t va){return va!=0;}
static void recomp_save_guest_cpu_context(recomp_saved_guest_cpu_context *s){s->esp=g_esp;s->marker=cpu_marker;}
static void recomp_restore_guest_cpu_context(const recomp_saved_guest_cpu_context *s){g_esp=s->esp;cpu_marker=s->marker;}
static int recomp_dev_take_boids(void){int result=pending;pending=0;return result;}
static void recomp_dev_spawn_result(int ok,const char *message){assert(message&&*message);reports++;success=ok;}
static int dev_resident_model(uint32_t hash){return !missing_assets;}
static void dev_property(uint32_t stack,uint32_t property,uint32_t list,const char *key,const char *value){}
static void xbox_preview_log_event(const char *tag,const char *format,...){}
static uint32_t boid_hash(const char *);
static uint32_t dev_call(uint32_t stack,uint32_t va,uint32_t object,unsigned count,const uint32_t *args){
 cpu_marker=456;
 switch(va){
 case 0x001EC220:{unsigned i=args[0]-1;return i<15&&live[i]?0x100000+i*0x1000:0;}
 case 0x001ED340:return !missing_assets;
 case 0x0008C7E0:assert(args[0]==0x660E4490u);return 0x200000;
 case 0x1100:assert(object==0x220000);memcpy(guest_ptr(args[0]),player_position,12);return 1;
 case 0x00174480:{
  unsigned i;assert(sscanf(guest_ptr(args[1]),"recomp_boid_%u",&i)==1&&i<15);assert(!live[i]);
  uint32_t actor=0x100000+i*0x1000,spore=actor+0x400;live[i]=1;created++;
  put(actor,0x002E0468);put(actor+4,boid_hash(guest_ptr(args[1])));put(actor+8,spore);put(spore+0x28,i+1);put(spore+0x30,0xE00);
  return actor;
 }
 case 0x1200:{unsigned i=(object-0x100000)/0x1000;assert(i<15&&live[i]);live[i]=0;removed++;return 1;}
 case 0x1300: /* Retail small-model animation setup enables throttling. */
  {
   uint32_t track=0x400000+(object-0x100000);float phase=0,rate=1;
   put(object+0x11C,object+0x500);put(object+0x504,1);put(object+0x508,track);memory[object+0x530]=1;
   memcpy(guest_ptr(track+0xB20),&phase,4);memcpy(guest_ptr(track+0xB3C),&rate,4);return 1;
  }
 case 0x1400:memcpy(guest_ptr(object+0xE0),guest_ptr(args[0]),12);return 1;
 default:return 0;
 }
}
#include "boids_guest.h"
static void tick(int request){
 for(unsigned i=0;i<15;i++)if(live[i]) {
  uint32_t track=0x400000+i*0x1000;float *phase=guest_ptr(track+0xB20),*rate=guest_ptr(track+0xB3C);
  *phase+=*rate/30; if(*phase>1)*phase-=1;
 }
 pending=request;union {float seconds;uint32_t bits;} delta={.seconds=1.f/30};
 /* World/camera arguments deliberately invalid: placement must use player0. */
 recomp_boids_tick(delta.bits,0,0);assert(!pending);assert(cpu_marker==123&&g_esp==0x700000);
 /* An unrelated actor/controller must retain every byte, including its throttle. */
 for(unsigned i=0;i<0x50;i++)assert(memory[0x300500+i]==0xA5);
}
int main(void){
 put(0x300000,0x002E0468);put(0x30011C,0x300500);memset(memory+0x300500,0xA5,0x50);
 put(0x413F6C,0x4249D707);put(0x413F68,0xC2CBD863);put(0x403970,0x4A5220AF);
 put(0x200000,0x210000);put(0x200998,0x220000);
 put(0x220000,0x230000);put(0x230034,0x1100);
 /* Controller transform is deliberately unrelated to the human. */
 float poison=NAN;memcpy(guest_ptr(0x2000D8),&poison,4);
 float backwards=-1;memcpy(guest_ptr(0x2200D8),&backwards,4);
 put(0x2E0468+0x10,0x1200);put(0x2E0468+0x12C,0x1300);put(0x2E0468+0x64,0x1400);
 tick(0);assert(!created&&!reports);
 put(0x200998,0);tick(1);assert(!success&&!created);put(0x200998,0x220000);reports=0;
 tick(1);assert(success&&created==15&&removed==0&&boid_active);
 for(unsigned i=0;i<15;i++)assert(memory[0x100530+i*0x1000]==0);
 assert(boid_world.center.x==10&&boid_world.center.y==23&&boid_world.center.z==55);
 tick(0);assert(created==15&&reports==1);
 unsigned holds[15]={0},resumes[15]={0};int held[15]={0};
 for(unsigned frame=0;frame<900;frame++){
  tick(0);
  for(unsigned i=0;i<15;i++){
   uint32_t track=0x400000+i*0x1000;float rate=*(float *)guest_ptr(track+0xB3C);
   if(rate==0){assert(*(float *)guest_ptr(track+0xB20)==BOID_GLIDE_PHASE);holds[i]++;}
   else {assert(rate==1);if(held[i])resumes[i]++;}
   held[i]=rate==0;
  }
 }
 for(unsigned i=0;i<15;i++)assert(holds[i]>0 && resumes[i]>0);
 player_position[0]=400;tick(1);assert(success&&created==30&&removed==15&&boid_world.center.x==400);
 memset(live,0,sizeof(live));tick(0);assert(!boid_active&&created==30);tick(0);assert(created==30);
 tick(1);assert(success&&created==45);
 put(0x403970,0x4A364A32);tick(0);assert(!boid_active&&removed==30&&created==45);tick(0);assert(created==45);
 put(0x403970,0);tick(1);assert(!success&&!boid_active&&created==45);
 put(0x403970,0x4A5220AF);missing_assets=1;tick(1);assert(!success&&!boid_active);missing_assets=0;tick(0);assert(created==45);
 tick(1);assert(success&&created==60);
 put(0x413F68,0);tick(1);assert(!success);put(0x413F68,0xC2CBD863);tick(0);assert(created==60);
 puts("PASS: explicit spawn, player placement, repeat replacement, reload/travel cleanup, no automatic retry, state rejection and CPU restoration");
}

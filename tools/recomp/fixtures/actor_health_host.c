
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
static unsigned char memory[0x400000];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp;
static float xmm0;
static double g_fp_stack[8];
static uint32_t g_fp_top;
static unsigned kills,updates,flashes;
#define g_esp esp
#define MEMD(a) (*(double *)(memory+(uint32_t)(a)))
#define HI8(a) ((uint8_t)((a)>>8))
#define SET_HI8(a,b) ((a)=((a)&0xffff00ffu)|((uint32_t)(uint8_t)(b)<<8))
#define EVEN_PARITY8(a) (!__builtin_parity((unsigned)(uint8_t)(a)))
#define RECOMP_ICALL_SAFE(t,s) virtual_call(t)
static void recomp_xmm_zero(float *v){*v=0;}
static void sub_00237629(void){g_fp_stack[--g_fp_top & 7u]=floor(MEMD(esp+4));esp+=4;}
static void sub_002375B4(void){eax=(int32_t)g_fp_stack[g_fp_top++ & 7u];esp+=4;}
static void sub_0003EC30(void){assert(!"rider branch not used");}
static void sub_000A4A60(void){assert(!"player rumble not used");}
static void sub_001F4540(void){flashes++;esp+=4;}
static void virtual_call(uint32_t target){
 switch(target){
 case 1: eax=0; break; /* no rider manager, AI or player */
 case 2: kills++; break;
 case 3: updates++; break;
 case 4: eax=0x22000;esp+=4;break; /* GetRenderable(mask), ret 4 */
 default: assert(!"unexpected virtual call");
 }
 esp+=4;
}
static unsigned cancelled;
#define MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define MEM8(a) memory[(uint32_t)(a)]
#define MEMF(a) (*(float *)(memory+(uint32_t)(a)))
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
#define PUSH32(s,v) do {uint32_t value=(v);(s)-=4;MEM32(s)=value;}while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;}while(0)
#define CMP_EQ(a,b) ((a)==(b))
#define CMP_NE(a,b) ((a)!=(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
#define xmm0v (&xmm0)
static void recomp_xmm_loadss(float *v,uint32_t a){*v=MEMF(a);}
static void sub_001EBC10(void){eax=MEM32(ecx+0x28);esp+=4;}
static void sub_0010FA80(void){cancelled++;esp+=4;}
static void sub_0010F510(void){assert(!"unrelated ODF event");}
void sub_0010F170(void);

"""Execute production DPC queue/ISR delivery with synthetic guest callbacks."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GuestDpcTests(unittest.TestCase):
    def test_recovered_directsound_timer_callback_contract(self):
        source = (ROOT / 'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        callback = re.search(
            r'static void mercenaries_dsound_timer_dpc_callback\(void\)\n\{.*?\n\}',
            source,
            re.S,
        )[0]

        self.assertIn('const uint32_t object = guest_u32(g_esp + 8u);', callback)
        self.assertIn('const uint32_t target = guest_u32(vtable + 4u);', callback)
        self.assertIn('*(volatile uint32_t *)guest_ptr(g_esp) = object;', callback)
        self.assertIn('*(volatile uint32_t *)guest_ptr(g_esp) = 0u;', callback)
        self.assertIn('g_ebx = saved_ebx;', callback)
        self.assertIn('g_esi = saved_esi;', callback)
        self.assertIn('g_edi = saved_edi;', callback)
        self.assertIn('g_esp += 20u; /* ret 16 */', callback)
        self.assertRegex(
            source,
            r'case 0x002830EC:\s*return mercenaries_dsound_timer_dpc_callback;',
        )

    def test_queue_interrupt_level_and_context(self):
        source = (ROOT / 'src/kernel/kernel_bridge.c').read_text(encoding='utf-8')
        def function(name):
            return re.search(r'(?:static )?(?:void|int|BOOLEAN) ' + name + r'\([^;]*?\)\n\{.*?\n\}', source, re.S)[0]
        globals_code = source[source.index('#define BRIDGE_GUEST_INTERRUPT_CAPACITY'):source.index('typedef struct bridge_guest_cpu_context')]
        context = source[source.index('typedef struct bridge_guest_cpu_context'):source.index('static void bridge_NtUserIoApcDispatcher')]
        hal = (ROOT / 'src/kernel/kernel_hal.c').read_text(encoding='utf-8')
        mapping = re.search(r'ULONG __stdcall xbox_HalGetInterruptVector\([^;]+?\)\n\{.*?\n\}', hal, re.S)[0]
        bodies = mapping + '\n' + '\n'.join(function(name) for name in (
            'bridge_HalGetInterruptVector', 'bridge_KeInitializeInterrupt', 'bridge_KeConnectInterrupt',
            'bridge_invoke_guest_dpc', 'bridge_queue_guest_dpc', 'bridge_remove_guest_dpc',
            'bridge_deliver_guest_dpcs', 'bridge_KeInsertQueueDpc', 'bridge_KeRemoveQueueDpc',
            'bridge_deliver_hardware_interrupts', 'bridge_KeSynchronizeExecution',
            'bridge_recompute_guest_timer_deadline', 'bridge_deliver_guest_timers'))
        self.assertIn('case 137: return bridge_KeRemoveQueueDpc;', source)
        self.assertIn('bridge_queue_guest_dpc(dpc_va, 0u, 0u);', source)
        prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"line %u\n",__LINE__);exit(2);}}while(0)
typedef int BOOLEAN;
typedef int32_t LONG;
typedef int64_t LONG64;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
#ifdef __declspec
#undef __declspec
#endif
#define __declspec(x)
typedef uint8_t KIRQL;
typedef KIRQL *PKIRQL;
typedef uint32_t ULONG;
typedef void(*recomp_func_t)(void);
#define TRUE 1
#define FALSE 0
#define DISPATCH_LEVEL 2
static unsigned char memory[0x10000];
#define BRIDGE_MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define BRIDGE_MEM8(a) memory[(uint32_t)(a)]
#define XBOX_TO_NATIVE(a) (memory+(uint32_t)(a))
#define STACK_ARG(i) BRIDGE_MEM32(g_esp+4*(i))
static uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp,g_fp_top,g_recomp_current_func;
static double g_fp_stack[8];
static uint16_t g_x87_control_word,g_x87_status_word;
static float g_xmm0[4],g_xmm1[4],g_xmm2[4],g_xmm3[4],g_xmm4[4],g_xmm5[4],g_xmm6[4],g_xmm7[4];
static uint64_t g_mm0,g_mm1,g_mm2,g_mm3,g_mm4,g_mm5,g_mm6,g_mm7;
static KIRQL level;
static int64_t clock_now;
static void xbox_KeQuerySystemTime(LARGE_INTEGER *p){p->QuadPart=clock_now;}
static LONG64 InterlockedExchange64(volatile LONG64 *p,LONG64 v){LONG64 old=*p;*p=v;return old;}
static LONG InterlockedExchange(volatile LONG *p,LONG v){LONG old=*p;*p=v;return old;}
static LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG c){LONG old=*p;if(old==c)*p=v;return old;}
static LONG InterlockedOr(volatile LONG *p,LONG v){LONG old=*p;*p|=v;return old;}
static KIRQL xbox_KfRaiseIrql(KIRQL v){CHECK(v>=level);KIRQL old=level;level=v;return old;}
static void xbox_KfLowerIrql(KIRQL v){CHECK(v<=level);level=v;}
static int bridge_trace_interrupts_enabled(void){return 0;}
static recomp_func_t bridge_lookup_guest_function(uint32_t);
'''
        harness = r'''
static unsigned calls,isr_calls,requeues,timer_mode;
static uint32_t order[1024],arg1[1024];
static void callback(void){
 uint32_t dpc=BRIDGE_MEM32(g_esp+4),a=BRIDGE_MEM32(g_esp+12);
 CHECK(level==2&&BRIDGE_MEM8(0x24)==2&&!g_delivering_hardware_interrupt);
 CHECK(BRIDGE_MEM32(g_esp+8)==0xbaba);CHECK(BRIDGE_MEM32(g_esp+16)==(timer_mode?0:a+1));
 CHECK(!g_delivering_guest_timers);
 CHECK(calls<1024);order[calls]=dpc;arg1[calls++]=a;
 if(requeues){--requeues;CHECK(bridge_queue_guest_dpc(dpc,a+2,a+3));CHECK(!bridge_deliver_guest_dpcs());}
 g_eax=1;g_ecx=2;g_edx=3;g_esi=4;g_edi=5;g_ebx=6;g_seh_ebp=7;g_fp_stack[0]=9;g_xmm3[2]=10;g_mm2=11;g_fp_top=3;g_recomp_current_func=12;
 g_esp+=20;
}
static void isr(void){
 CHECK(level==22&&BRIDGE_MEM8(0x24)==22);++isr_calls;
 CHECK(bridge_queue_guest_dpc(0x1000,31,32));CHECK(!bridge_deliver_guest_dpcs());
 CHECK(calls==0);g_eax=1;g_esp+=12;
}
static void sync_callback(void){CHECK(level==22&&BRIDGE_MEM8(0x24)==22);g_eax=17;g_esp+=8;}
static recomp_func_t bridge_lookup_guest_function(uint32_t va){return va==0xaaaa?callback:va==0xbbbb?isr:va==0xcccc?sync_callback:NULL;}
static void reset(void){
 memset(memory,0,sizeof(memory));g_esp=0x8000;g_eax=123;g_ecx=321;
 g_guest_dpc_count=0;g_kernel_pending_guest_dpcs=0;calls=isr_calls=requeues=timer_mode=0;
 memset(g_guest_timers,0,sizeof(g_guest_timers));clock_now=100;g_kernel_pending_guest_timers=0;
 g_delivering_guest_dpcs=g_delivering_hardware_interrupt=g_delivering_guest_timers=0;
 level=0;g_guest_interrupt_count=0;g_kernel_pending_hardware_interrupts=0;
 for(unsigned i=0;i<256;++i){BRIDGE_MEM32(0x1000+i*32+12)=0xaaaa;BRIDGE_MEM32(0x1000+i*32+16)=0xbaba;}
}
int main(void){
 bridge_guest_cpu_context before={0},after={0};
 reset();CHECK(!bridge_queue_guest_dpc(0,0,0));
 CHECK(bridge_queue_guest_dpc(0x1000,3,4));CHECK(!bridge_queue_guest_dpc(0x1000,7,8));
 CHECK(BRIDGE_MEM32(0x1014)==3&&calls==0);
 CHECK(bridge_queue_guest_dpc(0x1020,5,6));
 bridge_capture_guest_context(&before);level=BRIDGE_MEM8(0x24)=3;
 CHECK(!bridge_deliver_guest_dpcs()&&calls==0&&g_kernel_pending_guest_dpcs);
 level=BRIDGE_MEM8(0x24)=0;CHECK(bridge_deliver_guest_dpcs());bridge_capture_guest_context(&after);
 CHECK(!memcmp(&before,&after,sizeof(before)));CHECK(calls==2&&order[0]==0x1000&&order[1]==0x1020&&arg1[0]==3);
 CHECK(!g_kernel_pending_guest_dpcs&&level==0&&BRIDGE_MEM8(0x24)==0);
 reset();CHECK(bridge_queue_guest_dpc(0x1000,1,2));CHECK(bridge_queue_guest_dpc(0x1020,3,4));
 CHECK(bridge_remove_guest_dpc(0x1000));CHECK(!bridge_remove_guest_dpc(0x1000));bridge_deliver_guest_dpcs();CHECK(calls==1&&order[0]==0x1020);
 reset();CHECK(bridge_queue_guest_dpc(0x1000,1,2));requeues=3;bridge_deliver_guest_dpcs();CHECK(calls==4&&!g_kernel_pending_guest_dpcs);
 reset();CHECK(bridge_queue_guest_dpc(0x1000,1,2));requeues=300;bridge_deliver_guest_dpcs();CHECK(calls==256&&g_kernel_pending_guest_dpcs);bridge_deliver_guest_dpcs();CHECK(calls==301&&!g_kernel_pending_guest_dpcs);
 reset();for(unsigned i=0;i<256;++i)CHECK(bridge_queue_guest_dpc(0x1000+i*32,i,i+1));
 BRIDGE_MEM32(0x400c)=0xaaaa;CHECK(!bridge_queue_guest_dpc(0x4000,0,1));bridge_deliver_guest_dpcs();CHECK(calls==256);
 reset();BRIDGE_MEM32(g_esp)=5;BRIDGE_MEM32(g_esp+4)=0x400;bridge_HalGetInterruptVector();
 CHECK(g_eax==0x35&&BRIDGE_MEM8(0x400)==22);
 BRIDGE_MEM32(g_esp)=0x900;BRIDGE_MEM32(g_esp+4)=0xbbbb;BRIDGE_MEM32(g_esp+8)=0x123;
 BRIDGE_MEM32(g_esp+12)=0x35;BRIDGE_MEM32(g_esp+16)=0xbad00016; /* Only CL-width KIRQL is significant. */
 BRIDGE_MEM32(g_esp+20)=0;BRIDGE_MEM32(g_esp+24)=1;bridge_KeInitializeInterrupt();bridge_KeConnectInterrupt();
 CHECK(g_eax==TRUE&&g_guest_interrupt_count==1&&g_guest_interrupts[0].irql==22);
 g_kernel_pending_hardware_interrupts=1<<5;level=BRIDGE_MEM8(0x24)=22;
 CHECK(!bridge_deliver_hardware_interrupts()&&!isr_calls&&g_kernel_pending_hardware_interrupts);
 level=BRIDGE_MEM8(0x24)=0;bridge_capture_guest_context(&before);CHECK(bridge_deliver_hardware_interrupts());bridge_capture_guest_context(&after);
 CHECK(isr_calls==1&&calls==1&&!memcmp(&before,&after,sizeof(before))&&level==0);
 BRIDGE_MEM32(g_esp)=0x900;BRIDGE_MEM32(g_esp+4)=0xcccc;BRIDGE_MEM32(g_esp+8)=0x123;
 bridge_KeSynchronizeExecution();CHECK(g_eax==17&&g_esp==0x8000&&level==0&&BRIDGE_MEM8(0x24)==0);
 /* Expired timers queue work while elevated; they never enter a DPC in the ISR. */
 reset();timer_mode=1;level=BRIDGE_MEM8(0x24)=5;g_delivering_hardware_interrupt=1;
 g_guest_timers[0]=(bridge_guest_timer){0x5000,0x1000,90,0,1};
 g_guest_timers[1]=(bridge_guest_timer){0x5040,0x1020,90,2,1};
 g_guest_timers[2]=(bridge_guest_timer){0x5080,0x1040,200,0,1};
 CHECK(bridge_deliver_guest_timers()&&calls==0&&g_guest_dpc_count==2);
 CHECK(BRIDGE_MEM32(0x5004)==1&&!g_guest_timers[0].active&&g_guest_timers[1].due_time_100ns==20090);
 CHECK(g_guest_timer_next_due_100ns==200&&g_guest_timers[2].active);
 g_delivering_hardware_interrupt=0;level=BRIDGE_MEM8(0x24)=0;
 CHECK(bridge_deliver_guest_dpcs()&&calls==2&&order[0]==0x1000&&order[1]==0x1020);
 clock_now=201;CHECK(bridge_deliver_guest_timers()&&calls==3&&order[2]==0x1040);
 puts("DPC FIFO, duplicate, cancel, self-requeue, capacity, ISR ordering, IRQL and full CPU-context tests passed");return 0;
}
'''
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='merc-dpc-queue-') as directory:
            path = Path(directory)
            mutants = {
                'fixed': bodies,
                'discard_queue_head': bodies.replace('bridge_invoke_guest_dpc(work.dpc_va, work.argument1, work.argument2);', '(void)work;'),
                'missing_dispatch_level': bodies.replace('BRIDGE_MEM8(0x24u) = DISPATCH_LEVEL;', 'BRIDGE_MEM8(0x24u) = 0u;'),
                'timer_callback_in_isr': bodies.replace('bridge_queue_guest_dpc(dpc_va, 0u, 0u);', 'bridge_invoke_guest_dpc(dpc_va, 0u, 0u);'),
                'wide_irql_argument': bodies.replace('STACK_ARG(4) & 0xFFu', 'STACK_ARG(4)'),
            }
            for name, actual in mutants.items():
                with self.subTest(name=name):
                    (path / 'test.c').write_text(prelude + globals_code + context + actual + harness, encoding='utf-8')
                    exe = path / 'test.exe'
                    subprocess.run([compiler, '-O2', '-std=c11', str(path / 'test.c'), '-o', str(exe)], check=True)
                    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
                    if name == 'fixed':
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        print(result.stdout.strip())
                    else:
                        self.assertNotEqual(result.returncode, 0, name)
            print('HAL registration and timer integration passed; all four negative controls rejected')


if __name__ == '__main__':
    unittest.main()

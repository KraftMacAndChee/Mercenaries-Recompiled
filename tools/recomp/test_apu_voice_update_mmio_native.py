"""Execute the lifted voice update against an ordered MMIO event oracle."""
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DECLARATIONS = ('    extern uint32_t recomp_apu_read32(uint32_t);\n'
                '    extern void recomp_apu_write32(uint32_t, uint32_t);\n')


class VoiceUpdateMMIOTests(unittest.TestCase):
    def test_register_order_polling_and_guest_state(self):
        patch = runpy.run_path(str(ROOT / 'ports/mercenaries/scripts/Patch-Generated.py'))['patch_apu_voice_update_mmio']
        text = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0013.c').read_text(encoding='utf-8')
        body = re.search(r'void sub_002A4473\(void\)\n\{.*?\n\}', text, re.S)[0]
        original = body.replace(DECLARATIONS, '')
        original = re.sub(r'recomp_apu_read32\(\(uint32_t\)(-\d+)\)', r'MEM32(\1)', original)
        original = re.sub(r'recomp_apu_write32\(\(uint32_t\)(-\d+), (\w+)\);', r'MEM32(\1) = \2;', original)
        patched = patch(original)
        self.assertNotEqual(patched, original)
        self.assertEqual(patch(patched), patched)
        self.assertEqual(patch('void unrelated(void) {}'), 'void unrelated(void) {}')
        # Reversing only the seven IO substitutions must recover every other
        # instruction verbatim, including stack operations and lock ordering.
        recovered = patched.replace(DECLARATIONS, '')
        recovered = re.sub(r'recomp_apu_read32\(\(uint32_t\)(-\d+)\)', r'MEM32(\1)', recovered)
        recovered = re.sub(r'recomp_apu_write32\(\(uint32_t\)(-\d+), (\w+)\);', r'MEM32(\1) = \2;', recovered)
        self.assertEqual(recovered, original)
        for statement in re.findall(r'[^\n]*MEM32\(-\d+\)[^\n]*', original):
            with self.assertRaises(RuntimeError):
                patch(original.replace(statement, '    /* changed lift */', 1))
        with self.assertRaises(RuntimeError):
            patch(original.replace('MEM32(-25033988) = 1;', 'recomp_apu_write32((uint32_t)-25033988, 1);'))

        fixture = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static uint32_t ram[4096], initial[4096];
static uint32_t eax,ebx,ecx,edx,esi,edi,esp,g_seh_ebp;
static uint32_t payload[9];
#define MEM32(a) ram[(uint32_t)(a)/4]
#define MEM16(a) (*(uint16_t *)((uint8_t*)ram+(uint32_t)(a)))
#define MEM8(a) (*((uint8_t*)ram+(uint32_t)(a)))
#define PUSH32(s,v) do {uint32_t value=(v); (s)-=4; MEM32(s)=value;}while(0)
#define POP32(s,v) do {(v)=MEM32(s);(s)+=4;}while(0)
#define TEST_Z(a,b) (((a)&(b))==0)
#define CMP_B(a,b) ((uint32_t)(a)<(uint32_t)(b))
#define CMP_BE(a,b) ((uint32_t)(a)<=(uint32_t)(b))
#define ZX8(a) ((uint32_t)(uint8_t)(a))
#define ZX16(a) ((uint32_t)(uint16_t)(a))
#define RECOMP_TRACE_FUNC(a) ((void)0)
static unsigned count,poll,nevents;
static struct Event {uint32_t address,value;} events[64];
static unsigned delays;
uint32_t recomp_apu_read32(uint32_t address){
 assert(address==0xfe820010u); unsigned p=poll++;
 uint32_t value=p<delays?0:count*6*4;
 events[nevents++]=(struct Event){address,value};return value;
}
void recomp_apu_write32(uint32_t address,uint32_t value){
 assert(nevents<64);events[nevents++]=(struct Event){address,value};
}
static void sub_0029CBB0(void){esp+=4;}
static void sub_0029CBD2(void){esp+=4;}
static void sub_002A3311(void){memcpy((uint8_t*)ram+MEM32(esp+4),payload,sizeof(payload));esp+=8;}
'''
        fixture += patched
        fixture += r'''
static void reset(unsigned active,unsigned voices,unsigned waits,unsigned seed){
 count=voices;delays=waits;poll=nevents=0;
 for(unsigned i=0;i<4096;i++)ram[i]=seed+i*0x1234567u;
 for(unsigned i=0;i<9;i++)payload[i]=seed^(i*0x9e3779b1u);
 eax=0x1111;ebx=0x2222;ecx=0x400;edx=0x4444;esi=0x5555;edi=0x6666;esp=0x3000;g_seh_ebp=0x1234;
 MEM8(0x412)=active;MEM8(0x464)=voices;
 for(unsigned i=0;i<3;i++)MEM16(0x40c+i*2)=(uint16_t)(seed+i);
}
int main(void){
 unsigned cases=0;
 for(unsigned active=0;active<2;active++)for(unsigned voices=0;voices<=3;voices++)
 for(unsigned waits=0;waits<4;waits++)for(unsigned seed=0;seed<32;seed++){
  reset(active,voices,waits,seed);memcpy(initial,ram,sizeof(ram));
  sub_002A4473();
  assert(eax==0&&ebx==0x2222&&esi==0x5555&&edi==0x6666&&esp==0x3004);
  if(!active){assert(nevents==0&&poll==0);}else{
   unsigned reads=voices?waits+1:1;
   assert(poll==reads&&nevents==reads+6*voices);
   for(unsigned i=0;i<voices;i++){
    const uint32_t addresses[]={0xfe8202f8u,0xfe8202fcu,0xfe820360u,0xfe820364u,0xfe820368u,0xfe8202fcu};
    const uint32_t values[]={(uint16_t)(seed+i),1,payload[i],payload[3+i],payload[6+i],0};
    for(unsigned j=0;j<6;j++){assert(events[reads+6*i+j].address==addresses[j]);assert(events[reads+6*i+j].value==values[j]);}
   }
  }
  // The update may only modify its own stack scratch; actor and other RAM stay intact.
  assert(memcmp(ram,initial,0x2fc0)==0);
  assert(memcmp((uint8_t*)ram+0x3000,(uint8_t*)initial+0x3000,sizeof(ram)-0x3000)==0);
  cases++;
 }
 printf("PASS: %u lifted voice updates; FIFO retries, exact six-write voice order, payloads, registers and stack\n",cases);
}
'''
        with tempfile.TemporaryDirectory(prefix='apu-voice-update-') as directory:
            source = Path(directory) / 'test.c'
            exe = source.with_suffix('.exe')
            source.write_text(fixture, encoding='utf-8')
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe', '-std=c11', '-O2', str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()

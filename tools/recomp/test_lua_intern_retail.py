"""Check maintained string interning against the supported retail instructions.

Retail executes valid buckets. Corrupt-link recovery is separately tested as
project policy and is not attributed to the original Lua implementation.
"""
from pathlib import Path
import hashlib
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn import x86_const as r

BASE=0x100000
SIZE=0x20000
STATE,GLOBAL,TABLE,STRING,NODES,STACK,STOP=0x100000,0x100100,0x100200,0x101000,0x104000,0x11E000,0x11F000
ALLOCATED=0x120000
REGS=(r.UC_X86_REG_EBX,r.UC_X86_REG_ESI,r.UC_X86_REG_EDI,r.UC_X86_REG_EBP)

PRELUDE=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char memory[0x4000000];
static uint32_t g_eax,g_ebx,g_esi,g_edi,g_esp,g_seh_ebp;
static uint32_t allocation[5];
static void *guest_ptr(uint32_t a){return memory+a;}
static uint32_t guest_u32(uint32_t a){uint32_t v;memcpy(&v,memory+a,4);return v;}
static uint8_t guest_u8(uint32_t a){return memory[a];}
static void sub_001E2640(void){
 allocation[0]++;allocation[1]=g_eax;
 allocation[2]=guest_u32(g_esp+4);allocation[3]=guest_u32(g_esp+8);allocation[4]=guest_u32(g_esp+12);
 g_eax=0x120000;g_esp+=4;
}
'''
HARNESS=r'''
int main(int argc,char **argv){
 if(argc!=3)return 2;
 FILE *in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");if(!in||!out)return 3;
 while(fread(memory+0x100000,1,0x20000,in)==0x20000){
  memset(allocation,0,sizeof(allocation));g_eax=0;
  g_esp=0x11E000;g_ebx=11;g_esi=22;g_edi=33;g_seh_ebp=44;
  sub_001E26E0();
  uint32_t result[]={g_eax,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp};
  if(fwrite(result,4,6,out)!=6||fwrite(allocation,4,5,out)!=5)return 4;
  /* Include bucket/node bytes to prove valid entries survive recovery. */
  if(fwrite(memory+0x100200,1,0xA000-0x200,out)!=0xA000-0x200)return 5;
 }
 fclose(in);fclose(out);return 0;
}
'''


def string_hash(value):
    h=len(value);step=(len(value)>>5)+1
    for remaining in range(len(value),step-1,-step):
        h=(h^(((h<<5)+(h>>2)+value[remaining-1])&0xFFFFFFFF))&0xFFFFFFFF
    return h


def make_case(value,entries,tail=0):
    memory=bytearray(SIZE)
    def put(address,v):struct.pack_into('<I',memory,address-BASE,v)
    put(STATE+16,GLOBAL);put(GLOBAL,TABLE);put(GLOBAL+8,16)
    memory[STRING-BASE:STRING-BASE+len(value)]=value
    slot=TABLE+(string_hash(value)&15)*4
    put(slot,NODES if entries else tail)
    for index,entry in enumerate(entries):
        node=NODES+index*0x800
        put(node,node+0x800 if index+1<len(entries) else tail)
        put(node+8,string_hash(entry));put(node+12,len(entry))
        memory[node+16-BASE:node+16-BASE+len(entry)]=entry
    for offset,v in enumerate((STOP,STATE,STRING,len(value))):put(STACK+offset*4,v)
    return memory, (NODES+(len(entries)-1)*0x800 if entries else slot)


class RetailStringInterning(unittest.TestCase):
    def test_native_lookup_and_recovery(self):
        raw=(ROOT/'game_files/mercenaries-retail/default.xbe').read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),'aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7')
        config.configure_from_xbe(str(ROOT/'game_files/mercenaries-retail/default.xbe'))
        start=config.va_to_file_offset(0x1E26E0);code=raw[start:start+129]
        self.assertEqual(hashlib.sha256(code).hexdigest(),'1c0304a37244962d5f880e28a9666522d0a990c11615c09b44fd094a5615440c')
        u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0x100000,0x100000)
        u.mem_write(0x1E26E0,code);u.mem_write(0x1E2640,b'\xC3')
        allocation=[]
        def allocate(emu,address,size,user):
            if address!=0x1E2640:return
            sp=emu.reg_read(r.UC_X86_REG_ESP)
            allocation[:]=[1,emu.reg_read(r.UC_X86_REG_EAX),*struct.unpack('<III',emu.mem_read(sp+4,12))]
            emu.reg_write(r.UC_X86_REG_EAX,ALLOCATED)
        u.hook_add(UC_HOOK_CODE,allocate)
        rng=random.Random(20260929)
        cases=[]
        for length in (0,1,2,15,31,32,33,63,64,65,127,255,256,1024):
            for iteration in range(24):
                value=bytes(rng.randrange(256) for _ in range(length))
                entries=[bytes(rng.randrange(256) for _ in range(length+(i%2))) for i in range(iteration%5)]
                if iteration%3 and entries:entries[iteration%len(entries)]=value
                cases.append((make_case(value,entries)[0],None))
        # A malformed link after valid nonmatching nodes must only sever that
        # link. The retail oracle is deliberately not run on unsafe pointers.
        for tail in (1,0xFFFF,0x3FFFFF0,0xFFFFFFFF):
            for entries in ([],[b'other',b'longer']):
                memory,link=make_case(b'needle',entries,tail)
                cases.append((memory,link))
        manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
        function=re.search(r'void sub_001E26E0\(void\)\n\{.*?\n\}',manual,re.S)
        self.assertIsNotNone(function)
        compiler=Path('C:/msys64/mingw64/bin/gcc.exe');env=os.environ.copy()
        env['PATH']=str(compiler.parent)+os.pathsep+env['PATH']
        with tempfile.TemporaryDirectory(prefix='retail-string-intern-') as folder:
            d=Path(folder);(d/'test.c').write_text(PRELUDE+function.group()+HARNESS)
            (d/'input.bin').write_bytes(b''.join(memory for memory,_ in cases))
            subprocess.run([str(compiler),'-std=c11','-O2',str(d/'test.c'),'-o',str(d/'test.exe')],check=True,env=env)
            subprocess.run([str(d/'test.exe'),str(d/'input.bin'),str(d/'output.bin')],check=True,env=env,capture_output=True)
            output=(d/'output.bin').read_bytes()
        stride=44+0xA000-0x200
        self.assertEqual(len(output),len(cases)*stride)
        for index,(memory,repair_link) in enumerate(cases):
            with self.subTest(case=index):
                actual=output[index*stride:(index+1)*stride]
                if repair_link is None:
                    u.mem_write(BASE,bytes(memory));allocation[:]=[0]*5
                    u.reg_write(r.UC_X86_REG_ESP,STACK);u.reg_write(r.UC_X86_REG_EFLAGS,2)
                    for reg,value in zip(REGS,(11,22,33,44)):u.reg_write(reg,value)
                    u.emu_start(0x1E26E0,STOP,count=100000)
                    self.assertEqual(u.reg_read(r.UC_X86_REG_EIP),STOP)
                    expected=[u.reg_read(r.UC_X86_REG_EAX),u.reg_read(r.UC_X86_REG_ESP),*[u.reg_read(reg) for reg in REGS],*allocation]
                    self.assertEqual(actual[:44],struct.pack('<11I',*expected))
                else:
                    expected=[ALLOCATED,STACK+4,11,22,33,44,1,string_hash(b'needle'),STATE,STRING,6]
                    self.assertEqual(actual[:44],struct.pack('<11I',*expected))
                    struct.pack_into('<I',memory,repair_link-BASE,0)
                self.assertEqual(actual[44:],memory[0x200:0xA000])
        print(f'PASS: {len(cases)-8} retail interning comparisons and 8 isolated corrupt-link recoveries')

if __name__=='__main__':unittest.main()

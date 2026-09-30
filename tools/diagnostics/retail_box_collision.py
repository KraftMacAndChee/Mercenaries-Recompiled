"""Execute retail box collision routines using instruction-derived guest records.

This diagnostic loads the user's retail XBE under Unicorn. It neither loads nor
links any SDK. Guest offsets are documented in retail-collision-evidence.md.
"""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn import x86_const as r

IDENTITY=((1,0,0),(0,1,0),(0,0,1),(0,0,0))
REPRO={
 'a_half':(13.7495079,4.2082262,1.07783031),'b_half':(.104113415,.0795426518,.100000001),
 'a_skin':.100000001,'b_skin':.0500000007,
 'a_transform':((1,0,0),(0,1,0),(0,0,1),(2346.01904,-.551849365,-5.35593224)),
 'b_transform':((.316919535,4.8240536e-9,.948452413),(-.173192799,.983186245,.0578713007),(-.932505369,-.182605684,.31159091),(2348.09351,-1.64946258,-3.63017392)),
 'path':(-1.8503418,-.361977816,.616770983),
}

class RetailBoxCollision:
    HEAP=0x05000000
    A=HEAP+0x100; B=HEAP+0x200
    ASHAPE=HEAP+0x300; BSHAPE=HEAP+0x400
    AMOTION=HEAP+0x500; BMOTION=HEAP+0x600
    INPUT=HEAP+0x800; COLLECTOR=HEAP+0x900; VTABLE=HEAP+0xa00
    CALLBACK=HEAP+0xb00; STOP=HEAP+0xb10; STACK=HEAP+0xf0000
    DISPATCH=HEAP+0x1000; CONFIG=HEAP+0x9000; SHAPE_VTABLE=HEAP+0x9100; GET_TYPE=HEAP+0x9200
    def __init__(self,xbe=None):
        xbe=Path(xbe) if xbe else ROOT/'game_files/mercenaries-retail/default.xbe'
        raw=xbe.read_bytes()
        if hashlib.sha256(raw).hexdigest()!='aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7':
            raise ValueError('Unsupported retail XBE')
        config.configure_from_xbe(str(xbe))
        u=self.u=Uc(UC_ARCH_X86,UC_MODE_32)
        u.mem_map(0,0x1000000);u.mem_map(self.HEAP,0x100000)
        for section in config._SECTIONS:
            u.mem_write(section.va,raw[section.raw_addr:section.raw_addr+section.raw_size])
        self.code_hashes={}
        for address,length in [(0x1A4AC0,0x10f),(0x1B34C0,0x3dd)]:
            self.code_hashes[hex(address)]=hashlib.sha256(u.mem_read(address,length)).hexdigest()
        self.hits=[]
        self.word(0,0xffffffff)
        u.mem_write(self.CALLBACK,b'\xc2\x04\x00')
        # Minimal shape-type getter; only its returned table index is needed.
        u.mem_write(self.GET_TYPE,b'\x31\xc0\xc3')
        u.hook_add(UC_HOOK_CODE,self._collect,begin=self.CALLBACK,end=self.CALLBACK)
    def word(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
    def read_word(self,a):return struct.unpack('<I',self.u.mem_read(a,4))[0]
    def floats(self,a,values):self.u.mem_write(a,struct.pack('<'+'f'*len(values),*values))
    def _collect(self,u,address,size,data):
        assert u.reg_read(r.UC_X86_REG_ECX)==self.COLLECTOR
        ptr=self.read_word(u.reg_read(r.UC_X86_REG_ESP)+4)
        f=struct.unpack('<8f',u.mem_read(ptr,32))
        self.hits.append({'position':list(f[:3]),'distance':f[3],'normal':list(f[4:7])})
        self.floats(self.COLLECTOR+4,[f[3]])
    def prepare(self,case):
        for body,shape,motion,side in [(self.A,self.ASHAPE,self.AMOTION,'a'),(self.B,self.BSHAPE,self.BMOTION,'b')]:
            self.word(body,shape);self.word(body+8,motion)
            self.word(shape,self.SHAPE_VTABLE)
            self.floats(shape+0xc,[case[side+'_skin']])
            self.floats(shape+0x10,[*case[side+'_half'],0])
            for i,column in enumerate(case[side+'_transform']):self.floats(motion+0x20+16*i,[*column,0])
        self.word(self.SHAPE_VTABLE+0x14,self.GET_TYPE)
        self.word(self.COLLECTOR,self.VTABLE);self.word(self.VTABLE+4,self.CALLBACK)
        self.floats(self.COLLECTOR+4,[3.402823466e38])
        self.word(self.INPUT,self.DISPATCH);self.word(self.INPUT+0xc,self.CONFIG)
        self.word(0x4409E4,0);self.word(0x4409E8,0)  # Disable optional profiler output.
        self.word(self.DISPATCH+0x218c,0x1A4AC0)
        self.hits=[]
    def call(self,entry,args):
        u=self.u
        self.word(self.STACK,self.STOP)
        for i,v in enumerate(args):self.word(self.STACK+4+4*i,v)
        for reg,value in [(r.UC_X86_REG_ESP,self.STACK),(r.UC_X86_REG_EBP,0),(r.UC_X86_REG_FPCW,0x037f),(r.UC_X86_REG_FPTAG,0xffff),(r.UC_X86_REG_FPSW,0),(r.UC_X86_REG_MXCSR,0x1f80),(r.UC_X86_REG_EBX,0x13579),(r.UC_X86_REG_ESI,0x24680),(r.UC_X86_REG_EDI,0x12345)]:u.reg_write(reg,value)
        try:u.emu_start(entry,self.STOP,count=1000000)
        except Exception as e:raise RuntimeError(f'Retail execution failed at {u.reg_read(r.UC_X86_REG_EIP):08X}') from e
        if u.reg_read(r.UC_X86_REG_EIP)!=self.STOP or u.reg_read(r.UC_X86_REG_ESP)!=self.STACK+4:
            raise RuntimeError('Retail routine did not return within the instruction limit')
        assert self.read_word(0)==0xffffffff
        for reg,value in [(r.UC_X86_REG_EBX,0x13579),(r.UC_X86_REG_ESI,0x24680),(r.UC_X86_REG_EDI,0x12345),(r.UC_X86_REG_EBP,0)]:
            assert u.reg_read(reg)==value
        return list(self.hits)
    def closest(self,case,tolerance=10):
        self.prepare(case);self.floats(self.INPUT+8,[tolerance])
        return self.call(0x1A4AC0,[self.A,self.B,self.INPUT,self.COLLECTOR])
    def cast(self,case):
        self.prepare(case)
        self.floats(self.INPUT+8,[1.1920929e-7])
        self.floats(self.INPUT+0x10,[*case['path'],0])
        self.floats(self.INPUT+0x20,[.1,math.sqrt(sum(v*v for v in case['path']))])
        self.floats(self.CONFIG+0x18,[.01])
        self.word(self.CONFIG+0x1c,10)
        return self.call(0x1B34C0,[self.A,self.B,self.INPUT,self.COLLECTOR,0])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xbe',type=Path,help='User-supplied supported retail Xbox executable')
    args=parser.parse_args()
    oracle=RetailBoxCollision(args.xbe)
    print(json.dumps({'closest':oracle.closest(REPRO),'cast':oracle.cast(REPRO),'code_hashes':oracle.code_hashes},indent=2))
if __name__=='__main__':main()

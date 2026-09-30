"""Compare notification-list mutation behavior with direct retail x86 execution.

Only the user's retail executable is the oracle; no SDK/source tree is opened.
The fixture list and callback operations are constructed here, not extracted from
private declarations. See docs/runtime/retail-notification-evidence.md.
"""
from pathlib import Path
import hashlib
import re
import struct
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.recomp.test_notification_cursor_native import build_executables
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import x86_const as r

ENTRY=0x1FA0F0
CODE_SIZE=0x71
HEAD=0x30F074
COUNT=0x30F084
CURRENT=0x64DAC8
A,B,C,D=0x11000,0x11100,0x11200,0x11300
VTABLE,UPDATE,STOP,STACK=0x20000,0x20100,0x20200,0x100000


def retail_code():
    path=ROOT/'game_files/mercenaries-retail/default.xbe'
    raw=path.read_bytes()
    if hashlib.sha256(raw).hexdigest()!='aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7':
        raise ValueError('Unsupported retail XBE')
    config.configure_from_xbe(str(path))
    offset=config.va_to_file_offset(ENTRY)
    code=raw[offset:offset+CODE_SIZE]
    if code[-3:]!=b'\xc2\x04\x00':
        raise ValueError('Retail function boundary changed')
    return code


def run_retail(code, scenario):
    u=Uc(UC_ARCH_X86,UC_MODE_32)
    for base,size in [(0x10000,0x20000),(0xf0000,0x20000),(0x1FA000,0x1000),(0x30F000,0x1000),(0x64D000,0x1000)]:
        u.mem_map(base,size)
    u.mem_write(ENTRY,code)
    u.mem_write(UPDATE,b'\xc2\x04\x00')  # Synthetic callback uses the observed callee cleanup.
    def read(a):return struct.unpack('<I',u.mem_read(a,4))[0]
    def write(a,v):u.mem_write(a,struct.pack('<I',v))
    def insert(previous,obj):
        node=obj+4;after=read(previous)
        write(obj,VTABLE);write(node,after);write(node+4,previous);write(node+8,obj)
        write(previous,node);write(after+4,node);write(COUNT,read(COUNT)+1)
    def remove(obj):
        node=obj+4
        assert read(node+8)==obj
        write(read(node)+4,read(node+4));write(read(node+4),read(node));write(node+8,0)
        write(COUNT,read(COUNT)-1)
    write(HEAD,HEAD);write(HEAD+4,HEAD);write(VTABLE+4,UPDATE)
    insert(HEAD,A);insert(A+4,B);insert(B+4,C)
    visits=[]
    def callback(uc,address,size,unused):
        obj=uc.reg_read(r.UC_X86_REG_ECX)
        assert read(CURRENT)==obj
        assert read(uc.reg_read(r.UC_X86_REG_ESP)+4)==0x3C888889
        assert len(visits)<8
        visits.append(obj)
        if obj==A:
            if scenario in (1,2,5):remove(B)
            if scenario==2:remove(C)
            if scenario==3:insert(A+4,D)
            if scenario in (4,5):u.mem_write(A+0x18,b'\x01')
            if scenario==6:
                u.mem_write(A+0x18,b'\x01');u.mem_write(A+0x18,b'\x00')
            if scenario in (7,8):remove(A)
            if scenario==8:remove(B);remove(C)
        # Actual retail callbacks preserve nonvolatiles. Native test separately
        # stresses preservation across the recompiler's global-register bridge.
        for reg,value in [(r.UC_X86_REG_EAX,0xDEAD0000),(r.UC_X86_REG_ECX,0xDEAD0004),(r.UC_X86_REG_EDX,0xDEAD0008)]:
            uc.reg_write(reg,value)
    u.hook_add(UC_HOOK_CODE,callback,begin=UPDATE,end=UPDATE)
    write(STACK,STOP);write(STACK+4,0x3C888889)
    for reg,value in [(r.UC_X86_REG_ECX,HEAD),(r.UC_X86_REG_ESP,STACK),(r.UC_X86_REG_ESI,0xABCDEF00),(r.UC_X86_REG_EDI,0xABCDEF04),(r.UC_X86_REG_EBP,0xABCDEF08)]:
        u.reg_write(reg,value)
    u.emu_start(ENTRY,STOP,count=10000)
    assert u.reg_read(r.UC_X86_REG_EIP)==STOP
    assert u.reg_read(r.UC_X86_REG_ESP)==STACK+8
    assert u.reg_read(r.UC_X86_REG_ESI)==0xABCDEF00
    assert u.reg_read(r.UC_X86_REG_EDI)==0xABCDEF04
    assert u.reg_read(r.UC_X86_REG_EBP)==0xABCDEF08
    assert read(CURRENT)==0 and u.mem_read(A+0x18,1)==b'\x00'
    return visits,read(COUNT)


def main():
    code=retail_code()
    with tempfile.TemporaryDirectory(prefix='notification-retail-') as temp:
        native,mutant=build_executables(Path(temp))
        for scenario in range(9):
            visits,remaining=run_retail(code,scenario)
            result=subprocess.run([str(native),str(scenario)],check=True,capture_output=True,text=True,timeout=10)
            match=re.search(r'trace: ([0-9A-F ]+) remaining=(\d+)',result.stdout)
            assert match,result.stdout
            actual=[int(s,16) for s in match[1].split()]
            assert (actual,int(match[2]))==(visits,remaining),(scenario,actual,visits)
    print(f'PASS: nine notification callback-mutation cases match direct retail execution; code SHA-256 {hashlib.sha256(code).hexdigest()}')

if __name__=='__main__':main()

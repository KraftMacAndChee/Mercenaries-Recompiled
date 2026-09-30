"""Direct retail instruction checks for two source-informed loop bounds."""
from pathlib import Path
import hashlib
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
from unicorn import x86_const as r


def main():
    path=ROOT/'game_files/mercenaries-retail/default.xbe';raw=path.read_bytes()
    assert hashlib.sha256(raw).hexdigest()=='aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7'
    config.configure_from_xbe(str(path));u=Uc(UC_ARCH_X86,UC_MODE_32)
    u.mem_map(0x69000,0x1000);u.mem_map(0x90000,0x1000);u.mem_map(0x1000000,0x1000)
    records=[]
    for start,end in [(0x90295,0x902AB),(0x699F9,0x69A23)]:
        off=config.va_to_file_offset(start);code=raw[off:off+end-start]
        u.mem_write(start,code);records.append((hex(start),len(code),hashlib.sha256(code).hexdigest()))
    for count in [-2147483648,-100,-1,0,*range(1,100),2147483647]:
        u.reg_write(r.UC_X86_REG_EAX,count&0xffffffff)
        u.emu_start(0x90295,0x902AB,count=100)
        assert u.reg_read(r.UC_X86_REG_EIP)==0x902AB
        assert u.reg_read(r.UC_X86_REG_EAX)==max(1,min(6,count))
    stack=0x1000000
    for count in range(15):
        u.reg_write(r.UC_X86_REG_ESP,stack);u.reg_write(r.UC_X86_REG_EDI,0x20000)
        for offset,value in [(0x18,count),(0x24,0x30000),(0x28,1)]:u.mem_write(stack+offset,struct.pack('<I',value))
        u.emu_start(0x699F9,0x69A23,count=100)
        assert u.reg_read(r.UC_X86_REG_EIP)==0x69A23
        read=lambda off:struct.unpack('<I',u.mem_read(stack+off,4))[0]
        assert read(0x18)==count+1
        assert read(0x14)==0x20000+0x88 and read(0x24)==0x30000+0x38
        assert read(0x28)==int(count+1<15)
    print('PASS: retail camera clamps 104 signed counts to 1..6; ray append disables further allocation at 15')
    for address,length,digest in records:print(address,length,digest)
if __name__=='__main__':main()

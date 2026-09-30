"""Compare the independently implemented ASCII asset hash with retail x86."""
from pathlib import Path
import hashlib
import random
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.diagnostics.inspect_retail_templates import hash_string
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
from unicorn import x86_const as r


def main():
    path=ROOT/'game_files/mercenaries-retail/default.xbe';raw=path.read_bytes()
    assert hashlib.sha256(raw).hexdigest()=='aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7'
    config.configure_from_xbe(str(path));offset=config.va_to_file_offset(0x1F29F0)
    code=raw[offset:offset+45]
    assert hashlib.sha256(code).hexdigest()=='fec3b74e7c9c8d12200b51edd4ea0fa07680b015e5c3e5c478b5d72ff05accbb'
    u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0x1F2000,0x1000);u.mem_map(0x10000000,0x20000)
    u.mem_write(0x1F29F0,code)
    def retail(value):
        if value is not None:u.mem_write(0x10000000,value+b'\0')
        u.mem_write(0x10010000,struct.pack('<II',0x1001F000,0 if value is None else 0x10000000))
        u.reg_write(r.UC_X86_REG_ESP,0x10010000)
        u.emu_start(0x1F29F0,0x1001F000,count=10000)
        assert u.reg_read(r.UC_X86_REG_EIP)==0x1001F000
        assert u.reg_read(r.UC_X86_REG_ESP)==0x10010004
        return u.reg_read(r.UC_X86_REG_EAX)
    assert retail(None)==0
    samples=['','alliesbouncer','ALLIESBOUNCER','GeometryFile','template_vehicles_ch4']
    samples.extend(chr(i) for i in range(1,128))
    randomizer=random.Random(20260929)
    samples.extend(''.join(chr(randomizer.randrange(1,128)) for _ in range(randomizer.randrange(0,200))) for _ in range(2000))
    for sample in samples:assert retail(sample.encode('ascii'))==hash_string(sample),repr(sample)
    assert retail(b'alliesbouncer')==0x9A21CF9F
    # Retail sign-extends bytes >=128, then ORs 0x20. These binary checks are
    # separate from the ASCII metadata helper's deliberately narrower contract.
    for value in range(128,256):
        expected=((2166136261^((value-256)|32))*16777619)&0xffffffff
        assert retail(bytes([value]))==expected
    assert retail(b'abc\0ignored')==retail(b'abc')
    print(f'PASS: {len(samples)} ASCII cases, 128 signed-byte cases, NULL and NUL termination match retail hash routine')

if __name__=='__main__':main()

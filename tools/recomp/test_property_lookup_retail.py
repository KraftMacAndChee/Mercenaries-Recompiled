"""Execute retail property lookup contracts used by host corruption guards."""
from pathlib import Path
import hashlib
import random
import struct
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
from unicorn import x86_const as r

class RetailPropertyLookup(unittest.TestCase):
    def test_absence_values_and_parent_chain(self):
        path=ROOT/'game_files/mercenaries-retail/default.xbe';raw=path.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),'aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7')
        config.configure_from_xbe(str(path));u=Uc(UC_ARCH_X86,UC_MODE_32)
        u.mem_map(0x1EB000,0x2000);u.mem_map(0x10000000,0x20000)
        for address,length,digest in [
            (0x1EB1B0,101,'887b3c05ba389b5afa28c7d4ffd03775fe0342484ef322f205e3379be4376a16'),
            (0x1EC900,22,'5a0fbc4cbb0ca29239b54c5fce665490b158fc9031cf3b2cf89e3401c1a5afe3'),
            (0x1ECCD0,86,'41781e79ab0108567c12b8ec05c1dcbff9274ae9972c100e8623117c51512225'),
        ]:
            offset=config.va_to_file_offset(address);code=raw[offset:offset+length]
            self.assertEqual(hashlib.sha256(code).hexdigest(),digest);u.mem_write(address,code)
        root,parent,spore,success,stack,stop=0x10000000,0x10001000,0x10002000,0x10003000,0x10010000,0x1001F000
        def put(address,value):u.mem_write(address,struct.pack('<I',value))
        def table(address,items,next_address=0):
            pairs=sorted(items.items());put(address,next_address)
            u.mem_write(address+4,struct.pack('<h',len(pairs)));put(address+8,address+32)
            for i,(key,value) in enumerate(pairs):u.mem_write(address+32+i*8,struct.pack('<II',key,value))
        def call(entry,this,key):
            args=[stop,key,success] if entry==0x1EC900 else [stop,key]
            u.mem_write(stack,struct.pack('<'+'I'*len(args),*args));u.mem_write(success,b'\xCC')
            u.reg_write(r.UC_X86_REG_ESP,stack);u.reg_write(r.UC_X86_REG_ECX,this)
            for reg in (r.UC_X86_REG_EBX,r.UC_X86_REG_EBP,r.UC_X86_REG_ESI,r.UC_X86_REG_EDI):u.reg_write(reg,0x12345678)
            u.emu_start(entry,stop,count=10000)
            self.assertEqual(u.reg_read(r.UC_X86_REG_EIP),stop)
            self.assertEqual(u.reg_read(r.UC_X86_REG_ESP),stack+4*len(args))
            for reg in (r.UC_X86_REG_EBX,r.UC_X86_REG_EBP,r.UC_X86_REG_ESI,r.UC_X86_REG_EDI):self.assertEqual(u.reg_read(reg),0x12345678)
            return u.reg_read(r.UC_X86_REG_EAX),bytes(u.mem_read(success,1))[0]
        self.assertEqual(call(0x1EC900,0,123),(0,0))
        table(root,{})
        self.assertEqual(call(0x1EC900,root,123),(0,0))
        randomizer=random.Random(20260929)
        cases=0
        for _ in range(64):
            ancestor={key:randomizer.getrandbits(32) for key in randomizer.sample(range(1,128),20)}
            child={key:randomizer.getrandbits(32) for key in randomizer.sample(range(1,128),20)}
            # A present value of zero must remain distinguishable from absence.
            child[0]=0;ancestor[0xFFFFFFFF]=0xFEDCBA98
            table(parent,ancestor);table(root,child,parent)
            expected=dict(ancestor);expected.update(child)
            for key in [0,1,32,64,127,128,0xFFFFFFFF]:
                self.assertEqual(call(0x1EC900,root,key),(expected.get(key,0),int(key in expected)))
                cases+=1
        # Optional permanent-property fallback with no dynamic property list.
        put(spore+4,0);u.mem_write(spore+0x30,b'\0\0')
        self.assertEqual(call(0x1ECCD0,spore,123)[0],0)
        put(spore+4,root)
        for key in [0,32,128,0xFFFFFFFF]:
            self.assertEqual(call(0x1ECCD0,spore,key)[0],expected.get(key,0))
        print(f'PASS: {cases} inherited/overridden/missing property lookups, null/empty roots and optional fallback')

if __name__=='__main__':unittest.main()

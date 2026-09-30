"""Establish audio observer handle locations from retail x86 and asset names."""
from pathlib import Path
import hashlib
import struct
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.diagnostics.inspect_retail_templates import hash_string
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn import x86_const as r
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

class VehicleAudioRetail(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=ROOT/'game_files/mercenaries-retail/default.xbe'
        cls.raw=path.read_bytes()
        assert hashlib.sha256(cls.raw).hexdigest()=='aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7'
        config.configure_from_xbe(str(path))
        cls.u=Uc(UC_ARCH_X86,UC_MODE_32)
        cls.u.mem_map(0x140000,0x10000);cls.u.mem_map(0x10000000,0x10000)
        for address,length in ((0x14EDB0,0x100),(0x14F720,0x7BE)):
            offset=config.va_to_file_offset(address)
            cls.u.mem_write(address,cls.raw[offset:offset+length])
        cls.effect=0x10000000;cls.stack=0x10008000

    def test_handle_assignment_instructions(self):
        # Each store immediately retains the sound API result in EAX.
        # This executes the real assignment, not a copied struct declaration.
        md=Cs(CS_ARCH_X86,CS_MODE_32)
        for site,offset in ((0x14EE56,0xF0),(0x14F950,0xE8),
                            (0x14FB1E,0xE0),(0x14FBAE,0xFC),
                            (0x14FBED,0x100),(0x14FCD1,0xF4),
                            (0x14FD9B,0xEC)):
            ins=next(md.disasm(bytes(self.u.mem_read(site,15)),site))
            self.assertEqual((ins.mnemonic,ins.op_str),('mov',f'dword ptr [esi + {offset:#x}], eax'))
            for value in (0,1,0x12345678,0xFFFFFFFF):
                self.u.mem_write(self.effect,bytes([0xCC])*0x200)
                self.u.reg_write(r.UC_X86_REG_ESI,self.effect)
                self.u.reg_write(r.UC_X86_REG_EAX,value)
                self.u.emu_start(site,site+ins.size,count=1)
                data=bytes(self.u.mem_read(self.effect,0x200))
                expected=bytearray([0xCC]*0x200)
                struct.pack_into('<I',expected,offset,value)
                self.assertEqual(data,bytes(expected))
        # The prior-throttle observer field is copied at the real epilogue.
        for bits in (0,0x3F800000,0xBF800000,0x7FC01234):
            self.u.mem_write(self.effect+0xBC,struct.pack('<I',bits))
            self.u.reg_write(r.UC_X86_REG_ESI,self.effect)
            self.u.emu_start(0x14FEC8,0x14FED4,count=2)
            self.assertEqual(struct.unpack('<I',self.u.mem_read(self.effect+0xC0,4))[0],bits)

    def test_retail_sound_names_and_push_sites(self):
        assets=(ROOT/'game_files/mercenaries-retail/DATAxbox/assets.dsk').read_bytes()
        self.assertEqual(hashlib.sha256(assets).hexdigest(),'8b8ee254a245490ea4cda54ccea05b7a7a51afc41125aaf3cf5fd8ec512d1610')
        cases=((0x14EE1F,'engine_acc'),(0x14FC29,'engine_loop'),
               (0x14FCA3,'reverse_loop'),(0x14FB92,'base.vehicle_skid'),
               (0x14FBD1,'base.veh_slide'),(0x14F974,'high_detail'),
               (0x14F9CA,'noise_dirt'),(0x14F9D4,'brake_dirt'),
               (0x14F9DE,'noise_road'),(0x14F9E8,'brake_road'),
               (0x14F9F2,'noise_snow'),(0x14F9FC,'brake_snow'),
               (0x14FA06,'noise_grass'),(0x14FA10,'brake_grass'),
               (0x14FA1A,'noise_rock'),(0x14FA24,'brake_rock'),
               (0x14FA2E,'noise_gravel'),(0x14FA38,'brake_gravel'))
        for site,name in cases:
            self.assertIn(name.encode()+b'\0',assets)
            code=bytes(self.u.mem_read(site,5))
            self.assertEqual(code,b'\x68'+struct.pack('<I',hash_string(name)))
        for address,name in ((0x2F21CC,'RPM'),(0x2F2198,'Throttle'),
                             (0x2F21B0,'control_amount'),(0x2F21A4,'skid_amount'),
                             (0x2E7864,'speed'),(0x2F2184,'gear')):
            offset=config.va_to_file_offset(address)
            self.assertEqual(self.raw[offset:offset+len(name)+1],name.encode()+b'\0')

if __name__=='__main__':unittest.main()

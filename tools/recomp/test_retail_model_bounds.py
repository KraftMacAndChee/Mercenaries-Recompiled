"""Compare independently read model bounds with the retail INFO loader."""
from pathlib import Path
import hashlib
import math
import struct
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.diagnostics.inspect_retail_templates import vehicle_metadata
from tools.diagnostics.inspect_retail_model_bounds import model_records,decode_info,conservative_footprint
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn import x86_const as r

class RetailModelBounds(unittest.TestCase):
    def test_every_vehicle_model_against_retail_loader(self):
        path=ROOT/'game_files/mercenaries-retail/default.xbe';raw=path.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),'aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7')
        config.configure_from_xbe(str(path))
        offset=config.va_to_file_offset(0x1F5DD5)
        self.assertEqual(hashlib.sha256(raw[offset:offset+1127]).hexdigest(),'3f92d9bdf5469a07e27a25b6c72ac01d9727a1680f05fc6336cef885c538dc67')
        data=path.parent/'DATAxbox';rows=vehicle_metadata(data)
        models=model_records(data,{row['model_hash'] for row in rows})
        self.assertEqual(len(models),65)
        u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(0,0x4000000)
        for section in config._SECTIONS:
            if section.raw_size:
                u.mem_write(section.va,raw[section.raw_addr:section.raw_addr+section.raw_size])
        model,stream,vtable,reader,stack=0x3000000,0x3001000,0x3001100,0x3001200,0x3100000
        u.mem_write(reader,b'\xc2\x08\x00')
        # Stream reads are host-supplied input plumbing. The actual retail INFO
        # parser, field assignments and sphere math execute without replacement.
        u.mem_write(0x209070,b'\xc2\x08\x00')
        current=b'';cursor=0
        def read_callback(emu,address,size,user):
            nonlocal cursor
            if address not in (reader,0x209070):return
            esp=emu.reg_read(r.UC_X86_REG_ESP)
            dest,count=struct.unpack('<II',emu.mem_read(esp+4,8))
            if address==0x209070:
                end=current.index(0,cursor)+1
                self.assertLessEqual(end-cursor,count)
            else:end=cursor+count
            self.assertLessEqual(end,len(current))
            emu.mem_write(dest,current[cursor:end]);cursor=end
        u.hook_add(UC_HOOK_CODE,read_callback)
        for key,record in models.items():
            with self.subTest(model=record['name']):
                current=record['info'];cursor=0
                u.mem_write(model,bytes(0x100));u.mem_write(stack-0x100,bytes(0x1000))
                u.mem_write(stream,struct.pack('<I',vtable));u.mem_write(vtable+0x10,struct.pack('<I',reader))
                u.mem_write(stack+0x3C,struct.pack('<I',stream))
                u.mem_write(stack+0x208,struct.pack('<I',stream))
                for reg,value in [(r.UC_X86_REG_ESP,stack),(r.UC_X86_REG_EBP,stack+0x200),
                                  (r.UC_X86_REG_ESI,model),(r.UC_X86_REG_EDI,0),
                                  (r.UC_X86_REG_FPCW,0x37F),(r.UC_X86_REG_FPTAG,0xFFFF),
                                  (r.UC_X86_REG_FPSW,0),(r.UC_X86_REG_MXCSR,0x1F80)]:u.reg_write(reg,value)
                u.emu_start(0x1F5DD5,0x1F62C3,count=5000)
                self.assertEqual(u.reg_read(r.UC_X86_REG_EIP),0x1F62C3)
                self.assertEqual(cursor,len(current))
                actual=[struct.unpack('<6f',u.mem_read(model+offset,24)) for offset in (0x38,0x60)]
                self.assertEqual(actual,record['boxes'])
                sphere=struct.unpack('<4f',u.mem_read(model+0x50,16))
                box=record['boxes'][0]
                expected=[(box[i]+box[i+3])*0.5 for i in range(3)]
                expected.append(math.sqrt(sum((box[i+3]-box[i])**2 for i in range(3)))*0.5)
                for a,b in zip(sphere,expected):self.assertAlmostEqual(a,b,delta=max(0.00001,abs(b)*1e-6))
        print('PASS: 65 vehicle model bounds and spheres match direct retail INFO loading')

    def test_invalid_info_and_conservative_enclosure(self):
        valid=bytearray(77);struct.pack_into('<12f',valid,8,-1,-2,-3,4,5,6,-2,-1,-4,3,6,5)
        boxes=decode_info(valid);footprint=conservative_footprint(boxes)
        self.assertAlmostEqual(footprint['bottom'],-2.1)
        self.assertAlmostEqual(footprint['height'],8.4)
        self.assertAlmostEqual(footprint['half_width'],4.2)
        self.assertAlmostEqual(footprint['half_length'],6.3)
        for invalid in (valid[:76],):
            with self.assertRaises(ValueError):decode_info(invalid)
        for value in (float('nan'),float('inf'),99):
            bad=bytearray(valid);struct.pack_into('<f',bad,8,value)
            with self.assertRaises(ValueError):decode_info(bad)
        for scale in (0,-1,float('nan'),float('inf')):
            with self.assertRaises(ValueError):conservative_footprint(boxes,scale)

if __name__=='__main__':unittest.main()

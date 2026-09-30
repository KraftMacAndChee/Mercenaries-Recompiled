"""Execute retail human fields used by the read-only snapshot inspector.

Only address/hash metadata ships here. Instructions come from the user's XBE.
This establishes current field access independently; historical attribution stays.
"""
from pathlib import Path
import hashlib
import json
import random
import struct
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.diagnostics import compare_human_snapshots as inspector
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
from unicorn import x86_const as r

class HumanSnapshotRetail(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=ROOT/'game_files/mercenaries-retail/default.xbe'
        raw=path.read_bytes()
        manifest=json.loads((ROOT/'tools/recomp/fixtures/human-snapshot-retail.json').read_text())
        if hashlib.sha256(raw).hexdigest()!=manifest['retail_xbe_sha256']:
            raise ValueError('Unsupported retail XBE')
        config.configure_from_xbe(str(path))
        cls.u=Uc(UC_ARCH_X86,UC_MODE_32)
        cls.u.mem_map(0,0x900000)
        cls.u.mem_map(0x10000000,0x20000)
        for section in config._SECTIONS:
            cls.u.mem_write(section.va,raw[section.raw_addr:section.raw_addr+section.raw_size])
        for entry in manifest['regions']:
            a,e=int(entry['start'],16),int(entry['end'],16)
            assert hashlib.sha256(cls.u.mem_read(a,e-a)).hexdigest()==entry['sha256']
        cls.actor=0x10000000;cls.stack=0x1001E000;cls.stop=0x1001F000
        cls.raw=raw

    def put(self,address,value):self.u.mem_write(address,struct.pack('<I',value&0xffffffff))
    def get(self,address):return struct.unpack('<I',self.u.mem_read(address,4))[0]
    def byte(self,address):return bytes(self.u.mem_read(address,1))[0]
    def execute(self,start,end):
        self.u.emu_start(start,end,count=1000)
        self.assertEqual(self.u.reg_read(r.UC_X86_REG_EIP),end)
    def call(self,start,*args):
        self.u.reg_write(r.UC_X86_REG_ECX,self.actor)
        self.u.reg_write(r.UC_X86_REG_ESP,self.stack)
        self.u.mem_write(self.stack,struct.pack('<'+'I'*(len(args)+1),self.stop,*args))
        self.execute(start,self.stop)
        self.assertEqual(self.u.reg_read(r.UC_X86_REG_ESP),self.stack+4*(len(args)+1))
        return self.u.reg_read(r.UC_X86_REG_EAX)
    def reset_actor(self,vtable=0x2E2CE0):
        self.u.mem_write(self.actor,bytes(0x1000));self.put(self.actor,vtable)
        self.u.reg_write(r.UC_X86_REG_ESI,self.actor)

    def test_constructor_tables_and_position(self):
        rng=random.Random(20260929)
        for start,end,table in [(0x56E73,0x56E79,0x2E2CE0),(0x5BB85,0x5BB8B,0x2E32B8)]:
            self.reset_actor();self.execute(start,end)
            self.assertEqual(self.get(self.actor),table)
            self.assertIn(table,inspector.HUMAN_VTABLES)
            self.assertEqual(self.get(table+0x34),0x1EA400)
            for _ in range(32):
                position=struct.pack('<3f',*(rng.uniform(-4000,4000) for _ in range(3)))
                self.u.mem_write(self.actor+0xE0,position)
                output=self.actor+0x1000
                self.assertEqual(self.call(self.get(table+0x34),output),output)
                self.assertEqual(bytes(self.u.mem_read(output,12)),position)

    def test_nested_freeze_and_release(self):
        count=inspector.FROZEN_COUNT_OFFSET;state=inspector.FROZEN_STATE_OFFSET
        for table in inspector.HUMAN_VTABLES:
            self.reset_actor(table)
            for level in range(1,33):
                self.call(0x4CE70,0)
                self.assertEqual(self.byte(self.actor+count),level)
                self.assertEqual(self.get(self.actor+state),1)
                self.assertEqual(self.call(0x547F0),1)
            for level in reversed(range(32)):
                self.call(0x4CEC0)
                self.assertEqual(self.byte(self.actor+count),level)
                self.assertEqual(self.get(self.actor+state),int(level>0))
                self.assertEqual(self.call(0x547F0),int(level>0))
            # A pending state with no animation object does not dereference it.
            self.put(self.actor+state,2)
            self.call(0x4CE70,1)
            self.assertEqual(self.get(self.actor+state),2)
            self.assertEqual(self.byte(self.actor+count),1)

    def test_update_phase_and_both_counters(self):
        self.reset_actor()
        for phase in (0,1,123,0xffffffff):
            self.put(0x320794,phase)
            self.execute(0x50E08,0x50E1A)
            self.assertEqual(self.get(self.actor+inspector.UPDATE_OFFSET_OFFSET),phase)
            self.assertEqual(self.get(0x320794),(phase+1)&0xffffffff)
        self.u.reg_write(r.UC_X86_REG_EBX,0)
        self.execute(0x50E1E,0x50E2A)
        for offset in (inspector.UPPER_COUNTER_OFFSET,inspector.LOWER_COUNTER_OFFSET):
            self.assertEqual(self.get(self.actor+offset),0)
        cases=0
        for offset in (inspector.UPPER_COUNTER_OFFSET,inspector.LOWER_COUNTER_OFFSET):
            for divisor in (1,2,4,8,12):
                for phase in range(4):
                    for tick in range(64):
                        for previous in (0,(tick+phase)//divisor):
                            self.put(self.actor+inspector.UPDATE_OFFSET_OFFSET,phase)
                            self.put(self.actor+offset,previous)
                            self.put(0x793564,tick)
                            table=self.actor+0x1000
                            self.put(table+4,divisor)
                            self.put(self.stack+0x18,self.actor+offset)
                            self.u.reg_write(r.UC_X86_REG_ESI,self.actor)
                            self.u.reg_write(r.UC_X86_REG_ECX,table)
                            self.u.reg_write(r.UC_X86_REG_ESP,self.stack)
                            self.execute(0x4E822,0x4E84A)
                            expected=(tick+phase)//divisor
                            self.assertEqual(self.get(self.actor+offset),expected)
                            self.assertEqual(self.u.reg_read(r.UC_X86_REG_EBX),1 if previous==expected else 3)
                            cases+=1
        print(f'PASS: {cases} retail animation update counter cases')

    def test_animation_pointer_and_physics_flag_access(self):
        self.reset_actor()
        animation=self.actor+0x4000
        self.put(self.actor+inspector.ANIM_POINTER_OFFSET,animation)
        self.put(animation+0x28E8,0x12345678)
        self.execute(0x51F38,0x51F44)
        self.assertEqual(self.u.reg_read(r.UC_X86_REG_EDX),animation)
        self.assertEqual(self.u.reg_read(r.UC_X86_REG_EAX),0x12345678)
        offset=inspector.AI_PHYSICS_DISABLED_OFFSET
        for value in (0,1,0x7f,0xff):
            self.u.mem_write(self.actor+offset-1,b'\xAA\x00\xBB')
            self.u.reg_write(r.UC_X86_REG_EBX,value)
            self.execute(0x5073E,0x50744)
            self.assertEqual(bytes(self.u.mem_read(self.actor+offset-1,3)),bytes([0xAA,value,0xBB]))
            self.execute(0x53C22,0x53C29)
            self.assertEqual(bytes(self.u.mem_read(self.actor+offset-1,3)),b'\xAA\x00\xBB')

if __name__=='__main__':unittest.main()

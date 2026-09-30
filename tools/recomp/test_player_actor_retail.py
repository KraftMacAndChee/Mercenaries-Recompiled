"""Verify controller/actor accessors by executing supported retail instructions."""
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

class PlayerActorRetail(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=ROOT/'game_files/mercenaries-retail/default.xbe';raw=path.read_bytes()
        if hashlib.sha256(raw).hexdigest()!='aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7':
            raise ValueError('Unsupported retail executable')
        config.configure_from_xbe(str(path));cls.u=Uc(UC_ARCH_X86,UC_MODE_32)
        cls.u.mem_map(0,0x900000);cls.u.mem_map(0x10000000,0x10000)
        for section in config._SECTIONS:
            if section.raw_size:cls.u.mem_write(section.va,raw[section.raw_addr:section.raw_addr+section.raw_size])
        cls.controller=0x10000000;cls.human=0x10002000;cls.other=0x10004000
        cls.out=0x10006000;cls.stack=0x1000E000;cls.stop=0x1000F000
    def put(self,p,v):self.u.mem_write(p,struct.pack('<I',v))
    def get(self,p):return struct.unpack('<I',self.u.mem_read(p,4))[0]
    def call(self,va,this,*args,callee_pop=True):
        self.u.reg_write(r.UC_X86_REG_ECX,this);self.u.reg_write(r.UC_X86_REG_ESP,self.stack)
        self.u.mem_write(self.stack,struct.pack('<'+'I'*(len(args)+1),self.stop,*args))
        self.u.emu_start(va,self.stop,count=2000)
        self.assertEqual(self.u.reg_read(r.UC_X86_REG_EIP),self.stop)
        self.assertEqual(self.u.reg_read(r.UC_X86_REG_ESP),self.stack+4+(4*len(args) if callee_pop else 0))
        return self.u.reg_read(r.UC_X86_REG_EAX)
    def test_lookup_returns_controller_and_separate_human(self):
        self.u.mem_write(self.controller,bytes(0x1000));self.put(self.controller,0x2E65E0)
        self.put(0x322DC0,0x660E4490);self.put(0x322DB8,self.controller);self.put(0x322DB4,0)
        self.assertEqual(self.call(0x8C7E0,0,0x660E4490,callee_pop=False),self.controller)
        self.assertEqual(self.call(0x8C7E0,0,123,callee_pop=False),0)
        self.assertEqual(self.get(0x2E65E0+0x34),0x8C050)
        for human in (0,self.human,self.other):
            self.put(self.controller+0x998,human)
            self.assertEqual(self.call(0x8C050,self.controller),human)
        # Execute the real constructor's vtable assignment, without constructing
        # unrelated subsystems. This is not a stub for the accessor under test.
        self.u.reg_write(r.UC_X86_REG_ESI,self.controller)
        self.u.emu_start(0x8E955,0x8E95B,count=1)
        self.assertEqual(self.get(self.controller),0x2E65E0)
    def test_controller_and_human_positions_are_distinct_contracts(self):
        self.put(self.controller,0x2E65E0);self.put(self.controller+0x10,self.other)
        self.put(self.controller+0x998,self.human)
        self.put(self.human,0x2E2CE0);self.put(self.other,0x2E0468)
        self.u.mem_write(self.human+0xE0,struct.pack('<3f',10,20,30))
        self.u.mem_write(self.other+0xE0,struct.pack('<3f',100,200,300))
        self.assertEqual(self.get(0x2E65E0+0x3C),0x673F0)
        self.call(0x673F0,self.controller,self.out)
        self.assertEqual(struct.unpack('<3f',self.u.mem_read(self.out,12)),(100,200,300))
        self.call(self.get(0x2E2CE0+0x34),self.human,self.out)
        self.assertEqual(struct.unpack('<3f',self.u.mem_read(self.out,12)),(10,20,30))
    def test_forward_axis_and_static_position_setter(self):
        rng=random.Random(20260929)
        for table in (0x2E2CE0,0x2E32B8,0x2E0468):
            self.put(self.human,table)
            self.assertEqual(self.get(table+0x58),0x1EA470)
            for _ in range(64):
                xyz=struct.pack('<3f',*(rng.uniform(-1,1) for _ in range(3)))
                self.u.mem_write(self.human+0xD0,xyz)
                self.call(0x1EA470,self.human,self.out)
                actual=struct.unpack('<3f',self.u.mem_read(self.out,12))
                self.assertEqual(actual,tuple(-x for x in struct.unpack('<3f',xyz)))
        self.put(self.human,0x2E0468)
        self.assertEqual(self.get(0x2E0468+0x64),0x1EA510)
        for _ in range(64):
            xyz=struct.pack('<3f',*(rng.uniform(-4000,4000) for _ in range(3)))
            self.u.mem_write(self.out,xyz)
            self.call(0x1EA510,self.human,self.out)
            self.assertEqual(bytes(self.u.mem_read(self.human+0xE0,12)),xyz)
            self.assertEqual(self.get(self.human+0xEC),0x3F800000)

    def test_static_animation_dispatch_and_allocation(self):
        from unicorn import UC_HOOK_CODE
        self.assertEqual(self.get(0x2E0468+0x12C),0x15340)
        actor=self.human;animation=self.other
        for initially_present in (False,True):
            self.put(actor+0x11C,animation if initially_present else 0)
            events=[]
            def external(u,address,size,user):
                pops={0x1F71E0:0,0x15320:0,0x60550:4,0x609C0:12}
                if address not in pops:return
                sp=u.reg_read(r.UC_X86_REG_ESP);receiver=u.reg_read(r.UC_X86_REG_ECX)
                if address==0x1F71E0:
                    self.assertEqual(self.get(sp+4),0x50);result=animation
                elif address==0x15320:
                    self.assertEqual(receiver,animation);result=animation
                elif address==0x60550:
                    self.assertEqual(receiver,animation);self.assertEqual(self.get(sp+4),actor);result=1
                else:
                    self.assertEqual(receiver,animation)
                    self.assertEqual([self.get(sp+i) for i in (4,8,12)],[0x12345678,10,1]);result=1
                events.append(address);u.reg_write(r.UC_X86_REG_EAX,result)
                u.reg_write(r.UC_X86_REG_EIP,self.get(sp));u.reg_write(r.UC_X86_REG_ESP,sp+4+pops[address])
            hook=self.u.hook_add(UC_HOOK_CODE,external)
            try:self.assertEqual(self.call(0x15340,actor,0x12345678,10,1),1)
            finally:self.u.hook_del(hook)
            self.assertEqual(self.get(actor+0x11C),animation)
            self.assertEqual(events,[0x609C0] if initially_present else [0x1F71E0,0x15320,0x60550,0x609C0])

    def test_static_animation_distance_throttle(self):
        from unicorn import UC_HOOK_CODE
        animation=self.out;actor=self.human
        self.put(actor,0x2E0468);self.put(0x41410C,0)
        self.u.mem_write(0x643910,struct.pack('<3f',0,0,0))
        def visible(u,address,size,user):
            if address!=0x605F0:return
            stack=u.reg_read(r.UC_X86_REG_ESP)
            u.reg_write(r.UC_X86_REG_EAX,query_result)
            u.reg_write(r.UC_X86_REG_EIP,self.get(stack));u.reg_write(r.UC_X86_REG_ESP,stack+4)
        hook=self.u.hook_add(UC_HOOK_CODE,visible)
        try:
            for query_result in (0,1):
                for throttle in (0,1):
                    for distance,expected in ((0,10 if query_result==0 else 0),(20,.2 if query_result==0 else 10),(30,1.2 if query_result==0 else 10),(40,10),(100,10)):
                        self.u.mem_write(animation,bytes(0x50));self.put(animation+0x28,actor)
                        self.u.mem_write(animation+0x30,bytes([throttle]))
                        self.u.mem_write(actor+0xE0,struct.pack('<3f',distance,0,0))
                        self.put(0,0);self.u.mem_write(self.stack,struct.pack('<If',self.stop,1/60))
                        self.u.reg_write(r.UC_X86_REG_ECX,animation);self.u.reg_write(r.UC_X86_REG_ESP,self.stack)
                        self.u.emu_start(0x606F0,0x607DC,count=200)
                        self.assertEqual(self.u.reg_read(r.UC_X86_REG_EIP),0x607DC)
                        stack=self.u.reg_read(r.UC_X86_REG_ESP)
                        delay=struct.unpack('<f',self.u.mem_read(stack+8,4))[0]
                        self.assertAlmostEqual(delay,expected if throttle else 0,places=5)
                        self.assertAlmostEqual(struct.unpack('<f',self.u.mem_read(animation,4))[0],1/60,places=6)
        finally:self.u.hook_del(hook)

    def test_bird_track_rate_holds_phase_and_resumes(self):
        from unicorn import UC_HOOK_CODE
        track=self.human;clip=self.other
        def events(u,address,size,user):
            if address!=0x206CD0:return
            stack=u.reg_read(r.UC_X86_REG_ESP)
            u.reg_write(r.UC_X86_REG_EAX,0)
            u.reg_write(r.UC_X86_REG_EIP,self.get(stack));u.reg_write(r.UC_X86_REG_ESP,stack+12)
        hook=self.u.hook_add(UC_HOOK_CODE,events)
        try:
            for rate in (0.,1.,.75):
                self.u.mem_write(track,bytes(0xB50));self.put(track+0xB00,clip);self.put(track+0xB48,1)
                self.u.mem_write(clip+0x20,struct.pack('<H',30))
                self.u.mem_write(track+0xB1C,struct.pack('<ff',30.,.25))
                self.u.mem_write(track+0xB3C,struct.pack('<f',rate));self.u.mem_write(track+0xB2A,b'\x01')
                delta=struct.unpack('<I',struct.pack('<f',1/60))[0]
                self.call(0x62F70,track,delta)
                phase=struct.unpack('<f',self.u.mem_read(track+0xB20,4))[0]
                self.assertAlmostEqual(phase,.25+rate/60,places=6)
        finally:self.u.hook_del(hook)

    def test_static_teardown_dispatch(self):
        from unicorn import UC_HOOK_CODE
        actor=self.human;spore=self.other;table=self.out;tail=self.stop-32
        self.assertEqual(self.get(0x2E0468+0x10),0x30E40)
        self.put(actor,table);self.put(table+0x20,tail);events=[]
        def external(u,address,size,user):
            if address not in (0x113700,0x1EE0E0,tail):return
            sp=u.reg_read(r.UC_X86_REG_ESP)
            self.assertEqual(u.reg_read(r.UC_X86_REG_ECX),spore if address==0x1EE0E0 else actor)
            events.append(address);u.reg_write(r.UC_X86_REG_EAX,spore if address==0x113700 else 0)
            u.reg_write(r.UC_X86_REG_EIP,self.get(sp));u.reg_write(r.UC_X86_REG_ESP,sp+4)
        hook=self.u.hook_add(UC_HOOK_CODE,external)
        try:self.call(0x30E40,actor)
        finally:self.u.hook_del(hook)
        self.assertEqual(events,[0x113700,0x1EE0E0,tail])

    def test_transient_flag_clear_matches_retail_operation(self):
        # 0x1EAA90 clears exactly bits 9..11; low lifetime flags survive.
        for flags in range(0,65536,17):
            self.u.mem_write(self.other+0x30,struct.pack('<HH',flags,0xA55A))
            self.call(0x1EAA90,self.other)
            self.assertEqual(bytes(self.u.mem_read(self.other+0x30,4)),struct.pack('<HH',flags&~0xE00,0xA55A))

    def test_property_list_constructor_and_insertion(self):
        buffer=self.out;prop=self.other
        self.u.mem_write(buffer,bytes([0xCC])*1024)
        self.assertEqual(self.call(0x1EB010,buffer),buffer)
        self.assertEqual(bytes(self.u.mem_read(buffer,4)),b'\x00\x00\xcc\xcc')
        for i in range(16):
            self.put(prop,0x100+i);self.put(prop+4,0xA000+i)
            self.call(0x1EB080,buffer,prop)
        self.assertEqual(struct.unpack('<H',self.u.mem_read(buffer,2))[0],16)
        for i in range(16):
            self.assertEqual((self.get(buffer+4+i*8),self.get(buffer+8+i*8)),(0x100+i,0xA000+i))

    def test_static_birds_are_excluded_by_retail_save_classification(self):
        from unicorn import UC_HOOK_CODE
        spore=self.other;table=self.out;lookup=self.stop-48
        self.assertEqual(self.get(0x2F3190+0x30),0x1702B0)
        self.assertEqual(self.get(0x2F3190+0x34),0x173400)
        self.put(spore,table);self.put(table+0x10,lookup)
        # D290C23B is the retail hash of "static"; accepted types are positive
        # controls, not copied expected classifications for the replacement.
        for object_type in (0xD290C23B,0x38DF0375,0x1660EB12,0x19EBECDA,
                            0x1B94BA86,0x3C494860,0x4936726F,0x586C3B45):
            for flags in (0,1,4,8,12,0xE01):
                self.u.mem_write(spore+0x30,struct.pack('<H',flags));calls=[]
                def external(u,address,size,user):
                    if address!=lookup:return
                    stack=u.reg_read(r.UC_X86_REG_ESP)
                    self.assertEqual(u.reg_read(r.UC_X86_REG_ECX),spore)
                    self.assertEqual(self.get(stack+4),0xB1D4007A)
                    calls.append(address);u.reg_write(r.UC_X86_REG_EAX,object_type)
                    u.reg_write(r.UC_X86_REG_EIP,self.get(stack))
                    u.reg_write(r.UC_X86_REG_ESP,stack+8)
                hook=self.u.hook_add(UC_HOOK_CODE,external)
                try:actual=self.call(0x173400,0,spore)&0xFF
                finally:self.u.hook_del(hook)
                self.assertEqual(actual,int(object_type!=0xD290C23B and not flags&12))
                self.assertEqual(len(calls),int(not flags&12))
                self.assertEqual(self.call(0x1702B0,0,spore)&0xFF,0)

    def test_seat_manager_vehicle_chain(self):
        # Retail 0x590AA..0x590BC follows all three links; stop before it calls
        # vehicle behavior. Test payloads occupy different guest allocations.
        for i in range(64):
            seat=self.other+i*8;manager=self.out+i*8;vehicle=self.human+0x1000+i*8
            self.put(self.human+0x768,seat);self.put(seat,manager);self.put(manager,vehicle)
            self.u.reg_write(r.UC_X86_REG_EDI,self.human)
            self.u.emu_start(0x590AA,0x590BC,count=20)
            self.assertEqual(self.u.reg_read(r.UC_X86_REG_EIP),0x590BC)
            self.assertEqual(self.u.reg_read(r.UC_X86_REG_ESI),vehicle)

    def test_guid_lookup_lifetime_flags(self):
        self.u.mem_write(0x624070,bytes(0x10000));self.put(0x61406C,0)
        spore=self.other;guid=0xB01D1234;slot=guid&0x3FFF
        self.put(0x624070+slot*4,guid);self.put(0x614070+slot*4,spore)
        self.put(spore+0x24,self.human)
        for flags in range(256):
            self.u.mem_write(spore+0x30,bytes([flags]))
            result=self.call(0x1EC220,0,guid,callee_pop=False)
            self.assertEqual(result,self.human if flags&1 and not flags&4 else 0)
        self.assertEqual(self.call(0x1EC220,0,0,callee_pop=False),0)
        self.assertEqual(self.call(0x1EC220,0,guid+1,callee_pop=False),0)
        self.put(0x614070+slot*4,0)
        self.assertEqual(self.call(0x1EC220,0,guid,callee_pop=False),0)

    def test_effect_registry_links_and_payload_sentinel(self):
        root=0x30DC94;self.put(root,root);self.put(root+4,root)
        self.put(root+8,0);self.put(root+12,0)
        nodes=[self.out+i*32 for i in range(32)]
        def walk():
            result=[];node=self.get(root)
            while self.get(node+8):
                self.assertLess(len(result),33)
                self.assertEqual(self.get(node+8),node)
                self.assertEqual(self.get(self.get(node)+4),node)
                result.append(node);node=self.get(node)
            self.assertEqual(node,root)
            self.assertEqual(self.get(root+12),len(result))
            return result
        for node in nodes:
            self.call(0xB7650,root,node)
        self.assertEqual(walk(),nodes)
        remaining=list(nodes);order=list(nodes);random.Random(117).shuffle(order)
        for node in order:
            self.call(0xB7370,root,node)
            remaining.remove(node)
            self.assertEqual(self.get(node+8),0)
            self.assertEqual(walk(),remaining)
        # Verify the observer's root/payload reads from the actual traversal.
        self.u.reg_write(r.UC_X86_REG_ESP,self.stack)
        self.u.emu_start(0xB7877,0xB787F,count=2)
        self.assertEqual(self.u.reg_read(r.UC_X86_REG_EAX),root)
        self.assertEqual(self.u.reg_read(r.UC_X86_REG_EBX),0)

if __name__=='__main__':unittest.main()

"""Compare bounded script decoding with direct execution of retail x86."""
from pathlib import Path
import hashlib,struct,sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.diagnostics.inspect_retail_script import chunks,decompress
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
from unicorn import x86_const as r

class RetailDecoder:
    def __init__(self):
        path=ROOT/'game_files/mercenaries-retail/default.xbe';raw=path.read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7'
        config.configure_from_xbe(str(path));offset=config.va_to_file_offset(0x21A750)
        code=raw[offset:offset+264]
        assert hashlib.sha256(code).hexdigest()=='e7bfa932cb2d940dd3d4f4582cc0139da2daede3d36598704b732a14f39e61e7'
        self.uc=Uc(UC_ARCH_X86,UC_MODE_32)
        for address,size in [(0x21A000,0x1000),(0x85B000,0x1000),(0x10000000,0x1000000),(0x20000000,0x1001000),(0x30000000,0x10000)]:self.uc.mem_map(address,size)
        self.uc.mem_write(0x21A750,code)
    def decode(self,payload,expected):
        assert len(payload)<0x1000000 and 0<=expected<=0x1000000
        u=self.uc
        u.mem_write(0x10000000,payload)
        u.mem_write(0x20000000+expected,b'GUARD')
        u.mem_write(0x85BB1C,struct.pack('<III',0x20000000,0x10000000,0))
        u.mem_write(0x85BB28,struct.pack('<I',0xffffffff))
        u.mem_write(0x30008000,struct.pack('<I',0x30000000))
        u.reg_write(r.UC_X86_REG_ESP,0x30008000)
        u.emu_start(0x21A750,0x30000000,count=max(10000,expected*50))
        assert u.reg_read(r.UC_X86_REG_EIP)==0x30000000
        assert u.reg_read(r.UC_X86_REG_ESP)==0x30008004
        actual=struct.unpack('<I',u.mem_read(0x85BB28,4))[0]
        assert actual==expected and u.mem_read(0x20000000+expected,5)==b'GUARD'
        return bytes(u.mem_read(0x20000000,expected))

class Encoder:
    # Test-only token builder, with no reference to game source text.
    def __init__(self):self.data=bytearray(2);self.slot=0;self.bit_index=0
    def bit(self,value):
        self.data[self.slot+self.bit_index//8] |= value << (self.bit_index%8)
        self.bit_index+=1
        if self.bit_index==16:
            self.slot=len(self.data);self.data.extend(b'\0\0');self.bit_index=0
    def literal(self,value):self.bit(1);self.data.append(value)
    def short(self,distance,count):
        self.bit(0);self.bit(0);self.bit((count-3)>>1);self.bit((count-3)&1);self.data.append(256-distance)
    def long(self,distance,count):
        self.bit(0);self.bit(1);offset=4096-distance
        self.data.extend((offset&255,((offset>>4)&0xF0)|(count-3 if 4<=count<=18 else 0)))
        if not 4<=count<=18:self.data.append(count-1)
    def end(self):self.bit(0);self.bit(1);self.data.extend(b'\0\0\0');return bytes(self.data)

def test_decoder():
    oracle=RetailDecoder();cases=[]
    for length in (1,14,15,16,17,31,32,33,128):
        enc=Encoder();expected=bytes((i*17)&255 for i in range(length))
        for value in expected:enc.literal(value)
        cases.append((enc.end(),expected))
    enc=Encoder();enc.literal(65);enc.short(1,6);enc.long(1,18);enc.long(1,256);cases.append((enc.end(),b'A'*281))
    enc=Encoder();expected=bytes(i&255 for i in range(4096))
    for value in expected:enc.literal(value)
    enc.long(4096,4);cases.append((enc.end(),expected+expected[:4]))
    for payload,expected in cases:
        assert oracle.decode(payload,len(expected))==expected
        assert decompress(payload,len(expected))==expected
    failures=0
    for payload,expected in cases:
        for shortened in (payload[:1],payload[:-1]):
            try:decompress(shortened,len(expected))
            except ValueError:failures+=1
            else:raise AssertionError('Truncation accepted')
        try:decompress(payload,len(expected)-1)
        except ValueError:failures+=1
        else:raise AssertionError('Wrong length accepted')
    path=ROOT/'game_files/mercenaries-retail/DATAxbox/assets.dsk'
    count=0;raw_count=0;decoded_bytes=0;result_hash=hashlib.sha256()
    with path.open('rb') as stream:
        entries,_=struct.unpack('<II',stream.read(8));directory=stream.read(entries*12)
        offset=8+entries*12
        for i in range(entries):
            size,_,kind=struct.unpack_from('<III',directory,i*12)
            if kind==0x203E6FAA:
                stream.seek(offset);root=dict(chunks(stream.read(size)))
                fields=dict(chunks(dict(chunks(root[b'ucfb']))[b'scr_']))
                expected=struct.unpack_from('<I',fields[b'INFO'],1)[0]
                if expected==0:
                    assert fields[b'BODY'].rstrip(b'\0').decode('ascii')
                    raw_count+=1;offset+=size;continue
                try:native=oracle.decode(fields[b'BODY'],expected)
                except Exception as error:raise RuntimeError((i,fields[b'NAME'],fields[b'INFO'].hex(),expected)) from error
                assert decompress(fields[b'BODY'],expected)==native,fields[b'NAME']
                result_hash.update(fields[b'NAME']);result_hash.update(native)
                count+=1;decoded_bytes+=expected
            offset+=size
    assert count>0
    print(f'PASS: {len(cases)} constructed token cases, {failures} malformed cases; {count} compressed scripts and {raw_count} uncompressed scripts / {decoded_bytes} decoded bytes match retail x86; aggregate {result_hash.hexdigest()}')
if __name__=='__main__':test_decoder()

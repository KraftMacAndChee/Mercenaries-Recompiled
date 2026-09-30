"""Capture enemy-memory expectations by executing the supported retail x86 in Unicorn.

Only the user-supplied retail XBE and public emulator are reference inputs.
The deterministic scenario schedule is shared with the existing regression test.
The emitted trace contains inputs/results, not game code or source text.
"""
from pathlib import Path
import argparse, collections, gzip, hashlib, json, struct, sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.xbe_parser.xbe_parser import XBEParser
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, __version__ as unicorn_version
from unicorn import x86_const as r

XBE_SHA256 = 'aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7'
FUNCTIONS = {1: 0x641B0, 2: 0x641D0, 3: 0x642E0, 4: 0x64360, 5: 0x64320}
STATE, STACK, RETURN, FLOAT_OUT = 0x05000100, 0x0500F000, 0x05000000, 0x05000200

def capture(xbe_path, output):
    raw=xbe_path.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=XBE_SHA256: raise ValueError('Unsupported retail XBE')
    xbe=XBEParser(str(xbe_path)).parse()
    uc=Uc(UC_ARCH_X86,UC_MODE_32)
    uc.mem_map(0x10000,0x4000000)
    uc.mem_map(0x5000000,0x10000)
    for section in xbe.sections:
        uc.mem_write(section.virtual_addr,raw[section.raw_addr:section.raw_addr+section.raw_size])
    # A host-owned return stub rounds/pops ST(0) to binary32, matching the C API.
    uc.mem_write(RETURN,b'\xd9\x1d'+struct.pack('<I',FLOAT_OUT))
    uc.reg_write(r.UC_X86_REG_FPCW,0x037F)
    uc.reg_write(r.UC_X86_REG_FPTAG,0xFFFF)
    uc.reg_write(r.UC_X86_REG_MXCSR,0x1F80)
    trace=bytearray(b'ENMEM001'); counts=collections.Counter()
    def record(op,*words):
        trace.extend(struct.pack('<'+'I'*(len(words)+1),op,*words));counts[op]+=1
    def bits(value): return struct.unpack('<I',struct.pack('<f',value))[0]
    def call(op,*args):
        uc.reg_write(r.UC_X86_REG_ECX,STATE)
        uc.reg_write(r.UC_X86_REG_ESP,STACK)
        uc.mem_write(STACK,struct.pack('<'+'I'*(len(args)+1),RETURN if op==3 else RETURN+6,*args))
        stop=RETURN+6
        uc.emu_start(FUNCTIONS[op],stop,count=10000)
        if uc.reg_read(r.UC_X86_REG_EIP)!=stop:raise RuntimeError('Retail function failed to return')
        if uc.reg_read(r.UC_X86_REG_ESP)!=STACK+4+len(args)*4:raise RuntimeError('Unexpected retail stack ABI')
        if op==3:
            result=struct.unpack('<I',uc.mem_read(FLOAT_OUT,4))[0]
        elif op in (4,5): result=uc.reg_read(r.UC_X86_REG_EAX)&255
        else: result=None
        record(op,*args,*(() if result is None else (result,)))
    def compare():
        state=list(struct.unpack('<9I',uc.mem_read(STATE,36)))
        for i in range(4):
            if not state[1+i*2]:state[2+i*2]=0
        record(6,*state)
        for enemy in range(1,13):call(3,enemy)
    for fps in (30,60,120):
        call(1);call(4,1,bits(30));call(4,1,bits(10))
        for frame in range(fps*31):call(2,bits(1/fps));compare()
        call(3,1)
    call(1)
    for enemy in range(1,5):call(4,enemy,bits(enemy*10))
    call(4,5,bits(50));compare();call(3,1);call(3,5);call(5,3);compare()
    seed=0x13572468
    def random():
        nonlocal seed
        seed=(1664525*seed+1013904223)&0xffffffff
        return seed
    for step in range(100000):
        value=random();enemy=1+(value>>8)%12
        op=value%5
        if op==0:call(1)
        elif op in (1,2):call(4,enemy,bits((random()%2400)/16))
        elif op==3:call(5,enemy)
        else:call(2,bits((random()%400)/64))
        compare()
        if step%20000==19999:print('Captured',step+1,'random operations',flush=True)
    call(1);call(4,7,bits(30));call(2,bits(30.01));call(3,7)
    packed=gzip.compress(bytes(trace),compresslevel=9,mtime=0)
    output.mkdir(parents=True,exist_ok=True)
    (output/'expected.bin.gz').write_bytes(packed)
    metadata={'format':'ENMEM001','capture_date':'2026-09-29',
      'provenance':{'method':'Executed original retail x86 instructions under the public Unicorn emulator. No previous expected-result fixture was read.',
       'generator':'tools/recomp/capture_enemy_memory_retail.py',
       'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
       'unicorn_version':unicorn_version,
       'function_addresses':{str(op):hex(address) for op,address in FUNCTIONS.items()},
       'layout_evidence':'0x641B0 initializes base time and IDs at +4,+12,+20,+28; 0x642E0 walks four entries at 8-byte stride and reads timeout at +8; retail ret/ret4/ret8 establish call ABI.',
       'unused_slots':'Empty-slot timeout words canonicalized to zero; original regression comparison ignores them.'},
      'retail_xbe_sha256':XBE_SHA256,'compressed_sha256':hashlib.sha256(packed).hexdigest(),
      'uncompressed_sha256':hashlib.sha256(trace).hexdigest(),'uncompressed_bytes':len(trace),
      'record_counts':dict(sorted(counts.items())),
      'coverage':{'random_operations':100000,'seed':'0x13572468','expiry_fps':[30,60,120],'relation_values':2049}}
    (output/'manifest.json').write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'bytes':len(trace),'sha256':metadata['uncompressed_sha256']}))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('xbe',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args();capture(args.xbe,args.output)

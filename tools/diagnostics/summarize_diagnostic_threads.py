"""Bounded offline minidump stack candidates, not a debugger stack unwind.

Read-only: no process attachment, suspension, input, or memory modification.
Supply the map corresponding to the executable module actually in the dump.
"""
import argparse
import bisect
import mmap
from pathlib import Path
import re
import struct
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import inspect_minidump as dump


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('dump',type=Path)
    p.add_argument('--map',type=Path,required=True)
    p.add_argument('--threads',type=int,choices=range(1,129),default=1)
    args=p.parse_args()
    map_text=args.map.read_text(encoding='utf-8')
    preferred=int(re.search(r'Preferred load address is ([0-9A-Fa-f]+)',map_text)[1],16)
    text_end=max(int(offset,16)+int(length,16) for offset,length in
        re.findall(r'^\s*0001:([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]+)H\s+\S+\s+CODE',map_text,re.M))+0x1000
    with args.dump.open('rb') as stream,mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as data:
        if data[:4]!=b'MDMP':raise ValueError('Not a Windows minidump')
        streams={}
        for i in range(dump.u32(data,8)):
            kind,size,rva=struct.unpack_from('<III',data,dump.u32(data,12)+i*12)
            streams[kind]=(rva,size)
        modules=[];rva=streams[4][0]
        for i in range(dump.u32(data,rva)):
            o=rva+4+i*108
            modules.append((dump.u64(data,o),dump.u32(data,o+8),dump.u32(data,o+16),
                            dump.read_utf16(data,dump.u32(data,o+20))))
        module=next(m for m in modules if Path(m[3]).stem.lower()==args.map.stem.lower())
        base=module[0]
        symbols=sorted((int(address,16)-preferred+base,name) for name,address in
            re.findall(r'^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-f]{16})\s+f\s',map_text,re.M))
        addresses=[a for a,_ in symbols]
        def label(address):
            if base+0x1000<=address<base+text_end:
                index=bisect.bisect_right(addresses,address)-1
                if index>=0:return symbols[index][1]+'+'+hex(address-symbols[index][0])
            found=dump.module_for_address(modules,address)
            return Path(found[0]).name+'+'+hex(found[1]) if found else hex(address)
        rva=streams[3][0]
        for i in range(min(args.threads,dump.u32(data,rva))):
            o=rva+4+i*48;size=dump.u32(data,o+40);context=dump.u32(data,o+44)
            if size<0x100:continue
            rip=dump.u64(data,context+0xF8);rsp=dump.u64(data,context+0x98)
            print(f'thread {dump.u32(data,o)} RIP={label(rip)}')
            start=dump.u64(data,o+24);length=dump.u32(data,o+32);file_rva=dump.u32(data,o+36)
            offset=max(0,rsp-start);candidates=[]
            for k in range(offset,min(length-7,offset+2048),8):
                address=dump.u64(data,file_rva+k)
                if base+0x1000<=address<base+text_end:
                    value=label(address)
                    if value not in candidates:candidates.append(value)
            print('  stack candidates (NOT unwound): '+', '.join(candidates[:18]))


if __name__=='__main__':main()

"""Produce address/string evidence from the supported retail XBE only.

Names are retail string literals, not recovered C++ symbols. Instruction
references are exact operands; no private debug map or filename interpolation
is consumed. Whole functions are deliberately not renamed from a nearby string.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from capstone import Cs,CS_ARCH_X86,CS_MODE_32
from capstone.x86 import X86_OP_IMM,X86_OP_MEM
SUPPORTED="aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7"

def recover(path):
    path=Path(path);raw=path.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=SUPPORTED:raise ValueError("Unsupported retail XBE")
    config.configure_from_xbe(str(path));strings={}
    for section in config._SECTIONS:
        data=raw[section.raw_addr:section.raw_addr+section.raw_size]
        for match in re.finditer(rb"[ -~]{4,160}\x00",data):
            text=match.group()[:-1].decode("ascii")
            if not (re.search(r"(?:Rs|Red|Pbl)[A-Z][A-Za-z0-9_:]+",text) or
                    re.search(r"[\\/][A-Za-z0-9_.-]+\.(?:cpp|c|h)(?: \[\d+\])?$",text)):
                continue
            va=section.va+match.start()
            strings[va]={"address":f"0x{va:08X}","text":text,"confidence":"literal-bytes",
                         "file_offset":section.raw_addr+match.start(),"references":[]}
    md=Cs(CS_ARCH_X86,CS_MODE_32);md.detail=True;md.skipdata=True
    for section in config._SECTIONS:
        if not section.is_code:continue
        data=raw[section.raw_addr:section.raw_addr+section.raw_size]
        for ins in md.disasm(data,section.va):
            if not ins.id:continue
            for op in ins.operands:
                va=(op.imm&0xFFFFFFFF) if op.type==X86_OP_IMM else ((op.mem.disp&0xFFFFFFFF) if op.type==X86_OP_MEM and not op.mem.base and not op.mem.index else None)
                if va in strings:
                    strings[va]["references"].append({"instruction":f"0x{ins.address:08X}","bytes":ins.bytes.hex(),"confidence":"direct-operand-reference"})
    return {"schema":1,"retail_xbe_sha256":SUPPORTED,"method":"Bounded ASCII literals and decoded executable-section operands; not original method names or an interpolation map.","strings":list(strings.values())}

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument("xbe",type=Path);p.add_argument("output",type=Path)
    a=p.parse_args();data=recover(a.xbe);a.output.write_text(json.dumps(data,indent=2)+"\n")
    print(f"Recorded {len(data['strings'])} retail literals and {sum(len(s['references']) for s in data['strings'])} direct operands")

if __name__=="__main__":main()

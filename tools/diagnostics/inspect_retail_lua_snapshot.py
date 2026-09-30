"""Read briefing Lua globals from a saved retail guest-RAM snapshot.

Uses the public Lua 5.0.3 ABI, checked against retail table/string instructions.
See docs/runtime/retail-lua-snapshot-evidence.md. Does not open
processes, execute Lua, edit snapshots, or infer that an old heap error is live.
"""
import argparse
import json
from pathlib import Path
import struct


class Snapshot:
    def __init__(self, data):
        self.data = data

    def read(self, address, count):
        if not 0 <= address <= len(self.data)-count or count < 0:
            raise ValueError('Read outside supplied snapshot')
        return self.data[address:address+count]

    def u32(self, address):
        return struct.unpack('<I', self.read(address,4))[0]

    def string(self, address):
        if self.read(address+4,1) != b'\x04':
            raise ValueError('Not a Lua string object')
        size = self.u32(address+12)
        if size > 65536:
            raise ValueError('Unbounded Lua string')
        return self.read(address+16,size).decode('utf-8',errors='replace')

    def value(self, address):
        tag = self.u32(address)
        data = address+8
        if tag == 0:
            return None
        if tag == 1:
            return bool(self.u32(data))
        if tag == 3:
            return struct.unpack('<d',self.read(data,8))[0]
        if tag == 4:
            return self.string(self.u32(data))
        return {'lua_type':tag,'address':f'{self.u32(data):08X}'}

    def globals(self, table):
        if self.read(table+4,1) != b'\x05':
            raise ValueError('Not a Lua table')
        log_size = self.read(table+7,1)[0]
        if log_size > 16:
            raise ValueError('Unbounded Lua hash table')
        nodes = self.u32(table+16)
        self.read(nodes,(1<<log_size)*40)
        result = {}
        for i in range(1<<log_size):
            node = nodes+i*40
            if self.u32(node) == 4 and self.u32(node+16) != 0:
                result[self.string(self.u32(node+8))] = self.value(node+16)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot',type=Path)
    parser.add_argument('--globals',type=lambda s:int(s,0),dest='table')
    parser.add_argument('--key',action='append',default=[])
    args = parser.parse_args()
    if args.snapshot.stat().st_size != 0x4000000:
        raise ValueError('Expected a saved64MiB retail guest snapshot')
    memory = Snapshot(args.snapshot.read_bytes())
    table = args.table
    if table is None:
        state = memory.u32(0x403378)
        lua = memory.u32(state+0x1c)
        if memory.u32(lua+0x40) != 5:
            raise ValueError('Unexpected briefing globals TObject')
        table = memory.u32(lua+0x48)
        print(f'Briefing RsLuaState={state:08X} lua_State={lua:08X} globals={table:08X}')
    values = memory.globals(table)
    if args.key:
        values = {key:values.get(key,{'absent':True}) for key in args.key}
    else:
        values = {key:value for key,value in values.items()
                  if not isinstance(value,dict) or value.get('lua_type') != 6}
    print(json.dumps(values,indent=2,sort_keys=True))


if __name__ == '__main__':
    main()

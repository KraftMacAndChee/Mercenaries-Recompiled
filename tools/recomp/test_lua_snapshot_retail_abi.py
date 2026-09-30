"""Establish the snapshot inspector ABI from public Lua and retail instructions.

The compiler performs syntax/layout checks only; no native executable is produced.
Retail code executes in Unicorn against constructed memory, not the live game.
"""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config
from tools.diagnostics.inspect_retail_lua_snapshot import Snapshot
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn import x86_const as r

PUBLIC_LAYOUT = r'''
#include <stddef.h>
#include "lstate.h"
#define OFFSET(t,f,n) _Static_assert(offsetof(t,f)==n, #t "." #f)
_Static_assert(sizeof(void*)==4,"32-bit guest ABI");
_Static_assert(sizeof(lua_Number)==8,"double Lua numbers");
_Static_assert(sizeof(TObject)==16,"value stride");
_Static_assert(sizeof(Node)==40,"node stride");
_Static_assert(sizeof(TString)==16,"string payload offset");
OFFSET(TObject,tt,0); OFFSET(TObject,value,8);
OFFSET(TString,tsv.tt,4); OFFSET(TString,tsv.hash,8); OFFSET(TString,tsv.len,12);
OFFSET(Table,tt,4); OFFSET(Table,lsizenode,7); OFFSET(Table,node,16);
OFFSET(Node,i_key,0); OFFSET(Node,i_val,16); OFFSET(Node,next,32);
OFFSET(lua_State,_gt,64); OFFSET(lua_State,l_G,16);
_Static_assert(LUA_TNIL==0 && LUA_TBOOLEAN==1 && LUA_TNUMBER==3 &&
               LUA_TSTRING==4 && LUA_TTABLE==5,"public type tags");
'''


class LuaSnapshotEvidence(unittest.TestCase):
    def test_public_layout(self):
        public = ROOT / 'third_party/lua-5.0.3'
        manifest = json.loads((public / 'UPSTREAM.json').read_text())
        for name, expected in manifest['files'].items():
            self.assertEqual(hashlib.sha256((public/name).read_bytes()).hexdigest(), expected, name)
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='lua-public-layout-') as temp:
            source = Path(temp) / 'layout.c'
            source.write_text(PUBLIC_LAYOUT)
            subprocess.run([compiler, '-m32', '-std=c11', '-fsyntax-only',
                            '-I', str(public/'include'), '-I', str(public/'src'),
                            str(source)], check=True)

    def test_retail_string_table_lookup(self):
        path = ROOT / 'game_files/mercenaries-retail/default.xbe'
        raw = path.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),
                         'aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7')
        config.configure_from_xbe(str(path))
        u = Uc(UC_ARCH_X86, UC_MODE_32)
        u.mem_map(0, 0x400000)
        for address, length, digest in [
            (0x1E2FF0,62,'09776d7dbcad630cfd809268585229fcca0817d1cd6ff699a93892e1e74eefcb'),
            (0x175820,9,'888431255b62e84823457dccaaff17372cf66927b0c5d24b69a1b9669f396800'),
            (0x1DC820,76,'b4beb8ba5eaee05b656cddc682f37093a169387d4ab0b1ea7421827064bb1aa0'),
        ]:
            offset = config.va_to_file_offset(address)
            code = raw[offset:offset+length]
            self.assertEqual(hashlib.sha256(code).hexdigest(), digest)
            u.mem_write(address, code)
        memory = bytearray(0x10000)
        table, nodes, strings, stack, stop = 0x100, 0x200, 0x2000, 0xF000, 0xF100
        memory[table+4] = 5
        memory[table+7] = 6
        struct.pack_into('<I', memory, table+16, nodes)
        expected = {}
        for index in range(64):
            name = ('probe_%02d' % index).encode()
            text = strings+index*64
            memory[text+4] = 4
            # Several keys share one bucket, exercising linked collision nodes.
            struct.pack_into('<II', memory, text+8, index % 16, len(name))
            memory[text+16:text+16+len(name)] = name
            node = nodes+index*40
            struct.pack_into('<I', memory, node, 4)
            struct.pack_into('<I', memory, node+8, text)
            struct.pack_into('<I', memory, node+32, node+16*40 if index < 48 else 0)
            if index % 3 == 0:
                struct.pack_into('<I', memory, node+16, 3)
                struct.pack_into('<d', memory, node+24, index+0.25)
                expected[name.decode()] = index+0.25
            elif index % 3 == 1:
                struct.pack_into('<I', memory, node+16, 1)
                struct.pack_into('<I', memory, node+24, index % 2)
                expected[name.decode()] = bool(index % 2)
            else:
                struct.pack_into('<I', memory, node+16, 4)
                struct.pack_into('<I', memory, node+24, text)
                expected[name.decode()] = name.decode()
        u.mem_write(0, bytes(memory))
        # The public global pseudo-index must resolve to the same TObject that
        # automatic briefing lookup reads. ECX is the retail internal state arg.
        u.mem_write(stack, struct.pack('<I', stop))
        u.reg_write(r.UC_X86_REG_ESP, stack)
        u.reg_write(r.UC_X86_REG_ECX, 0x8000)
        u.reg_write(r.UC_X86_REG_EAX, (-10001) & 0xFFFFFFFF)
        u.emu_start(0x1DC820, stop, count=100)
        self.assertEqual(u.reg_read(r.UC_X86_REG_EIP), stop)
        self.assertEqual(u.reg_read(r.UC_X86_REG_EAX), 0x8040)
        snapshot = Snapshot(memory)
        self.assertEqual(snapshot.globals(table), expected)
        for index in range(64):
            u.mem_write(stack, struct.pack('<III', stop, table, strings+index*64))
            u.reg_write(r.UC_X86_REG_ESP, stack)
            u.reg_write(r.UC_X86_REG_ESI, 0xC0FFEE)
            u.emu_start(0x1E2FF0, stop, count=1000)
            self.assertEqual(u.reg_read(r.UC_X86_REG_EIP), stop)
            self.assertEqual(u.reg_read(r.UC_X86_REG_ESP), stack+4)
            self.assertEqual(u.reg_read(r.UC_X86_REG_ESI), 0xC0FFEE)
            address = u.reg_read(r.UC_X86_REG_EAX)
            self.assertEqual(address, nodes+index*40+16)
            self.assertEqual(snapshot.value(address), expected['probe_%02d' % index])


if __name__ == '__main__':
    unittest.main()

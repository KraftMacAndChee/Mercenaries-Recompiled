"""Execute the production Lua protected frame and host unwind bridge.

Current expectations are independently checked with unmodified public Lua 5.0.3,
and guest offsets are supported by retail instructions at VA 0x1DE970.
See docs/runtime/retail-lua-protected-evidence.md for evidence and history.
"""
from pathlib import Path
import re
import hashlib
import json
import sys
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

PRELUDE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c); exit(2); } } while(0)
static unsigned char memory[0x100000];
static ptrdiff_t g_xbox_mem_offset;
static uint32_t g_eax,g_ebx,g_ecx,g_edx,g_esi,g_edi,g_esp,g_seh_ebp;
#define eax g_eax
#define ebx g_ebx
#define ecx g_ecx
#define edx g_edx
#define esi g_esi
#define edi g_edi
#define esp g_esp
#define MEM32(a) (*(uint32_t*)(void*)(memory+(uint32_t)(a)))
#define MEM16(a) (*(uint16_t*)(void*)(memory+(uint32_t)(a)))
#define MEM8(a) (*(uint8_t*)(void*)(memory+(uint32_t)(a)))
#define XBOX_PTR(a) ((uintptr_t)(memory+(uint32_t)(a)))
#define PUSH32(s,v) do { uint32_t value_=(v); (s)-=4; MEM32(s)=value_; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
#define CMP_NE(a,b) ((uint32_t)(a)!=(uint32_t)(b))
#define CMP_BE(a,b) ((uint32_t)(a)<=(uint32_t)(b))
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define XBOX_MEMCPY(d,s,n) memcpy(memory+(d),memory+(s),(n))
#define RECOMP_TRACE_FUNC(a) ((void)0)
static void callback(uint32_t target);
#define RECOMP_ICALL_SAFE(t,s) callback(t)
'''

HARNESS = r'''
enum { STATE=0x10000, STACK=0xF0000, CALLBACK=0xBEEF };
static unsigned calls, level, max_level, mode;
static void invoke(void) {
 uint32_t before=esp;
 PUSH32(esp,0x1234); PUSH32(esp,CALLBACK); PUSH32(esp,STATE); PUSH32(esp,0);
 sub_001DE970(); esp+=12;
 CHECK(esp==before);
}
static void callback(uint32_t target) {
 CHECK(target==CALLBACK); CHECK(MEM32(esp+4)==STATE); CHECK(MEM32(esp+8)==0x1234);
 uint32_t handler=MEM32(STATE+0x58); CHECK(handler!=0); ++calls;
 if(level<max_level) {
  ++level; invoke(); --level;
  CHECK(MEM32(STATE+0x58)==handler);
  CHECK(eax==(mode ? 4 : 0));
 }
 if(mode) {
  MEM32(handler+0x44)=4;
  esp-=128; ebx=0xCC; edi=0xDD; esi=0xEE; MEM32(0)=0;
  CHECK(recomp_lua_host_longjmp(handler+4,1));
  CHECK(!"throw must not return");
 }
 esp+=4;
}
int main(void) {
 g_xbox_mem_offset=(ptrdiff_t)memory;
 for(mode=0;mode<2;++mode) for(max_level=0;max_level<8;++max_level) {
  for(unsigned repeat=0;repeat<1000;++repeat) {
   memset(memory,0,sizeof(memory)); MEM32(0)=0xFFFFFFFFu;
   esp=STACK; ebx=0x12; edi=0x34; esi=0x56; g_seh_ebp=0;
   level=0; calls=0; invoke();
   CHECK(calls==max_level+1); CHECK(eax==(mode ? 4 : 0));
   CHECK(MEM32(STATE+0x58)==0); CHECK(g_recomp_lua_host_jmp_depth==0);
   CHECK(esi==0x56); CHECK(MEM32(0)==0xFFFFFFFFu);
  }
 }
 puts("16000 nested/repeated protected calls preserve stack, handler and status");
 return 0;
}
'''


class LuaProtectedUnwindTests(unittest.TestCase):
    def test_protected_unwind(self):
        manual = (ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        bridge = manual[manual.index('#define RECOMP_LUA_HOST_JMP_MAX'):manual.index('double recomp_native_strtod')]
        types = (ROOT/'ports/mercenaries/src/recomp/recomp_types.h').read_text(encoding='utf-8')
        movs = re.search(r'static __forceinline void XBOX_REP_MOVS\(.*?\n\}', types, re.S)[0]
        movs = movs.replace('__forceinline', 'inline')
        bodies = [movs]
        for file, name in [('recomp_0011.c','0023940C'),('recomp_0009.c','001DE970')]:
            text = (ROOT/'ports/mercenaries/src/recomp/gen'/file).read_text(encoding='utf-8')
            bodies.append(re.search(r'void sub_'+name+r'\(void\)\n\{.*?\n\}',text,re.S)[0])
        macro = '#define RECOMP_LUA_HOST_SETJMP(b) setjmp(*(jmp_buf*)recomp_lua_host_jmp_register(b))\n'
        compiler = shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='mercs-lua-unwind-') as temp:
            source=Path(temp)/'test.c'; exe=Path(temp)/'test.exe'
            source.write_text(PRELUDE+bridge+macro+'\n'.join(bodies)+HARNESS)
            build=subprocess.run([compiler,'-std=c11','-O2',str(source),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            print(result.stdout.strip())


    def test_public_lua_and_retail_layout(self):
        """Establish handler behavior from public code and offsets from the XBE."""
        sys.path.insert(0,str(ROOT))
        from tools.recomp import config
        retail=ROOT/'game_files/mercenaries-retail/default.xbe'
        raw=retail.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(),
            'aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7')
        config.configure_from_xbe(str(retail))
        offset=config.va_to_file_offset(0x1DE970)
        code=raw[offset:offset+0x5c]
        self.assertEqual(hashlib.sha256(code).hexdigest(),
            'faf9d21328586ee92c41dd19b06c67475d7f8022bdfe4bedede6af903ac30b6f')
        for index,expected in ((9,'8b4858'),(16,'895058'),(0x43,'895658'),(0x51,'894858')):
            self.assertEqual(code[index:index+3],bytes.fromhex(expected))
        lua=ROOT/'third_party/lua-5.0.3'
        manifest=json.loads((lua/'UPSTREAM.json').read_text())
        for name,digest in manifest['files'].items():
            self.assertEqual(hashlib.sha256((lua/name).read_bytes()).hexdigest(),digest,name)
        program=r"""
#include <assert.h>
#include <stdio.h>
#include "lua.h"
#include "ldo.h"
#include "lstate.h"
static unsigned depth,limit,mode,calls;
static void nested(lua_State *state,void *data) {
    struct lua_longjmp *handler=state->errorJmp;
    assert(handler && data==&calls);
    ++calls;
    if(depth<limit) {
        ++depth;
        int status=luaD_rawrunprotected(state,nested,data);
        --depth;
        assert(status==(mode ? 4 : 0));
        assert(state->errorJmp==handler);
    }
    if(mode)luaD_throw(state,4);
}
int main(void) {
    lua_State *state=lua_open();assert(state);
    for(mode=0;mode<2;++mode)for(limit=0;limit<8;++limit)
        for(unsigned repeat=0;repeat<1000;++repeat) {
            depth=calls=0;
            struct lua_longjmp *prior=state->errorJmp;
            int status=luaD_rawrunprotected(state,nested,&calls);
            assert(status==(mode ? 4 : 0));
            assert(calls==limit+1 && state->errorJmp==prior);
        }
    lua_close(state);
    puts("Public Lua: 16000 nested/repeated calls restore handler and status");
}
"""
        with tempfile.TemporaryDirectory(prefix='public-lua-unwind-') as temp:
            source=Path(temp)/'test.c';exe=Path(temp)/'test.exe'
            source.write_text(program)
            compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
            command=[compiler,'-O1','-I'+str(lua/'include'),'-I'+str(lua/'src'),
                     str(source),*map(str,(lua/'src').glob('*.c')),'-o',str(exe)]
            result=subprocess.run(command,capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr)
            print(result.stdout.strip())

if __name__=='__main__': unittest.main()

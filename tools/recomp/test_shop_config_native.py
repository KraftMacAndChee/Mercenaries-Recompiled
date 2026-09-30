"""Exercise the retail config parser at the shop's 64-item overflow boundary."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CC = Path('C:/msys64/mingw64/bin/gcc.exe')

class ShopConfigTests(unittest.TestCase):
    def test_retail_parser_and_private_storage(self):
        gen = (ROOT / 'ports/mercenaries/src/recomp/gen/recomp_0009.c').read_text(encoding='utf-8')
        bodies = '\n'.join(re.search(r'void sub_' + address + r'\(void\)\n\{.*?\n\}', gen, re.S)[0]
                           for address in ('001EEF80', '001EEFA0', '001EF410', '001EF500'))
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char memory[0xA00000];
ptrdiff_t g_xbox_mem_offset;
static uint32_t eax,ebx,ecx,edx,esi,edi,esp;
#define g_esp esp
#define MEM32(a) (*(uint32_t *)(memory+(uint32_t)(a)))
#define MEM8(a) (*(uint8_t *)(memory+(uint32_t)(a)))
#define LO8(a) ((uint8_t)(a))
#define SET_LO8(a,b) ((a)=((a)&0xffffff00u)|(uint8_t)(b))
#define PUSH32(s,v) do { uint32_t val=(v); (s)-=4; MEM32(s)=val; } while(0)
#define POP32(s,v) do { (v)=MEM32(s); (s)+=4; } while(0)
#define CMP_GE(a,b) ((int32_t)(a)>=(int32_t)(b))
#define CMP_LE(a,b) ((int32_t)(a)<=(int32_t)(b))
#define TEST_Z(a,b) (((a)&(b))==0)
#define TEST_NZ(a,b) (((a)&(b))!=0)
#define RECOMP_TRACE_FUNC(a) ((void)0)
static unsigned allocations,frees,fail_alloc;
static uint32_t allocation_size;
uint32_t xbox_HeapAlloc(uint32_t size,uint32_t alignment) {
    assert(alignment==16 && size<0x100000);
    if(fail_alloc) return 0;
    ++allocations; allocation_size=size;
    memset(memory+0x800000,0xCD,size+32); return 0x800000;
}
void xbox_HeapFree(uint32_t p) {assert(p==0x800000);++frees;}
static void icall(uint32_t target) {
    if(target==1) {eax=MEM32(ecx+8);esp+=4;}
    else if(target==2) {eax=MEM32(ecx+0x10);esp+=4;}
    else if(target==3) {
        uint32_t dest=MEM32(esp+4),count=MEM32(esp+8),pos=MEM32(ecx+0x10);
        assert(pos+count<=MEM32(ecx+8));
        memcpy(memory+dest,memory+MEM32(ecx+4)+pos,count);
        MEM32(ecx+0x10)=pos+count;eax=count;esp+=12;
    } else assert(0);
}
#define RECOMP_ICALL_SAFE(target,restore) icall(target)
/* Hash values are immaterial to the index boundary; use a deterministic hash. */
static void sub_001F29F0(void) {
    const unsigned char *s=memory+MEM32(esp+4);eax=2166136261u;
    while(*s) {eax^=*s++;eax*=16777619u;} esp+=4;
}
"""
        source += '#include "' + (ROOT/'ports/mercenaries/src/shop_config.c').as_posix() + '"\n'
        source += bodies + r"""
static void run(unsigned items,const char *newline,int fixed,int trim) {
    const uint32_t cfg=0x1000,file=0x2000,data=0x10000;
    memset(memory,0xA5,sizeof(memory));
    unsigned old_allocations=allocations,old_frees=frees;
    char *out=(char*)memory+data;
    for(unsigned i=0;i<items;i++) {
        out+=sprintf(out,"[ShopItem]%s",newline);
        for(unsigned j=0;j<7;j++)out+=sprintf(out,"Key%u value_%u%s",j,i,newline);
    }
    uint32_t size=(uint32_t)(out-(char*)memory-data);
    if(trim&&size)size-=(uint32_t)strlen(newline);
    MEM32(file)=0x3000;MEM32(file+4)=data;MEM32(file+8)=size;MEM32(file+0x10)=0;
    MEM32(0x3004)=1;MEM32(0x301C)=2;MEM32(0x3010)=3;
    ecx=cfg;esp=0x900000;sub_001EEF80();
    if(fixed)recomp_shop_config_storage(cfg,data,size);
    uint32_t table=MEM32(cfg+0x14),capacity=MEM32(cfg+0x10);
    ecx=cfg;esp=0x900000;ebx=0xABCD;esi=0x1234;edi=0x5678;
    PUSH32(esp,file);PUSH32(esp,0);sub_001EF500();
    assert(esp==0x900000 && ebx==0xABCD && esi==0x1234 && edi==0x5678);
    assert(MEM32(cfg+0xC)==items*8+1);
    assert(MEM32(table+items*64)==0 && MEM32(table+items*64+4)==0xffffffff);
    if(!fixed&&items==64) {
        assert(MEM32(0x642F30)==0); /* Retail corruption reproduced exactly. */
        assert(MEM32(0x642F34)==0xffffffff);
    } else {
        for(unsigned i=0;i<8192;i++)assert(memory[0x642F30+i]==0xA5);
        assert(items*8+1<=capacity);
        if(table==0x800000)for(unsigned i=0;i<32;i++)assert(memory[table+allocation_size+i]==0xCD);
    }
    ecx=cfg;esp=0x900000;sub_001EEFA0();
    if(fixed)recomp_shop_config_release(cfg);
    assert(MEM32(cfg+0x14)==0x641F30 && MEM32(cfg+0x10)==512);
    assert(allocations-old_allocations==frees-old_frees);
}
int main(int argc,char **argv) {
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;
    if(argc>1) {fail_alloc=1;run(64,"\r\n",1,0);return 1;}
    run(63,"\r\n",0,0);run(64,"\r\n",0,0);
    for(unsigned repeat=0;repeat<3;repeat++)for(unsigned count=0;count<=256;count++) {
        run(count,"\r\n",1,0);run(count,"\n",1,1);run(count,"\r",1,0);
    }
    puts("PASS: retail 64th-item corruption reproduced; private index protects adjacent globals through 256 items, all line endings, EOF and reloads");
    return 0;
}
"""
        env=os.environ.copy();env['PATH']=str(CC.parent)+os.pathsep+env['PATH']
        with tempfile.TemporaryDirectory(prefix='merc-shop-') as directory:
            tmp=Path(directory);(tmp/'test.c').write_text(source,encoding='utf-8')
            subprocess.run([str(CC),'-std=c11','-O2','-I',str(ROOT/'src'),str(tmp/'test.c'),'-o',str(tmp/'test.exe')],check=True,env=env,capture_output=True,text=True)
            result=subprocess.run([str(tmp/'test.exe')],env=env,capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout.strip())
            result=subprocess.run([str(tmp/'test.exe'),'fail'],env=env,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,86,result.stdout+result.stderr)

if __name__=='__main__':
    unittest.main()

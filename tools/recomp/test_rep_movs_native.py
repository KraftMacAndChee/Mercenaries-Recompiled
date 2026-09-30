"""Compare lifted REP MOVS against native x86, including destructive overlap."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator


def main():
    cases=[];functions=[]
    for width,opcode in ((1,'f3a4'),(2,'66f3a5'),(4,'f3a5')):
        for backward in (False,True):
            address=0x15000+len(cases)*0x100
            code=bytes.fromhex(('fd' if backward else 'fc')+opcode+'fc'+'c3')
            config._install([config.Section('.text',address,len(code),0,len(code),True)],entry_point=address,kernel_thunk_addr=0,origin='native-rep-copy-test')
            fn={'name':f'sub_{address:08X}','start':f'0x{address:08X}','end':address+len(code),'size':len(code)}
            translated=FunctionTranslator(code,{address:fn}).translate_function(address,fn)
            functions.append(re.search(r'void sub_[0-9A-F]+\(void\)\n\{.*?\n\}',translated,re.S)[0])
            cases.append(f'run(sub_{address:08X},{width},{int(backward)});')
    fixture='#define RECOMP_GENERATED_CODE\n#include "'+(ROOT/'templates/runtime/recomp_types.h').as_posix()+'"\n'+r'''
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[4096],expected[4096];
static unsigned failures,total;
static uint32_t guest_base;
#undef RECOMP_TRACE_FUNC
#define RECOMP_TRACE_FUNC(a) ((void)0)
'''+ '\n'.join(functions)+r'''
static void native_copy(unsigned d,unsigned s,unsigned count,unsigned width,unsigned backward) {
 void *dst=expected+d;const void *src=expected+s;size_t n=count;
 #define COPY(op) do{if(backward)__asm__ volatile("std; rep " op "; cld" : "+D"(dst),"+S"(src),"+c"(n)::"memory","cc");else __asm__ volatile("cld; rep " op : "+D"(dst),"+S"(src),"+c"(n)::"memory","cc");}while(0)
 if(width==1)COPY("movsb");else if(width==2)COPY("movsw");else COPY("movsl");
 #undef COPY
}
static void run(void (*fn)(void),unsigned width,unsigned backward) {
 for(unsigned count=0;count<=128;count++)for(int delta=-129;delta<=129;delta++){
  unsigned s=1024,d=s+delta;
  for(unsigned i=0;i<sizeof(memory);i++)memory[i]=(unsigned char)((i*37+(i>>4)*13)^(i>>8));
  memcpy(expected,memory,sizeof(memory));native_copy(d,s,count,width,backward);
  esi=guest_base+s;edi=guest_base+d;ecx=count;esp=guest_base+4000;fn();
  int step=backward?-(int)width:(int)width;
  assert(esi==guest_base+s+count*step&&edi==guest_base+d+count*step&&ecx==0&&esp==guest_base+4004);
  total++;if(memcmp(memory,expected,sizeof(memory))){if(failures<6)printf("mismatch width=%u backward=%u count=%u delta=%d\n",width,backward,count,delta);failures++;}
 }
}
int main(void){for(unsigned mode=0;mode<2;mode++){guest_base=mode?0xFD000000u:0;g_xbox_mem_offset=(ptrdiff_t)memory-guest_base;
'''+''.join(cases)+r'''
 }
 printf("REP MOVS native oracle: %u cases, %u mismatches\n",total,failures);return failures?1:0;
}
'''
    with tempfile.TemporaryDirectory(prefix='rep-movs-native-') as temp:
        p=Path(temp);(p/'fixture.c').write_text(fixture)
        cc=shutil.which('gcc') or 'C:/msys64/mingw64/bin/gcc.exe'
        subprocess.run([cc,'-O2','-fno-strict-aliasing',str(p/'fixture.c'),'-o',str(p/'test.exe')],check=True)
        subprocess.run([str(p/'test.exe')],check=True,timeout=60)


if __name__=='__main__':
    main()

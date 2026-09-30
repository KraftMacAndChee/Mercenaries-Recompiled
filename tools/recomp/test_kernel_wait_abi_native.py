"""Pin the two retail NT single-wait imports to their actual x86 stack ABI.

This verifies stack cleanup, not implementation of native wait semantics.
The wait bridges are still absent; do not describe this as a save-hang fix.
"""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp import config


class KernelWaitAbiTests(unittest.TestCase):
    def test_retail_calls_and_dispatch_cleanup(self):
        xbe=ROOT/'game_files/mercenaries-retail/default.xbe'
        config.configure_from_xbe(str(xbe)); raw=xbe.read_bytes()
        decoder=Cs(CS_ARCH_X86,CS_MODE_32)
        for ordinal,thunk,start,end,count in (
            (233,0x2DBD60,0x229DB5,0x229DC0,3),
            (233,0x2DBD60,0x229E96,0x229EA1,3),
            (234,0x2DBD28,0x2292A1,0x2292B0,4),
        ):
            self.assertEqual(struct.unpack_from('<I',raw,config.va_to_file_offset(thunk))[0],0x80000000|ordinal)
            offset=config.va_to_file_offset(start)
            instructions=list(decoder.disasm(raw[offset:offset+end-start],start))
            self.assertEqual(sum(i.mnemonic=='push' for i in instructions),count)
            self.assertEqual(instructions[-1].mnemonic,'call')
            self.assertIn(hex(thunk),instructions[-1].op_str)
        text=(ROOT/'src/kernel/kernel_bridge.c').read_text(encoding='utf-8')
        body=re.search(r'static int stdcall_args_for_ordinal\(ULONG ordinal\)\n\{.*?\n\}',text,re.S)[0]
        self.assertIn('case 235: return 24;',body)
        header=(ROOT/'src/kernel/kernel.h').read_text(encoding='utf-8')
        sync=(ROOT/'src/kernel/kernel_sync.c').read_text(encoding='utf-8')
        expected=('xbox_NtWaitForMultipleObjectsEx(ULONG Count, HANDLE Handles[], '
                  'ULONG WaitType, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, '
                  'PLARGE_INTEGER Timeout)')
        self.assertIn(expected,header)
        self.assertRegex(sync,r'xbox_NtWaitForMultipleObjectsEx\(\s*ULONG Count,\s*'
                         r'HANDLE Handles\[\],\s*ULONG WaitType,\s*KPROCESSOR_MODE WaitMode,\s*'
                         r'BOOLEAN Alertable,\s*PLARGE_INTEGER Timeout\)')
        old=body.replace('case 233: return 12;','case 233: return 20;').replace('case 234: return 16;','case 234: return 12;')
        self.assertNotEqual(body,old)
        prelude='#include <stdint.h>\n#include <stdio.h>\ntypedef uint32_t ULONG;\n'
        harness=r'''
int main(void){
 const unsigned ordinals[3]={233,234,235};
 const unsigned counts[3]={3,4,6};
 for(unsigned index=0;index<3;++index){
  unsigned ordinal=ordinals[index],count=counts[index]; uint32_t esp=0x8BFFF0u;
  for(unsigned call=0;call<100000;++call){
   esp-=4u*(count+1u); /* retail args plus CALL's return address */
   esp+=4u+(uint32_t)stdcall_args_for_ordinal(ordinal);
   if(esp!=0x8BFFF0u){fprintf(stderr,"ordinal %u drift at call %u\n",ordinal,call);return 2;}
  }
 }
 puts("300000 Xbox wait dispatches preserve guest ESP");return 0;
}
'''
        compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
        with tempfile.TemporaryDirectory(prefix='mercs-wait-abi-') as temp:
            source=Path(temp)/'test.c'; exe=Path(temp)/'test.exe'
            for label,code in [('fixed',body),('old',old)]:
                source.write_text(prelude+code+harness)
                build=subprocess.run([compiler,'-std=c11','-O2',str(source),'-o',str(exe)],capture_output=True,text=True)
                self.assertEqual(build.returncode,0,build.stderr)
                result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
                if label=='fixed':
                    self.assertEqual(result.returncode,0,result.stderr); print(result.stdout.strip())
                else:self.assertNotEqual(result.returncode,0,'Old wait ABI escaped the regression')


if __name__=='__main__':unittest.main()

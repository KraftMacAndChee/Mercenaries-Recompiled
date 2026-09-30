"""Exercise production Win32 file reads/writes in one temporary scratch file.

This does not open a game process or touch game saves. It checks whether the
host accepts the DWORD-aligned buffers used by retail PblMemCard with the
FILE_FLAG_NO_BUFFERING flag. Alignment enforcement can vary by filesystem.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]


def main():
    text=(ROOT/'src/kernel/kernel_file.c').read_text()
    bodies='\n'.join(re.search(r'NTSTATUS __stdcall xbox_Nt'+name+r'\(.*?\n\}',text,re.S)[0]
                     for name in ('ReadFile','WriteFile'))
    source=r'''
#include "src/kernel/kernel.h"
#include <stdio.h>
#include <string.h>
#undef XBOX_TRACE
#define XBOX_TRACE(...) ((void)0)
'''+bodies+r'''
int main(int argc,char **argv){
 if(argc!=2)return 1;
 unsigned char *data=VirtualAlloc(NULL,65536,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
 if(!data)return 2;
 for(unsigned buffered=0;buffered<2;++buffered){
  HANDLE h=CreateFileA(argv[1],GENERIC_READ|GENERIC_WRITE,0,NULL,CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL|(buffered?0:FILE_FLAG_NO_BUFFERING),NULL);
  if(h==INVALID_HANDLE_VALUE)return 3;
  for(unsigned misaligned=0;misaligned<2;++misaligned){
   unsigned char *buffer=data+(misaligned?16:0);
   memset(buffer,0xA5,4096);XBOX_IO_STATUS_BLOCK ios;
   LARGE_INTEGER offset;offset.QuadPart=misaligned?4096:0;
   NTSTATUS status=xbox_NtWriteFile(h,NULL,NULL,NULL,&ios,buffer,4096,&offset);
   printf("buffered=%u address_offset=%u write_status=%08lX bytes=%llu ",
       buffered,misaligned?16:0,(unsigned long)status,(unsigned long long)ios.Information);
   if(status==0){
    memset(buffer,0,4096);
    status=xbox_NtReadFile(h,NULL,NULL,NULL,&ios,buffer,4096,&offset);
    unsigned equal=1;for(unsigned i=0;i<4096;++i)if(buffer[i]!=0xA5)equal=0;
    printf("read_status=%08lX bytes=%llu equal=%u\n",(unsigned long)status,
       (unsigned long long)ios.Information,equal);
    if(status!=0||ios.Information!=4096||!equal)return 4;
   }else printf("read_skipped\n");
  }
  CloseHandle(h);
 }
 VirtualFree(data,0,MEM_RELEASE);return 0;
}
'''
    compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
    with tempfile.TemporaryDirectory(prefix='mercs-save-io-') as tmp:
        cpp=Path(tmp)/'test.c';exe=Path(tmp)/'test.exe'
        cpp.write_text(source)
        subprocess.run([compiler,'-std=c11','-I',str(ROOT),'-I',str(ROOT/'src'),str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe),str(Path(tmp)/'scratch.bin')],check=True)


if __name__=='__main__':main()

"""Deliberate controller activity, held input, drift and slow movement."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
source=r"""
#include <assert.h>
#include <stdio.h>
#include "prompt_activity.h"
int main(void) {
 PromptPadActivity s={0};uint8_t b[8]={0};int16_t a[4]={0};
 assert(!prompt_pad_activity(&s,0,b,a));
 for(int i=0;i<100;++i){a[0]=(i%2?4000:-4000);assert(!prompt_pad_activity(&s,0,b,a));}
 a[0]=16000;assert(prompt_pad_activity(&s,0,b,a));
 for(int i=0;i<100;++i)assert(!prompt_pad_activity(&s,0,b,a));
 a[0]=16300;assert(!prompt_pad_activity(&s,0,b,a));
 a[0]=18000;assert(prompt_pad_activity(&s,0,b,a));
 a[0]=0;assert(!prompt_pad_activity(&s,0,b,a));
 assert(prompt_pad_activity(&s,1,b,a));assert(!prompt_pad_activity(&s,1,b,a));
 assert(!prompt_pad_activity(&s,0,b,a));
 b[7]=2;assert(!prompt_pad_activity(&s,0,b,a));
 b[7]=128;assert(prompt_pad_activity(&s,0,b,a));assert(!prompt_pad_activity(&s,0,b,a));
 b[7]=0;assert(!prompt_pad_activity(&s,0,b,a));
 b[3]=255;assert(prompt_pad_activity(&s,0,b,a));
 puts("prompt activity: drift, holds, releases, sticks, triggers and button edges pass");
}
"""
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.c').write_text(source)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-I'+str(root/'src/input'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)

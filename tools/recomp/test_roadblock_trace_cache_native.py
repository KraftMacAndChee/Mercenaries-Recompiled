"""Execute production render trace gates with absent/empty/present options."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RoadblockTraceCacheTests(unittest.TestCase):
    def test_native_gates_filters_caps_and_independent_abi_reports(self):
        source = (ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
        body = source[source.index('static uint32_t roadblock_trace_model;'):
                      source.index('void recomp_player_update_lifetime_checkpoint(')]
        self.assertEqual(body.count('getenv('), 1)
        self.assertEqual(body.count('!recomp_roadblock_model_trace_enabled()'), 3)
        harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static unsigned env_calls,reads,models,queues,primitives,abi,flushes;
static const char *option;
static uint32_t g_mercenaries_roadblock_model_draw_active;
static char *fake_getenv(const char *name){
 assert(!strcmp(name,"MERCENARIES_TRACE_ROADBLOCK_MODEL"));
 ++env_calls;return (char*)option;
}
static uint32_t guest_u32(uint32_t address){
 ++reads;
 if(address==0x10080)return 0x20000;
 if(address==0x30060)return 0x20000;
 return 0;
}
static uint16_t guest_u16(uint32_t address){
 ++reads;return address==0x10078?2:0;
}
static uint8_t guest_u8(uint32_t address){++reads;return 0;}
static int fake_fprintf(FILE *stream,const char *format,...){
 assert(stream==stderr);
 if(strstr(format,"[RENDER-QUEUE-ABI]"))++abi;
 else if(strstr(format,"[ROADBLOCK-MODEL]"))++models;
 else if(strstr(format,"[ROADBLOCK-QUEUE]"))++queues;
 else if(strstr(format,"[ROADBLOCK-PRIMITIVE]"))++primitives;
 else assert(0);
 return 0;
}
static int fake_fflush(FILE *stream){assert(stream==stderr);++flushes;return 0;}
#define getenv fake_getenv
#define fprintf fake_fprintf
#define fflush fake_fflush
'''+body+r'''
static void queue(uint32_t stage,uint32_t item,int mismatch){
 recomp_redmodel_queue_checkpoint(stage,0,1,item,0,0,1,1,2,2,3,mismatch?4:3);
}
int main(int argc,char **argv){
 assert(argc==2);int mode=atoi(argv[1]);
 option=mode==0?NULL:mode==1?"":mode==2?"1":mode==3?"0xDB4DF610":"0xNOTHEX";
 int active=mode>0&&mode<4;
 /* Nonmatching model must not activate downstream tracing. */
 recomp_redmodel_render_checkpoint(1,0,0,0x10000);
 queue(0,0x30000,0);recomp_redprimitive_draw_checkpoint(0,0x20000);
 assert(!reads&&!models&&!queues&&!primitives);
 assert(!g_mercenaries_roadblock_model_draw_active);
 /* ABI failures remain observable even with the diagnostic disabled. */
 for(unsigned i=0;i<20;i++)queue(1,0x30000,1);
 assert(abi==16&&!reads);
 if(mode==3){
  recomp_redmodel_render_checkpoint(1,0,0x89D23BAE,0x10000);
  assert(!models);
 }
 recomp_redmodel_render_checkpoint(1,0,mode==3?0xDB4DF610:0x89D23BAE,0x10000);
 unsigned before=reads;
 queue(0,0,0);recomp_redprimitive_draw_checkpoint(0,0);
 recomp_redmodel_render_checkpoint(2,0x10004,0,0);
 assert(reads==before);
 for(unsigned i=0;i<100000;i++){
  recomp_redmodel_render_checkpoint(6,0x10000,0x20000,0);
  queue(0,0x30000,0);
  recomp_redprimitive_draw_checkpoint(0,0x20000);
 }
 assert(env_calls==1);
 if(!active){
  assert(!reads&&!models&&!queues&&!primitives&&!flushes);
  assert(!g_mercenaries_roadblock_model_draw_active);
 }else{
  assert(models==1024&&queues==512&&primitives==1024);
  assert(reads>0&&flushes==2560);
  assert(g_mercenaries_roadblock_model_draw_active==1);
 }
 /* This is explicitly a launch option, not a live controller command. */
 option=mode?NULL:"1";
 assert(recomp_roadblock_model_trace_enabled()==active);
 assert(env_calls==1);
 printf("mode=%d: 300000 hot gates, one lookup, filters/caps/ABI passed\n",mode);
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-roadblock-cache-') as directory:
            path=Path(directory); c=path/'test.c'; exe=path/'test.exe'
            c.write_text(harness,encoding='utf-8')
            subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe',
                            '-O2','-std=c11',str(c),'-o',str(exe)],check=True)
            for mode in range(5):
                subprocess.run([str(exe),str(mode)],check=True)


if __name__=='__main__':unittest.main()

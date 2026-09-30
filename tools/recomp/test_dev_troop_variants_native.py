"""Check isolated Combat role dependencies and spawn properties without mission loads."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[2]
fixture=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dev_menu.h"
static uint32_t hash(const char *s){uint32_t h=2166136261u;for(;*s;++s)h=(h^((unsigned char)*s|32u))*16777619u;return h;}
#define dev_hash hash
static uint32_t loaded[16],queued[16],missing;static unsigned count,requests,loads,properties;
static int recomp_lookup(uint32_t va){return 1;}
static uint32_t dev_call(uint32_t stack,uint32_t va,uint32_t object,unsigned argc,const uint32_t *args){
 if(va==0x178020){for(unsigned i=0;i<count;i++)if(loaded[i]==args[0])return 1;return 0;}
 if(va==0x1785D0){assert(argc==2&&args[1]==0xB08B665Au);assert(requests<16);queued[requests++]=args[0];return 0;}
 assert(va==0x17F490&&argc==0);for(unsigned i=0;i<requests;i++)if(queued[i]!=missing)loaded[count++]=queued[i];loads+=requests;requests=0;return 0;
}
static uint32_t dev_resident_model(uint32_t h){for(unsigned i=0;i<count;i++)if(loaded[i]==h)return 1;return 0;}
static char keys[16][64],values[16][96];
static void dev_property(uint32_t stack,uint32_t scratch,uint32_t list,const char *k,const char *v){assert(properties<16);strcpy(keys[properties],k);strcpy(values[properties++],v);}
static const char *property(const char *key){for(unsigned i=0;i<properties;i++)if(!strcmp(keys[i],key))return values[i];return "";}
#include "dev_troop_variants.h"
int main(void){
 DevVehicle soldier={0};soldier.template_hash=hash("template_mafia_soldier");soldier.model_hash=hash("mafia_hum_soldier");assert(!dev_troop_variant(&soldier));
 for(unsigned i=0;i<4;i++){
  const DevTroopVariant *v=&dev_troop_variants[i];soldier.template_hash=hash(v->base_template);soldier.model_hash=hash(v->model);assert(dev_troop_variant(&soldier)==v);
  count=requests=loads=properties=0;assert(dev_prepare_troop_variant(0,v));unsigned first=loads;assert(first==1);
  assert(dev_prepare_troop_variant(0,v)&&loads==first);
  dev_apply_troop_variant(0,0,0,v);assert(!strcmp(property("geometryfile"),v->model));assert(!strcmp(property("hitPoints"),v->hit_points));
  assert(!strcmp(property("weapon_A_template"),v->weapon));
  assert(!strcmp(property("secondaryWeapon_A_template"),v->secondary));
  assert(!strcmp(property("ai_accuracy"),v->accuracy));
  count=requests=loads=0;missing=hash(v->model);assert(!dev_prepare_troop_variant(0,v));missing=0;
 }
 soldier.template_hash=hash("template_allies_soldier");assert(!dev_troop_variant(&soldier));
 puts("PASS: Combat role identity, isolated model loading, cached reuse, missing dependency rejection, original weapons/HP/accuracy");
}
'''
with tempfile.TemporaryDirectory(prefix='merc-mafia-roles-') as d:
 p=Path(d);(p/'test.c').write_text(fixture)
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O1','-I',str(ROOT/'ports/mercenaries/src'),str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)

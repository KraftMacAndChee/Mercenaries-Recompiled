"""Retail-only catalogue reproducibility and production placement decisions."""
from pathlib import Path
import json
import math
import re
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.extract_dev_vehicle_catalog import extract
from tools.diagnostics.inspect_retail_model_bounds import model_records


def test_catalogue_regenerates_and_encloses_retail_boxes():
    catalogue,evidence=extract(ROOT/'game_files/mercenaries-retail/DATAxbox')
    assert catalogue==json.loads((ROOT/'ports/mercenaries/data/dev-vehicles.json').read_text())
    assert evidence==json.loads((ROOT/'ports/mercenaries/data/dev-vehicles-retail-evidence.json').read_text())
    rows=catalogue['vehicles'];assert len(rows)==101
    assert len({r['template'].lower() for r in rows})==101
    models=model_records(ROOT/'game_files/mercenaries-retail/DATAxbox',{r['model_hash'] for r in rows})
    assert len(models)==65
    for row,record in zip(rows,evidence['records']):
        scale=max(1.05,record['geometry_scale'])
        assert all(math.isfinite(row[k]) for k in ('radius','bottom','height','half_width','half_length'))
        for box in models[row['model_hash']]['boxes']:
            for x in (box[0],box[3]):
                for y in (box[1],box[4]):
                    for z in (box[2],box[5]):
                        assert abs(x*scale)<=row['half_width']+0.0001
                        assert abs(z*scale)<=row['half_length']+0.0001
                        assert row['bottom']-0.0001<=y*scale<=row['bottom']+row['height']+0.0001
                        assert math.sqrt(x*x+y*y+z*z)*scale<=row['radius']+0.0001


def test_every_vehicle_uses_production_placement_and_rejection():
    rows=json.loads((ROOT/'ports/mercenaries/data/dev-vehicles.json').read_text())['vehicles']
    fixture=(ROOT/'tools/recomp/fixtures/dev_spawn.c').read_text().split('int main(void){')[0]
    entries=[]
    for row in rows:
        strings=[json.dumps(row[k]) for k in ('name','template','faction','kind')]
        values=[str(row[k]) for k in ('template_hash','model_hash','radius','bottom','height','half_width','half_length')]
        entries.append('{'+','.join(strings+values+[json.dumps(row['source'].removesuffix('.lyr'))])+'}')
    fixture=re.sub(r'static const DevVehicle catalog\[\]=\{.*?\n\};',
                   'static const DevVehicle catalog[]={\n'+',\n'.join(entries)+'\n};',fixture,flags=re.S)
    fixture=fixture.replace('return 2;','return 101;').replace('return i<2?catalog+i:NULL;','return i<101?catalog+i:NULL;')
    fixture=fixture.replace('static int pending,','static uint32_t active_model_hash;\nstatic int pending,')
    fixture=fixture.replace('key==0x134603d7?200:100','key==0x134603d7?active_model_hash:100')
    fixture+=r"""
int main(void){
 for(unsigned i=0;i<101;i++){
  const DevVehicle *v=catalog+i;
  for(unsigned scenario=0;scenario<4;scenario++){
   reset();pending=i;active_model_hash=v->model_hash;put(0x230000,active_model_hash);
   if(scenario==1){float minus=-1,zero=0;memcpy(guest_ptr(0x220030),&minus,4);memcpy(guest_ptr(0x220038),&zero,4);}
   if(scenario==2)obstruction=1;
   if(scenario==3)uneven=1;
   run();
   if(scenario>=2){assert(!success&&!spawns);continue;}
   assert(success&&spawns==1&&ray_count==10);
   assert(fabsf(spawn_position[1]-(-v->bottom+.25f))<.0001f);
   assert(spawn_position[1]+v->bottom>.249f);
   if(scenario==0){assert(spawn_position[0]==10);assert(fabsf(spawn_position[2]-(20+v->radius+3.5f))<.0001f);}
   else {assert(spawn_position[2]==20);assert(fabsf(spawn_position[0]-(10+v->radius+3.5f))<.0001f);}
  }
 }
 puts("PASS: all 101 vehicles, two orientations, ground clearance, wall/uneven-ground rejection, CPU preservation (404 cases)");
}
"""
    compiler=shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe'
    with tempfile.TemporaryDirectory(prefix='retail-vehicle-placement-') as td:
        folder=Path(td);source=folder/'test.c';exe=folder/'test.exe';source.write_text(fixture)
        subprocess.run([compiler,'-std=c11','-O2','-I',str(ROOT/'ports/mercenaries/src'),
                        '-I',str(ROOT/'src/input'),'-I',str(ROOT/'src'),str(source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

"""Regenerate independently verified route points from retail navigation.

Retail road/anchor points and explicitly project-selected detours are separate
inputs. Historical comments remain untouched; project detours are not described
as original retail path nodes.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.retail_navigation import navigation_paths,world_positions

ARRAY=re.compile(r'const float (\w+)\[\]\[2\] = \{(.*?)\n    \};',re.S)
POINT=re.compile(r'\{([\d.-]+)f,\s*([\d.-]+)f\}')


def regenerate(assets,source,manifest):
    if hashlib.sha256(Path(assets).read_bytes()).hexdigest()!=manifest['retail_assets_sha256']:
        raise ValueError('Unsupported retail navigation input')
    paths={}
    for path in navigation_paths(assets):
        key=(path['record'],path['name'])
        if key in paths:raise ValueError('Duplicate path identity')
        paths[key]=path['points']
    wanted={}
    for entry in manifest['selectors']:
        key=(entry['array'],entry['index'])
        if key in wanted:raise ValueError('Duplicate route selector')
        x,y,z=paths[(entry['record'],entry['path'])][entry['node']]
        wanted[key]='{'+f'{x:.3f}f, {z:.3f}f'+'}'
    world_selectors=manifest.get('world_selectors',[])
    axis_selectors=manifest.get('axis_selectors',[])
    world_names={e['world'] for e in world_selectors}
    world_names.update(e[axis]['world'] for e in axis_selectors for axis in ('x','z') if e[axis]['kind']=='world')
    worlds={(p['record'],p['world'],p['instance']):p
            for p in world_positions(assets,world_names)}
    for entry in world_selectors:
        key=(entry['array'],entry['index'])
        if key in wanted:raise ValueError('Duplicate world route selector')
        anchor=worlds[(entry['record'],entry['world'],entry['instance'])]
        if anchor['name']!=entry['name'] or anchor['model']!=entry['model']:
            raise ValueError('World anchor identity mismatch')
        x,y,z=anchor['position']
        wanted[key]='{'+f'{x:.3f}f, {z:.3f}f'+'}'
    for entry in axis_selectors:
        key=(entry['array'],entry['index'])
        if key in wanted:raise ValueError('Duplicate mixed-axis route selector')
        values=[]
        for axis,coordinate in (('x',0),('z',2)):
            anchor=entry[axis]
            if anchor['kind']=='navigation':
                point=paths[(anchor['record'],anchor['path'])][anchor['node']]
            elif anchor['kind']=='world':
                obj=worlds[(anchor['record'],anchor['world'],anchor['instance'])]
                if obj['name']!=anchor['name'] or obj['model']!=anchor['model']:
                    raise ValueError('Mixed-axis world identity mismatch')
                point=obj['position']
            else:raise ValueError('Unsupported mixed-axis anchor')
            values.append(point[coordinate])
        wanted[key]='{'+f'{values[0]:.3f}f, {values[1]:.3f}f'+'}'
    for entry in manifest.get('project_policy_selectors',[]):
        key=(entry['array'],entry['index'])
        if key in wanted:raise ValueError('Duplicate project policy selector')
        x,z=entry['position']
        if not (-10000<x<10000 and -10000<z<10000):raise ValueError('Invalid project destination')
        wanted[key]='{'+f'{x:.3f}f, {z:.3f}f'+'}'
    start=source.index('static int recomp_apply_test_post_aircraft_gate_route(')
    end=source.index('static void recomp_input_ensure_initialized',start)
    region=source[start:end];seen=set()
    def array(match):
        name,body=match.group(1,2);index=0
        def point(item):
            nonlocal index
            key=(name,index);index+=1
            if key in wanted:seen.add(key);return wanted[key]
            return item.group()
        replaced=POINT.sub(point,body)
        return match.group()[:match.start(2)-match.start()]+replaced+match.group()[match.end(2)-match.start():]
    region=ARRAY.sub(array,region)
    if seen!=set(wanted):raise ValueError('Missing route array/index')
    return source[:start]+region+source[end:]


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('assets',type=Path)
    p.add_argument('--source',type=Path,default=ROOT/'ports/mercenaries/src/recomp_manual.c')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--selectors',type=Path,default=ROOT/'ports/mercenaries/data/diagnostic-route-retail-selectors.json')
    a=p.parse_args();manifest=json.loads(a.selectors.read_text())
    a.output.write_text(regenerate(a.assets,a.source.read_text(),manifest))
    print(f'Regenerated {len(manifest['selectors'])+len(manifest.get('world_selectors',[]))+len(manifest.get('axis_selectors',[]))+len(manifest.get('project_policy_selectors',[]))} points; {len(manifest["remaining_review"])} points left unchanged for separate review')

if __name__=='__main__':main()

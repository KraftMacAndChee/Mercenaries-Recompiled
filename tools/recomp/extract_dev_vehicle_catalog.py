"""Regenerate developer vehicles solely from user-owned retail DATAxbox files.

This replaces the earlier editor-data generator. Historical provenance remains
in the audit; current bounds use a deliberately conservative retail-box policy.
No existing catalogue, private editor files, or authoring meshes are read.
"""
from pathlib import Path
import argparse
import hashlib
import json
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.diagnostics.inspect_retail_templates import vehicle_metadata,template_instances
from tools.diagnostics.inspect_retail_model_bounds import model_records,conservative_footprint


def sha(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def extract(data):
    data=Path(data)
    if sha(data/'assets.dsk')!='8b8ee254a245490ea4cda54ccea05b7a7a51afc41125aaf3cf5fd8ec512d1610':
        raise ValueError('Unsupported retail template archive')
    rows=vehicle_metadata(data)
    definitions={r['template'].lower():r for r in template_instances(data/'assets.dsk')}
    models=model_records(data,{r['model_hash'] for r in rows})
    records=[]
    for row in rows:
        template=definitions[row['template'].lower()];model=models[row['model_hash']]
        scale=float(template['properties'].get('geometryscale','1'))
        row.update({k:round(v,4) for k,v in conservative_footprint(model['boxes'],scale).items()})
        records.append({'template':row['template'],'world_record':template['record_index'],
                        'layer':template['layer'],'model_archive':model['archive'],
                        'model_record':model['record_index'],'model_info_sha256':hashlib.sha256(model['info']).hexdigest(),
                        'geometry_scale':scale})
    catalogue={'provenance':'Regenerated from supported retail template/localization/model records by extract_dev_vehicle_catalog.py. Footprints enclose both compiled INFO boxes with project placement padding; these replace the previous editor-mesh bounds. See retail-model-bounds-evidence.md.',
               'vehicles':rows}
    evidence={'schema':1,'policy':'retail-info-union-v1',
              'inputs':{name:sha(data/name) for name in ('assets.dsk','english.dsk','locked.dsk','streamed.dsk')},
              'records':records}
    return catalogue,evidence


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('data',type=Path,help='Supported retail DATAxbox directory')
    p.add_argument('--output',type=Path,default=ROOT/'ports/mercenaries/data/dev-vehicles.json')
    p.add_argument('--evidence',type=Path,default=ROOT/'ports/mercenaries/data/dev-vehicles-retail-evidence.json')
    a=p.parse_args();catalogue,evidence=extract(a.data)
    for path,data in ((a.output,catalogue),(a.evidence,evidence)):
        path.write_text(json.dumps(data,indent=2)+'\n',encoding='utf-8')
    print(f'Regenerated {len(catalogue["vehicles"])} vehicle entries from retail inputs')

if __name__=='__main__':main()

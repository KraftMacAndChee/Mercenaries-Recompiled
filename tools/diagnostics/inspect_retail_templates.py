"""Inspect template properties and localization directly in a retail DSK.

Container contracts were observed in assets.dsk/english.dsk. This parser reads
retail containers directly. It deliberately does not infer model bounds.
"""
from pathlib import Path
import argparse
import json
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.diagnostics.inspect_retail_script import chunks


def dsk_records(path):
    with Path(path).open('rb') as stream:
        total=Path(path).stat().st_size
        header=stream.read(8)
        if len(header)!=8:raise ValueError('Truncated DSK header')
        count,_=struct.unpack('<II',header)
        if count>(total-8)//12:raise ValueError('Invalid directory length')
        directory=stream.read(count*12)
        offset=8+count*12
        for index in range(count):
            size,key,kind=struct.unpack_from('<III',directory,index*12)
            if offset+size>total:raise ValueError('Record exceeds DSK')
            stream.seek(offset);payload=stream.read(size)
            yield index,key,kind,payload
            offset+=size
        if total-offset not in (0,2048):raise ValueError('Unexpected DSK trailer length')
        stream.seek(offset)
        if any(stream.read()):raise ValueError('Nonzero DSK trailer')


def hash_string(value):
    if not value:return 0
    if "\0" in value:raise ValueError("Embedded NUL in an asset identifier")
    result=2166136261
    for c in value.encode('ascii'):
        result=((result^(c|32))*16777619)&0xffffffff
    return result


def string_table(payload):
    if len(payload)<4:raise ValueError('Truncated string table')
    count=struct.unpack_from('<I',payload)[0];offset=4;result={}
    if count>(len(payload)-4)//9:raise ValueError('Invalid string table count')
    for _ in range(count):
        if offset+8>len(payload):raise ValueError('Truncated table entry')
        key,size=struct.unpack_from('<II',payload,offset);offset+=8
        if offset+size>=len(payload) or payload[offset+size]!=0:raise ValueError('Invalid table string')
        value=payload[offset:offset+size].decode('ascii');offset+=size+1
        if hash_string(value)!=key:raise ValueError('String hash mismatch')
        if key in result and result[key]!=value:raise ValueError('Ambiguous string hash')
        result[key]=value
    if offset!=len(payload):raise ValueError('Trailing string table data')
    return result


def template_instances(archive,prefix='template_vehicles',geometry_models=None):
    for index,key,kind,payload in dsk_records(archive):
        # The observed world-kind directory value rejects unrelated binary records
        # before interpreting their payload as nested chunks.
        if kind!=0x37A3E893:continue
        root=dict(chunks(payload))
        for tag,world in chunks(root[b'ucfb']):
            if tag!=b'wrld':continue
            fields=list(chunks(world));single=dict(fields)
            name=single[b'NAME'].rstrip(b'\0').decode('ascii')
            if not name.startswith(prefix):continue
            if hash_string(name)!=key:raise ValueError('World key/name disagreement')
            table=string_table(single[b'TABL'])
            for tag,record in fields:
                if tag!=b'inst':continue
                parts=dict(chunks(record));data=parts[b'PROP']
                if len(data)<8 or len(data)%8:raise ValueError('Invalid property record length')
                words=struct.unpack('<'+'I'*(len(data)//4),data)
                if geometry_models is not None:
                    geometries={table[words[j+1]].lower() for j in range(2,len(words),2)
                                if table[words[j]].lower()=='geometryfile'}
                    if not geometries.intersection(geometry_models):continue
                properties={}
                for j in range(2,len(words),2):
                    field=table[words[j]].lower();value=table[words[j+1]]
                    if field in properties and properties[field]!=value:raise ValueError(f'Conflicting property {field}: {properties[field]!r} / {value!r}')
                    properties[field]=value
                yield {'layer':name,'record_index':index,'template':table[words[1]] if words[1] else '',
                       'template_hash':words[1],'model':table[words[0]],'model_hash':words[0],
                       'properties':properties}


def localization(archive):
    result={}
    def walk(data,prefix):
        for tag,child in chunks(data):
            if tag not in (b'SCPE',b'BIN_'):continue
            fields=dict(chunks(child));info=fields[b'INFO'].split(b'\0')
            if len(info)<3 or info[0]!=b'name':raise ValueError('Unknown localization name record')
            name=info[1].decode('ascii');key=prefix+name
            if not prefix and name.lower()!='veh':continue
            if tag==b'SCPE':walk(fields[b'DATA'],key+'.')
            else:
                value=fields[b'DATA'].decode('utf-16-le').rstrip('\0')
                if key.lower() in result and result[key.lower()]!=value:raise ValueError('Conflicting localization')
                result[key.lower()]=value
    for _,_,kind,payload in dsk_records(archive):
        if kind!=0x3884598E:continue
        root=dict(chunks(payload))
        for tag,child in chunks(root[b'ucfb']):
            if tag==b'reg_':walk(child,'')
    return result


def vehicle_metadata(data):
    data=Path(data);labels=localization(data/'english.dsk');rows={}
    for entry in template_instances(data/'assets.dsk'):
        props=entry['properties'];model=props.get('geometryfile',entry['model'])
        if not any(value.lower()=='driver' for key,value in props.items() if key.startswith('ridertype')):continue
        if props.get('objecttype','').lower() not in ('jeep','truck','trucklarge','tank','helicopter','boat') or '_ruin' in entry['model'].lower():continue
        label=props.get('displayname','').lower()
        if label and label not in labels:raise ValueError(f'Missing vehicle display name: {label}')
        row={'template':entry['template'],'template_hash':entry['template_hash'],
             'model':model,'model_hash':hash_string(model),'name':labels[label] if label else entry['model'],
             'faction':props.get('faction','unknown').lower(),'kind':props['objecttype'],
             'source':entry['layer']+'.lyr',
             'disabled_debug_spawn':props.get('disabledebugmenuspawn','FALSE').upper()=='TRUE'}
        # The retail chapter layers occur after the base layer. Later variants
        # of the same template select that chapter's asset list.
        rows[row['template'].lower()]=row
    return sorted(rows.values(),key=lambda row:(row['name'].lower(),row['template']))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data',type=Path,help='Retail DATAxbox directory')
    parser.add_argument('--output',type=Path,required=True,help='Inspection output; omits unverified footprint data')
    args=parser.parse_args()
    rows=vehicle_metadata(args.data)
    result={'basis':'Retail template records and English localization; no footprint reconstruction claimed.','vehicles':rows}
    args.output.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(f'Extracted {len(rows)} retail vehicle metadata records')
if __name__=='__main__':main()

"""Read retail navigation points; no editor PTH input is used."""
from pathlib import Path
import math
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.diagnostics.inspect_retail_templates import dsk_records,string_table,hash_string
from tools.diagnostics.inspect_retail_script import chunks


def navigation_paths(archive):
    for record,key,kind,payload in dsk_records(archive):
        if kind!=0x84874D36:continue
        for tag,body in chunks(dict(chunks(payload))[b'ucfb']):
            if tag!=b'path':raise ValueError('Unexpected navigation tag')
            fields=dict(chunks(body));info=fields[b'INFO']
            if len(info)!=6:raise ValueError('Invalid navigation INFO size')
            count,mode,flags=struct.unpack('<HHH',info)
            points=fields[b'PNTS']
            if len(points)!=count*12:raise ValueError('Navigation point count mismatch')
            values=list(struct.iter_unpack('<3f',points))
            if any(not all(math.isfinite(v) for v in point) for point in values):raise ValueError('Nonfinite point')
            if b'ORNT' in fields and len(fields[b'ORNT'])!=count*16:raise ValueError('Orientation count mismatch')
            name=fields[b'NAME']
            if not name.endswith(b'\0') or b'\0' in name[:-1]:raise ValueError('Invalid path name')
            yield {'record':record,'record_key':key,'name':name[:-1].decode('ascii'),
                   'mode':mode,'flags':flags,'points':values}


def world_positions(archive, world_names):
    """Read only transforms and identifiers needed for diagnostic route anchors."""
    for record, key, kind, payload in dsk_records(archive):
        if kind != 0x37A3E893:
            continue
        for tag, world in chunks(dict(chunks(payload))[b'ucfb']):
            if tag != b'wrld':
                continue
            fields = list(chunks(world)); single = dict(fields)
            name = single[b'NAME'].rstrip(b'\0').decode('ascii')
            if name not in world_names:
                continue
            if hash_string(name) != key:
                raise ValueError('World directory/name mismatch')
            table = string_table(single[b'TABL']); ordinal = 0
            for tag, data in fields:
                if tag != b'inst':
                    continue
                parts = dict(chunks(data))
                model, obj = struct.unpack_from('<II', parts[b'PROP'])
                transform = parts[b'XFRM']
                if len(transform) != 48:
                    raise ValueError('Unexpected world transform size')
                values = struct.unpack('<12f', transform)
                if not all(math.isfinite(value) for value in values):
                    raise ValueError('Nonfinite world transform')
                yield {'record': record, 'world': name, 'instance': ordinal,
                       'name': table[obj] if obj else '', 'model': table[model],
                       'position': values[-3:]}
                ordinal += 1

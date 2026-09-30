"""Read compiled retail model bounds for independent placement analysis.

The two INFO boxes are read by retail VA 1F5DD5..1F62C3 into model+38 and
model+60. This reader does not use MSH/editor bounds or the existing catalogue.
"""
from pathlib import Path
import math
import struct
from tools.diagnostics.inspect_retail_templates import dsk_records,hash_string
from tools.diagnostics.inspect_retail_script import chunks


def decode_info(info):
    if len(info)<77:raise ValueError('Truncated model INFO')
    values=struct.unpack_from('<12f',info,8)
    if not all(math.isfinite(x) for x in values):raise ValueError('Nonfinite model bounds')
    boxes=[values[:6],values[6:]]
    if any(any(box[i]>box[i+3] for i in range(3)) for box in boxes):
        raise ValueError('Inverted model bounds')
    return boxes


def model_records(data,model_keys):
    result={}
    for archive_name in ('locked.dsk','streamed.dsk'):
        archive=Path(data)/archive_name
        for index,key,kind,payload in dsk_records(archive):
            if kind!=0xB08B665A or key not in model_keys:continue
            root=dict(chunks(payload));model=dict(chunks(root[b'ucfb']))[b'modl']
            fields=dict(chunks(model));name=fields[b'NAME'].rstrip(b'\0').decode('ascii')
            if hash_string(name)!=key:raise ValueError('Model directory/name disagreement')
            info=fields[b'INFO'];boxes=decode_info(info)
            record={'name':name,'info':info,'boxes':boxes,'archive':archive_name,'record_index':index}
            if key in result:
                if result[key]['info']!=info:raise ValueError('Conflicting retail model INFO')
            else:result[key]=record
    missing=set(model_keys)-set(result)
    if missing:raise ValueError('Missing compiled model bounds: '+','.join(f'{key:08X}' for key in sorted(missing)))
    return result


def conservative_footprint(boxes,scale=1.0):
    """Project placement policy: enclose both retail boxes with 5% minimum margin.

    This is project placement policy, not a claim of reconstructing old
    authoring-file bounds. The runtime samples terrain across this envelope.
    """
    if not math.isfinite(scale) or scale<=0:raise ValueError('Invalid model scale')
    scale=max(1.05,scale)
    low=[min(box[i] for box in boxes) for i in range(3)]
    high=[max(box[i+3] for box in boxes) for i in range(3)]
    center=[(a+b)*0.5 for a,b in zip(low,high)]
    extent=[(b-a)*0.5 for a,b in zip(low,high)]
    return {'radius':(math.sqrt(sum(x*x for x in center))+math.sqrt(sum(x*x for x in extent)))*scale,
            'bottom':low[1]*scale,'height':(high[1]-low[1])*scale,
            'half_width':max(abs(low[0]),abs(high[0]))*scale,
            'half_length':max(abs(low[2]),abs(high[2]))*scale}

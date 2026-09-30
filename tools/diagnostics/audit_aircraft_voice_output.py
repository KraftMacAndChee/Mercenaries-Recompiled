"""Compare fresh aircraft cue/track metadata with the actual retail output.

This is a read-only diagnostic for the observed version3 vo.xwb layout. It
rejects mismatching metadata and does not use stale pointers after trace caps.
Unknown/missing mappings are reported, not treated as silent or successful.
"""
import argparse
from bisect import bisect_right
import json
import math
from pathlib import Path
import re
import struct

import numpy as np

from correlate_retail_voice import decode_adpcm,best_match
from inspect_retail_script import read_script
from inspect_xact_early_retirement import name_hash


def cue_metadata(lines,names):
    # APU summaries are normally one second apart. Bracket the cue instead of
    # assuming its most recent epoch is within half a second of playback.
    # The old fixed window falsely reported missing voices when the next
    # summary had not yet been logged.
    lines=list(lines)
    clocks=[]
    for number,line in enumerate(lines,1):
        match=re.search(r'\bep=(\d+)\b',line) if '[APU-' in line else None
        if match:
            clocks.append((number,int(match.group(1))))
    clock_lines=[number for number,_ in clocks]
    owners={}
    ep=None
    for number,line in enumerate(lines,1):
        fields=dict(re.findall(r"([\w-]+)=([^\s]+)",line))
        if "ep" in fields and "[APU-" in line:
            ep=int(fields["ep"])
        if "[XACT-SOUND-UPDATE]" in line and "owner" in fields:
            owners[fields["owner"]]=(number,fields)
        if "[XACT-CUE] stage=2 " not in line or "hash" not in fields:
            continue
        key=int(fields["hash"],16)
        if key not in names:
            continue
        row={"name":names[key],"line":number,"epoch":ep,
             "result":fields["result"],"authored_seconds":float(fields["length"])}
        next_clock=bisect_right(clock_lines,number)
        if next_clock<len(clocks):
            row['epoch_after']=clocks[next_clock][1]
        prior=owners.get(fields["pCue"])
        if ep is None or not prior or number-prior[0]>32:
            row["unresolved"]="No fresh track metadata; trace may be capped"
        elif int(fields["result"],16)&0x80000000:
            row["unresolved"]="Cue request failed"
        else:
            track=prior[1]
            row.update(wave=int(track["timing"].split("/")[0],16)>>16,
                       byte_length=int(track["tail"].split("/")[0],16),
                       format=int(track["timing"].split("/")[-1],16))
        yield row


def search_bounds(row,reference_samples,total_samples):
    """Bound waveform alignment to fresh surrounding APU clock observations."""
    before,after=row.get('epoch'),row.get('epoch_after')
    if (not isinstance(before,int) or not isinstance(after,int) or
            before<0 or not before<=after<=before+3000):
        raise ValueError('Missing, reversed or stale APU clock bracket')
    if not 1<reference_samples<=60*48000 or total_samples<0:
        raise ValueError('Invalid bounded waveform length')
    first=max(0,before*32-24000)
    last=min(total_samples,after*32+24000+reference_samples)
    if last-first<reference_samples:
        raise ValueError('Recording ends before the bracketed reference')
    return first,last


def wave_slice(bank,index,length,fmt):
    if len(bank)<80 or bank[:4]!=b"WBND" or struct.unpack_from('<I',bank,4)[0]!=3:
        raise ValueError("Requires Xbox version3 wavebank")
    header,header_size,meta,meta_size,_,_,data,data_size=struct.unpack_from('<8I',bank,8)
    if header_size<40 or header+header_size>len(bank) or data+data_size>len(bank):
        raise ValueError("Invalid wavebank ranges")
    count=struct.unpack_from('<I',bank,header+4)[0]
    stride=struct.unpack_from('<I',bank,header+24)[0]
    if stride!=24 or not 0<=index<count or count*stride>meta_size or meta+meta_size>len(bank):
        raise ValueError("Invalid metadata entry")
    _,entry_format,offset,size,_,_=struct.unpack_from('<6I',bank,meta+index*stride)
    if size!=length or entry_format!=fmt or fmt!=0xAC445 or size<=0 or offset+size>data_size:
        raise ValueError("Track metadata does not match verified mono22050 ADPCM entry")
    return bank[data+offset:data+offset+size]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log',type=Path)
    parser.add_argument('recording',type=Path)
    parser.add_argument('--archive',type=Path,required=True)
    parser.add_argument('--wavebank',type=Path,required=True)
    parser.add_argument('--pitch',type=int,required=True,help='Observed mono voice pitch, not game FPS')
    args=parser.parse_args()
    if not -32768<=args.pitch<=32767:
        parser.error('Invalid observed hardware pitch')
    script=read_script(args.archive,'c17_american')
    cue_names=re.findall(r"\{\s*'v',\s*[0-9.]+,\s*'([^']+)'",script)
    names={name_hash(name):name for name in cue_names}
    bank=args.wavebank.read_bytes()
    raw=np.memmap(args.recording,dtype='<i2',mode='r')
    if len(raw)%2:
        parser.error('Expected stereo signed16 PCM')
    raw=raw.reshape(-1,2)
    for row in cue_metadata(args.log.read_text(errors='replace').splitlines(),names):
        if 'unresolved' not in row:
            try:
                data=wave_slice(bank,row['wave'],row['byte_length'],row['format'])
                decoded=decode_adpcm(data,1)[:,0]
                step=2**(args.pitch/4096)
                count=math.ceil(len(decoded)/step)
                if count>60*48000:
                    raise ValueError('Reference exceeds bounded60second window')
                reference=np.interp(np.arange(count)*step,np.arange(len(decoded)),decoded)
                first,last=search_bounds(row,count,len(raw))
                audio=np.asarray(raw[first:last],dtype=np.float64).mean(axis=1)
                match=best_match(audio,reference)
                row.update(match)
                row['start_seconds']=(first+match['sample'])/48000
                row['reference_seconds']=count/48000
                row['local_matches']=[]
                start=first+match['sample']
                for offset in range(0,count-12000+1,12000):
                    ref=reference[offset:offset+12000]
                    rms=float(np.std(ref))
                    if rms<100:continue  # silence cannot identify waveform alignment
                    lower=start+offset-2400
                    if lower<0:continue
                    window=np.asarray(raw[lower:lower+16800],dtype=np.float64).mean(axis=1)
                    part=best_match(window,ref)
                    row['local_matches'].append({'seconds':offset/48000,
                        'offset_ms':(part['sample']-2400)/48,
                        'correlation':part['correlation'],'gain':part['fitted_gain']})
            except ValueError as error:
                row['unresolved']=str(error)
        print(json.dumps(row),flush=True)


if __name__=='__main__':main()

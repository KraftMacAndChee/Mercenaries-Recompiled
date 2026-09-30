"""Verify the existing vertex warmup microcode against user-owned retail files.

This proves byte correspondence, not the origin of input-layout or pixel-state
captures. Existing catalogue provenance is retained unchanged.
"""
from pathlib import Path
import hashlib
import json
import struct
import sys
ROOT=Path(__file__).resolve().parents[2]


def capture():
    retail=ROOT/'game_files/mercenaries-retail'
    xbe=retail/'default.xbe'
    assert hashlib.sha256(xbe.read_bytes()).hexdigest()=='aa08ea21d952ac35f49c02c7e2ed08aa25ad7535fbbbccc95636775f37be99d7'
    catalogue=json.loads((ROOT/'ports/mercenaries/data/shader-warmup.json').read_text())
    inputs=[(p,p.read_bytes()) for p in sorted((retail/'shaders').glob('*.xvu'))]
    inputs.append((xbe,xbe.read_bytes()))
    result={}
    for entry in catalogue['vertex_states']:
        code=struct.pack('<'+'I'*len(entry['microcode']),*entry['microcode'])
        matches=[]
        for path,data in inputs:
            at=data.find(code)
            if at<0:continue
            if path.suffix=='.xvu':
                assert at==4 and len(data)==len(code)+4,path
            matches.append({'path':path.relative_to(retail).as_posix(),
                            'offset':at,'bytes':len(code),
                            'file_sha256':hashlib.sha256(data).hexdigest(),
                            'microcode_sha256':hashlib.sha256(code).hexdigest()})
        assert matches, 'No retail byte match for '+entry['hash']
        assert entry['hash'] not in result,'Duplicate warmup identity'
        result[entry['hash']]=matches
    return result


def test_warmup_programs_match_retail():
    expected=json.loads((ROOT/'tools/recomp/fixtures/vertex-warmup-retail.json').read_text())
    assert capture()==expected['programs']


if __name__=='__main__':
    if sys.argv[1:]==['--capture']:
        print(json.dumps({'basis':'Exact bytes from user-owned retail shader files and supported XBE; no claim about pixel-state or declaration capture provenance.','programs':capture()},indent=2))
    else:
        test_warmup_programs_match_retail()
        print('PASS: all 38 warmup vertex programs match retail file bytes and recorded input hashes')

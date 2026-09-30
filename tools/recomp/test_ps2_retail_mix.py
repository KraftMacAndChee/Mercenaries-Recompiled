"""Regeneration and malformed-input checks for the retail PS2 mix parameters."""
from pathlib import Path
import json
import sys
import pytest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.extract_ps2_retail_mix import extract,parameters,header,pairs


def test_retail_regeneration():
    sounds,evidence=extract(ROOT/'game_files/mercenaries-ps2-reference/ASSETS.DSK')
    values=parameters(sounds)
    assert header(values)==(ROOT/'ports/mercenaries/src/ps2_retail_mix.h').read_text()
    recorded=json.loads((ROOT/'ports/mercenaries/data/ps2-retail-mix-evidence.json').read_text())
    assert recorded['retail_assets_sha256']==evidence['retail_assets_sha256']
    assert recorded['records']==evidence['records']
    assert recorded['parameters']==values
    # The independent disc records also establish layering and wave choices;
    # Xbox bank indices differ and remain handled by the compatibility code.
    def waves(label,index):
        return next(e['waves'] for e in sounds[label]['tracks'][index] if e['type']=='PLAY')
    assert len(waves('drag',0))==2
    assert waves('drag',1)[0]['name']=='w_dragun.wpn_rifle_AK47_fire_onesht'
    assert waves('drag',2)[0]['name']=='combat.wpn_cannon_20mm_fire'
    assert waves('drag',3)[0]['name']=='w_dragun.rifle_shot1'
    assert waves('auto',0)[0]['name']=='combat.wpn_cannon_20mm_fire_01b'
    assert waves('auto',1)[0]['name']=='combat.wpn_cannon_20mm_fire'
    assert not any(e['type']=='VEVT' for e in sounds['drag']['tracks'][3])
    assert 'loopcount' not in next(e for e in sounds['auto']['tracks'][0] if e['type']=='PLAY')


@pytest.mark.parametrize('data',[b'name',b'name\0',b'a\0b\0a\0c\0',b'\0value\0',b'name\0\xff\0'])
def test_bad_properties_are_rejected(data):
    with pytest.raises((ValueError,UnicodeDecodeError)):pairs(data)

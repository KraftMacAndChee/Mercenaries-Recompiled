"""Normal generated runtime must preserve retail helicopter junction admission."""
from pathlib import Path
import runpy
ROOT=Path(__file__).resolve().parents[2]
ns=runpy.run_path(str(ROOT/'ports/mercenaries/scripts/Patch-Generated.py'))
for p in (ROOT/'ports/mercenaries/src/recomp/gen').glob('*.c'):
 s=ns['remove_experimental_traffic_admission'](p.read_text(encoding='utf-8'))
 assert 'recomp_traffic_helicopter_transfer_allowed' not in s
assert 'recomp_traffic_helicopter_transfer_allowed' not in (ROOT/'ports/mercenaries/src/recomp_manual.c').read_text(encoding='utf-8')
print('PASS: no helicopter admission override remains')

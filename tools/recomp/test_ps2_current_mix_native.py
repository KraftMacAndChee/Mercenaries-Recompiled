"""Freeze the current PS2 mix without claiming independent provenance for it.

This is a behavior-preservation baseline. It is intentionally separate from the
independent retail-parameter checks in test_ps2_retail_mix.py.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.test_ps2_upgrades_native import SOURCE


def capture():
    resource=ROOT/'ports/mercenaries/resources/ps2_upgrades'
    assets={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(resource.glob('*.wav'))}
    assert len(assets)==4
    source=SOURCE[:SOURCE.index('int main(int argc,char **argv)')]
    marker='  unsigned count=upgrade_banks[kind].retail_cues;'
    assert marker in source
    source=source.replace(marker,r'''
  printf("MIX:%s:",upgrade_banks[kind].name);
  for(unsigned j=0;j<read32(copy+20);++j)printf("%02x",memory[copy+j]);
  puts("");
'''+marker)
    source+='\nint main(int argc,char **argv){assert(argc==2);g_xbox_mem_offset=(ptrdiff_t)memory;bank_conversion(argv[1]);return 0;}\n'
    env=os.environ.copy();compiler=Path('C:/msys64/mingw64/bin/gcc.exe')
    env['PATH']=str(compiler.parent)+os.pathsep+env['PATH']
    with tempfile.TemporaryDirectory(prefix='ps2-current-mix-') as folder:
        d=Path(folder);(d/'test.c').write_text(source)
        retail=ROOT/'game_files/mercenaries-retail/DATAxbox/SOUND/sfx/w'
        for name,kind in [('w_autoca','cannon'),('w_dragun','rifle')]:
            shutil.copyfile(retail/kind/(name+'.xsb'),d/(name+'.xsb'))
        includes=[ROOT/'ports/mercenaries/src',ROOT/'src/kernel',ROOT/'src/input',ROOT/'src',ROOT/'include']
        subprocess.run([str(compiler),'-O2','-std=c11',*['-I'+str(p) for p in includes],str(d/'test.c'),'-o',str(d/'test.exe')],check=True,env=env)
        result=subprocess.run([str(d/'test.exe'),str(d)],check=True,env=env,text=True,capture_output=True)
    banks={}
    for line in result.stdout.splitlines():
        if not line.startswith('MIX:'):continue
        _,name,value=line.split(':');data=bytes.fromhex(value)
        banks[name]={'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
    assert set(banks)=={'w_autoca','w_dragun'}
    return {'converted_banks':banks,'supplied_waves':assets}


def test_current_mix_is_unchanged():
    actual=capture()
    expected=json.loads((ROOT/'tools/recomp/fixtures/ps2-current-mix.json').read_text())
    assert actual==expected['baseline'],(
        'Current mixing changed. Preserve the existing bank output and supplied '
        'audio; do not refresh this baseline to accept a dependency-replacement change.',
        actual,expected['baseline'])


def main():
    if sys.argv[1:]==['--capture']:
        print(json.dumps(capture(),indent=2));return
    test_current_mix_is_unchanged()
    print('PASS: current PS2 converted banks and all supplied WAVs match their exact preservation baseline')

if __name__=='__main__':main()

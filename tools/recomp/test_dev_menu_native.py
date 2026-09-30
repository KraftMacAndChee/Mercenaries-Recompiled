"""Exercise and render the production F9 panel in an offscreen test window."""
from pathlib import Path
import argparse,json,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,help='Keep the vehicle and troop panel renders here')
args=p.parse_args()
with tempfile.TemporaryDirectory(prefix='merc-dev-menu-') as directory:
 work=Path(directory)
 for category,filename in [('vehicles','dev_vehicle_catalog.inc'),('troops','dev_troop_catalog.inc')]:
  records=json.loads((ROOT/f'ports/mercenaries/data/dev-{category}.json').read_text())[category]
  lines=[]
  for r in records:
   fields=[json.dumps(r[k]) for k in ('name','template','faction','kind')]
   fields += [str(r[k]) for k in ('template_hash','model_hash','radius','bottom','height','half_width','half_length')]
   fields += [json.dumps(r['source'][:-4])]
   lines.append('{'+','.join(fields)+'},')
  (work/filename).write_text('\n'.join(lines))
 exe=work/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11','-O1','-I',str(ROOT/'ports/mercenaries/src'),'-I',str(ROOT/'src/input'),'-I',str(ROOT/'src'),'-I',str(ROOT/'include'),'-I',str(work),str(ROOT/'tools/recomp/fixtures/dev_menu_preview.c'),'-o',str(exe),'-lgdi32','-luser32'],check=True)
 output=args.output.resolve() if args.output else work
 output.mkdir(parents=True,exist_ok=True)
 subprocess.run([str(exe),str(output/'vehicles.bmp'),str(output/'troops.bmp')],check=True)

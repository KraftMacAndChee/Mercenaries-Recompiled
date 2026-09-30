"""Test the real developer opt-in parser beside an isolated executable."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
production=(ROOT/'ports/mercenaries/src/dev_menu.c').read_text()
start=production.index('static int developer_setting(const char *name)')
body=production[start:production.index('\n#define C_BG',start)]
source='#include <windows.h>\n#include <string.h>\n#include <stdio.h>\n#include <stdlib.h>\n'+body+'\nint main(int n,char**v){int value=recomp_dev_menu_allowed();return value==atoi(v[1])?0:1;}\n'

with tempfile.TemporaryDirectory(prefix='merc-dev-ini-') as folder:
 d=Path(folder);(d/'test.c').write_text(source);exe=d/'test.exe'
 subprocess.run(['C:/MinGW/bin/gcc.exe','-std=c11',str(d/'test.c'),'-o',str(exe)],check=True)
 for content,expected in [(None,0),('[Developer]\ndeveloper_menu=0\n',0),('[Developer]\ndeveloper_menu=1\n',1),('[Developer]\ndeveloper_menu=2\n',0),('[Developer]\ndeveloper_menu=-1\n',0),('[Developer]\ndeveloper_menu=garbage\n',0),('[Other]\ndeveloper_menu=1\n',0)]:
  if content is not None:(d/'developer.ini').write_text(content)
  subprocess.run([str(exe),str(expected)],cwd=ROOT,check=True)
 template=ROOT/'ports/mercenaries/resources/developer.ini'
 assert 'developer_menu=0' in template.read_text()
 cmake=ROOT/'.venv/Scripts/cmake.exe'
 dest=d/'keep.ini';dest.write_text('[Developer]\ndeveloper_menu=1\n')
 subprocess.run([str(cmake),'-DSOURCE='+str(template),'-DDESTINATION='+str(dest),'-P',str(ROOT/'ports/mercenaries/scripts/Copy-Developer-Config.cmake')],check=True)
 assert 'developer_menu=1' in dest.read_text()
 print('PASS: absent/default/invalid denied, exact opt-in allowed, executable-relative lookup and existing INI preserved')

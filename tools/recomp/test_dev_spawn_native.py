"""Exercise production developer-spawn decisions without running the game."""
from pathlib import Path
import subprocess,tempfile,shutil
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='merc-dev-spawn-') as directory:
 exe=Path(directory)/'test.exe'
 subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11','-O2','-I',str(ROOT/'ports/mercenaries/src'),'-I',str(ROOT/'src/input'),'-I',str(ROOT/'src'),str(ROOT/'tools/recomp/fixtures/dev_spawn.c'),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

"""Exercise the maintained VSH translator on layered planes and screen-space UI.
No retail assets or generated game C are used. D24 occlusion is checked on WARP.
"""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'src/d3d/d3d8_vsh.c').read_text(encoding='utf-8')
source=source[:source.index(' * Input Layout Management')]
source=source[:source.rfind('/* ================================================================')]
fixture=(ROOT/'tools/recomp/fixtures/vsh_subpixel_projection.c').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='merc-subpixel-') as temp:
 p=Path(temp);(p/'test.c').write_text(source+'\n'+fixture,encoding='utf-8')
 (p/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(subpixel C)
add_executable(subpixel test.c)
target_include_directories(subpixel PRIVATE "{ROOT.as_posix()}/src" "{ROOT.as_posix()}/src/d3d" "{ROOT.as_posix()}/src/platform")
target_link_libraries(subpixel PRIVATE d3d11 d3dcompiler dxguid)
''')
 def run(args):
  r=subprocess.run(args,capture_output=True,text=True,encoding='utf-8',errors='replace')
  if r.returncode: print(r.stdout);print(r.stderr)
  r.check_returncode();return r.stdout
 cmake=str(ROOT/'.venv/Scripts/cmake.exe')
 run([cmake,'-S',str(p),'-B',str(p/'build'),'-G','Visual Studio 17 2022','-A','x64,version=10.0.26100.0','-T','v143,version=14.44.35207,host=x64'])
 run([cmake,'--build',str(p/'build'),'--config','Release'])
 print(run([str(p/'build/Release/subpixel.exe')]))

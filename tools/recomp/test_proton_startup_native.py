"""Exercise Windows shader equivalence and fresh-cache isolation with MSVC."""
from pathlib import Path
import os, shutil, subprocess, tempfile, re
ROOT = Path(__file__).resolve().parents[2]
def main():
    if os.name != 'nt':
        print('SKIP: native Windows regression requires Windows/MSVC')
        return
    bundled=ROOT/'.venv/Lib/site-packages/cmake/data/bin/cmake.exe'
    cmake=str(bundled) if bundled.exists() else shutil.which('cmake')
    if not cmake: raise RuntimeError('CMake is required')
    with tempfile.TemporaryDirectory(prefix='merc-proton-native-') as work:
        p=Path(work)
        source=(ROOT/'tools/recomp/fixtures/proton_startup.c').read_text()
        manual=(ROOT/'ports/mercenaries/src/recomp_manual.c').read_text()
        memory=(ROOT/'src/kernel/xbox_memory_layout.c').read_text()
        helpers=re.search(r'int recomp_prepare_empty_cache_volume\(void\).*?\n\}',manual,re.S)[0]
        helpers+='\n'+re.search(r'static BOOL xbox_guest_arena_available\(uintptr_t base\).*?\n\}',memory,re.S)[0]
        (p/'test.c').write_text(source.replace('/* PRODUCTION_HELPERS */',helpers))
        r=ROOT.as_posix()
        (p/'CMakeLists.txt').write_text(f"""cmake_minimum_required(VERSION 3.20)
project(proton_regression C)
add_executable(proton_regression test.c "{r}/src/kernel/kernel_path.c" "{r}/src/kernel/mod_overlay.c" "{r}/src/d3d/d3d8_compiler.c")
target_include_directories(proton_regression PRIVATE "{r}/src" "{r}/src/kernel" "{r}/src/d3d")
target_compile_definitions(proton_regression PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(proton_regression PRIVATE /UNDEBUG)
target_link_libraries(proton_regression PRIVATE shell32 d3dcompiler)
""")
        for args in ([cmake,'-S',str(p),'-B',str(p/'build'),'-A','x64'],[cmake,'--build',str(p/'build'),'--config','Release']):
            result=subprocess.run(args,capture_output=True,text=True)
            if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        subprocess.run([str(p/'build/Release/proton_regression.exe'),str(p/'scratch')],check=True)
if __name__=='__main__': main()

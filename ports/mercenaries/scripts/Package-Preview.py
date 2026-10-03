"""Package a validated ISO-installing preview and its maintained source toolchain."""
from __future__ import annotations
import argparse, datetime, hashlib, json, os, shutil, subprocess, tempfile, zipfile
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[3]))
from tools.recomp.source_package_policy import verify_source_zip

PAYLOAD = ('Mercenaries Recompiled.exe', 'mercenaries_recomp.exe', 'SDL2.dll',
           'SDL2-LICENSE.txt', 'tools/AgencyFB-COPYRIGHT.txt',
           'licenses/xboxrecomp-LICENSE.txt', 'licenses/DSP56300-LICENSE.txt',
           'licenses/xemu-LGPL-2.1.txt', 'licenses/xemu-GPL-2.0.txt', 'compat/d3dcompiler_47_native.dll',
           'compat/sdk_license.rtf', 'compat/sdk_third_party_notices.rtf',
           'compat/README.txt', 'tools/xdvdfs.exe', 'tools/xdvdfs-LICENSE.txt')

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda:f.read(1024*1024),b''): h.update(chunk)
    return h.hexdigest().upper()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build',type=Path,required=True,help='Completed bin/Release directory')
    p.add_argument('--archive',type=Path,required=True,help='Content-addressed provenance directory')
    p.add_argument('--validation',type=Path,required=True,help='JSON with gameRuntimeSha256 and validation evidence')
    p.add_argument('--notes',type=Path,required=True,help='Human-readable release notes')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--public-release',action='store_true',help='Package the main public release instead of a preview')
    p.add_argument('--include-toolchain',action='store_true',help='Bundle developer sources inside the player ZIP (default: separate download)')
    p.add_argument('--toolchain-output',type=Path,help='Opt in to copying a separate developer source ZIP (default: keep it only in provenance storage)')
    a=p.parse_args()
    if a.output.exists(): raise RuntimeError('Refusing to replace an existing preview ZIP')
    if a.output.suffix.lower()!='.zip': raise ValueError('Output must be a ZIP')
    provenance=json.loads((a.archive/'provenance.json').read_text(encoding='utf-8-sig'))
    validation=json.loads(a.validation.read_text(encoding='utf-8-sig'))
    runtime_hash=sha(a.build/'mercenaries_recomp.exe')
    if runtime_hash!=validation['gameRuntimeSha256'].upper():
        raise RuntimeError('Build runtime differs from the validated executable')
    for file,key in [('toolchain-source.zip','toolchainArchiveSha256'),('generated-tree.zip','generatedArchiveSha256')]:
        if sha(a.archive/file)!=provenance[key]: raise RuntimeError('Provenance archive checksum mismatch: '+file)
        with zipfile.ZipFile(a.archive/file) as z:
            if z.testzip(): raise RuntimeError('Corrupt provenance archive: '+file)
    verify_source_zip(a.archive/'toolchain-source.zip')
    flags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0
    with tempfile.TemporaryDirectory(prefix='mercenaries-preview-') as work:
        title = 'Mercenaries Recompiled' if a.public_release else 'Mercenaries Recompiled Setup Preview'
        root=Path(work)/title;root.mkdir()
        for rel in PAYLOAD:
            source=a.build/rel
            if not source.is_file() or source.is_symlink(): raise RuntimeError('Missing/linked payload: '+rel)
            target=root/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        shutil.copy2(Path(__file__).resolve().parents[1]/'resources/developer.ini',root/'developer.ini')
        shutil.copy2(Path(__file__).resolve().parents[1]/'resources/modcompatibility.ini',root/'modcompatibility.ini')
        shutil.copy2(Path(__file__).resolve().parents[3]/'docs/MODDING.md',root/'MODDING.md')
        if a.include_toolchain:
            shutil.copy2(a.archive/'toolchain-source.zip',root/'ISO-to-Port-Toolchain.zip')
        notes=a.notes.read_text(encoding='utf-8-sig').strip()
        readme=title+'''

Extract this ZIP, then run Mercenaries Recompiled.exe. On first launch select
your retail Mercenaries ISO using Browse to begin installation. Game data is not bundled.

For an existing installation, close the game and copy these files into its
folder. Keep your installed GameFiles, saves and settings. This package has
no saves or gameplay settings INI to overwrite them. Keep your existing
developer.ini if you enabled developer tools, and preserve your modcompatibility.ini
if you enabled mod capacity settings. Both mod options ship disabled; set them
to 1 when using expansive mods, then restart the game. New installs include
developer.ini with [Developer] developer_menu=0. Change it to 1 and restart
to enable F9 and F11. F11 toggles Free Cam. In Free Cam, 1 toggles freeze,
2 toggles 20% speed; leaving Free Cam
restores normal time. FPS / Frametime enables an on-screen performance graph.
F9 Missions selects contracts in the current province or travels between provinces.
Use test saves: mission selection changes campaign progression. Province travel
requires no active contract.
F9 Troops spawns combat soldiers by faction and quantity. Vehicles can be empty,
crewed by a driver/pilot, or filled with crew and passengers from a chosen faction.
Untargetable prevents AI targeting of the player/current vehicle; Passive Mode
suppresses AI hostility toward everyone. Uncheck either to restore normal AI.
These do not prevent incidental damage or change faction standings. Boids Simulation
creates a flock near the player; click again to replace it. Flocks are not saved
and do not automatically respawn after loading or province travel.

Linux / Steam Proton

Add Mercenaries Recompiled.exe as a non-Steam game and enable Proton in its
Compatibility settings. Keep the entire extracted package together, including
the compat folder. Use the same Steam entry/prefix for installation and play.
The bundled shader compiler is selected automatically on Wine/Proton; no
compiler DLL override or separate .NET installation is required. Native
Windows continues to use its existing system shader compiler.

For a Linux startup report, set Steam Launch Options to PROTON_LOG=1 %command%
and include the resulting steam-*.log plus the game's preview-logs. Do not
include your ISO or game files. Compatibility testing includes GE-Proton11-6
with DXVK and plain Wine9; hardware/driver-specific problems may remain.

The matching ISO-to-Port toolchain is retained with the project build provenance,
separate from the normal player ZIP. It contains the maintained runtime, translator,
seeds and build scripts used by this build. It is not needed to install or play.
If you obtain the developer archive, extract it into a separate source folder.
Install Python 3.12 and Visual Studio 2022 C++ build tools with MSVC
14.44.35207 and Windows SDK 10.0.26100.0. Run from that source folder:
  powershell -ExecutionPolicy Bypass -File ports/mercenaries/scripts/Build-From-Iso.ps1 -IsoPath "C:\\path\\Mercenaries.iso"
The script installs its tool dependencies, extracts the ISO, regenerates C,
builds the port and records provenance. Generated C is disposable and is not
included as a required source input in the toolchain ZIP.

Preview diagnostics

Logging is off by default. Set logging=1 under [Developer] in developer.ini
and restart to write diagnostics to preview-logs beside the runtime. Press F8
during or after a glitch while the game has focus, then include the recent
nonempty logs with your report. F8 does not take a screenshot. The logger uses
a bounded background queue, automatic one-second recent history, five-second
summaries and warning limits;
it does not enable full tracing. Files rotate at 4 MiB, retaining up to 16 files
(about 64 MiB; active sessions are preserved). Nothing is uploaded automatically.
Set logging=0 and restart to turn logging off again.

Release notes

'''+notes+'\n\nBuild identity and validation limits are recorded in mercenaries-distribution-provenance.json.\n'
        if a.public_release:
            readme = (Path(__file__).resolve().parents[1]/'resources/public-readme.txt').read_text(encoding='utf-8').rstrip()
            readme += '\n\nRelease notes\n\n' + notes + '\n\nBuild identity and validation limits are recorded in mercenaries-distribution-provenance.json.\n'
        (root/'README-FIRST-LAUNCH.txt').write_text(readme,encoding='utf-8')
        manifest={'schema':18,'kind':'validated-iso-to-port-release' if a.public_release else 'validated-iso-to-port-preview',
                  'packagedUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  'generatedCDisposable':True,'sourceProvenance':provenance,
                  'validation':validation,
                  'files':{f.relative_to(root).as_posix():sha(f) for f in sorted(root.rglob('*')) if f.is_file()}}
        (root/'mercenaries-distribution-provenance.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
        staged={f.relative_to(root).as_posix():sha(f) for f in root.rglob('*') if f.is_file()}
        a.output.parent.mkdir(parents=True,exist_ok=True)
        temporary=a.output.with_suffix('.zip.partial')
        if temporary.exists(): raise RuntimeError('A previous partial package exists')
        with zipfile.ZipFile(temporary,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
            # Keep the mod-install location discoverable before any mod exists.
            z.writestr(root.name + '/mods/', b'')
            for rel in sorted(staged):
                z.write(root/rel,root.name+'/'+rel,compress_type=zipfile.ZIP_STORED if rel.endswith('.zip') else zipfile.ZIP_DEFLATED)
        with zipfile.ZipFile(temporary) as z:
            if z.testzip(): raise RuntimeError('Preview CRC verification failed')
            for rel,expected in staged.items():
                if hashlib.sha256(z.read(root.name+'/'+rel)).hexdigest().upper()!=expected:
                    raise RuntimeError('Preview member verification failed: '+rel)
            check=Path(work)/'extracted';z.extractall(check)
        extracted=check/root.name
        if not (extracted/'mods').is_dir() or any((extracted/'mods').iterdir()):
            raise RuntimeError('Player package must contain an empty mods directory')
        result=subprocess.run([str(extracted/'Mercenaries Recompiled.exe'),'--validate-only'],creationflags=flags,timeout=20)
        if result.returncode!=2: raise RuntimeError('Clean launcher did not report missing game data as expected')
        extractor=subprocess.run([str(extracted/'tools/xdvdfs.exe'),'--version'],capture_output=True,text=True,creationflags=flags,timeout=20,check=True)
        # Keep source reproducibility available without burdening player installs.
        source_output = None
        if not a.include_toolchain and a.toolchain_output:
            source_output = a.toolchain_output
            if source_output.resolve() == a.output.resolve(): raise ValueError('Toolchain and player ZIP must differ')
            source_output.parent.mkdir(parents=True,exist_ok=True)
            if source_output.exists():
                if sha(source_output)!=provenance['toolchainArchiveSha256']: raise RuntimeError('Refusing to overwrite different toolchain ZIP')
            else:
                shutil.copy2(a.archive/'toolchain-source.zip',source_output)
            if sha(source_output)!=provenance['toolchainArchiveSha256']: raise RuntimeError('Separate toolchain verification failed')
        # Publish only after CRC, member hashes and extracted setup tools pass.
        temporary.rename(a.output)
        report={'zip':str(a.output.resolve()),'zipSha256':sha(a.output),'zipBytes':a.output.stat().st_size,
                'gameRuntimeSha256':runtime_hash,'files':len(staged),'crcAndMemberHashes':'passed',
                'cleanSetupValidationExitCode':result.returncode,'extractorVersion':extractor.stdout.strip(),
                'maintainedToolchainIncluded':a.include_toolchain,
                'separateToolchain':str(source_output.resolve()) if source_output else None,
                'toolchainArchiveSha256':provenance['toolchainArchiveSha256'],
                'noGameDataSavesOrSettings':True, 'emptyModsDirectory':True}
        a.output.with_suffix('.verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(report,indent=2))
if __name__=='__main__':main()

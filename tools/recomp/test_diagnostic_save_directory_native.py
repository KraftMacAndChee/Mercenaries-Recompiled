"""Normal saves remain unchanged; explicit isolated tests get private storage."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]


class DiagnosticSaveDirectoryTests(unittest.TestCase):
    def test_manual_launcher_resolution_override_survives_environment_sanitization(self):
        launcher=(ROOT/'tools/recomp/Run-HiddenRetailManual.ps1').read_text(encoding='utf-8')
        self.assertIn('[ValidateRange(-1, 5)]', launcher)
        self.assertIn('[int]$ResolutionScaleOverride = -1', launcher)
        assignment='$settings.MERCENARIES_TEST_RESOLUTION_SCALE = [string]$ResolutionScaleOverride'
        self.assertIn(assignment, launcher)
        self.assertLess(launcher.index(assignment), launcher.index('foreach ($entry in Get-ChildItem Env:MERCENARIES_*)'))
        self.assertIn('[int]$TraceSurfaceTexturesAfterMs', launcher)
        self.assertIn("$settings.MERCENARIES_TRACE_SURFACE_TEXTURES = '1'", launcher)
        self.assertIn("$settings.MERCENARIES_TRACE_SURFACE_TEXTURE_REJECTS = '1'", launcher)
        self.assertIn('$settings.MERCENARIES_TRACE_SURFACE_DELAY_MS', launcher)

    def test_actual_selector(self):
        source=(ROOT/'ports/mercenaries/src/main.c').read_text(encoding='utf-8')
        body=re.search(r'static const char \*diagnostic_save_directory\(.*?\n\}',source,re.S)[0]
        self.assertIn('xbox_path_init(g_game_dir, diagnostic_save_directory(g_save_dir));',source)
        prelude=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static const char *isolated,*directory;
static unsigned directory_reads;
static const char *fake_getenv(const char *name){
 if(!strcmp(name,"MERCENARIES_TEST_ISOLATE_INPUT"))return isolated;
 assert(!strcmp(name,"MERCENARIES_TEST_SAVE_DIR"));++directory_reads;return directory;
}
#define getenv fake_getenv
'''
        tail=r'''
int main(void){
 const char *modes[]={NULL,"","0","1"},*paths[]={NULL,"","C:/test/private/saves"};
 const char *normal="C:/existing/user/saves";
 for(unsigned i=0;i<4;i++)for(unsigned j=0;j<3;j++){
  isolated=modes[i];directory=paths[j];directory_reads=0;
  assert(diagnostic_save_directory(normal)==(i==3 && j==2?directory:normal));
  assert(directory_reads==(i==3));
 }
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='merc-private-save-') as directory:
            path=Path(directory);c=path/'test.c';exe=path/'test.exe'
            c.write_text(prelude+body+tail,encoding='utf-8')
            build=subprocess.run([shutil.which('gcc') or 'C:/MinGW/bin/gcc.exe','-std=c11',str(c),'-o',str(exe)],capture_output=True)
            self.assertEqual(build.returncode,0,build.stderr.decode(errors='replace'))
            run=subprocess.run([str(exe)],capture_output=True)
            self.assertEqual(run.returncode,0,run.stderr.decode(errors='replace'))
        launcher=(ROOT/'tools/recomp/Run-HiddenRetailRoute.ps1').read_text()
        self.assertIn("MERCENARIES_TEST_SAVE_DIR = (Join-Path $runDirectory 'saves')",launcher)
        self.assertIn('[string]$BootMetadataRun', launcher)
        self.assertIn("foreach ($directory in @('System', 'Cache'))", launcher)
        self.assertNotIn("foreach ($directory in @('System', 'Cache', 'UDATA'", launcher)
        self.assertIn('if (Test-Path -LiteralPath $runDirectory)', launcher)
        self.assertIn('Use a new diagnostic run directory', launcher)
        self.assertIn('[switch]$TraceLuaPoscall', launcher)
        self.assertIn("MERCENARIES_TRACE_LUA_POSCALL_SITES = '1'", launcher)
        self.assertIn("MERCENARIES_TRACE_LUA_CALLFRAMES = '1'", launcher)
        self.assertIn("if ($settings.Contains('MERCENARIES_TEST_GAMEPAD_FILE'))", launcher)
        self.assertIn("-Value '0 1 1 0 0 0 0 0 0 0'", launcher)


if __name__=='__main__':unittest.main()

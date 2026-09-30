"""Distribution guard regressions: no Xbox/PS2 game copies in source ZIPs."""
from pathlib import Path
import sys
import tempfile
import zipfile
import pytest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.source_package_policy import forbidden_source_path,verify_source_zip

@pytest.mark.parametrize('name',[
 'disc.iso','DISC.XISO','backup.chd','dump.bin','default.xbe','SLUS_209.32',
 'DATAPS2/ASSETS.DSK','ports/mercenaries/GameFiles/arbitrary.dat',
 'tools/recomp/game_files/ps2/other.dat',
 'artifacts/backup.zip','.git/objects/data','game.elf','boot.irx','sound.msb',
 'sound.msh','bank.xsb','bank.xwb','movie.pss','nested.zip','../escape.c',
 'C:/game.iso','tools\\game_files\\renamed.data',
])
def test_reject_game_and_local_paths(name):
 assert forbidden_source_path(name)

@pytest.mark.parametrize('name',[
 'src/audio.c','third_party/lua-5.0.3/COPYRIGHT','docs/runtime/ps2-upgrades.md',
 'ports/mercenaries/resources/ps2_upgrades/rifle_shot1.wav',
 'ports/mercenaries/assets/prompts/playstation.png',
 'tools/recomp/fixtures/enemy_memory/expected.bin.gz',
])
def test_keep_source_and_reviewed_assets(name):
 assert forbidden_source_path(name) is None

@pytest.mark.parametrize('payload',[
 b'XBEH'+bytes(100),
 b'\x7fELF\x01\x01'+bytes(12)+b'\x08\0'+bytes(100),
 bytes(32768)+b'\x01CD001'+bytes(200),
],ids=['xbe','mips','iso'])
def test_reject_renamed_game_contents(payload):
 with tempfile.TemporaryDirectory() as folder:
  p=Path(folder)/'source.zip'
  with zipfile.ZipFile(p,'w') as z:z.writestr('src/innocent.dat',payload)
  with pytest.raises(ValueError,match='Unsafe source content'):verify_source_zip(p)


def test_reviewed_fixture_must_match_digest():
 with tempfile.TemporaryDirectory() as folder:
  p=Path(folder)/'source.zip'
  with zipfile.ZipFile(p,'w') as z:z.writestr('tools/recomp/fixtures/enemy_memory/expected.bin.gz',b'not the reviewed fixture')
  with pytest.raises(ValueError,match='fixture digest'):verify_source_zip(p)


def test_allowed_files_and_symlink_boundary():
 with tempfile.TemporaryDirectory() as folder:
  p=Path(folder)/'source.zip'
  with zipfile.ZipFile(p,'w') as z:
   z.writestr('src/README.md','source only')
   z.writestr('resources/optional.wav',b'RIFF'+bytes(40))
  assert verify_source_zip(p)==2
  with zipfile.ZipFile(p,'a') as z:
   i=zipfile.ZipInfo('src/link');i.external_attr=0o120777<<16
   z.writestr(i,'F:/Game/default.xbe')
  with pytest.raises(ValueError,match='Linked'):verify_source_zip(p)


@pytest.mark.parametrize('signature',[b'PK\x03\x04',b'7z\xbc\xaf\x27\x1c',b'Rar!\x1a\x07',b'\x1f\x8b',b'BZh',b'\xfd7zXZ\x00'])
def test_reject_renamed_nested_archives(tmp_path,signature):
 p=tmp_path/'source.zip'
 with zipfile.ZipFile(p,'w') as z:z.writestr('src/renamed.dat',signature+bytes(50))
 with pytest.raises(ValueError,match='nested archive'):verify_source_zip(p)


def test_reject_renamed_xbox_iso(tmp_path):
 from tools.recomp.source_package_policy import XBOX_VOLUME_MAGIC
 p=tmp_path/'source.zip'
 with zipfile.ZipFile(p,'w') as z:z.writestr('src/renamed.dat',bytes(65536)+XBOX_VOLUME_MAGIC)
 with pytest.raises(ValueError,match='Xbox disc'):verify_source_zip(p)


def test_partitioned_xbox_headers_without_large_fixtures():
 from tools.recomp.source_package_policy import XBOX_PARTITIONS,XBOX_VOLUME_MAGIC,game_stream_reason
 # Sparse read-only test device: no full disc image is created or bundled.
 class SparseImage:
  def __init__(self,offset):self.offset=offset;self.position=0
  def seek(self,position):self.position=position
  def read(self,size):
   start=self.position;self.position+=size
   result=bytearray(size)
   for i,value in enumerate(XBOX_VOLUME_MAGIC):
    if start<=self.offset+i<start+size:result[self.offset+i-start]=value
   return bytes(result)
 for partition in XBOX_PARTITIONS:
  offset=partition+65536
  assert game_stream_reason(SparseImage(offset),offset+20)=='Xbox disc volume descriptor'

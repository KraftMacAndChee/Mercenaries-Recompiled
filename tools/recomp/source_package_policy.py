"""Keep complete game inputs and local archives out of distributable sources.

This is a packaging boundary, not a claim about licensing or code provenance.
Optional maintainer-supplied artwork/audio are reviewed separately and are not
removed by this policy.
"""
from pathlib import PurePosixPath
import fnmatch
import hashlib
import zipfile

REVIEWED_FIXTURES={'tools/recomp/fixtures/enemy_memory/expected.bin.gz':
    'f4b8c68f1203eb1488436ce0f66f98af5c5e66112428e9580fc9baecd240fac1'}

GAME_DIRECTORIES=frozenset({
    'game_files','gamefiles','dataxbox','dataps2',
    '.git','artifacts','references','saves',
})
GAME_SUFFIXES=frozenset({
    '.iso','.xiso','.cso','.chd','.cue','.bin','.mdf','.mds','.nrg',
    '.xbe','.elf','.dsk','.msb','.msh','.xsb','.xwb','.xmv','.pss','.irx',
})
ARCHIVE_SUFFIXES=frozenset({'.zip','.7z','.rar','.tar','.gz','.bz2','.xz'})
# Format offsets from pinned xdvdfs v0.8.3 blockdev.rs and read.rs.
# https://github.com/antangelo/xdvdfs/blob/v0.8.3/xdvdfs-core/src/blockdev.rs
XBOX_PARTITIONS=(0,405798912,265879552,34078720)
XBOX_VOLUME_MAGIC=b'MICROSOFT*XBOX*MEDIA'



def forbidden_source_path(name):
    """Return a reason for an unsafe source member, or None."""
    path=PurePosixPath(name.replace('\\','/'))
    if path.is_absolute() or '..' in path.parts or any(':' in p for p in path.parts):
        return 'non-relative archive path'
    if any(p.lower() in GAME_DIRECTORIES for p in path.parts):
        return 'game/local/private directory'
    if path.as_posix() in REVIEWED_FIXTURES:return None
    leaf=path.name.lower()
    # Generated oracle fixtures may use .bin only when separately reviewed;
    # no blanket binary-data exception is made in the published source tree.
    if path.suffix.lower() in GAME_SUFFIXES:
        return 'disc image, game executable or extracted game data'
    if path.suffix.lower() in ARCHIVE_SUFFIXES:
        return 'nested archive requires a separate reviewed distribution'
    if leaf=='system.cnf' or any(fnmatch.fnmatch(leaf,p) for p in ('slus_*','sles_*','scus_*','sces_*','slpm_*','slps_*','sips_*')):
        return 'PS2 boot file'
    return None


def game_content_reason(prefix):
    if any(prefix.startswith(magic) for magic in (
            b'PK\x03\x04',b'PK\x05\x06',b'PK\x07\x08',b'7z\xbc\xaf\x27\x1c',
            b'Rar!\x1a\x07',b'\x1f\x8b',b'BZh',b'\xfd7zXZ\x00')):
        return 'nested archive header'
    if prefix[257:262]==b'ustar':return 'nested tar archive header'
    if prefix[:4]==b'XBEH':return 'Xbox executable header'
    if len(prefix)>=20 and prefix[:4]==b'\x7fELF':
        order='little' if prefix[5]==1 else 'big'
        if int.from_bytes(prefix[18:20],order)==8:return 'MIPS executable header'
    if len(prefix)>=32774 and prefix[32769:32774]==b'CD001':return 'ISO-9660 volume descriptor'
    return None


def game_stream_reason(stream,size):
    prefix=stream.read(65536+len(XBOX_VOLUME_MAGIC))
    reason=game_content_reason(prefix)
    if reason:return reason
    for partition in XBOX_PARTITIONS:
        offset=partition+32*2048
        if size<offset+len(XBOX_VOLUME_MAGIC):continue
        if partition==0:
            signature=prefix[offset:offset+len(XBOX_VOLUME_MAGIC)]
        else:
            stream.seek(offset)
            signature=stream.read(len(XBOX_VOLUME_MAGIC))
        if signature==XBOX_VOLUME_MAGIC:return 'Xbox disc volume descriptor'
    return None


def verify_source_zip(path):
    count=0
    with zipfile.ZipFile(path) as archive:
        for entry in archive.infolist():
            reason=forbidden_source_path(entry.filename)
            if reason:raise ValueError(f'Unsafe source member {entry.filename}: {reason}')
            if entry.is_dir():continue
            if (entry.external_attr>>16)&0o170000==0o120000:
                raise ValueError(f'Linked source member: {entry.filename}')
            if entry.filename in REVIEWED_FIXTURES:
                with archive.open(entry) as stream:
                    h=hashlib.sha256()
                    for block in iter(lambda:stream.read(1048576),b''):h.update(block)
                if h.hexdigest()!=REVIEWED_FIXTURES[entry.filename]:
                    raise ValueError('Reviewed fixture digest mismatch: '+entry.filename)
                count+=1
                continue
            with archive.open(entry) as stream:
                reason=game_stream_reason(stream,entry.file_size)
            if reason:raise ValueError(f'Unsafe source content {entry.filename}: {reason}')
            count+=1
    return count

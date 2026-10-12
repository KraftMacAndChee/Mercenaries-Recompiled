"""Synthetic localization fixtures; no retail strings or assets."""
from pathlib import Path
import struct
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.modding.transfer_localization import archive, main, pack, serialize, transfer, walk


def registry(values):
    leaves = []
    for name, value in values.items():
        leaves.append(pack(b'BIN_', pack(b'INFO', b'name\0' + name.encode() + b'\0') +
                           pack(b'DATA', (value + '\0').encode('utf-16le'))))
    return pack(b'ucfb', pack(b'reg_', pack(b'SCPE', pack(b'INFO', b'name\0sample\0') +
                                         pack(b'DATA', b''.join(leaves)))))


def dsk(values):
    return serialize({(123, 1): registry(values)}, bytes(2048))


def test_transfer_preserves_mod_changes_and_platform_only_strings():
    base = dsk({'greet': 'Hello', 'mod': 'Old', 'xbox': 'Only Xbox', 'format': '%d items'})
    target = dsk({'greet': 'Hello', 'mod': 'New', 'xbox': 'Only Xbox', 'format': '%d items'})
    source = dsk({'greet': 'Olá', 'mod': 'Antigo', 'format': '%s coisas'})
    output, report = transfer(source, base, target)
    values = walk(archive(output)[0][123, 1])[1]
    assert values == {('sample', 'greet'): 'Olá', ('sample', 'mod'): 'New',
                      ('sample', 'xbox'): 'Only Xbox', ('sample', 'format'): '%d items'}
    assert report['changed_strings'] == 1
    assert report['preserved_mod_strings'] == 1
    assert report['missing_source_strings'] == 1
    assert report['placeholder_mismatches'] == 1
    assert 'Olá' not in str(report)


def test_identity_preserves_unknown_chunks_and_padding():
    opaque = pack(b'UNKN', b'opaque')[:-2] + b'XY'
    raw = serialize({(123, 1): registry({'greet': 'Hi'}) + opaque}, bytes(2048))
    assert transfer(raw, raw, raw)[0] == raw


@pytest.mark.parametrize('raw', [b'', struct.pack('<II', 1, 0),
                                 struct.pack('<II', 0, 1),
                                 struct.pack('<II', 0, 0) + b'index'])
def test_reject_bad_dsk(raw):
    with pytest.raises(ValueError):
        archive(raw)


def test_duplicate_records_and_string_paths_rejected():
    entry = struct.pack('<III', 0, 123, 1)
    with pytest.raises(ValueError, match='duplicate'):
        archive(struct.pack('<II', 2, 0) + entry * 2)
    with pytest.raises(ValueError, match='Duplicate'):
        walk(registry({'greet': 'Hi'}) * 2)


def test_truncated_utf16_and_padding_rejected():
    with pytest.raises(UnicodeError):
        walk(pack(b'BIN_', pack(b'INFO', b'name\0x\0') + pack(b'DATA', b'x')))
    with pytest.raises(ValueError, match='padding'):
        walk(pack(b'UNKN', b'x')[:-1])


def test_cli_dry_run_and_no_overwrite(tmp_path, capsys):
    paths = [tmp_path / name for name in ['source.dsk', 'base.dsk', 'target.dsk']]
    for path in paths:
        path.write_bytes(dsk({'greet': 'Hi'}))
    args = [str(x) for pair in zip(['--source', '--base', '--target'], paths) for x in pair]
    main(args)
    assert 'changed_strings' in capsys.readouterr().out
    output = tmp_path / 'new.dsk'
    main(args + ['--output', str(output)])
    assert output.read_bytes() == paths[0].read_bytes()
    for destination in [output, paths[0]]:
        with pytest.raises(SystemExit) as error:
            main(args + ['--output', str(destination)])
        assert error.value.code == 2

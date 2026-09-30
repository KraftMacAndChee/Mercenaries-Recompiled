"""Resolve named Lua registration entries directly from the retail XBE."""
import argparse
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.recomp import config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name', nargs='+')
    args = parser.parse_args()
    xbe = ROOT / 'game_files/mercenaries-retail/default.xbe'
    config.configure_from_xbe(str(xbe))
    raw = xbe.read_bytes()

    def file_to_va(offset):
        for section in config._SECTIONS:
            if section.raw_addr <= offset < section.raw_addr + section.raw_size:
                return section.va + offset - section.raw_addr
        raise ValueError(f'file offset {offset:X} is not in an XBE section')

    for name in args.name:
        string_offset = raw.find(name.encode('ascii') + b'\0')
        if string_offset < 0:
            print(f'{name}: not present')
            continue
        string_va = file_to_va(string_offset)
        ref_needle = struct.pack('<I', string_va)
        refs = []
        start = 0
        while True:
            offset = raw.find(ref_needle, start)
            if offset < 0:
                break
            try:
                ref_va = file_to_va(offset)
            except ValueError:
                start = offset + 1
                continue
            target = struct.unpack_from('<I', raw, offset + 4)[0]
            refs.append((ref_va, target))
            start = offset + 1
        print(f'{name}: string={string_va:08X} refs=' +
              ','.join(f'{ref:08X}->{target:08X}' for ref, target in refs))


if __name__ == '__main__':
    main()

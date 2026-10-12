"""Transfer matching registry strings between locally supplied Mercenaries DSKs.

SPDX-License-Identifier: MIT. No game data or translations are bundled.
Only flat archives with named UTF-16 registry leaves are supported.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct

MAX_BYTES = 64 * 1024 * 1024
CONTAINERS = {b'ucfb', b'reg_', b'SCPE', b'BIN_'}


@dataclass
class Chunk:
    tag: bytes
    data: bytes
    encoded: bytes


def chunks(raw):
    offset = 0
    result = []
    while offset < len(raw):
        if len(raw) - offset < 8:
            raise ValueError('Truncated registry chunk header')
        size = struct.unpack_from('<I', raw, offset + 4)[0]
        end = offset + 8 + size
        padded = offset + 8 + ((size + 3) & ~3)
        if padded > len(raw):
            raise ValueError('Truncated registry chunk payload/padding')
        result.append(Chunk(raw[offset:offset + 4], raw[offset + 8:end], raw[offset:padded]))
        offset = padded
    return result


def pack(tag, data):
    return tag + struct.pack('<I', len(data)) + data + bytes((-len(data)) % 4)


def named_path(parts, parent):
    names = []
    for part in parts:
        if part.tag == b'INFO' and part.data.startswith(b'name\0'):
            fields = part.data.split(b'\0')
            if len(fields) < 3 or not fields[1]:
                raise ValueError('Malformed registry name')
            names.append(fields[1].decode('ascii'))
    if len(names) > 1:
        raise ValueError('Ambiguous registry name')
    return parent + tuple(names)


def string_value(data):
    value = data.decode('utf-16le')
    body = value.rstrip('\0')
    if '\0' in body:
        raise ValueError('Embedded NUL in registry string')
    return body, value[len(body):]


def walk(raw, replacements=None, path=(), depth=0, found=None):
    if depth > 64:
        raise ValueError('Registry nesting limit exceeded')
    if found is None:
        found = {}
    output = []
    for part in chunks(raw):
        encoded = part.encoded
        if part.tag in CONTAINERS:
            parts = chunks(part.data)
            here = named_path(parts, path)
            payload = part.data
            if part.tag in (b'BIN_', b'SCPE'):
                data_parts = [p for p in parts if p.tag == b'DATA']
                if len(data_parts) != 1:
                    raise ValueError('Registry leaf/scope needs exactly one DATA chunk')
                leaf = data_parts[0]
                if part.tag == b'BIN_':
                    if not here or here in found:
                        raise ValueError('Duplicate or unnamed string path')
                    old, suffix = string_value(leaf.data)
                    found[here] = old
                    new = old if replacements is None else replacements.get(here, old)
                    if not isinstance(new, str) or '\0' in new:
                        raise ValueError('Invalid replacement string')
                    data = (new + suffix).encode('utf-16le')
                else:
                    data, _ = walk(leaf.data, replacements, here, depth + 1, found)
                payload = b''.join(pack(p.tag, data) if p is leaf and data != leaf.data
                                   else p.encoded for p in parts)
            else:
                payload, _ = walk(part.data, replacements, here, depth + 1, found)
            if payload != part.data:
                encoded = pack(part.tag, payload)
        output.append(encoded)
    return b''.join(output), found


def archive(raw):
    if len(raw) < 8 or len(raw) > MAX_BYTES:
        raise ValueError('Archive size outside supported bounds')
    count, grid = struct.unpack_from('<II', raw)
    if grid or count > (len(raw) - 8) // 12:
        raise ValueError('Only bounded flat DSK archives are supported')
    offset = 8 + count * 12
    records = {}
    for i in range(count):
        size, key, kind = struct.unpack_from('<III', raw, 8 + i * 12)
        if size > len(raw) - offset or (key, kind) in records:
            raise ValueError('Truncated or duplicate DSK record')
        records[key, kind] = raw[offset:offset + size]
        offset += size
    trailer = raw[offset:]
    if len(trailer) > 2048 or any(trailer):
        raise ValueError('Unsupported DSK trailing data')
    return records, trailer


def serialize(records, trailer):
    header = struct.pack('<II', len(records), 0)
    directory = b''.join(struct.pack('<III', len(data), *key) for key, data in records.items())
    return header + directory + b''.join(records.values()) + trailer


def placeholders(text):
    # Conservative guard for printf tokens; UI glyph/button syntax still needs
    # platform-specific review by the translation author.
    pattern = r'%(?:\d+\$)?[-+#0 ]*\d*(?:\.\d+)?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn]|%%'
    return Counter(re.findall(pattern, text))


def transfer(source_raw, base_raw, target_raw):
    source, _ = archive(source_raw)
    base, _ = archive(base_raw)
    target, trailer = archive(target_raw)
    donor = {key: walk(data)[1] for key, data in source.items()}
    original = {key: walk(data)[1] for key, data in base.items()}
    result = {}
    report = dict(target_strings=0, matched_strings=0, changed_strings=0,
                  preserved_mod_strings=0, missing_source_strings=0,
                  placeholder_mismatches=0)
    for key, data in target.items():
        leaves = walk(data)[1]
        selected = {}
        report['target_strings'] += len(leaves)
        for path, value in leaves.items():
            if original.get(key, {}).get(path) != value:
                report['preserved_mod_strings'] += 1
                continue
            if path not in donor.get(key, {}):
                report['missing_source_strings'] += 1
                continue
            translated = donor[key][path]
            if placeholders(value) != placeholders(translated):
                report['placeholder_mismatches'] += 1
                continue
            selected[path] = translated
            report['matched_strings'] += 1
            report['changed_strings'] += translated != value
        result[key] = walk(data, selected)[0]
        actual = walk(result[key])[1]
        if actual != {p: selected.get(p, v) for p, v in leaves.items()}:
            raise ValueError('Rebuilt registry validation failed')
    output = serialize(result, trailer)
    if archive(output)[0].keys() != target.keys():
        raise ValueError('Rebuilt record directory changed')
    report['sha256'] = {label: hashlib.sha256(raw).hexdigest() for label, raw in
                        [('source', source_raw), ('base', base_raw),
                         ('target', target_raw), ('output', output)]}
    return output, report


def bounded_read(path):
    with path.open('rb') as stream:
        raw = stream.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise ValueError('Input exceeds the 64 MiB limit')
    return raw


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Local translated PS2 registry DSK')
    parser.add_argument('--base', type=Path, required=True, help='Local unmodified Xbox registry DSK')
    parser.add_argument('--target', type=Path, required=True, help='Local Xbox registry DSK, vanilla or modded')
    parser.add_argument('--output', type=Path, help='New file only; never overwrites an existing file')
    args = parser.parse_args(argv)
    inputs = [p.resolve() for p in (args.source, args.base, args.target)]
    if args.output and (args.output.resolve() in inputs or args.output.exists()):
        parser.error('Output must be a new file distinct from every input')
    try:
        output, report = transfer(*(bounded_read(p) for p in inputs))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open('xb') as stream:
                stream.write(output)
    except (ValueError, OSError, UnicodeError) as error:
        parser.exit(2, f'Localization transfer refused: {error}\n')
    # Deliberately excludes translated text, local paths and usernames.
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()

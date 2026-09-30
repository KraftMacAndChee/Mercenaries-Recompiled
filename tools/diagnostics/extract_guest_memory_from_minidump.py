"""Extract the 64 MiB Xbox guest RAM image from a Windows minidump.

The recompiler keeps ``g_xbox_mem_offset`` in its main image. This utility
resolves that map-addressed global, reads the native mapping base from the
dump, then reconstructs guest addresses 0..0x03ffffff. It is dependency-free
so crash analysis works on a clean host.
"""

import argparse
import struct
from pathlib import Path


MINIDUMP_SIGNATURE = 0x504D444D
MODULE_LIST_STREAM = 4
MEMORY_LIST_STREAM = 5
MEMORY64_LIST_STREAM = 9
GUEST_SIZE = 0x04000000


def unpack(data, fmt, offset):
    return struct.unpack_from(fmt, data, offset)


def streams(data):
    signature, _version, count, directory_rva = unpack(data, "<IIII", 0)
    if signature != MINIDUMP_SIGNATURE:
        raise ValueError("Not a Windows minidump")
    result = {}
    for index in range(count):
        stream_type, size, rva = unpack(data, "<III", directory_rva + index * 12)
        result[stream_type] = (rva, size)
    return result


def memory_ranges(data, directory):
    result = []
    if MEMORY64_LIST_STREAM in directory:
        rva, _size = directory[MEMORY64_LIST_STREAM]
        count, payload_rva = unpack(data, "<QQ", rva)
        descriptor = rva + 16
        for index in range(count):
            address, size = unpack(data, "<QQ", descriptor + index * 16)
            result.append((address, address + size, payload_rva))
            payload_rva += size
    if MEMORY_LIST_STREAM in directory:
        rva, _size = directory[MEMORY_LIST_STREAM]
        count, = unpack(data, "<I", rva)
        descriptor = rva + 4
        for index in range(count):
            address, size, payload_rva = unpack(data, "<QII", descriptor + index * 16)
            result.append((address, address + size, payload_rva))
    return sorted(result)


def read_virtual(data, ranges, address, size):
    output = bytearray()
    cursor = address
    remaining = size
    while remaining:
        match = next((item for item in ranges if item[0] <= cursor < item[1]), None)
        if match is None:
            return None
        start, end, payload = match
        amount = min(remaining, end - cursor)
        source = payload + cursor - start
        output.extend(data[source:source + amount])
        cursor += amount
        remaining -= amount
    return bytes(output)


def read_virtual_sparse(data, ranges, address, size):
    """Read a region while leaving dump-omitted pages zero-filled."""
    output = bytearray(size)
    covered = 0
    end = address + size
    for start, range_end, payload in ranges:
        copy_start = max(address, start)
        copy_end = min(end, range_end)
        if copy_start >= copy_end:
            continue
        amount = copy_end - copy_start
        source = payload + copy_start - start
        destination = copy_start - address
        output[destination:destination + amount] = data[source:source + amount]
        covered += amount
    return bytes(output), covered


def module_base(data, directory, executable_name):
    rva, _size = directory[MODULE_LIST_STREAM]
    count, = unpack(data, "<I", rva)
    for index in range(count):
        entry = rva + 4 + index * 108
        base, _image_size, _checksum, _timestamp, name_rva = unpack(data, "<QIIII", entry)
        byte_length, = unpack(data, "<I", name_rva)
        name = data[name_rva + 4:name_rva + 4 + byte_length].decode("utf-16-le", errors="replace")
        if Path(name).name.casefold() == executable_name.casefold():
            return base, name
    raise ValueError(f"Module not found: {executable_name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--module", default="mercenaries_recomp.exe")
    parser.add_argument("--mem-offset-rva", type=lambda value: int(value, 0), default=0x022BE2A0,
                        help="RVA of g_xbox_mem_offset from the matching linker map")
    args = parser.parse_args()

    data = args.dump.read_bytes()
    directory = streams(data)
    ranges = memory_ranges(data, directory)
    base, module = module_base(data, directory, args.module)
    encoded_offset = read_virtual(data, ranges, base + args.mem_offset_rva, 8)
    if encoded_offset is None:
        raise ValueError("g_xbox_mem_offset is absent from the minidump")
    memory_offset, = struct.unpack("<q", encoded_offset)
    guest_base = memory_offset
    memory, covered = read_virtual_sparse(data, ranges, guest_base, GUEST_SIZE)
    if covered < 4096:
        raise ValueError(
            f"Guest mapping is effectively absent from the minidump "
            f"({covered} bytes covered)"
        )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(memory)
    print(f"module={module} base={base:016X} g_xbox_mem_offset={memory_offset:016X} "
          f"output={args.output} bytes={len(memory)} covered={covered}")


if __name__ == "__main__":
    main()

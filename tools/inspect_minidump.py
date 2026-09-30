#!/usr/bin/env python3
"""Print threads, exceptions, and loaded modules from a Windows minidump."""

from __future__ import annotations

import argparse
import mmap
import struct
from pathlib import Path


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def read_utf16(data: bytes, rva: int) -> str:
    length = u32(data, rva)
    return data[rva + 4 : rva + 4 + length].decode("utf-16-le", errors="replace")


def memory_ranges(data: bytes, streams: dict[int, tuple[int, int]]):
    """Yield (virtual_address, size, file_rva) ranges stored in the dump."""
    if 9 in streams:  # Memory64ListStream
        rva, _ = streams[9]
        count = u64(data, rva)
        file_rva = u64(data, rva + 8)
        for index in range(count):
            offset = rva + 16 + index * 16
            address = u64(data, offset)
            size = u64(data, offset + 8)
            yield address, size, file_rva
            file_rva += size
    if 5 in streams:  # MemoryListStream
        rva, _ = streams[5]
        count = u32(data, rva)
        for index in range(count):
            offset = rva + 4 + index * 16
            address = u64(data, offset)
            size = u32(data, offset + 8)
            file_rva = u32(data, offset + 12)
            yield address, size, file_rva


def read_memory(data: bytes, ranges, address: int, size: int) -> bytes | None:
    for start, range_size, file_rva in ranges:
        if start <= address and size <= start + range_size - address:
            offset = file_rva + address - start
            return data[offset : offset + size]
    return None


def module_for_address(
    modules: list[tuple[int, int, int, str]], address: int
) -> tuple[str, int] | None:
    for base, size, _, name in modules:
        if base <= address < base + size:
            return name, address - base
    return None


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("dump", type=Path)
    parser.add_argument(
        "--read", action="append", default=[], metavar="ADDRESS:SIZE",
        help="hex virtual address and byte count to dump (repeatable)",
    )
    parser.add_argument(
        "--ranges", action="store_true",
        help="list captured virtual-memory ranges",
    )
    parser.add_argument(
        "--min-range-size", type=lambda value: int(value, 0), default=0,
        help="with --ranges, only list ranges at least this many bytes",
    )
    parser.add_argument(
        "--extract", action="append", default=[],
        metavar="ADDRESS:SIZE:OUTPUT",
        help="extract exactly the requested captured-memory range to OUTPUT",
    )
    args = parser.parse_args()
    dump_file = args.dump.open("rb")
    data = mmap.mmap(dump_file.fileno(), 0, access=mmap.ACCESS_READ)
    if data[:4] != b"MDMP":
        raise SystemExit("not a minidump")

    stream_count = u32(data, 8)
    directory_rva = u32(data, 12)
    streams: dict[int, tuple[int, int]] = {}
    for index in range(stream_count):
        offset = directory_rva + index * 12
        stream_type, size, rva = struct.unpack_from("<III", data, offset)
        streams[stream_type] = (rva, size)

    modules: list[tuple[int, int, int, str]] = []
    if 4 in streams:
        rva, _ = streams[4]
        module_count = u32(data, rva)
        for index in range(module_count):
            offset = rva + 4 + index * 108
            modules.append((
                u64(data, offset),
                u32(data, offset + 8),
                u32(data, offset + 16),
                read_utf16(data, u32(data, offset + 20)),
            ))

    ranges = list(memory_ranges(data, streams))
    if args.ranges:
        for address, size, file_rva in ranges:
            if size < args.min_range_size:
                continue
            print(
                f"range address=0x{address:016X} size=0x{size:X} "
                f"file_rva=0x{file_rva:X}"
            )
    for request in args.extract:
        address_text, size_text, output_text = request.split(":", 2)
        address = int(address_text, 0)
        size = int(size_text, 0)
        payload = read_memory(data, ranges, address, size)
        if payload is None:
            raise SystemExit(
                f"memory 0x{address:016X}+0x{size:X} is not present"
            )
        output_path = Path(output_text)
        with output_path.open("wb") as output:
            output.write(payload)
        print(
            f"extracted 0x{size:X} bytes at 0x{address:016X} "
            f"to {output_path}"
        )
    for request in args.read:
        address_text, size_text = request.split(":", 1)
        address = int(address_text, 0)
        size = int(size_text, 0)
        payload = read_memory(data, ranges, address, size)
        if payload is None:
            print(f"memory 0x{address:016X}+0x{size:X}: not present")
            continue
        print(f"memory 0x{address:016X}+0x{size:X}:")
        for offset in range(0, len(payload), 16):
            row = payload[offset : offset + 16]
            words = " ".join(
                f"{int.from_bytes(row[i:i + 4], 'little'):08X}"
                for i in range(0, len(row), 4)
            )
            print(f"  {address + offset:016X}: {words}")

    if 3 in streams:  # ThreadListStream
        rva, _ = streams[3]
        thread_count = u32(data, rva)
        for index in range(thread_count):
            offset = rva + 4 + index * 48
            thread_id = u32(data, offset)
            context_size = u32(data, offset + 40)
            context_rva = u32(data, offset + 44)
            if context_size < 0x100 or context_rva + context_size > len(data):
                print(f"thread id={thread_id} context=unavailable")
                continue
            rax = u64(data, context_rva + 0x78)
            rcx = u64(data, context_rva + 0x80)
            rdx = u64(data, context_rva + 0x88)
            rsp = u64(data, context_rva + 0x98)
            r8 = u64(data, context_rva + 0xB8)
            r10 = u64(data, context_rva + 0xC8)
            r11 = u64(data, context_rva + 0xD0)
            rip = u64(data, context_rva + 0xF8)
            match = module_for_address(modules, rip)
            module_text = (
                f"module={match[0]} rva=0x{match[1]:X}"
                if match is not None
                else "module=<unknown>"
            )
            print(
                f"thread id={thread_id} rip=0x{rip:016X} rsp=0x{rsp:016X} "
                f"rax=0x{rax:016X} rcx=0x{rcx:016X} rdx=0x{rdx:016X} "
                f"r8=0x{r8:016X} r10=0x{r10:016X} r11=0x{r11:016X} "
                f"{module_text}"
            )

    exception_address = None
    if 6 in streams:
        rva, _ = streams[6]
        thread_id = u32(data, rva)
        record = rva + 8
        code = u32(data, record)
        flags = u32(data, record + 4)
        exception_address = u64(data, record + 16)
        parameter_count = min(u32(data, record + 24), 15)
        parameters = [u64(data, record + 32 + i * 8) for i in range(parameter_count)]
        context_size = u32(data, rva + 160)
        context_rva = u32(data, rva + 164)
        print(
            f"exception thread={thread_id} code=0x{code:08X} flags=0x{flags:X} "
            f"address=0x{exception_address:016X} parameters="
            + ",".join(f"0x{value:X}" for value in parameters)
        )
        if context_size >= 0x100 and context_rva + context_size <= len(data):
            regs = {
                "rax": 0x78, "rcx": 0x80, "rdx": 0x88, "rbx": 0x90,
                "rsp": 0x98, "rbp": 0xA0, "rsi": 0xA8, "rdi": 0xB0,
                "r8": 0xB8, "r9": 0xC0, "r10": 0xC8, "r11": 0xD0,
                "r12": 0xD8, "r13": 0xE0, "r14": 0xE8, "r15": 0xF0,
                "rip": 0xF8,
            }
            print(
                "context "
                + " ".join(
                    f"{name}=0x{u64(data, context_rva + offset):016X}"
                    for name, offset in regs.items()
                )
            )

    for base, size, timestamp, name in modules:
        marker = ""
        if exception_address is not None and base <= exception_address < base + size:
            marker = f"  <-- exception RVA 0x{exception_address - base:X}"
        print(
            f"module base=0x{base:016X} size=0x{size:X} "
            f"timestamp=0x{timestamp:08X} name={name}{marker}"
        )


if __name__ == "__main__":
    main()

"""Read a named script from the user-provided retail DSK.

The current decoder is implemented from retail x86 and checked against direct
execution. Container fields are documented from retail archive observations in
docs/runtime/retail-script-format.md. This replaces a source-informed decoder;
see the dated replacement record for the historical distinction.
"""
import argparse
from pathlib import Path
import re
import struct


class _CompressedInput:
    """16-bit little-endian control words interleaved with payload bytes."""
    def __init__(self, data):
        self.data=memoryview(data)
        self.cursor=0
        self.control=0
        self.available=0
        self.refill()

    def byte(self):
        if self.cursor>=len(self.data):
            raise ValueError("Truncated compressed script")
        result=self.data[self.cursor]
        self.cursor+=1
        return result

    def refill(self):
        self.control=self.byte() | self.byte()<<8
        self.available=16

    def bit(self):
        result=self.control & 1
        self.control >>= 1
        self.available -= 1
        # Retail 0x21A778..0x21A787 refills before the payload of bit 16.
        if not self.available:
            self.refill()
        return result


def decompress(data, expected):
    """Bounded decoder implemented from retail 0x21A750..0x21A857.

    See docs/runtime/retail-script-format.md for instruction-level evidence and
    the independent emulator comparison. Bounds/error handling is host policy.
    """
    if not isinstance(expected,int) or not 0<=expected<=16*1024*1024:
        raise ValueError("Invalid decompressed script size")
    stream=_CompressedInput(data)
    result=bytearray()
    while True:
        if stream.bit():
            if len(result)==expected:
                raise ValueError("Literal exceeds declared script size")
            result.append(stream.byte())
            continue
        if not stream.bit():
            count=3+2*stream.bit()+stream.bit()
            distance=256-stream.byte()
        else:
            low=stream.byte()
            packed=stream.byte()
            distance=4096-((packed & 0xF0)<<4)-low
            count=3+(packed & 15)
            if count==3:
                extension=stream.byte()
                if not extension:
                    if len(result)!=expected:
                        raise ValueError("End marker disagrees with declared script size")
                    return bytes(result)
                count=extension+1
        if distance>len(result) or len(result)+count>expected:
            raise ValueError("Invalid compressed-script copy")
        # Forward byte copies deliberately permit overlapping history spans.
        for _ in range(count):
            result.append(result[-distance])


def chunks(data):
    position = 0
    while position < len(data):
        if position + 8 > len(data):
            raise ValueError("Truncated chunk header")
        tag, size = struct.unpack_from("<4sI", data, position)
        end = position + 8 + size
        if end > len(data):
            raise ValueError("Chunk exceeds container")
        yield tag, data[position + 8:end]
        position = (end + 3) & ~3


def read_script(path, requested):
    with path.open("rb") as stream:
        total = path.stat().st_size
        count, _ = struct.unpack("<II", stream.read(8))
        if count > (total - 8) // 12:
            raise ValueError("Invalid DSK directory count")
        directory = stream.read(count * 12)
        offset = 8 + count * 12
        for index in range(count):
            size, _, kind = struct.unpack_from("<III", directory, index * 12)
            if offset + size > total:
                raise ValueError("DSK entry outside archive")
            if kind == 0x203E6FAA:  # scr_
                stream.seek(offset)
                root = dict(chunks(stream.read(size)))
                script = dict(chunks(root[b"ucfb"]))
                fields = dict(chunks(script[b"scr_"]))
                name = fields[b"NAME"].rstrip(b"\0").decode("ascii")
                if name == requested:
                    expected = struct.unpack_from("<I", fields[b"INFO"], 1)[0]
                    body = fields[b"BODY"]
                    decoded = decompress(body, expected) if expected else body
                    return decoded.rstrip(b"\0").decode("ascii")
            offset += size
    raise ValueError(f"Script not found: {requested}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("script")
    parser.add_argument("--match", help="Print matching lines with context")
    parser.add_argument("--context", type=int, default=4)
    args = parser.parse_args()
    lines = read_script(args.archive, args.script).splitlines()
    selected = set(range(len(lines))) if args.match is None else set()
    if args.match is not None:
        for index, line in enumerate(lines):
            if re.search(args.match, line, re.IGNORECASE):
                selected.update(range(max(0, index - args.context), min(len(lines), index + args.context + 1)))
    for index in sorted(selected):
        print(f"{index + 1}: {lines[index]}")


if __name__ == "__main__":
    main()

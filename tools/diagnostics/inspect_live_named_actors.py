"""Read-only retail actor/name census from one explicitly selected diagnostic PID.

Reads guest RAM only. It cannot write memory, inject inputs, or change missions.
The offset must come from the selected process's startup log.
"""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import struct
import re
import math
import hashlib
from collections import Counter


def project_world_point(inverse, frustum, point):
    """Read-only RedCamera projection; forward is negative view-space Z."""
    if len(inverse) != 16 or len(frustum) != 4 or len(point) != 3:
        raise ValueError('Invalid camera/point dimensions')
    if not all(math.isfinite(v) for v in (*inverse, *frustum, *point)):
        raise ValueError('Non-finite camera or target')
    near, far, width, height = frustum
    if not 0 < near < far or width <= 0 or height <= 0:
        raise ValueError('Invalid frustum')
    p = (*point, 1)
    view = tuple(sum(p[i]*inverse[i*4+j] for i in range(4)) for j in range(3))
    if view[2] >= -near:
        return view, None
    screen = (.5+view[0]*near/(-view[2]*width),
              .5-view[1]*near/(-view[2]*height))
    return view, screen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("--executable", type=Path,
                        help="Explicit diagnostic executable within build/mercenaries/bin/Release")
    parser.add_argument("--offset", type=lambda x: int(x, 0), required=True)
    parser.add_argument("--name", action="append", default=[])
    parser.add_argument("--class-vtable", action="append", type=lambda x: int(x, 0), default=[])
    parser.add_argument("--word", action="append", type=lambda x: int(x, 0), default=[],
                        help="Also print a bounded guest DWORD as hex/float (read-only)")
    parser.add_argument("--raw-matches", action="store_true",
                        help="Show bounded hash occurrences even outside the actor heuristic")
    parser.add_argument('--project-point', type=float, nargs=3, action='append', default=[],
                        help='Project a world point through the current unzoomed retail RedCamera (read-only)')
    parser.add_argument("--dump-guest", type=Path,
                        help="Save this read-only 64 MiB guest snapshot to a new artifact file")
    parser.add_argument("--heap-base", type=lambda x: int(x, 0),
                        help="Observed native image base; inspect allocator metadata using its sibling map")
    args = parser.parse_args()
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                 wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                         ctypes.c_void_p, ctypes.c_size_t,
                                         ctypes.POINTER(ctypes.c_size_t)]
    process = kernel.OpenProcess(0x1000 | 0x10, False, args.pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        path = ctypes.create_unicode_buffer(32768)
        length = wintypes.DWORD(len(path))
        if not kernel.QueryFullProcessImageNameW(process, 0, path, ctypes.byref(length)):
            raise ctypes.WinError(ctypes.get_last_error())
        expected = Path(__file__).resolve().parents[2] / "build/mercenaries/bin/Release/mercenaries_recomp.exe"
        if args.executable:
            candidate = args.executable.resolve()
            public_preview = (Path(__file__).resolve().parents[2] /
                              "artifacts/user-preview/mercenaries_recomp_preview.exe").resolve()
            same_as_public_preview = False
            if candidate.is_file() and public_preview.is_file():
                def sha256(path):
                    digest = hashlib.sha256()
                    with path.open("rb") as source:
                        for chunk in iter(lambda: source.read(1024 * 1024), b""):
                            digest.update(chunk)
                    return digest.digest()
                same_as_public_preview = sha256(candidate) == sha256(public_preview)
            private_run = candidate.parent.parent
            private_runtime = (candidate.parent.name == 'runtime' and
                private_run.parent == (Path(__file__).resolve().parents[2]/'artifacts/test-runs').resolve() and
                candidate.name.startswith('mercenaries_recomp_run') and
                (private_run/'pid.txt').is_file() and
                int((private_run/'pid.txt').read_text().strip()) == args.pid)
            if ((candidate.parent != expected.parent.resolve() and not private_runtime and
                 candidate != public_preview and not same_as_public_preview)
                    or candidate.suffix.lower() != ".exe"):
                raise ValueError(
                    "Executable must be a workspace diagnostic or byte-identical "
                    "to the exact public preview")
            expected = candidate
        if Path(path.value).resolve() != expected.resolve():
            raise RuntimeError(f"Refusing unrelated process: {path.value}")
        if args.heap_base:
            def native_read(address, count):
                output = ctypes.create_string_buffer(count)
                copied = ctypes.c_size_t()
                if not kernel.ReadProcessMemory(process, address, output, count, ctypes.byref(copied)):
                    raise ctypes.WinError(ctypes.get_last_error())
                if copied.value != count:
                    raise RuntimeError("Incomplete allocator metadata read")
                return output.raw
            header = native_read(args.heap_base, 4096)
            pe = struct.unpack_from("<I", header, 0x3C)[0]
            if header[:2] != b"MZ" or header[pe:pe+4] != b"PE\0\0":
                raise ValueError("Observed base does not identify a PE image")
            image_base = struct.unpack_from("<Q", header, pe + 48)[0]
            image_size = struct.unpack_from("<I", header, pe + 80)[0]
            map_text = expected.with_suffix(".map").read_text(encoding="utf-8")
            preferred = re.search(r"Preferred load address is ([0-9a-fA-F]+)", map_text)
            if preferred:
                image_base = int(preferred[1], 16)
            def symbol(name):
                match = re.search(r"\s" + name + r"\s+([0-9a-fA-F]{16})\s", map_text)
                if not match:
                    raise ValueError(f"Missing allocator symbol: {name}")
                offset = int(match[1], 16) - image_base
                if not 0 <= offset < image_size:
                    raise ValueError("Map symbol outside native image")
                return args.heap_base + offset
            count = struct.unpack("<I", native_read(symbol("g_heap_alloc_count"), 4))[0]
            if count > 16384:
                raise ValueError("Invalid allocator record count")
            records = native_read(symbol("g_heap_allocations"), count * 48)
            live, free = [], []
            for index in range(count):
                va, requested, size, *owner = struct.unpack_from("<11I", records, index * 48)
                if size:
                    (live if records[index * 48 + 44] else free).append((va, requested, size, owner))
            print(f"HEAP live={len(live)} bytes={sum(row[2] for row in live)} free={sum(row[2] for row in free)} largest={max((row[2] for row in free), default=0)}")
            print("HEAP size histogram:", Counter(row[2] for row in live).most_common(24))
            for va, requested, size, owner in sorted(live, key=lambda row: row[2], reverse=True)[:96]:
                print(f"HEAP va={va:08X} size={size} requested={requested} owner=" + ",".join(f"{word:08X}" for word in owner))
        print(f"PID {args.pid}: {path.value}; guest offset={args.offset:#x}")
        size = 0x04000000
        buffer = ctypes.create_string_buffer(size)
        read = ctypes.c_size_t()
        if not kernel.ReadProcessMemory(process, args.offset, buffer, size, ctypes.byref(read)):
            raise ctypes.WinError(ctypes.get_last_error())
        if read.value != size:
            raise RuntimeError("Incomplete guest RAM read")
        memory = buffer.raw
        nonzero_bytes = len(memory) - memory.count(0)
        if nonzero_bytes < 4096:
            raise RuntimeError(
                "Guest RAM read is effectively empty "
                f"({nonzero_bytes} nonzero bytes); refusing invalid diagnostics"
            )
        if args.project_point:
            camera = struct.unpack_from('<I', memory, 0x414104)[0]
            if not 0x10000 <= camera <= size-0xA0:
                raise ValueError('Invalid current RedCamera pointer')
            inverse = struct.unpack_from('<16f', memory, camera+0x50)
            frustum = struct.unpack_from('<4f', memory, camera+0x90)
            print(f'CAMERA {camera:08X} frustum={frustum} '
                  f'position={struct.unpack_from("<3f", memory, camera+0x40)} '
                  f'back={struct.unpack_from("<3f", memory, camera+0x30)}')
            for point in args.project_point:
                view, screen = project_world_point(inverse, frustum, point)
                print(f'PROJECT point={point} view={view} normalized_screen={screen}')
        if args.dump_guest:
            artifact_root = Path(__file__).resolve().parents[2] / "artifacts"
            destination = args.dump_guest.resolve()
            if not destination.is_relative_to(artifact_root.resolve()):
                raise ValueError("Guest snapshots must remain in workspace artifacts")
            with destination.open("xb") as output:
                output.write(memory)
            print(f"Guest snapshot: {destination} ({len(memory)} bytes)")
        for address in args.word:
            if not 0 <= address <= size - 4:
                raise ValueError(f"Guest word outside RAM: {address:#x}")
            integer = struct.unpack_from("<I", memory, address)[0]
            scalar = struct.unpack_from("<f", memory, address)[0]
            print(f"word[{address:08X}]={integer:08X} float={scalar:g}")
        for vtable in args.class_vtable:
            if not 0x10000 <= vtable < 0x4000000:
                raise ValueError("Vtable outside guest memory")
            offset=0x800000; found=0; needle=struct.pack("<I",vtable)
            while found<64:
                offset=memory.find(needle,offset)
                if offset<0: break
                if offset%16==0 and offset+0xEC<=size:
                    print(f'class_actor={offset:08X} vtable={vtable:08X} E0={struct.unpack_from("<3f",memory,offset+0xE0)}')
                    found+=1
                offset+=4
        for name in args.name:
            value = 2166136261
            for byte in name.encode("ascii"):
                value = ((value ^ (byte | 0x20)) * 16777619) & 0xFFFFFFFF
            needle = struct.pack("<I", value)
            print(f"name={name} hash={value:08X}")
            offset = 0x10000 if args.raw_matches else 0x800000
            matches = 0
            while matches < 64:
                offset = memory.find(needle, offset)
                if offset < 0:
                    break
                actor = offset - 4
                if args.raw_matches:
                    prefix = struct.unpack_from("<I", memory, actor)[0]
                    position = struct.unpack_from("<3f", memory, actor + 0xE0) if actor + 0xEC <= size else ()
                    print(f"raw={actor:08X} prefix={prefix:08X} E0={position}")
                    matches += 1
                if actor % 16 == 0 and actor + 0x100 <= size:
                    vtable = struct.unpack_from("<I", memory, actor)[0]
                    if 0x2D0000 <= vtable <= 0x310000:
                        position = struct.unpack_from("<3f", memory, actor + 0xE0)
                        words = struct.unpack_from("<16I", memory, actor)
                        print(f"candidate={actor:08X} vtable={vtable:08X} E0={position}")
                        print("  prefix=" + " ".join(f"{word:08X}" for word in words))
                        matches += 1
                offset += 4
    finally:
        kernel.CloseHandle(process)


if __name__ == "__main__":
    main()

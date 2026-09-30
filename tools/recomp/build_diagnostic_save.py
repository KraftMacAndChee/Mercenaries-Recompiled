#!/usr/bin/env python3
"""Build a checksum-valid *diagnostic* Mercenaries save container.

This tool extracts the retail GameBlock object from a 64 MiB guest-memory
snapshot. It isolates the native file-I/O/load path from the in-game save
writer. Its output must not be treated as proof that the retail game saved.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


GAME_BLOCK_OBJECT_VA = 0x00432B3C
GAME_BLOCK_VTABLE = 0x002F5E3C
GAME_BLOCK_PAYLOAD_OFFSET = GAME_BLOCK_OBJECT_VA + 8

SCRIBBLE_SIZE = 0x400C
SPORE_SIZE = 0x9C5C
OPTIONS_SIZE = 0x54
GAME_PAYLOAD_SIZE = SCRIBBLE_SIZE + SPORE_SIZE + OPTIONS_SIZE

HEADER_DATA_SIZE = 0x160
HEADER_BLOCK_SIZE = 0x200
GAME_BLOCK_SIZE = 0xDE00
OPTIONS_BLOCK_SIZE = 0x200
SLOT_COUNT = 8
RETAIL_CONTAINER_SIZE = HEADER_BLOCK_SIZE + SLOT_COUNT * GAME_BLOCK_SIZE + OPTIONS_BLOCK_SIZE


def signed_char_fnv(data: bytes, *, skip: int = 4) -> int:
    """Match the retail MSVC build's FNV loop over signed ``char`` bytes."""

    value = 0x811C9DC5
    for byte in data[skip:]:
        signed = byte if byte < 0x80 else byte - 0x100
        value = ((value ^ (signed & 0xFFFFFFFF)) * 0x01000193) & 0xFFFFFFFF
    return value


def game_reference_crc(payload: bytes) -> int:
    if len(payload) != GAME_PAYLOAD_SIZE:
        raise ValueError(f"expected {GAME_PAYLOAD_SIZE} payload bytes, got {len(payload)}")

    scribble = payload[:SCRIBBLE_SIZE]
    spore = payload[SCRIBBLE_SIZE : SCRIBBLE_SIZE + SPORE_SIZE]
    options = payload[SCRIBBLE_SIZE + SPORE_SIZE :]
    return signed_char_fnv(scribble) ^ signed_char_fnv(spore) ^ signed_char_fnv(options)


def aligned_block(payload: bytes, block_size: int) -> bytes:
    if len(payload) + 4 > block_size:
        raise ValueError("payload does not fit aligned block")
    block = bytearray(block_size)
    block[4 : 4 + len(payload)] = payload
    struct.pack_into("<I", block, 0, signed_char_fnv(block))
    return bytes(block)


def extract_game_payload(snapshot: bytes, *, object_va: int = GAME_BLOCK_OBJECT_VA) -> bytes:
    payload_offset = object_va + 8
    if object_va < 0 or len(snapshot) < payload_offset + GAME_PAYLOAD_SIZE:
        raise ValueError("snapshot is too small to contain the retail GameBlock")
    (vtable,) = struct.unpack_from("<I", snapshot, object_va)
    if vtable != GAME_BLOCK_VTABLE:
        raise ValueError(
            f"GameBlock vtable mismatch: expected 0x{GAME_BLOCK_VTABLE:08X}, "
            f"found 0x{vtable:08X}"
        )
    return snapshot[payload_offset : payload_offset + GAME_PAYLOAD_SIZE]


def build_container(payload: bytes, *, slot_name: str = "JAC01", timestamp: bytes = bytes(8)) -> bytes:
    encoded_name = slot_name.encode("ascii")
    if not encoded_name or len(encoded_name) >= 32:
        raise ValueError("slot name must contain 1-31 ASCII bytes")
    if len(timestamp) != 8:
        raise ValueError("timestamp must contain exactly eight bytes")

    populated_reference = game_reference_crc(payload)
    empty_payload = bytes(GAME_PAYLOAD_SIZE)
    empty_reference = game_reference_crc(empty_payload)

    header_payload = bytearray(HEADER_DATA_SIZE)
    for slot in range(SLOT_COUNT):
        struct.pack_into("<I", header_payload, slot * 4, populated_reference if slot == 0 else empty_reference)
    header_payload[0x20 : 0x28] = timestamp
    header_payload[0x60 : 0x60 + len(encoded_name)] = encoded_name

    blocks = [aligned_block(header_payload, HEADER_BLOCK_SIZE)]
    blocks.append(aligned_block(payload, GAME_BLOCK_SIZE))
    empty_block = aligned_block(empty_payload, GAME_BLOCK_SIZE)
    blocks.extend([empty_block] * (SLOT_COUNT - 1))
    # The retail executable''s _GetTotalFileSize() includes one aligned
    # RsGameOptions block in addition to the overlapping header/game offsets.
    # Its _GetBlockOffset() still places header at zero and game slot 0 at
    # HEADER_BLOCK_SIZE, so the extra allocation is required trailing padding.
    blocks.append(bytes(OPTIONS_BLOCK_SIZE))
    return b"".join(blocks)


def validate_container(container: bytes) -> None:
    expected_size = RETAIL_CONTAINER_SIZE
    if len(container) != expected_size:
        raise ValueError(f"expected {expected_size} container bytes, got {len(container)}")

    header = container[:HEADER_BLOCK_SIZE]
    if struct.unpack_from("<I", header)[0] != signed_char_fnv(header):
        raise ValueError("header aligned CRC is invalid")

    for slot in range(SLOT_COUNT):
        offset = HEADER_BLOCK_SIZE + slot * GAME_BLOCK_SIZE
        block = container[offset : offset + GAME_BLOCK_SIZE]
        if struct.unpack_from("<I", block)[0] != signed_char_fnv(block):
            raise ValueError(f"slot {slot} aligned CRC is invalid")
        reference = game_reference_crc(block[4 : 4 + GAME_PAYLOAD_SIZE])
        if reference != struct.unpack_from("<I", header, 4 + slot * 4)[0]:
            raise ValueError(f"slot {slot} reference CRC does not match the header")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path, help="64 MiB guest-memory snapshot")
    parser.add_argument("output", type=Path, help="diagnostic 'Mercenaries Saves' output")
    parser.add_argument(
        "--game-block-va",
        type=lambda value: int(value, 0),
        default=GAME_BLOCK_OBJECT_VA,
        help="guest VA of the checksum-ready GameBlock object in the snapshot",
    )
    parser.add_argument("--slot-name", default="JAC01")
    parser.add_argument(
        "--timestamp-hex",
        default="0000000000000000",
        help="eight retail TimeOfDay bytes as 16 hexadecimal digits",
    )
    args = parser.parse_args()

    timestamp = bytes.fromhex(args.timestamp_hex)
    snapshot = args.snapshot.read_bytes()
    payload = extract_game_payload(snapshot, object_va=args.game_block_va)
    container = build_container(payload, slot_name=args.slot_name, timestamp=timestamp)
    validate_container(container)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(container)
    print(f"wrote diagnostic container: {args.output}")
    print(f"slot 0 reference CRC: 0x{game_reference_crc(payload):08X}")
    print(f"container bytes: {len(container)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

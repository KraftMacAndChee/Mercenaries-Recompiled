import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.recomp.build_diagnostic_save import (
    GAME_BLOCK_OBJECT_VA,
    GAME_BLOCK_PAYLOAD_OFFSET,
    GAME_BLOCK_VTABLE,
    GAME_PAYLOAD_SIZE,
    HEADER_BLOCK_SIZE,
    OPTIONS_BLOCK_SIZE,
    RETAIL_CONTAINER_SIZE,
    SLOT_COUNT,
    GAME_BLOCK_SIZE,
    build_container,
    extract_game_payload,
    game_reference_crc,
    signed_char_fnv,
    validate_container,
)


class DiagnosticSaveTests(unittest.TestCase):
    def test_signed_char_crc_differs_from_unsigned_for_high_bytes(self):
        data = bytes([0, 0, 0, 0, 0x80])
        self.assertEqual(signed_char_fnv(data), 0x04F3B29F)

    def test_build_and_validate_container(self):
        payload = bytes((index * 37) & 0xFF for index in range(GAME_PAYLOAD_SIZE))
        container = build_container(payload, timestamp=bytes.fromhex("2413141F0800EA07"))
        self.assertEqual(len(container), RETAIL_CONTAINER_SIZE)
        self.assertEqual(container[-OPTIONS_BLOCK_SIZE:], bytes(OPTIONS_BLOCK_SIZE))
        self.assertEqual(struct.unpack_from("<I", container, 4)[0], game_reference_crc(payload))
        validate_container(container)

    def test_extract_requires_retail_vtable(self):
        snapshot = bytearray(GAME_BLOCK_PAYLOAD_OFFSET + GAME_PAYLOAD_SIZE)
        struct.pack_into("<I", snapshot, GAME_BLOCK_OBJECT_VA, GAME_BLOCK_VTABLE)
        snapshot[GAME_BLOCK_PAYLOAD_OFFSET:] = bytes([0x5A]) * GAME_PAYLOAD_SIZE
        self.assertEqual(extract_game_payload(snapshot), bytes([0x5A]) * GAME_PAYLOAD_SIZE)

    def test_extract_accepts_explicit_heap_game_block(self):
        object_va = 0x40
        snapshot = bytearray(object_va + 8 + GAME_PAYLOAD_SIZE)
        struct.pack_into("<I", snapshot, object_va, GAME_BLOCK_VTABLE)
        snapshot[object_va + 8 :] = bytes([0xA5]) * GAME_PAYLOAD_SIZE
        self.assertEqual(
            extract_game_payload(snapshot, object_va=object_va),
            bytes([0xA5]) * GAME_PAYLOAD_SIZE,
        )


if __name__ == "__main__":
    unittest.main()

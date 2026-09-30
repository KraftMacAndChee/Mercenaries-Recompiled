"""Offline layout tests; never open a process or deliver input."""
import struct
import unittest
from sample_human_movement import decode_player


class MovementSampleTests(unittest.TestCase):
    def test_exact_human_layout(self):
        b=bytearray(0x400)
        struct.pack_into('<II',b,0,0x2E32B8,0x660E4490)
        struct.pack_into('<3f',b,0xE0,1,2,3)
        struct.pack_into('<f',b,0x98,98)
        struct.pack_into('<II',b,0x288,2,2)
        struct.pack_into('<2f',b,0x2B0,.6,.8)
        struct.pack_into('<2f',b,0x2C4,2,6)
        b[0x2E5]=1; b[0x324]=1
        struct.pack_into('<3f',b,0x334,.01,.02,.03)
        struct.pack_into('<3f',b,0x364,4,5,6)
        r=decode_player(b)
        self.assertEqual(r['position'],(1,2,3))
        self.assertEqual(r['health'],98)
        self.assertEqual((r['move_mode'],r['move_speed']),(2,2))
        self.assertAlmostEqual(r['run_stick'][0],.6)
        self.assertAlmostEqual(r['run_stick'][1],.8)
        self.assertEqual(r['authored_speeds'],(2,6))
        self.assertEqual((r['run_enabled'],r['human_controlled']),(1,1))
        self.assertAlmostEqual(r['physics_accumulated_dt'],.01)
        self.assertAlmostEqual(r['physics_min_update_dt'],.02)
        self.assertAlmostEqual(r['physics_last_accumulated_dt'],.03)
        self.assertEqual(r['physics_velocity'],(4,5,6))
        b[0]=0
        with self.assertRaises(ValueError):decode_player(b)

    def test_rejects_wrong_object_and_partial_read(self):
        for b in (bytes(0x400),bytes(0x3FF),bytes(0x401)):
            with self.assertRaises(ValueError):decode_player(b)


if __name__=='__main__':unittest.main()

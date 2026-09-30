"""Offline sampler layout/filter checks; no process or controller access."""
import math
import struct
import unittest
from sample_retail_airstrike import decode_actors


class AirstrikeSamplerTests(unittest.TestCase):
    def record(self, vtable, name=0):
        data=bytearray(0x300)
        struct.pack_into('<II',data,0,vtable,name)
        struct.pack_into('<I',data,0x34,0x1400000)
        struct.pack_into('<3f',data,0xE0,1518,55.65,540)
        return data

    def test_beacon_layout(self):
        m=self.record(0x2DD068)
        struct.pack_into('<IIf',m,0x204,0x12345678,2,45)
        m[0x210]=1;m[0x212]=1
        struct.pack_into('<f',m,0x214,3.5)
        struct.pack_into('<3f',m,0x234,1518,58,540)
        row=decode_actors(m,0x1400000)[0]
        self.assertEqual(row['aircraft_guid'],'12345678')
        self.assertEqual(row['fire_permission'],1)
        self.assertEqual(row['tracking_laser'],1)
        self.assertEqual(row['laser_position'],(1518,58,540))
        self.assertEqual(row['drift'],3.5)

    def test_artillery_health_and_filters(self):
        m=self.record(0x2E1C20,0x892949B9)
        struct.pack_into('<f',m,0x98,800)
        self.assertEqual(decode_actors(m,0x1400000)[0]['health'],800)
        struct.pack_into('<I',m,4,123)
        self.assertEqual(decode_actors(m,0x1400000),[])

    def test_derived_airplane_is_not_lost_or_conflated(self):
        for vtable in (0x2E1050,0x2E12E0):
            m=self.record(vtable,0x12345678)
            row=decode_actors(m,0x1400000)[0]
            self.assertEqual(row['kind'],'airplane')
            self.assertEqual(row['vtable'],f'{vtable:08X}')
            self.assertEqual(row['name_hash'],'12345678')
        m=self.record(0x2E1050)
        self.assertEqual(decode_actors(m,0x1400000)[0]['kind'],'airplane')
        self.assertEqual(decode_actors(m,0x1400001),[])
        self.assertEqual(decode_actors(m[:0x240],0x1400000),[])
        struct.pack_into('<f',m,0xE0,math.nan)
        self.assertEqual(decode_actors(m,0x1400000),[])


if __name__=='__main__':unittest.main()

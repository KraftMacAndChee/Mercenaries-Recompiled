import struct
import unittest
from inspect_retail_lua_snapshot import Snapshot


class SnapshotTests(unittest.TestCase):
    def fixture(self):
        m=bytearray(2048)
        struct.pack_into('<I',m,0x104,5)
        m[0x107]=1
        struct.pack_into('<I',m,0x110,0x200)
        struct.pack_into('<I',m,0x304,4)
        struct.pack_into('<I',m,0x30c,7)
        m[0x310:0x317]=b'chapter'
        struct.pack_into('<I',m,0x200,4)
        struct.pack_into('<I',m,0x208,0x300)
        struct.pack_into('<I',m,0x210,3)
        struct.pack_into('<d',m,0x218,1.0)
        return m

    def test_number_global(self):
        self.assertEqual(Snapshot(self.fixture()).globals(0x100),{'chapter':1.0})

    def test_nil_ignored(self):
        m=self.fixture();struct.pack_into('<I',m,0x210,0)
        self.assertEqual(Snapshot(m).globals(0x100),{})

    def test_bounds(self):
        with self.assertRaises(ValueError):Snapshot(self.fixture()).u32(2047)
        with self.assertRaises(ValueError):Snapshot(self.fixture()).read(-1,2)

    def test_corrupt_table(self):
        m=self.fixture();m[0x107]=31
        with self.assertRaises(ValueError):Snapshot(m).globals(0x100)

    def test_invalid_string(self):
        m=self.fixture();m[0x304]=5
        with self.assertRaises(ValueError):Snapshot(m).globals(0x100)


if __name__=='__main__':unittest.main()

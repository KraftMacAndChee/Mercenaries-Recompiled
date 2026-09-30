import struct
import unittest
import numpy as np
from decode_snapshot_bc1 import decode_bc1


class BC1Tests(unittest.TestCase):
    def test_four_color_palette(self):
        data=struct.pack('<HHI',0xf800,0x07e0,0xe4e4e4e4)
        image=decode_bc1(data,4,4)
        np.testing.assert_array_equal(image[0],[[255,0,0,255],[0,255,0,255],
                                              [170,85,0,255],[85,170,0,255]])
        np.testing.assert_array_equal(image[3],image[0])

    def test_transparency_and_partial_block(self):
        image=decode_bc1(struct.pack('<HHI',0,0xffff,0xe4e4e4e4),3,2)
        self.assertEqual(image.shape,(2,3,4))
        np.testing.assert_array_equal(image[0,2],[127,127,127,255])
        image=decode_bc1(struct.pack('<HHI',0,0xffff,0xffffffff),1,1)
        np.testing.assert_array_equal(image[0,0],[0,0,0,0])

    def test_block_order(self):
        colors=[0xf800,0x07e0,0x001f,0xffff]
        image=decode_bc1(b''.join(struct.pack('<HHI',c,0,0) for c in colors),8,8)
        np.testing.assert_array_equal(image[0,0],[255,0,0,255])
        np.testing.assert_array_equal(image[0,4],[0,255,0,255])
        np.testing.assert_array_equal(image[4,0],[0,0,255,255])
        np.testing.assert_array_equal(image[4,4],[255,255,255,255])

    def test_rejects_invalid_payloads(self):
        for data,w,h in [(b'',4,4),(bytes(9),4,4),(bytes(8),0,4),(bytes(8),8193,1)]:
            with self.assertRaises(ValueError):decode_bc1(data,w,h)


if __name__=='__main__':unittest.main()

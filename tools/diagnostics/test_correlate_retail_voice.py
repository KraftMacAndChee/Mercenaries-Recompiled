"""Synthetic and decoder checks for read-only retail waveform correlation."""
import sys
from pathlib import Path
import struct
import unittest
import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parent))
from correlate_retail_voice import best_match,decode_adpcm


class CorrelationTests(unittest.TestCase):
    def test_known_delay_gain_and_background(self):
        rng=np.random.default_rng(41)
        reference=rng.normal(size=500)
        recording=rng.normal(scale=.03,size=1800)+17
        recording[731:1231]+=.25*reference
        result=best_match(recording,reference)
        self.assertEqual(result['sample'],731)
        self.assertGreater(result['correlation'],.99)
        self.assertAlmostEqual(result['fitted_gain'],.25,delta=.005)

    def test_reject_silence_and_invalid_ranges(self):
        with self.assertRaises(ValueError):best_match(np.zeros(100),np.zeros(30))
        with self.assertRaises(ValueError):best_match(np.zeros(10),np.ones(30))
        with self.assertRaises(ValueError):decode_adpcm(b'\0'*35,1)
        with self.assertRaises(ValueError):decode_adpcm(b'\0'*36,3)
        with self.assertRaises(ValueError):decode_adpcm(b'\0\0\x59\0'+b'\0'*32,1)

    def test_production_block_decoder_mono_and_stereo(self):
        # Zero nibbles at the minimum step preserve the initial predictor.
        mono=struct.pack('<hBB',1234,0,0)+b'\0'*32
        decoded=decode_adpcm(mono*3,1)
        self.assertEqual(decoded.shape,(192,1))
        np.testing.assert_array_equal(decoded,np.full((192,1),1234))
        stereo=struct.pack('<hBBhBB',1234,0,0,-2345,0,0)+b'\0'*64
        decoded=decode_adpcm(stereo,2)
        np.testing.assert_array_equal(decoded,np.tile([1234,-2345],(64,1)))


if __name__=='__main__':unittest.main()

"""Ensure the waveform audit rejects stale handles and invalid bank slices."""
import sys
from pathlib import Path
import struct
import unittest
import numpy as np

sys.path.insert(0,str(Path(__file__).resolve().parent))
from audit_aircraft_voice_output import cue_metadata,wave_slice,search_bounds
from correlate_retail_voice import best_match


class AircraftAuditTests(unittest.TestCase):
    def test_fresh_metadata_and_trace_cap(self):
        names={0x1234:'example'}
        track='[XACT-SOUND-UPDATE] owner=1000 timing=04000000/00000000/00010000/000AC445 tail=00003BC4/0/1'
        cue='[XACT-CUE] stage=2 pCue=1000 result=00000000 hash=00001234 length=1.233'
        rows=list(cue_metadata(['[APU-SSL] ep=1500',track,cue]+['other']*33+[cue],names))
        self.assertEqual(rows[0]['wave'],1024)
        self.assertEqual(rows[0]['byte_length'],15300)
        self.assertEqual(rows[0]['epoch'],1500)
        self.assertIn('unresolved',rows[1])
        rows=list(cue_metadata([track,cue],names))
        self.assertIn('unresolved',rows[0])

    def test_bank_validation(self):
        bank=bytearray(140)
        struct.pack_into('<4sI8I',bank,0,b'WBND',3,40,40,80,24,0,0,104,36)
        struct.pack_into('<I',bank,44,1)
        struct.pack_into('<I',bank,64,24)
        struct.pack_into('<6I',bank,80,0x10000,0xAC445,0,36,0,0)
        self.assertEqual(wave_slice(bank,0,36,0xAC445),b'\0'*36)
        for index,length,fmt in ((1,36,0xAC445),(0,72,0xAC445),(0,36,0)):
            with self.assertRaises(ValueError):wave_slice(bank,index,length,fmt)
        with self.assertRaises(ValueError):wave_slice(bank[:-1],0,36,0xAC445)
        struct.pack_into('<I',bank,88,1000)
        with self.assertRaises(ValueError):wave_slice(bank,0,36,0xAC445)

    def test_clock_bracket_covers_sparse_summary(self):
        names={0x1234:'example'}
        track='[XACT-SOUND-UPDATE] owner=1000 timing=04000000/0/0/000AC445 tail=00003BC4/0/1'
        cue='[XACT-CUE] stage=2 pCue=1000 result=00000000 hash=00001234 length=1.233'
        row=list(cue_metadata(['[APU-PERF] ep=150000',track,cue,
                               '[APU-PERF] ep=151500'],names))[0]
        self.assertEqual(row['epoch_after'],151500)
        first,last=search_bounds(row,12000,6000000)
        self.assertEqual((first,last),(4776000,4884000))
        rng=np.random.default_rng(537)
        reference=rng.normal(size=12000)
        recording=rng.normal(scale=.01,size=last-first)
        offset=round(100.85*48000)-first
        recording[offset:offset+len(reference)]+=.25*reference
        self.assertEqual(best_match(recording,reference)['sample'],offset)
        # Negative control: the old +/-0.5s alignment window cannot see it.
        old=best_match(recording[:len(reference)+48000],reference)
        self.assertLess(abs(old['correlation']),.05)

    def test_missing_stale_reversed_and_truncated_clock(self):
        for row in ({'epoch':1500},{'epoch':1500,'epoch_after':1499},
                    {'epoch':1500,'epoch_after':4501},
                    {'epoch':-1,'epoch_after':0}):
            with self.assertRaises(ValueError):search_bounds(row,12000,1000000)
        with self.assertRaises(ValueError):
            search_bounds({'epoch':1500,'epoch_after':3000},12000,24000)


if __name__=='__main__':unittest.main()

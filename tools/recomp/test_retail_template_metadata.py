"""Check independently extracted retail template metadata and parser bounds."""
from pathlib import Path
import json
import struct
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.diagnostics.inspect_retail_templates import dsk_records,string_table,hash_string,vehicle_metadata

class RetailTemplateTests(unittest.TestCase):
    def test_existing_vehicle_metadata(self):
        retail=vehicle_metadata(ROOT/'game_files/mercenaries-retail/DATAxbox')
        existing=json.loads((ROOT/'ports/mercenaries/data/dev-vehicles.json').read_text())['vehicles']
        existing={row['template']:row for row in existing}
        self.assertEqual(len(retail),101)
        self.assertEqual({row['template'] for row in retail},set(existing))
        for row in retail:
            for field,value in row.items():
                self.assertEqual(value,existing[row['template']][field],(row['template'],field))
        # Footprints are deliberately absent: this test does not establish them.
        self.assertTrue(all('radius' not in row for row in retail))
    def test_string_table_bounds(self):
        word=b'GeometryFile';valid=struct.pack('<III',1,hash_string(word.decode()),len(word))+word+b'\0'
        self.assertEqual(string_table(valid),{hash_string(word.decode()):word.decode()})
        for end in range(len(valid)):
            with self.subTest(end=end):
                with self.assertRaises(ValueError):string_table(valid[:end])
        wrong=bytearray(valid);wrong[4]^=1
        for invalid in [bytes(wrong),valid+b'\0',valid[:-1]+b'x']:
            with self.assertRaises(ValueError):string_table(invalid)
    def test_archive_bounds(self):
        payload=b'example'
        header=struct.pack('<IIIII',1,0,len(payload),123,456)
        with tempfile.TemporaryDirectory(prefix='retail-dsk-') as temp:
            path=Path(temp)/'test.dsk'
            for padding in [b'',bytes(2048)]:
                path.write_bytes(header+payload+padding)
                self.assertEqual(list(dsk_records(path)),[(0,123,456,payload)])
            for bad in [b'',header[:-1],header+payload[:-1],header+payload+b'x',header+payload+bytes(2047)+b'x']:
                path.write_bytes(bad)
                with self.assertRaises(ValueError):list(dsk_records(path))

if __name__=='__main__':unittest.main()

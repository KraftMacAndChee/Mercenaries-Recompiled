"""Retail regeneration must preserve every developer troop entry and role field."""
from pathlib import Path
import json
import re
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.recomp.extract_dev_troop_catalog import extract

class TroopRegeneration(unittest.TestCase):
    def test_exact_current_catalogue_and_variants(self):
        catalog,header,evidence=extract(ROOT/'game_files/mercenaries-retail/DATAxbox')
        maintained=json.loads((ROOT/'ports/mercenaries/data/dev-troops.json').read_text())
        self.assertEqual(catalog['troops'],maintained['troops'])
        source=(ROOT/'ports/mercenaries/src/dev_troop_variants.h').read_text()
        if '#include "dev_troop_variant_data.inc"' in source:
            current=(ROOT/'ports/mercenaries/src/dev_troop_variant_data.inc').read_text()
        else:
            current=source.split('static const DevTroopVariant dev_troop_variants[] = {',1)[1].split('\n};',1)[0]
        def fields(text):
            return [re.findall(r'"[^"\n]*"|NULL',row) for row in re.findall(r'\{([^{}]*)\}',text)]
        self.assertEqual(fields(header),fields(current))
        self.assertEqual(len(catalog['troops']),20)
        self.assertEqual(len(fields(header)),4)
        self.assertEqual(len(evidence['records']),20)
        self.assertTrue(all(len(row)==13 for row in fields(header)))

if __name__=='__main__':unittest.main()

"""Retail-only replacement for the old harness."""
from pathlib import Path
import math
import sys
import unittest
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT))
from tools.diagnostics.retail_box_collision import RetailBoxCollision,IDENTITY,REPRO

class RetailBoxCollisionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):cls.oracle=RetailBoxCollision()
    def test_camera_reproduction(self):
        # Captured anew by retail x86 execution on 2026-09-29; see evidence doc.
        closest=self.oracle.closest(REPRO)
        cast=self.oracle.cast(REPRO)
        self.assertEqual(len(closest),1);self.assertEqual(len(cast),1)
        self.assertAlmostEqual(closest[0]['distance'],.3475233316421509,places=7)
        self.assertAlmostEqual(cast[0]['distance'],.5634562373161316,places=7)
        for hit in [closest[0],cast[0]]:
            self.assertEqual(hit['normal'],[0,0,-1])
            for a,b in zip(hit['position'],[2348.20703125,-1.7494362592697144,-3.830578565597534]):
                self.assertAlmostEqual(a,b,places=5)
    def case(self,axis=0,sign=1,offset=5,path_length=4,skin=0):
        translation=[0,0,0];translation[axis]=sign*offset
        path=[0,0,0];path[axis]=sign*path_length
        return dict(a_half=(1,1,1),b_half=(1,1,1),a_skin=skin,b_skin=skin,
                    a_transform=IDENTITY,b_transform=(*IDENTITY[:3],translation),path=path)
    def test_geometric_axis_cases(self):
        # Axis-aligned unit-box surface gap is known independently: d - 2.
        for axis in range(3):
            for sign in [-1,1]:
                for offset in [1,2,5]:
                    with self.subTest(axis=axis,sign=sign,offset=offset):
                        case=self.case(axis,sign,offset)
                        close=self.oracle.closest(case);cast=self.oracle.cast(case)
                        self.assertEqual(len(close),1);self.assertEqual(len(cast),1)
                        self.assertAlmostEqual(close[0]['distance'],offset-2,places=5)
                        self.assertAlmostEqual(cast[0]['distance'],max(0,offset-2)/4,places=5)
                        expected=[0,0,0];expected[axis]=-sign
                        self.assertEqual(close[0]['normal'],expected)
                for speed in [-4,0,1]:
                    self.assertEqual(self.oracle.cast(self.case(axis,sign,5,speed)),[])
                self.assertEqual(self.oracle.closest(self.case(axis,sign),tolerance=1),[])
    def test_skin_and_rigid_transform(self):
        case=self.case(skin=.125)
        self.assertAlmostEqual(self.oracle.closest(case)[0]['distance'],2.75,places=5)
        self.assertAlmostEqual(self.oracle.cast(case)[0]['distance'],2.75/4,places=5)
        baseline=self.oracle.closest(REPRO)[0]
        # Translate both bodies together: contact point translates, gap/normal do not.
        for delta in [(0,0,0),(12,-7,3),(-1024,128,1024)]:
            shifted=dict(REPRO)
            for side in ['a','b']:
                transform=REPRO[side+'_transform']
                shifted[side+'_transform']=(*transform[:3],tuple(x+y for x,y in zip(transform[3],delta)))
            result=self.oracle.closest(shifted)[0]
            self.assertAlmostEqual(result['distance'],baseline['distance'],delta=.0003)
            for a,b in zip(result['normal'],baseline['normal']):self.assertAlmostEqual(a,b,places=5)
            for a,b,d in zip(result['position'],baseline['position'],delta):self.assertAlmostEqual(a,b+d,delta=.0005)

if __name__=='__main__':unittest.main()

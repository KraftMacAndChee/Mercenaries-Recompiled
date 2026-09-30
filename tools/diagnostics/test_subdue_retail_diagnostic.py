"""Pure input-scheduling checks; no game/process/controller access."""
import math
import unittest
from subdue_retail_diagnostic import capture_buttons


class CaptureInputTests(unittest.TestCase):
    def test_bounded_use_range_and_button_order(self):
        for distance in (0,1,1.5,1.999):
            for cycle in range(10):
                for t,expected in ((0,4),(.15,4),(.4,0),(.7,8),(1,8),(1.2,0),(2.9,0)):
                    self.assertEqual(capture_buttons(distance,t+3*cycle),expected)
        for distance in (2,2.01,15,500):
            for t in (0,.2,.7,1,3):self.assertEqual(capture_buttons(distance,t),0)

    def test_invalid_values(self):
        for distance,elapsed in ((-1,0),(1,-1),(math.nan,0),(0,math.inf),(math.inf,0)):
            with self.assertRaises(ValueError):capture_buttons(distance,elapsed)


if __name__=='__main__':unittest.main()

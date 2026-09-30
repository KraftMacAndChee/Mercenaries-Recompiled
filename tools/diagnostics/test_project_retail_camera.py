import math
import unittest
from inspect_live_named_actors import project_world_point


class ProjectionTests(unittest.TestCase):
    def test_identity_and_translation(self):
        identity = (1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
        frustum = (1,1000,2,2)
        self.assertEqual(project_world_point(identity,frustum,(0,0,-10))[1],(.5,.5))
        self.assertEqual(project_world_point(identity,frustum,(10,10,-10))[1],(1,0))
        shifted = (*identity[:12],-10,-20,-30,1)
        self.assertEqual(project_world_point(shifted,frustum,(10,20,20))[1],(.5,.5))
        self.assertIsNone(project_world_point(identity,frustum,(0,0,10))[1])
        self.assertIsNone(project_world_point(identity,frustum,(0,0,-.5))[1])
        with self.assertRaises(ValueError):project_world_point(identity,frustum,(math.nan,0,0))
        with self.assertRaises(ValueError):project_world_point(identity,(1,1000,0,2),(0,0,-10))


if __name__ == '__main__':unittest.main()

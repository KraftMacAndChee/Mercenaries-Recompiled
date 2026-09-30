"""Pure coordinate tests: no process access, memory changes, or input delivery."""
import math
import unittest
import tempfile
from pathlib import Path
from walk_retail_diagnostic import stick_toward, capture_acknowledged, validate_waypoints, aim_errors, trigger_renewal_ms


class WalkingTests(unittest.TestCase):
    def test_trigger_is_bounded_and_expires(self):
        self.assertEqual(trigger_renewal_ms(0,6),1500)
        self.assertEqual(trigger_renewal_ms(5.5,6),500)
        self.assertEqual(trigger_renewal_ms(6,6),0)
        self.assertEqual(trigger_renewal_ms(7,6),0)
        self.assertEqual(trigger_renewal_ms(0,30),1500)
        self.assertEqual(trigger_renewal_ms(29.5,30),500)
        self.assertEqual(trigger_renewal_ms(30,30),0)
        for elapsed,duration in ((0,0),(0,30.1),(0,math.inf),(0,math.nan),(-1,1),(math.nan,1)):
            with self.assertRaises(ValueError):trigger_renewal_ms(elapsed,duration)

    def test_aim_errors_use_authored_crosshair(self):
        matrix=(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
        frustum=(1,1000,2,2)
        yaw,pitch,screen=aim_errors(matrix,frustum,(0,10/3,-10))
        self.assertAlmostEqual(yaw,0); self.assertAlmostEqual(pitch,0)
        self.assertAlmostEqual(screen[0],.5); self.assertAlmostEqual(screen[1],1/3)
        self.assertGreater(aim_errors(matrix,frustum,(10,0,-10))[0],0)
        self.assertLess(aim_errors(matrix,frustum,(-10,0,-10))[0],0)
        self.assertGreater(aim_errors(matrix,frustum,(0,10,-10))[1],0)
        self.assertAlmostEqual(abs(aim_errors(matrix,frustum,(0,0,10))[0]),math.pi)
        with self.assertRaises(ValueError):aim_errors(matrix,frustum,(0,0,0))

    def test_bounded_route_data(self):
        self.assertEqual(validate_waypoints([[1, 2], [3.5, -4]]), [(1, 2), (3.5, -4)])
        for bad in ([], {}, [[0, 0]] * 65, [[None, 1]], [[True, 1]],
                    [[0, math.nan]], [[0, math.inf]], [[10001, 0]],
                    [[1]], [[1, 2, 3]], [['123', 2]]):
            with self.assertRaises(ValueError): validate_waypoints(bad)

    def test_aim_uses_effective_zoomed_frustum(self):
        matrix=(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1)
        frustum=(1,1000,2,2)
        for zoom in (.5,1,1.5,2,4):
            point=(0,10/(3*zoom),-10)
            yaw,pitch,screen=aim_errors(matrix,frustum,point,zoom)
            self.assertAlmostEqual(yaw,0)
            self.assertAlmostEqual(pitch,0)
            self.assertAlmostEqual(screen[1],1/3)
        # The old unzoomed helper considers this zoomed laser ray too low.
        self.assertLess(aim_errors(matrix,frustum,(0,10/4.5,-10))[1],0)
        for zoom in (0,-1,math.nan,math.inf):
            with self.assertRaises(ValueError):
                aim_errors(matrix,frustum,(0,0,-10),zoom)
        for malformed in ((), (1,1000,2), (1,1000,2,2,2)):
            with self.assertRaises(ValueError):
                aim_errors(matrix,malformed,(0,0,-10),1.5)

    def test_capture_acknowledgment(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            self.assertFalse(capture_acknowledged(directory, 7))
            for name in ('gamepad-000006.bmp', 'gamepad-000007.bmp-bad.bmp',
                         'gamepad-000007.bmp-8.bmp.tmp'):
                (directory / name).touch()
            self.assertFalse(capture_acknowledged(directory, 7))
            (directory / 'gamepad-000007.bmp-1899.bmp').touch()
            self.assertTrue(capture_acknowledged(directory, 7))
            self.assertFalse(capture_acknowledged(directory, 8))
            (directory / 'gamepad-000008.bmp').touch()
            self.assertTrue(capture_acknowledged(directory, 8))

    def test_camera_relative_basis(self):
        for i in range(360):
            a=math.radians(i); f=(math.cos(a),math.sin(a)); r=(-f[1],f[0])
            player=(1500.,600.); camera=(player[0]-f[0]*3,player[1]-f[1]*3)
            for direction,want in ((f,(0,32700)),(r,(32700,0)),((-f[0],-f[1]),(0,-32700))):
                target=(player[0]+direction[0]*20,player[1]+direction[1]*20)
                x,y,d=stick_toward(player,camera,target)
                self.assertEqual((x,y),want);self.assertAlmostEqual(d,20)
    def test_invalid_camera_and_arrival(self):
        self.assertEqual(stick_toward((1,2),(1,0),(1,2)),(0,0,0))
        for camera in ((1,2),(math.nan,2),(math.inf,2)):
            with self.assertRaises(ValueError):stick_toward((1,2),camera,(5,5))


if __name__ == '__main__':unittest.main()

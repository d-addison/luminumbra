"""Numerical acceptance for image/depth alignment; does not qualify Blender GPU output."""
import importlib.util
import math
from pathlib import Path
import sys
import types
import unittest

root = Path(__file__).parents[1] / 'extension'
package = types.ModuleType('adapter_math_fixture')
package.__path__ = [str(root)]
sys.modules[package.__name__] = package
from adapter_math_fixture import viewport_adapter_state as adapter, viewport_math as vm


def perspective(width, height):
    near, far, scale = .1, 100., 2.41421356237
    return (scale * height / width, 0, 0, 0, 0, scale, 0, 0,
            0, 0, -(far + near) / (far - near), -1, 0, 0, -2 * near * far / (far - near), 0)


def ortho(width, height):
    return (.5 * height / width, 0, 0, 0, 0, .5, 0, 0,
            0, 0, -.01, 0, -.2, .15, 0, 1)


class AdapterMath(unittest.TestCase):
    def aligned(self, projection, width, height, points):
        # A rotated, translated Blender view catches missing basis conversion.
        view = (0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, .2, -.3, -2, 1)
        result = adapter.camera(view, projection, width, height)
        x, y, drawn_width, drawn_height = result['rectangle']
        self.assertLessEqual(result['width'], 1280)
        self.assertLessEqual(result['height'], 720)
        for point in points:
            blender = vm.transform(vm.multiply(projection, view), point)
            host = vm.transform(vm.multiply(result['projection'], result['view']), vm.transform(vm.BASIS, point))
            host_ndc = tuple(v / host[3] for v in host)
            actual = (x + (host_ndc[0] + 1) * drawn_width / 2, y + (host_ndc[1] + 1) * drawn_height / 2)
            expected = ((blender[0] / blender[3] + 1) * width / 2, (blender[1] / blender[3] + 1) * height / 2)
            for a, b in zip(actual, expected):
                self.assertAlmostEqual(a, b, delta=.0002)
            depth = vm.transform(result['depth_transform'], host_ndc)
            self.assertAlmostEqual(depth[2] / depth[3], blender[2] / blender[3], places=9)
            self.assertGreater(host_ndc[2], 0)
            self.assertLessEqual(host_ndc[2], 1)

    def test_large_odd_perspective_region_keeps_exact_overlay_pixels_and_depth(self):
        for width, height in ((1919, 1079), (1215, 891), (337, 229), (3840, 1600)):
            with self.subTest(extent=(width, height)):
                self.aligned(perspective(width, height), width, height, ((.1, .2, 0, 1), (-.7, 1, -10, 1)))

    def test_orthographic_negative_near_and_shifted_center_keep_overlay_pixels(self):
        for width, height in ((1919, 1079), (1215, 891), (640, 480)):
            with self.subTest(extent=(width, height)):
                self.aligned(ortho(width, height), width, height, ((.1, .2, 20, 1), (-.7, 1, -20, 1)))

    def test_unsupported_or_unbounded_view_fails_before_submission(self):
        for index, value in ((10, -1), (11, 1), (8, .1), (0, 0)):
            bad = list(perspective(640, 480)); bad[index] = value
            with self.subTest(index=index), self.assertRaises(vm.UnsupportedView):
                adapter.camera(vm.IDENTITY, bad, 640, 480)
        for width, height in ((0, 10), (32769, 10), (10, True)):
            with self.assertRaises(vm.UnsupportedView):
                adapter.camera(vm.IDENTITY, perspective(640, 480), width, height)

    def test_generation_roundtrip_camera_and_scene_revisions_do_not_reset(self):
        revisions = adapter.Revisions()
        cam = adapter.camera(vm.IDENTITY, perspective(640, 480), 640, 480)
        a = dict(generation_id='a' * 32, manifest_sha256='a' * 64)
        b = dict(generation_id='b' * 32, manifest_sha256='b' * 64)
        first = revisions.state(a, cam, [])
        unchanged = revisions.state(a, cam, [])
        second = revisions.state(b, cam, [])
        third = revisions.state(a, cam, [])
        moved = dict(cam, view=list(cam['view'])); moved['view'][12] += 1
        fourth = revisions.state(a, moved, [])
        self.assertEqual(first, unchanged)
        self.assertEqual([x['scene_revision'] for x in (first, second, third, fourth)], [0, 1, 2, 2])
        self.assertEqual([x['camera_revision'] for x in (first, second, third, fourth)], [0, 0, 0, 1])

    def test_same_host_resolution_resize_requires_a_fresh_presentation_rectangle(self):
        generation = dict(generation_id='a' * 32, manifest_sha256='b' * 64)
        revisions = adapter.Revisions()
        large = adapter.camera(vm.IDENTITY, perspective(3840, 2160), 3840, 2160)
        small = adapter.camera(vm.IDENTITY, perspective(1920, 1080), 1920, 1080)
        self.assertEqual(revisions.state(generation, large, []), revisions.state(generation, small, []))
        for actual, expected in ((large['rectangle'], (0., 0., 3840., 2160.)),
                                 (small['rectangle'], (0., 0., 1920., 1080.))):
            for a, b in zip(actual, expected):
                self.assertAlmostEqual(a, b, places=9)


if __name__ == '__main__':
    unittest.main()

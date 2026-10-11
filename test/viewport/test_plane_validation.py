"""The vectorized decoder must refuse exactly what the portable decoder refuses."""
import hashlib
from pathlib import Path
import random
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/blender/authoring/viewport'))
import protocol as wire


class PlaneValidation(unittest.TestCase):
    def test_numpy_and_portable_agree_on_boundary_and_corrupted_planes(self):
        numpy = wire._numpy
        if numpy is None:
            self.skipTest('NumPy acceleration is not installed')
        matrix = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
        header = {'kind': 'frame', 'session': 'a'*32, 'sequence': 1, 'state': {
            'generation_id': 'b'*32, 'manifest_sha256': 'c'*64,
            'scene_revision': 0, 'camera_revision': 0, 'width': 32, 'height': 2,
            'near_plane': .1, 'far_plane': 1000., 'view': matrix,
            'projection': matrix, 'locals': []}, 'planes_sha256': ''}
        rng = random.Random(713)
        examples = []
        values = [0., -0., 1., 1e-40, .5, -1., 1.00001, float('nan'),
                  float('inf'), -float('inf')]
        for target in values:
            depth = [rng.random() for _ in range(64)]
            depth[27] = target
            covered = bytes(int(value > 0) for value in depth)
            rgba = bytearray(bytes((20, 40, 60, 255)) * 64)
            rgba[3::4] = covered.translate(bytes([0, 255]) + bytes(254))
            original = bytes(rgba) + struct.pack('<64f', *depth) + covered
            examples.append(original)
            for offset, replacement in ((8*64+27, 2), (8*64+27, 0), (4*27+3, 0)):
                changed = bytearray(original)
                changed[offset] = replacement
                examples.append(bytes(changed))
        try:
            for payload in examples:
                header['planes_sha256'] = hashlib.sha256(payload).hexdigest()
                decisions = []
                for backend in (None, numpy):
                    wire._numpy = backend
                    try:
                        wire.validate(header, payload)
                        decisions.append('accepted')
                    except wire.Refusal as error:
                        decisions.append(str(error))
                self.assertEqual(decisions[0], decisions[1])
        finally:
            wire._numpy = numpy


if __name__ == '__main__':
    unittest.main()

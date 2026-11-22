#!/usr/bin/env python3
"""Synthetic labelled temporal SVG evidence, using the existing montage builder."""
import builtins
import contextlib
import io
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parent
FIXTURE = runpy.run_path(str(HERE / 'test_temporal_capture.py'))
M = runpy.run_path(str(HERE / 'build-visual-sweep-montages.py'))


class TemporalMontages(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='temporal_montages_')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.seq = FIXTURE['Sequence'](self.root)
        self.output = self.root / 'worst.svg'

    def build(self):
        report = M['temporal_montage'](self.seq.path, self.output, self.seq.rois)
        source = self.output.read_text(encoding='utf-8')
        labels = '\n'.join(element.text or '' for element in ET.fromstring(source).iter('{http://www.w3.org/2000/svg}text'))
        return report, source, labels

    def test_labelled_failure_slots_neighbors_delta_mask_and_probes(self):
        FIXTURE['synthetic_baseline'](self.seq, 'delta')
        for frame in self.seq.frames:
            frame['temporal'].update(site='<site&>', tod='dawn', weather='clear')
        self.seq.frames[1]['light'][0] = 1.
        self.seq.write()
        report, source, labels = self.build()
        for label in ('temporal fail', 'terrain / delta / qualified qualified outcome=fail',
                      'frame=1 tick=102+0/60', 'site=<site&>', 'camera=', 'TOD=dawn weather=clear',
                      'before', 'current', 'after', 'raw delta', 'static attribution mask',
                      'draw/asset probes: draw=1', 'res/terrain0.glb pass=1'):
            self.assertIn(label, labels)
        self.assertIn('&lt;site&amp;&gt;', source)
        event = next(r for r in report['results'] if r['metric'] == 'delta')['events'][0]
        self.assertEqual([event['preview'][k]['frame_index'] for k in ('before', 'current', 'after')], [0, 1, 2])
        self.assertEqual(event['preview']['before']['pixels'][0][0], [.25]*3)
        self.assertEqual(event['preview']['current']['pixels'][0][0], [1.]*3)
        self.assertIn('fill="#bfbfbf"', source)  # .75 raw delta, not a fabricated map.
        self.assertIn('fill="#ffffff"', source)  # Static attribution mask.
        self.output.unlink()
        _, second, _ = self.build()
        self.assertEqual(source, second)

    def test_unavailable_qualified_and_endpoint_slots_are_labelled(self):
        FIXTURE['synthetic_baseline'](self.seq, 'pops')
        for frame in self.seq.frames:
            frame['temporal'].pop('smooth_luminance')
        self.seq.write()
        _, _, labels = self.build()
        for label in ('pops / qualified ineligible outcome=unavailable', 'UNAVAILABLE:',
                      'before: UNAVAILABLE', 'current: UNAVAILABLE', 'after: UNAVAILABLE',
                      'site=unknown', 'TOD=unknown weather=unknown'):
            self.assertIn(label, labels)
        self.assertIn('frame=0 tick=100', labels)  # First seam event has no previous frame.
        self.assertIn('frame=2 tick=104', labels)  # Last continuity event has no next frame.

    def test_flicker_montage_uses_actual_peak_and_bounded_payloads(self):
        self.seq = FIXTURE['Sequence'](self.root, 120, True)
        FIXTURE['synthetic_baseline'](self.seq, 'flicker')
        self.seq.frames[60]['light'][0] = 1.
        self.seq.frames[61]['mask'] = False  # The neighbor exists despite lost producer coverage.
        self.seq.write()
        report, _, labels = self.build()
        record = next(r for r in report['results'] if r['metric'] == 'flicker')
        self.assertEqual(record['outcome'], 'fail')
        event = record['events'][0]
        self.assertEqual(event['frame_index'], 60)
        self.assertEqual([event['preview'][k]['frame_index'] for k in ('before', 'current', 'after')], [59, 60, 61])
        self.assertIn('frame=60 tick=100', labels)
        for record in report['results']:
            for event in record['events']:
                for preview in event['preview'].values():
                    if preview is not None:
                        self.assertLessEqual(len(preview['pixels']), 9)
                        self.assertTrue(all(len(row) <= 9 for row in preview['pixels']))

    def test_refusals_preserve_output_and_never_publish_invalid_sequence(self):
        self.output.write_text('prior review', encoding='utf-8')
        with self.assertRaises(FileExistsError):
            self.build()
        self.assertEqual(self.output.read_text(encoding='utf-8'), 'prior review')
        self.output.unlink()
        payload = self.root / 'frame-0.ppm.lighting.pfm'
        payload.write_bytes(b'partial')
        with self.assertRaises(ValueError):
            self.build()
        self.assertFalse(self.output.exists())
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(M['main'](['--temporal', str(self.seq.path), '--rois', str(self.seq.rois),
                                        '--output', str(self.output)]), 2)
        self.assertFalse(self.output.exists())
        for arguments in (['--temporal', str(self.seq.path)],
                ['--temporal', str(self.seq.path), '--diagnostics', str(self.seq.path),
                 '--output', str(self.output)]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as refused:
                M['main'](arguments)
            self.assertEqual(refused.exception.code, 2)
            self.assertFalse(self.output.exists())

    def test_stdlib_cli_and_registration_and_failure_slot_cap(self):
        original_import = builtins.__import__
        def stdlib_only(name, *args, **kwargs):
            if name == 'PIL' or name.startswith('PIL.'):
                raise AssertionError('temporal SVG must not import Pillow')
            return original_import(name, *args, **kwargs)
        with mock.patch('builtins.__import__', side_effect=stdlib_only):
            self.build()
        self.output.unlink()
        process = subprocess.run([sys.executable, '-I', '-B', str(HERE / 'build-visual-sweep-montages.py'),
            '--temporal', str(self.seq.path), '--output', str(self.output)], capture_output=True, text=True)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIn('full / delta / advisory', self.output.read_text(encoding='utf-8'))
        self.output.unlink()
        FIXTURE['synthetic_baseline'](self.seq, 'delta')
        self.seq.frames[1]['light'][0] = 1.
        self.seq.write()
        process = subprocess.run([sys.executable, '-I', '-B', str(HERE / 'build-visual-sweep-montages.py'),
            '--temporal', str(self.seq.path), '--rois', str(self.seq.rois), '--output', str(self.output)],
            capture_output=True, text=True)
        self.assertEqual(process.returncode, 1, process.stderr)
        self.assertTrue(self.output.exists())
        self.output.unlink()
        self.seq.regions = [dict(name=f'roi-{i:02}', rect=[0, 0, 1, 1], metrics=self.seq.regions[0]['metrics'])
                            for i in range(64)]
        self.seq.write()
        report, _, labels = self.build()
        self.assertIn('slots=40/', labels)
        self.assertEqual(labels.count('outcome='), 40)
        self.assertEqual(labels.count('frame='), sum(len(r['events']) for r in report['results']))
        root = HERE.parents[1]
        paths = runpy.run_path(str(root / 'tools/ci/ci_paths.py'))
        test = 'tools/gates/test_temporal_montages.py'
        self.assertEqual(paths['PYTHON_TESTS'].count(test), 1)
        self.assertTrue(paths['needs_python'](test))
        self.assertIn(test, (root / 'test/CMakeLists.txt').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()

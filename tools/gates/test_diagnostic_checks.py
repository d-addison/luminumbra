#!/usr/bin/env python3
"""Same-frame synthetic evidence and isolated defect oracles for capture checks."""
import copy
import io
import json
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

FIXTURE = runpy.run_path(str(Path(__file__).with_name('test_diagnostic_capture.py')))
checker, Bundle = FIXTURE['checker'], FIXTURE['Bundle']
KINDS = tuple(checker.CHECK_CHANNELS) + ('weather',)
# Keep the contract independent of the implementation's enumeration for mutations.
EXPECTED_KINDS = ('holes', 'seam', 'boundary', 'card', 'banding', 'wedge',
                  'detail', 'weather_delta', 'weights', 'composition', 'weather')
WIDTH, HEIGHT = 8 * len(EXPECTED_KINDS), 8
MONTAGE = Path(__file__).with_name('build-visual-sweep-montages.py')


class CaptureChecks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bundle = Bundle(self.root)
        self.bundle.meta.update(schema=3, width=WIDTH, height=HEIGHT)
        self.bundle.table['draws'][0]['lod'] = 0
        self.values = {c: [(0.,) * n] * (WIDTH * HEIGHT)
                       for c, n in dict(checker.PLANES, **checker.CONTRIBUTIONS).items()}
        self.ids = [(1, 1, 0, 259)] * (WIDTH * HEIGHT)
        for y in range(HEIGHT):
            for x in range(WIDTH):
                index = y * WIDTH + x
                color = .25 + (.02 * (x % 2) if x // 8 == EXPECTED_KINDS.index('detail') else 0)
                for channel in ('albedo', 'reference'):
                    self.values[channel][index] = (color,) * 3
                self.values['dry_reference'][index] = (color - .02,) * 3
                self.values['normal'][index] = (0., 1., 0.)
                for channel in ('depth', 'fog', 'cloud', 'cutout'):
                    self.values[channel][index] = (.5,)
                for channel in ('expected_mask', 'material', 'transmittance'):
                    self.values[channel][index] = (1.,)
                self.values['alpha'][index] = (float(x % 2),)
                self.values['depth_written'][index] = (float(x % 2),)
                self.values['triplanar'][index] = (.25, .25, .5)
        self.regions = []
        for index, kind in enumerate(EXPECTED_KINDS):
            channel = dict(holes='ids', seam='albedo', boundary='albedo', card='alpha',
                           banding='albedo', wedge='albedo', detail='albedo',
                           weather_delta='albedo', weights='triplanar', composition='stars',
                           weather='fog')[kind]
            bounds = ([.01, .99] if kind == 'card' else [.01, 1] if kind in ('detail', 'weather_delta')
                      else [0, 1] if kind == 'weather' else [0, .005] if kind == 'seam'
                      else [0, 8/255] if kind == 'boundary' else [0, .001] if kind == 'weights'
                      else [0, 0] if kind in ('holes', 'composition') else [0, .02])
            metric = dict(kind=kind, channel=channel, thresholds=bounds)
            if kind != 'weather':
                metric['mask'] = 'expected_mask'
            if kind in ('banding', 'boundary', 'detail'):
                metric['axis'] = 'x'
            if kind == 'boundary':
                metric.update(direction=1, p99_limit=8/255)
            if kind == 'seam':
                metric['p99_limit'] = .02
            self.regions.append(dict(name=kind, rect=[index/len(EXPECTED_KINDS), 0,
                                                     (index+1)/len(EXPECTED_KINDS), 1], metrics=[metric]))
        self.config = self.root / 'rois.json'
        self.write()

    def write(self):
        b = self.bundle
        b.path('.probe-1-0.json').unlink(missing_ok=True)
        b.meta['encodings'] = dict(checker.ENCODINGS, **checker.CONTRIBUTION_ENCODINGS)
        planes = dict(checker.PLANES, **checker.CONTRIBUTIONS)
        for channel, channels in planes.items():
            data = bytearray()
            for y in range(HEIGHT - 1, -1, -1):
                for sample in self.values[channel][y * WIDTH:(y + 1) * WIDTH]:
                    data.extend(struct.pack('<' + 'f' * channels, *sample))
            b.path('.' + channel + '.pfm').write_bytes(
                ('PF\n' if channels == 3 else 'Pf\n').encode() +
                f'{WIDTH} {HEIGHT}\n-1.0\n'.encode() + data)
        b.path('.ids.bin').write_bytes(struct.pack('<4sIII', b'LVID', 1, WIDTH, HEIGHT) +
                                      b''.join(struct.pack('<4I', *p) for p in self.ids))
        b.json('.draws.json', b.table)
        (self.root / 'view.manifest.json').write_text(json.dumps(dict(
            schema=1, status='complete', resolution=dict(width=WIDTH, height=HEIGHT),
            camera={k: b.meta['camera'][k] for k in ('view', 'projection')})), encoding='utf-8')
        b.meta['files'] = ['view.png.' + c + '.pfm' for c in checker.PLANES]
        b.meta['files'] += ['view.png.ids.bin', 'view.png.draws.json']
        b.meta['files'] += ['view.png.' + c + '.pfm' for c in checker.CONTRIBUTIONS]
        b.meta['files'].append('view.png.diagnostics.json')
        self.config.write_text(json.dumps(dict(schema=1, rois=self.regions)), encoding='utf-8')
        b.finish()

    def measure(self):
        records = []
        findings = checker.validate(*checker.discover(self.bundle.path(checker.SUFFIX))[0],
                                    rois=checker.load_rois(self.config), metrics=records)
        return findings, records

    def inject(self, kind):
        x, y = EXPECTED_KINDS.index(kind) * 8 + 4, 4
        index = y * WIDTH + x
        if kind == 'holes':
            self.ids[index] = (0, 0, 0, 0)
        elif kind in ('seam', 'boundary', 'banding', 'wedge'):
            if kind != 'boundary':
                self.values['albedo'][index] = (1.,) * 3
            if kind in ('seam', 'banding'):
                for py in range(HEIGHT):
                    self.values['albedo'][py * WIDTH + x] = (1.,) * 3
            elif kind == 'wedge':
                for py in range(4):
                    for px in range(py + 1):
                        self.values['albedo'][(py + 2) * WIDTH + x - 2 + px] = (1.,) * 3
            elif kind == 'boundary':
                for py in range(HEIGHT):
                    self.values['albedo'][py * WIDTH + x - 4] = (1.,) * 3
        elif kind == 'card':
            self.values['depth_written'][index] = (1.,)
        elif kind == 'detail':
            for py in range(HEIGHT):
                for px in range(x - 4, x + 4):
                    self.values['albedo'][py * WIDTH + px] = (.25,) * 3
        elif kind == 'weather_delta':
            for py in range(HEIGHT):
                for px in range(x - 4, x + 4):
                    self.values['albedo'][py * WIDTH + px] = self.values['dry_reference'][py * WIDTH + px]
        else:
            channel = dict(weights='triplanar', composition='stars', weather='fog')[kind]
            self.values[channel][index] = (2.,) * len(self.values[channel][index])
        self.write()
        return x, y

    def test_good_capture_passes_every_metric(self):
        self.assertEqual(KINDS, EXPECTED_KINDS)
        findings, records = self.measure()
        self.assertEqual(findings, [])
        self.assertEqual(len(records), len(EXPECTED_KINDS))
        self.assertTrue(all(r['status'] == 'pass' for r in records), records)
        self.assertTrue(all(r['qualification'] == 'advisory' for r in records))
        original_metrics = [dict(kind='coverage', channel='ids', field=field, value=value,
                                 thresholds=[1, 1])
                            for field, value in (('background', 0), ('draw', 1), ('mesh', 1),
                                                 ('lod', 0), ('ownership', 3), ('pass', 1))]
        original_metrics += [dict(kind='validity', channel=c, thresholds=[1, 1]) for c in checker.PLANES]
        original_metrics += [dict(kind='edge', channel='albedo', axis=a, thresholds=[0, 0])
                             for a in ('x', 'y')]
        self.regions.append(dict(name='original-checks', rect=self.regions[-1]['rect'], metrics=original_metrics))
        self.write()
        findings, records = self.measure()
        self.assertEqual(findings, [])
        self.assertEqual(len(records), len(EXPECTED_KINDS) + len(original_metrics))
        self.assertTrue(all(r['status'] == 'pass' for r in records), records)

    def check_defect(self, kind):
        x, y = self.inject(kind)
        findings, records = self.measure()
        self.assertEqual(findings, [])  # Advisory failures never block bundle validity.
        failed = [r for r in records if r['status'] != 'pass']
        self.assertEqual([r['roi'] for r in failed], [kind], records)
        expected = ([x - 4, 0] if kind in ('detail', 'weather_delta', 'boundary') else
                    [x, 0] if kind == 'seam' else [x - 1, 0] if kind == 'banding' else
                    [x - 2, 2] if kind == 'wedge' else
                    [x, y])
        self.assertEqual(failed[0]['worst_pixel'], expected)
        output = self.root / 'worst.svg'
        result = subprocess.run([sys.executable, '-I', '-B', str(MONTAGE), '--diagnostics',
                                 str(self.bundle.path(checker.SUFFIX)), '--rois', str(self.config),
                                 '--output', str(output)], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        document = ET.fromstring(output.read_text(encoding='utf-8'))
        labels = ''.join(document.itertext())
        self.assertIn(f'{kind} / {kind} /', labels)
        self.assertIn('advisory fail value=', labels)
        self.assertIn(f'worst pixel: {expected}', labels)
        self.assertIn('stroke="yellow"', output.read_text(encoding='utf-8'))
        if kind != 'holes':
            self.assertIn('draw=1 pass=1 asset=res/mesh.glb', labels)
        else:
            self.assertEqual(failed[0]['worst_probe'], dict(id=[0, 0, 0, 0], background=True))

    def direct(self, kind, omit=(), regions=None):
        streams, offsets = {}, {}
        for channel, samples in dict(self.values, ids=self.ids).items():
            if channel in omit:
                continue
            channels = len(samples[0])
            rows = [samples[y * WIDTH:(y + 1) * WIDTH] for y in range(HEIGHT)]
            if channel != 'ids':
                rows.reverse()
            streams[channel] = io.BytesIO(b''.join(
                struct.pack('<' + ('I' if channel == 'ids' else 'f') * channels, *p)
                for row in rows for p in row))
            offsets[channel] = 0
        selected = regions or [r for r in self.regions if r['name'] == kind]
        return checker.image_metrics(streams, offsets, WIDTH, HEIGHT, selected)[0]

    def test_every_required_plane_is_required(self):
        requirements = dict(holes=('ids',), seam=('reference',),
                            boundary=('ids', 'depth'), card=('cutout', 'depth_written'),
                            banding=('reference',), wedge=('reference',),
                            detail=('ids', 'depth', 'precipitation'),
                            weather_delta=('dry_reference',), weights=('triplanar',),
                            composition=('transmittance',))
        for kind, channels in requirements.items():
            for channel in channels:
                with self.subTest(kind=kind, channel=channel):
                    record = self.direct(kind, omit=(channel,))
                    self.assertEqual((record['status'], record['value'], record['samples']),
                                     ('unavailable', None, 0))

    def test_nonfinite_inputs_never_pass_including_neighbor_references(self):
        for kind in EXPECTED_KINDS[:-1]:
            original = copy.deepcopy(self.values)
            region = next(r for r in self.regions if r['name'] == kind)
            channel = region['metrics'][0]['channel']
            if channel == 'ids':
                continue  # Integer identity validation belongs to the bundle reader.
            index = 4 * WIDTH + EXPECTED_KINDS.index(kind) * 8 + 4
            self.values[channel][index] = (float('nan'),) * len(self.values[channel][index])
            self.assertEqual(self.direct(kind)['status'], 'unavailable', kind)
            self.values = original
        index = EXPECTED_KINDS.index('banding') * 8 + 1
        self.values['reference'][index] = (float('inf'),) * 3
        self.assertEqual(self.direct('banding')['status'], 'unavailable')

    def test_invalid_masks_scalars_and_weather_refuse_or_are_unavailable(self):
        for value in (.5, -1, float('nan')):
            self.values['expected_mask'][0] = (value,)
            with self.assertRaises(checker.Invalid):
                self.direct('holes')
        self.values['expected_mask'][0] = (1.,)
        for channel, value in (('alpha', -1), ('cutout', 2), ('depth_written', .5),
                               ('depth', -1), ('depth', float('nan')),
                               ('precipitation', -1), ('precipitation', float('nan'))):
            kind = 'card' if channel in ('alpha', 'cutout', 'depth_written') else 'detail'
            index = EXPECTED_KINDS.index(kind) * 8
            original = self.values[channel][index]
            self.values[channel][index] = (value,)
            self.assertEqual(self.direct(kind)['status'], 'unavailable', channel)
            self.values[channel][index] = original

    def test_hole_erosion_and_empty_small_roi(self):
        self.ids[0] = (0, 0, 0, 0)
        self.assertEqual(self.direct('holes')['status'], 'pass')
        self.ids[2 * WIDTH + 2] = (0, 0, 0, 0)
        self.values['expected_mask'][WIDTH + 1] = (0.,)
        self.assertEqual(self.direct('holes')['status'], 'pass')
        self.values['expected_mask'][WIDTH + 1] = (1.,)
        self.assertEqual(self.direct('holes')['worst_pixel'], [2, 2])
        region = copy.deepcopy(self.regions[0])
        region['rect'] = [0, 0, 1/WIDTH, 1/HEIGHT]
        self.assertEqual(self.direct('holes', regions=[region])['status'], 'unavailable')

    def test_mask_erosion_reads_rows_once_and_ties_choose_top_left(self):
        with mock.patch.object(checker, 'pixels_row', wraps=checker.pixels_row) as rows:
            result = self.direct('holes')
        self.assertEqual(result['samples'], 16)
        self.assertEqual(result['worst_pixel'], [2, 2])
        self.assertLessEqual(rows.call_count, HEIGHT * 2)
        self.ids[2 * WIDTH + 2] = (0, 0, 0, 0)
        self.ids[2 * WIDTH + 3] = (0, 0, 0, 0)
        self.ids[3 * WIDTH + 2] = (0, 0, 0, 0)
        self.assertEqual(self.direct('holes')['worst_pixel'], [2, 2])

    def test_boundary_axes_directions_and_silhouette_rejection(self):
        region = copy.deepcopy(self.regions[2])
        for axis in ('x', 'y'):
            for direction in (-1, 1):
                region['metrics'][0].update(axis=axis, direction=direction)
                result = self.direct('boundary', regions=[region])
                self.assertEqual(result['status'], 'pass')
                self.assertEqual(result['value'], 0)
        self.inject('boundary')
        start = EXPECTED_KINDS.index('boundary') * 8
        for y in range(HEIGHT):
            for x in range(start, start + 8):
                self.ids[y * WIDTH + x] = (0, 0, 0, 0)
        self.assertEqual(self.direct('boundary')['status'], 'unavailable')
        for y in range(HEIGHT):
            for x in range(start, start + 8):
                self.ids[y * WIDTH + x] = (1, 1, 0, 259)
                self.values['depth'][y * WIDTH + x] = (x / WIDTH,)
        self.assertEqual(self.direct('boundary')['status'], 'unavailable')

    def test_vertical_bands_and_detail_and_neighbor_mismatch(self):
        for kind in ('banding', 'detail'):
            region = copy.deepcopy(next(r for r in self.regions if r['name'] == kind))
            region['metrics'][0]['axis'] = 'y'
            start = EXPECTED_KINDS.index(kind) * 8
            for y in range(HEIGHT):
                for x in range(start, start + 8):
                    self.values['albedo'][y * WIDTH + x] = (.25 + .02 * (y % 2),) * 3
                    self.values['reference'][y * WIDTH + x] = self.values['albedo'][y * WIDTH + x]
            self.assertEqual(self.direct(kind, regions=[region])['status'], 'pass')
            for x in range(start, start + 8):
                self.values['albedo'][4 * WIDTH + x] = (.25,) * 3 if kind == 'detail' else (1.,) * 3
            if kind == 'banding':
                self.assertEqual(self.direct(kind, regions=[region])['status'], 'fail')
        start = EXPECTED_KINDS.index('detail') * 8
        for y in range(HEIGHT):
            for x in range(start, start + 8):
                self.ids[y * WIDTH + x] = (1, 1, x % 2, 259)
        self.assertEqual(self.direct('detail')['status'], 'unavailable')

    def test_boundary_only_direction_cannot_bypass_other_neighbor_checks(self):
        self.inject('banding')
        for kind in ('banding', 'detail'):
            for direction in (0, -1, 1e38, [], {}, None, False):
                region = copy.deepcopy(next(r for r in self.regions if r['name'] == kind))
                region['metrics'][0]['direction'] = direction
                record = self.direct(kind, regions=[region])
                self.assertEqual(record['status'], 'fail' if kind == 'banding' else 'pass')

    def test_material_coverage_detects_missing_water_and_refuses_invalid_evidence(self):
        region = dict(name='expected-water', rect=[0, 0, 1, 1], metrics=[dict(
            kind='coverage', channel='material', field='material', value=1, thresholds=[.01, 1])])
        self.regions = [region]
        self.write()
        self.assertEqual(self.measure()[1][0]['status'], 'pass')
        self.values['material'] = [(0.,)] * (WIDTH * HEIGHT)
        self.write()
        record = self.measure()[1][0]
        self.assertEqual((record['status'], record['value'], record['worst_pixel']),
                         ('fail', 0., [0, 0]))
        output = self.root / 'water.svg'
        module = runpy.run_path(str(MONTAGE))
        self.assertEqual(module['diagnostic_montage'](self.bundle.path(checker.SUFFIX), output, self.config), [])
        labels = ''.join(ET.fromstring(output.read_text(encoding='utf-8')).itertext())
        self.assertIn('expected-water / coverage / material material=1 advisory fail', labels)
        self.assertIn('worst pixel: [0, 0]', labels)
        self.assertEqual(self.direct('coverage', omit=('material',), regions=[region])['status'], 'unavailable')
        for value in (float('nan'), -1, 256, .5):
            self.values['material'][0] = (value,)
            self.assertEqual(self.direct('coverage', regions=[region])['status'], 'unavailable')
        for change in (dict(value=256), dict(channel='ids'), dict(field='unknown')):
            invalid = copy.deepcopy(region)
            invalid['metrics'][0].update(change)
            self.config.write_text(json.dumps(dict(schema=1, rois=[invalid])), encoding='utf-8')
            with self.assertRaises(checker.Invalid):
                checker.load_rois(self.config)
    def test_detail_rejects_wet_or_non_lod0_and_composition_checks_transmittance(self):
        start = EXPECTED_KINDS.index('detail') * 8
        for channel in ('precipitation', 'ids'):
            original = copy.deepcopy(self.values['precipitation']), list(self.ids)
            for y in range(HEIGHT):
                for x in range(start, start + 8):
                    if channel == 'ids':
                        self.ids[y * WIDTH + x] = (1, 1, 2, 259)
                    else:
                        self.values['precipitation'][y * WIDTH + x] = (.5,)
            self.assertEqual(self.direct('detail')['status'], 'unavailable')
            self.values['precipitation'], self.ids = original
        for value in (-.1, 1.1):
            self.values['transmittance'][EXPECTED_KINDS.index('composition') * 8] = (value,)
            self.assertEqual(self.direct('composition')['status'], 'fail')

    def test_card_coverage_extremes_fail_and_cutout_equality_is_opaque(self):
        for alpha in (0., 1.):
            self.values['alpha'] = [(alpha,)] * (WIDTH * HEIGHT)
            self.values['depth_written'] = [(0.,)] * (WIDTH * HEIGHT)
            result = self.direct('card')
            self.assertEqual(result['status'], 'fail')
            self.assertEqual(result['value'], alpha)
        self.values['alpha'] = [(.5 if x % 2 else 0.,) for y in range(HEIGHT) for x in range(WIDTH)]
        self.assertEqual(self.direct('card')['status'], 'pass')

    def test_seam_and_boundary_p99_histograms_include_outliers_and_overflow(self):
        self.inject('seam')
        record = self.direct('seam')
        self.assertGreaterEqual(record['p99_upper'], .75)
        self.assertLess(record['p99_upper'], .75 + 1/65536)
        start = EXPECTED_KINDS.index('seam') * 8
        self.values['albedo'][start] = (3.,) * 3
        self.assertEqual(self.direct('seam')['p99_upper'], 2.75)
        region = copy.deepcopy(self.regions[2])
        region['rect'] = [0, 0, 1, 1]
        self.values['albedo'] = [(.25,)*3] * (WIDTH * HEIGHT)
        self.values['albedo'][2 * WIDTH + WIDTH - 1] = (1.,)*3
        record = self.direct('boundary', regions=[region])
        self.assertGreater(record['maximum'], 0)
        self.assertEqual((record['p99_upper'], record['status']), (1/65536, 'pass'))

    def test_new_evidence_payloads_refuse_partial_or_unknown_encoding(self):
        for channel in checker.EVIDENCE:
            path = self.bundle.path('.' + channel + '.pfm')
            original = path.read_bytes()
            path.write_bytes(original[:-1])
            self.bundle.finish()
            with self.assertRaises(checker.Invalid):
                self.measure()
            path.write_bytes(original)
        self.bundle.meta['encodings']['reference'] = 'linear_contribution_float32'
        self.bundle.finish()
        with self.assertRaises(checker.Invalid):
            self.measure()

    def test_auxiliary_evidence_requires_new_schema_and_weather_is_optional(self):
        self.bundle.meta['schema'] = 2
        self.bundle.finish()
        with self.assertRaises(checker.Invalid):
            self.measure()
        self.bundle.meta['schema'] = 3
        for channel in checker.WEATHER:
            self.bundle.meta['encodings'].pop(channel)
            name = 'view.png.' + channel + '.pfm'
            self.bundle.meta['files'].remove(name)
            (self.root / name).unlink()
        self.bundle.finish()
        findings, records = self.measure()
        self.assertEqual(findings, [])
        self.assertEqual([r['roi'] for r in records if r['status'] == 'unavailable'],
                         ['detail', 'weather'])

    def test_qualified_unavailable_fails_and_io_refusal_preserves_output(self):
        self.regions[0]['enforce'] = True
        self.values['expected_mask'] = [(0.,)] * (WIDTH * HEIGHT)
        self.write()
        findings, _ = self.measure()
        self.assertEqual(findings, ['holes: holes unavailable'])
        with mock.patch.object(checker, 'open_regular', side_effect=PermissionError('denied')):
            with self.assertRaises(PermissionError):
                self.measure()
        self.bundle.path('.reference.pfm').write_bytes(b'partial')
        module = runpy.run_path(str(MONTAGE))
        output = self.root / 'refused.svg'
        with self.assertRaises(ValueError):
            module['diagnostic_montage'](self.bundle.path(checker.SUFFIX), output, self.config)
        self.assertFalse(output.exists())

    def test_missing_channels_and_empty_masks_never_pass(self):
        rois = checker.load_rois(self.config)
        for kind in EXPECTED_KINDS[:-1]:
            region = next(r for r in rois if r['name'] == kind)
            for absent in ('expected_mask', region['metrics'][0]['channel']):
                streams = {c: io.BytesIO() for c in self.values if c != absent}
                result = checker.image_metrics(streams, {}, WIDTH, HEIGHT, [region])[0]
                self.assertEqual((result['status'], result['value']), ('unavailable', None))
        self.values['expected_mask'] = [(0.,)] * (WIDTH * HEIGHT)
        self.write()
        _, records = self.measure()
        self.assertTrue(all(r['status'] == 'unavailable' and r['worst_pixel'] is None
                            for r in records[:-1]))

    def test_unknown_roi_mask_and_channel_are_refused(self):
        for change in (dict(mask='unknown'), dict(channel='unknown'), dict(kind='unknown'),
                       dict(axis='z'), dict(direction=0), dict(p99_limit=float('inf'))):
            regions = copy.deepcopy(self.regions)
            regions[2]['metrics'][0].update(change)
            self.config.write_text(json.dumps(dict(schema=1, rois=regions)), encoding='utf-8')
            with self.assertRaises(checker.Invalid):
                checker.load_rois(self.config)
        self.regions[0]['name'] = None
        self.write()
        with self.assertRaises(checker.Invalid):
            checker.load_rois(self.config)

    def test_new_suite_is_registered(self):
        root = Path(__file__).resolve().parents[2]
        paths = runpy.run_path(str(root / 'tools/ci/ci_paths.py'))
        self.assertIn('tools/gates/test_diagnostic_checks.py', paths['PYTHON_TESTS'])
        self.assertEqual(paths['python_test_commands'](root).count(
            ('tools/gates/test_diagnostic_checks.py',)), 1)
        for path in ('tools/gates/test_diagnostic_checks.py',
                     'tools/gates/check-diagnostic-capture.py',
                     'tools/gates/baselines/diagnostic-rois.json'):
            self.assertTrue(paths['needs_python'](path))
        self.assertIn('tools/gates/test_diagnostic_checks.py',
                      (root / 'test/CMakeLists.txt').read_text(encoding='utf-8'))


def defect_test(kind):
    def test(self):
        self.check_defect(kind)
    return test


for defect in EXPECTED_KINDS:
    setattr(CaptureChecks, 'test_injected_' + defect, defect_test(defect))


if __name__ == '__main__':
    unittest.main()

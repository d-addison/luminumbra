#!/usr/bin/env python3
"""Bounded ROI, contribution-plane and stdlib montage regressions."""
import builtins
import io
import json
import os
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
checker = FIXTURE['checker']
Bundle = FIXTURE['Bundle']
MONTAGE = Path(__file__).with_name('build-visual-sweep-montages.py')


def metric(kind='coverage', channel='ids', **options):
    return dict(kind=kind, channel=channel, thresholds=[0, 1], **options)


def roi(item, **options):
    return dict(name='test-region', rect=[0, 0, 1, 1], metrics=[item], **options)


class Regions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bundle = Bundle(self.root)

    def config(self, regions):
        path = self.root / 'rois.json'
        path.write_text(json.dumps(dict(schema=1, rois=regions)), encoding='utf-8')
        return checker.load_rois(path)

    def measure(self, item, **options):
        records = []
        regions = self.config([roi(item, **options)])
        findings = checker.validate(self.bundle.path(checker.SUFFIX),
                                    checker.discover(self.root)[0][1], rois=regions, metrics=records)
        return findings, records[0]

    def weather(self, channel='fog', values=(.1, .2, .3, .4)):
        b = self.bundle
        b.meta['schema'] = 2
        b.meta['encodings'][channel] = 'linear_contribution_float32'
        # Fixed contribution order before the optional probe and completion.
        b.meta['files'] = b.meta['files'][:6] + [
            'view.png.' + c + '.pfm' for c in checker.WEATHER if c in b.meta['encodings']
        ] + ['view.png.probe-1-0.json', 'view.png.diagnostics.json']
        b.path('.' + channel + '.pfm').write_bytes(b'Pf\n2 2\n-1.0\n' +
            struct.pack('<4f', *(values[2:] + values[:2])))
        b.finish()

    def test_id_class_fractions_include_background_and_zero_lod(self):
        for field, value, expected in [('background', 1, .25), ('draw', 1, .75),
                                       ('mesh', 1, .75), ('lod', 2, .75),
                                       ('ownership', 3, .75), ('pass', 1, .75), ('lod', 0, .25)]:
            with self.subTest(field=field):
                findings, record = self.measure(metric(field=field, value=value))
                self.assertEqual(findings, [])
                self.assertEqual(record['value'], expected)
                self.assertEqual(record['samples'], 4)

    def test_normalized_rectangle_uses_top_left_and_half_open_rounding(self):
        item = metric(field='background', value=1)
        config = self.config([dict(roi(item), rect=[.5, 0, 1, .5])])
        records = []
        checker.validate(*checker.discover(self.root)[0], rois=config, metrics=records)
        self.assertEqual(records[0]['value'], 1)
        self.assertEqual(records[0]['rect'], [1, 0, 2, 1])
        self.assertEqual(checker.rectangle([.1, .1, .2, .2], 2, 2), (0, 0, 1, 1))

    def test_validity_all_aovs_and_nonfinite_fraction(self):
        for channel in checker.PLANES:
            self.assertEqual(self.measure(metric('validity', channel))[1]['value'], 1)
        self.bundle.values['normal'][0] = (float('nan'), 0, 0)
        self.bundle.write()
        findings, record = self.measure(metric('validity', 'normal'))
        self.assertEqual(record['value'], .75)
        self.assertIn('normal: 1 nonfinite samples', findings)
        self.bundle.values['normal'][0] = (2, 0, 0)
        self.bundle.values['depth'][0] = (-.1,)
        self.bundle.write()
        for channel in ('normal', 'depth'):
            self.assertEqual(self.measure(metric('validity', channel))[1]['value'], .75)

    def test_enforcement_defaults_advisory_then_qualified(self):
        item = dict(metric(field='background', value=1), thresholds=[0, 0])
        findings, record = self.measure(item)
        self.assertEqual(findings, [])
        self.assertEqual((record['qualification'], record['status']), ('advisory', 'fail'))
        findings, record = self.measure(item, enforce=True)
        self.assertTrue(findings)
        self.assertEqual(record['qualification'], 'qualified')
        self.assertEqual(record['worst_pixel'], [1, 0])
        item['thresholds'] = [.25, .25]
        self.assertEqual(self.measure(item, enforce=True)[0], [])

    def test_weather_missing_is_unavailable_in_both_modes(self):
        for channel in checker.WEATHER:
            for enforce in (False, True):
                findings, record = self.measure(metric('weather', channel), enforce=enforce)
                self.assertEqual(bool(findings), enforce)
                self.assertEqual(record['status'], 'unavailable')
                self.assertIsNone(record['value'])

    def test_weather_all_channels_range_not_just_mean_and_order(self):
        for channel in checker.WEATHER:
            self.weather(channel, (0., .5, 1., .5))
        for channel in checker.WEATHER:
            findings, record = self.measure(metric('weather', channel))
            self.assertEqual(findings, [])
            self.assertEqual(record['value'], .5)
            self.assertEqual((record['minimum'], record['maximum']), (0, 1))
            item = dict(metric('weather', channel), thresholds=[.25, .75])
            self.assertEqual(self.measure(item, enforce=True)[1]['status'], 'fail')
        self.weather(values=(2., 3., 4., 5.))
        record = self.measure(metric('weather', 'fog'))[1]
        self.assertEqual((record['minimum'], record['maximum']), (2, 5))
        self.assertEqual(record['worst_pixel'], [1, 1])

    def test_weather_nonfinite_is_unavailable_and_invalid(self):
        self.weather(values=(float('nan'), .2, .3, .4))
        findings, record = self.measure(metric('weather', 'fog'))
        self.assertEqual(record['status'], 'unavailable')
        self.assertIsNone(record['value'])
        self.assertIn('fog: 1 nonfinite samples', findings)

    def test_edge_column_row_and_deterministic_worst_location(self):
        for axis, value, location in [('x', 1, [1, 0]), ('y', 2, [0, 1])]:
            item = dict(metric('edge', 'albedo', axis=axis), thresholds=[0, .02])
            findings, record = self.measure(item)
            self.assertEqual(findings, [])
            self.assertEqual(record['value'], value)
            self.assertEqual(record['worst_pixel'], location)
            self.assertEqual(record['samples'], 2)
            self.assertEqual(self.measure(item, enforce=True)[1]['status'], 'fail')
        self.bundle.values['albedo'] = [(0, 0, 0)] * 4
        self.bundle.probe['albedo_linear'] = [0, 0, 0]
        self.bundle.write()
        self.assertEqual(self.measure(metric('edge', 'albedo', axis='x'), enforce=True)[0], [])

    def test_edge_no_pairs_or_nonfinite_is_unavailable(self):
        item = metric('edge', 'albedo', axis='x')
        for axis in ('x', 'y'):
            regions = self.config([dict(roi(dict(item, axis=axis)), rect=[0, 0, .5, .5])])
            records = []
            checker.validate(*checker.discover(self.root)[0], rois=regions, metrics=records)
            self.assertEqual(records[0]['status'], 'unavailable')
        self.bundle.values['albedo'][0] = (float('inf'), 0, 0)
        self.bundle.write()
        self.assertEqual(self.measure(item)[1]['status'], 'unavailable')

    def test_montage_stdlib_cli_labels_value_crop_and_exclusive_output(self):
        output = self.root / 'worst.svg'
        command = [sys.executable, '-I', '-B', str(MONTAGE), '--diagnostics',
                   str(self.bundle.path(checker.SUFFIX)), '--output', str(output)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        document = ET.fromstring(output.read_text(encoding='utf-8'))
        text = ''.join(document.itertext())
        self.assertIn('advisory fail value=', text)
        self.assertIn('advisory unavailable value=None', text)
        self.assertIn('worst pixel:', text)
        self.assertIn('raw albedo crop', text)
        self.assertGreater(len(document.findall('{http://www.w3.org/2000/svg}rect')), 10)
        original = output.read_bytes()
        self.assertEqual(subprocess.run(command, capture_output=True, timeout=20).returncode, 2)
        self.assertEqual(output.read_bytes(), original)

    def test_montage_refusals_do_not_create_output(self):
        module = runpy.run_path(str(MONTAGE))
        output = self.root / 'worst.svg'
        self.bundle.path('.depth.pfm').write_bytes(b'partial')
        with self.assertRaises(ValueError):
            module['diagnostic_montage'](self.bundle.path(checker.SUFFIX), output)
        self.assertFalse(output.exists())

    def test_retained_streams_are_checked_after_metric_scans(self):
        original = checker.image_metrics
        def changed(*args, **kwargs):
            result = original(*args, **kwargs)
            path = self.bundle.path('.albedo.pfm')
            os.utime(path, ns=(3000000000, 3000000000))
            return result
        with mock.patch.object(checker, 'image_metrics', changed):
            with self.assertRaisesRegex(checker.Invalid, 'member changed'):
                self.measure(metric(field='draw', value=1))

    def test_configuration_unreadable_partial_and_unknown_metadata(self):
        path = self.root / 'config.json'
        for data in (b'{', b'[]', b'{"schema":1,"schema":1,"rois":[]}',
                     b'{"schema":1,"rois":[{"name":"x","rect":[0,0,1,1e999]}]}'):
            path.write_bytes(data)
            with self.assertRaises(checker.Invalid):
                checker.load_rois(path)
        with mock.patch.object(checker, 'open_regular', side_effect=PermissionError('denied')):
            with self.assertRaises(PermissionError):
                checker.load_rois(path)

    def test_weather_edge_and_top_left_crop_samples(self):
        self.weather(values=(0., 1., 0., 1.))
        record = self.measure(metric('edge', 'fog', axis='x'))[1]
        self.assertEqual(record['value'], 1)
        self.assertEqual(record['worst_pixel'], [1, 0])
        crops = []
        regions = self.config([roi(metric(field='background', value=1))])
        checker.validate(*checker.discover(self.root)[0], rois=regions, crops=crops)
        self.assertEqual(crops[0]['box'], [0, 0, 2, 2])
        self.assertEqual(crops[0]['pixels'][0][0], self.bundle.values['albedo'][0])
        self.assertEqual(crops[0]['pixels'][1][0], self.bundle.values['albedo'][2])

    def test_montage_qualified_missing_and_empty_configuration(self):
        module = runpy.run_path(str(MONTAGE))
        path = self.root / 'rois.json'
        self.config([roi(metric('weather', 'cloud'), enforce=True)])
        output = self.root / 'missing.svg'
        self.assertTrue(module['diagnostic_montage'](self.bundle.path(checker.SUFFIX), output, path))
        self.assertIn('qualified unavailable', output.read_text(encoding='utf-8'))
        self.config([])
        output = self.root / 'empty.svg'
        self.assertEqual(module['diagnostic_montage'](self.bundle.path(checker.SUFFIX), output, path), [])
        ET.parse(output)
        self.assertEqual(module['main'](['--diagnostics', str(self.root/'missing.json'),
                                        '--output', str(self.root/'bad.svg')]), 2)
        self.assertFalse((self.root/'bad.svg').exists())

    def test_ci_registry_covers_tests_and_baseline_changes(self):
        paths = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'ci' / 'ci_paths.py'))
        self.assertIn('tools/gates/test_diagnostic_rois.py', paths['PYTHON_TESTS'])
        self.assertTrue(paths['needs_python']('tools/gates/baselines/diagnostic-rois.json'))

    def test_maximum_crop_is_bounded_and_labels_escape_text_without_pillow(self):
        size = 40
        rows = [(0., 0., 0.)] * (size * size)
        rows[(size - 1 - 20) * size + 20] = (1., 0., 0.)
        stream = io.BytesIO(b''.join(struct.pack('<3f', *pixel) for pixel in rows))
        crops = []
        checker.image_metrics({'albedo': stream}, {'albedo': 0}, size, size,
                              self.config([roi(metric('edge', 'albedo', axis='x'))]), crops)
        self.assertEqual(crops[0]['box'], [4, 4, 37, 37])
        self.assertEqual(len(crops[0]['pixels']), 33)
        self.assertTrue(all(len(row) == 33 for row in crops[0]['pixels']))
        self.assertEqual(crops[0]['pixels'][16][16], (1., 0., 0.))
        self.config([dict(roi(metric(field='draw', value=1)), name='<region&>')])
        original = builtins.__import__
        def stdlib_only(name, *args, **kwargs):
            if name == 'PIL' or name.startswith('PIL.'):
                raise AssertionError('diagnostic montage must not import Pillow')
            return original(name, *args, **kwargs)
        output = self.root / 'escaped.svg'
        with mock.patch('builtins.__import__', stdlib_only):
            module = runpy.run_path(str(MONTAGE))
            module['diagnostic_montage'](self.bundle.path(checker.SUFFIX), output, self.root/'rois.json')
        data = output.read_text(encoding='utf-8')
        self.assertIn('&lt;region&amp;&gt;', data)
        self.assertIn('draw=1', data)
        ET.parse(output)

    def test_configuration_empty_set_is_valid(self):
        self.assertEqual(self.config([]), [])


INVALID_CONFIGS = {
    'schema': dict(schema=2),
    'null_rois': dict(rois=None),
    'too_many_rois': dict(rois=[roi(metric(field='draw', value=1))] * 33),
    'roi_type': dict(rois=[None]),
    'duplicate_names': dict(rois=[roi(metric(field='draw', value=1))] * 2),
}
INVALID_ROIS = {
    'name_escape': dict(name='../escape'), 'name_null': dict(name=None),
    'rect_short': dict(rect=[0, 1]), 'rect_empty': dict(rect=[0, 0, 0, 1]),
    'rect_reversed': dict(rect=[1, 0, 0, 1]), 'rect_outside': dict(rect=[0, 0, 2, 1]),
    'rect_bool': dict(rect=[False, 0, 1, 1]), 'rect_huge': dict(rect=[0, 0, 1e38, 1]),
    'enforce_null': dict(enforce=None), 'enforce_string': dict(enforce='false'),
    'metrics_empty': dict(metrics=[]), 'metric_null': dict(metrics=[None]),
    'metric_count': dict(metrics=[metric(field='draw', value=1)] * 33),
}
INVALID_METRICS = {
    'unknown': dict(kind='unknown'), 'channel': dict(channel='unknown'),
    'channel_null': dict(channel=None), 'channel_list': dict(kind='validity', channel=[]),
    'channel_object': dict(kind='edge', channel={}), 'kind_list': dict(kind=[]),
    'field': dict(field='unknown'), 'value_bool': dict(value=True),
    'value_negative': dict(value=-1), 'value_large': dict(value=65537),
    'threshold_null': dict(thresholds=None), 'threshold_short': dict(thresholds=[0]),
    'threshold_reversed': dict(thresholds=[1, 0]), 'threshold_negative': dict(thresholds=[-1, 1]),
    'threshold_bool': dict(thresholds=[False, 1]), 'threshold_large': dict(thresholds=[0, 2]),
    'weather_channel': dict(kind='weather', channel='albedo'),
    'validity_channel': dict(kind='validity', channel='ids'),
    'edge_channel': dict(kind='edge', channel='ids', axis='x'),
    'edge_axis': dict(kind='edge', channel='depth', axis='z'),
}


def invalid_config_test(change):
    def test(self):
        config = dict(schema=1, rois=[roi(metric(field='draw', value=1))])
        change(config)
        path = self.root / 'invalid.json'
        path.write_text(json.dumps(config), encoding='utf-8')
        with self.assertRaises(checker.Invalid):
            checker.load_rois(path)
    return test


for name, fields in INVALID_CONFIGS.items():
    setattr(Regions, 'test_refuse_config_' + name,
            invalid_config_test(lambda c, fields=fields: c.update(fields)))
for name, fields in INVALID_ROIS.items():
    setattr(Regions, 'test_refuse_roi_' + name,
            invalid_config_test(lambda c, fields=fields: c['rois'][0].update(fields)))
for name, fields in INVALID_METRICS.items():
    setattr(Regions, 'test_refuse_metric_' + name,
            invalid_config_test(lambda c, fields=fields: c['rois'][0]['metrics'][0].update(fields)))


def contribution_refusal(change):
    def test(self):
        self.weather()
        change(self.bundle)
        self.bundle.finish()
        with self.assertRaises((checker.Invalid, ValueError)):
            checker.validate(*checker.discover(self.root)[0], rois=self.config([]))
    return test


for name, change in {
    'partial': lambda b: b.path('.fog.pfm').write_bytes(b'Pf\n2 2\n-1.0\n'),
    'missing': lambda b: b.path('.fog.pfm').unlink(),
    'unknown_encoding': lambda b: b.meta['encodings'].update(fog='unknown'),
    'unknown_channel': lambda b: b.meta['encodings'].update(rain='linear_contribution_float32'),
    'old_schema': lambda b: b.meta.update(schema=1),
    'wrong_dimensions': lambda b: b.path('.fog.pfm').write_bytes(b'Pf\n3 2\n-1.0\n' + b'0' * 24),
    'wrong_order': lambda b: b.meta['files'].reverse(),
    'big_endian': lambda b: b.edit_bytes('.fog.pfm', lambda data: data.replace(b'-1.0', b'+1.0')),
    'unknown_version': lambda b: b.meta.update(schema=3),
}.items():
    setattr(Regions, 'test_refuse_contribution_' + name, contribution_refusal(change))


if __name__ == '__main__':
    unittest.main()

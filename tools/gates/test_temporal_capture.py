#!/usr/bin/env python3
"""Synthetic manifest-bound temporal measurements; no renderer or dependencies."""
import hashlib
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

PATH = Path(__file__).with_name('check-temporal-capture.py')
C = runpy.run_path(str(PATH))
D = C['D']


class Sequence:
    def __init__(self, root, count=3, hold=False, width=16, height=8):
        self.root, self.count, self.hold = root, count, hold
        self.width, self.height = width, height
        self.path = root / 'sequence.json'
        self.rois = root / 'rois.json'
        self.regions = [dict(name='terrain', rect=[0, 0, 1, 1], metrics=[dict(
            kind='seam', channel='albedo', mask='expected_mask', thresholds=[0, .01], p99_limit=.02)])]
        self.frames = []
        self.omit = set()
        for index in range(count):
            self.frames.append(dict(light=[.25, .5], color=[.25, .5], depth=[.5, .5],
                material=[1., 1.], lod=[0, 0], ownership=[3, 3], mask=True,
                reorder=False, renumber=False, temporal=dict(version=1, steady_light=True,
                    exposure=1., smooth_exposure=1., smooth_luminance=.375,
                    solar_luminance=.375, phase='dawn',
                    labels=dict(horizon_crossing=False, cascade_change=False, cloud=False, strike=False),
                    classes={'1': 'static', '2': 'static'},
                    seams={'terrain': dict(id='world-x-0', mask_version='v1', openings_excluded=True)})))
        self.write()

    def write(self):
        # Fixtures replace only closed files. Completion metadata is always last.
        planned, recorded = [], []
        for index, frame in enumerate(self.frames):
            prefix = f'frame-{index}.ppm'
            draws = [dict(id=i + 1, mesh=i + 1, lod=frame['lod'][i], ownership=frame['ownership'][i],
                     **{'pass': 1}, chunk=[i + frame.get('producer_offset', 0) * 10, 0, 0], shader_variant='terrain',
                     lod_convention='zero_for_non_lod', supported=True) for i in range(2)]
            meshes = [dict(id=i + 1, path=f'res/terrain{i}.glb', provenance=1) for i in range(2)]
            if frame['renumber']:
                draws.reverse()
                meshes.reverse()
                for i, draw in enumerate(draws):
                    draw.update(id=i + 1, mesh=i + 1)
                    meshes[i]['id'] = i + 1
            if frame.get('ambiguous'):
                draws[1]['chunk'] = draws[0]['chunk'][:]
                meshes[1]['path'] = meshes[0]['path']
            table = dict(schema=1, draws=draws, meshes=meshes)
            values = {c: [] for c in (*D['PLANES'], 'lighting', 'expected_mask', 'reference') if c not in self.omit}
            identities = []
            for y in range(self.height):
                for x in range(self.width):
                    producer = int(x >= self.width // 2) ^ int(frame['reorder'])
                    draw_index = 1 - producer if frame['renumber'] else producer
                    draw = draws[draw_index]
                    identities.append((draw_index + 1, draw_index + 1, draw['lod'], draw['ownership'] | 256))
                    for channel in values:
                        value = ((frame['light'][producer],) * 3 if channel == 'lighting' else
                                 (frame['color'][producer],) * 3 if channel == 'albedo' else
                                 ((.25, .5)[producer],) * 3 if channel == 'reference' else
                                 (0., 1., 0.) if channel == 'normal' else
                                 (float(frame['mask_pixels'][y * self.width + x] if 'mask_pixels' in frame else frame['mask']),) if channel == 'expected_mask' else
                                 (frame[channel][producer],))
                        values[channel].append(value)
            camera = dict(view=[0.] * 16, projection=[0.] * 16,
                          matrix_order='column_major', reversed_z=True)
            if frame.get('camera_change'):
                camera['projection'][0] = 1.
            capture = dict(schema=1, status='complete', resolution=dict(width=self.width, height=self.height),
                           camera={k: camera[k] for k in ('view', 'projection')})
            if frame['temporal'] is not None:
                capture['temporal'] = frame['temporal']
            data = {prefix: b'P6\n1 1\n255\nabc',
                    f'frame-{index}.manifest.json': json.dumps(capture).encode(),
                    prefix + '.draws.json': json.dumps(table).encode(),
                    prefix + '.ids.bin': struct.pack('<4sIII', b'LVID', 1, self.width, self.height) +
                      b''.join(struct.pack('<4I', *p) for p in identities)}
            for channel, pixels in values.items():
                n = len(pixels[0])
                ordered = [v for y in range(self.height - 1, -1, -1)
                           for p in pixels[y * self.width:(y + 1) * self.width] for v in p]
                data[prefix + '.' + channel + '.pfm'] = (
                    ('PF\n' if n == 3 else 'Pf\n') + f'{self.width} {self.height}\n-1.0\n').encode() + struct.pack('<' + 'f' * len(ordered), *ordered)
            files = [prefix + '.' + c + '.pfm' for c in D['PLANES']]
            files += [prefix + '.ids.bin', prefix + '.draws.json']
            files += [prefix + '.' + c + '.pfm' for c in D['CHANNELS'] if c in values]
            completion = prefix + D['SUFFIX']
            files.append(completion)
            meta = dict(schema=frame.get('schema', 4 if 'lighting' in values else 3), status='complete', frame=index, width=self.width, height=self.height,
                        origin='top_left', surface='deferred_only', identity_complete=True,
                        coverage_complete=True, unsupported_producers=[], camera=camera,
                        capture_manifest=f'frame-{index}.manifest.json', files=files,
                        encodings=dict(D['ENCODINGS'], **{c: D['CHANNEL_ENCODINGS'][c]
                                       for c in D['CHANNELS'] if c in values}))
            data[completion] = json.dumps(meta).encode()
            for name, content in data.items():
                path = self.root / name
                path.write_bytes(content)
                os.utime(path, ns=(1000000000, 1000000000))
            os.utime(self.root / completion, ns=(2000000000, 2000000000))
            scheduled = dict(index=index, tick=100 if self.hold else 100 + index * 2, sub_tick_60=0,
                             kind=1 if self.hold else 0, control=1 if self.hold else 0,
                             dense=self.hold, hold_frame=index if self.hold else 0)
            planned.append(scheduled)
            recorded.append(dict(scheduled, payloads=[dict(path=name, bytes=len(content),
                sha256=hashlib.sha256(content).hexdigest()) for name, content in data.items()]))
        self.document = dict(schema='luminumbra.temporal_audit_sequence', version=1,
            status='complete', plan=dict(span=0, start_tick=100, end_tick=100 + self.count * 2,
            ticks_per_day=100, ticks_per_second=30, dawn_tick=20, dusk_tick=80,
            strike_tick=0, night_moon_phase_milli=1000, tick_limit=10000000, frame_limit=4096,
            sparse_interval_ticks=2, boundary_ticks=[100, 100 + self.count * 2],
            presentation_steps=400, max_render_attempts=1000, settling=dict(warmup_frames=90,
            initial_readiness_retries=300, sample_settle_frames=3, sample_readiness_retries=60),
            frames=planned), frames=recorded)
        self.write_manifest()
        self.rois.write_text(json.dumps(dict(schema=1, rois=self.regions)), encoding='utf-8')

    def write_manifest(self):
        canonical = json.dumps(self.document['plan'], sort_keys=True, separators=(',', ':'),
                               ensure_ascii=False).encode()
        self.document['plan_sha256'] = hashlib.sha256(canonical).hexdigest()
        self.path.write_text(json.dumps(self.document) + '\r\n', encoding='utf-8')

    def measure(self):
        return C['measure'](self.path, self.rois)


class TemporalCapture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='temporal_capture_')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.seq = Sequence(self.root)

    def metric(self, name, report=None):
        return next(r for r in (report or self.seq.measure())['results'] if r['metric'] == name)

    def test_delta_positive_negative_and_actual_cadence(self):
        good = self.metric('delta')
        self.assertEqual(good['status'], 'advisory')
        self.assertEqual(good['events'][0]['rgb']['mean'], 0.)
        self.seq.frames[1].update(light=[.75, .5], depth=[.2, .5], material=[2., 1.], ownership=[2, 3])
        self.seq.write()
        event = self.metric('delta')['events'][0]
        self.assertAlmostEqual(event['rgb']['mean'], .25)
        self.assertAlmostEqual(event['rgb']['p95_upper'], .5)
        self.assertEqual(event['changed_area'], .5)
        for field in ('depth_changed_area', 'material_changed_area', 'ownership_changed_area'):
            self.assertEqual(event[field], .5)
        self.assertEqual(event['delta_ticks'], 2.)

    def test_pops_positive_negative_and_attribution(self):
        good = self.metric('pops')
        self.assertEqual(good['events'][0]['residual'], 0.)
        self.assertEqual(good['events'][0]['area'], 0.)
        self.seq.frames[1].update(light=[1., .5], lod=[1, 0])
        self.seq.write()
        record = self.metric('pops')
        event = record['events'][0]
        self.assertAlmostEqual(event['residual'], .75)
        self.assertEqual(event['transitions'], ['lod'])
        self.assertEqual((event['before']['lod'], event['after']['lod']), (0, 1))
        self.assertEqual(event['interpretation'], 'unexplained')
        self.assertEqual(event['before']['asset'], 'res/terrain0.glb')

    def test_continuity_positive_negative_and_tick_normalization(self):
        for i, frame in enumerate(self.seq.frames):
            frame['light'] = [.25 + i * .125, .5 + i * .125]
            frame['temporal'].update(solar_luminance=.375 + i * .125, exposure=1. + i * .25)
        self.seq.write()
        event = self.metric('lighting_continuity')['events'][0]
        self.assertEqual(event['light']['first'], [.0625, .0625])
        self.assertEqual(event['exposure']['first'], [.125, .125])
        self.assertEqual(event['residual_second'], 0.)
        self.seq.frames[1]['light'] = [1., 1.25]
        self.seq.write()
        self.assertGreater(self.metric('lighting_continuity')['events'][0]['residual_second'], .1)
        for field, value in (('phase', 'night'), ('labels', {}),
                ('labels', dict(horizon_crossing=False, cascade_change=False, cloud=False, strike=0))):
            seq = Sequence(self.new_root())
            seq.frames[1]['temporal'][field] = value
            seq.write()
            self.assertEqual(self.metric('lighting_continuity', seq.measure())['status'], 'ineligible')
        for count, hold in ((2, False), (3, True)):
            seq = Sequence(self.new_root(), count, hold)
            self.assertEqual(self.metric('lighting_continuity', seq.measure())['status'], 'ineligible')

    def test_seam_positive_negative_persistence_and_reset(self):
        self.assertEqual(self.metric('seam_persistence')['events'][0]['consecutive_affected'], 0)
        for frame in self.seq.frames:
            frame['color'][0] = 1.
        self.seq.write()
        event = self.metric('seam_persistence')['events'][0]
        self.assertEqual((event['consecutive_affected'], event['elapsed_ticks'], event['affected_area']), (3, 4, .5))
        for frames in (self.seq.document['plan']['frames'], self.seq.document['frames']):
            frames[-1]['sub_tick_60'] = 30
        self.seq.write_manifest()
        self.assertEqual(self.metric('seam_persistence')['events'][0]['elapsed_ticks'], 4.5)
        self.seq.frames[1]['color'][0] = .25
        self.seq.write()
        self.assertEqual(self.metric('seam_persistence')['events'][0]['consecutive_affected'], 1)
        self.seq.frames[1]['color'][0] = 1.
        self.seq.frames[1]['temporal']['seams']['terrain']['id'] = 'other-world-seam'
        self.seq.write()
        self.assertEqual(self.metric('seam_persistence')['events'][0]['consecutive_affected'], 1)

    def test_flicker_positive_negative_and_recorded_trend(self):
        seq = Sequence(self.new_root(), 120, True)
        self.seq = seq
        for frame in seq.frames:
            frame['light'] = [.375, .375]
        seq.write()
        record = self.metric('flicker')
        self.assertEqual(record['status'], 'advisory')
        self.assertEqual(record['events'][0]['peak_to_peak'], 0.)
        for i, frame in enumerate(seq.frames):
            frame['light'] = [.375 + .125 * (i % 2), .375]
        seq.write()
        event = self.metric('flicker')['events'][0]
        self.assertEqual(event['peak_to_peak'], .125)
        self.assertEqual(event['alternating_sign_differences'], 118)
        self.assertEqual(event['p95_amplitude_upper'], .125)
        for i, frame in enumerate(seq.frames):
            frame['light'] = [.375 + i / 128, .375 + i / 128]
            frame['temporal']['smooth_luminance'] = .375 + i / 128
        seq.write()
        self.assertAlmostEqual(self.metric('flicker')['events'][0]['peak_to_peak'], 0.)

    def new_root(self):
        path = self.root / ('sequence-' + str(len(list(self.root.iterdir()))))
        path.mkdir()
        return path

    def test_flicker_insufficient_samples_pixels_coverage_and_steady_window(self):
        for count, width, height in ((29, 16, 8), (120, 8, 4)):
            with self.subTest(count=count, width=width):
                seq = Sequence(self.new_root(), count, True, width, height)
                self.assertEqual(self.metric('flicker', seq.measure())['status'], 'ineligible')
        seq = Sequence(self.new_root(), 120, True)
        seq.frames[60]['temporal']['steady_light'] = False
        seq.write()
        self.assertEqual(self.metric('flicker', seq.measure())['status'], 'ineligible')
        # Isolate the 1% condition from the 64-pixel condition without allocating
        # large full-resolution fixtures: feed bounded decoded summaries publicly.
        fn = C['flicker']
        key = ((0, 0, 0), 'res/a.glb', 1, 1, 'terrain')
        current = dict(channel='lighting', temporal=seq.frames[0]['temporal'],
            rects=[(0, 0, 100, 100)], groups=[{key: dict(classification='static', count=64,
            rgb=[.25]*3, lod=0, ownership=3)}])
        state, record = {}, C['result']('flicker', seq.regions[0])
        for i in range(120):
            fn(state, current, dict(kind=1, tick=100, control=1, hold_frame=i, index=i),
               seq.regions[0], 0, record)
        self.assertEqual(record['status'], 'ineligible')
        current['rects'] = [(0, 0, 80, 80)]
        state, record = {}, C['result']('flicker', seq.regions[0])
        for i in range(120):
            fn(state, current, dict(kind=1, tick=100, control=1, hold_frame=i, index=i),
               seq.regions[0], 0, record)
        self.assertEqual(record['status'], 'advisory')
        self.assertEqual(record['coverage'], .01)

    def test_flicker_thirty_usable_samples_and_missing_final_surface(self):
        seq = Sequence(self.new_root(), 120, True)
        for frame in seq.frames[30:]:
            frame['mask'] = False
        seq.write()
        record = self.metric('flicker', seq.measure())
        self.assertEqual(record['status'], 'advisory')
        self.assertEqual(record['events'][0]['usable_samples'], 30)
        seq.frames[29]['mask'] = False
        seq.write()
        self.assertEqual(self.metric('flicker', seq.measure())['status'], 'ineligible')

    def test_identity_survives_reordered_pixels_and_capture_local_ids(self):
        for frame in self.seq.frames[1:]:
            frame.update(reorder=True, renumber=True)
        self.seq.write()
        report = self.seq.measure()
        event = self.metric('delta', report)['events'][0]
        self.assertEqual(event['rgb']['maximum'], 0.)
        self.assertEqual(event['unmasked']['maximum'], .25)
        self.assertEqual(self.metric('pops', report)['events'][0]['residual'], 0.)
        self.assertEqual(self.metric('lighting_continuity', report)['events'][0]['residual_second'], 0.)

    def test_unknown_metadata_and_empty_masks_never_pass(self):
        for change in ('absent', 'unknown-version', 'unknown-class', 'empty-mask', 'ambiguous', 'unknown-field'):
            with self.subTest(change=change):
                seq = Sequence(self.new_root())
                for frame in seq.frames:
                    if change == 'absent':
                        frame['temporal'] = None
                    elif change == 'unknown-version':
                        frame['temporal']['version'] = 2
                    elif change == 'unknown-class':
                        frame['temporal']['classes']['1'] = 'unknown'
                    elif change == 'ambiguous':
                        frame['ambiguous'] = True
                    elif change == 'unknown-field':
                        frame['temporal']['future_field'] = True
                    else:
                        frame['mask'] = False
                seq.write()
                for record in seq.measure()['results']:
                    self.assertEqual(record['status'], 'ineligible', (change, record))
                    self.assertTrue(record['reason'])
                    self.assertEqual(record['events'], [])

    def test_missing_light_trends_masks_and_expected_openings(self):
        for field in ('exposure', 'smooth_exposure', 'smooth_luminance', 'solar_luminance', 'labels'):
            seq = Sequence(self.new_root())
            for frame in seq.frames:
                frame['temporal'].pop(field)
            seq.write()
            if field in ('exposure', 'solar_luminance', 'labels'):
                self.assertEqual(self.metric('lighting_continuity', seq.measure())['status'], 'ineligible')
            if field == 'smooth_luminance':
                self.assertEqual(self.metric('pops', seq.measure())['status'], 'ineligible')
        for omit in ('lighting', 'expected_mask', 'reference'):
            seq = Sequence(self.new_root())
            seq.omit.add(omit)
            # Remove omitted files from the existing fixture inventory.
            for path in seq.root.glob('*.' + omit + '.pfm'):
                path.unlink()
            seq.write()
            record = self.metric('lighting_continuity' if omit == 'lighting' else 'seam_persistence', seq.measure())
            self.assertEqual(record['status'], 'ineligible')
        for frame in self.seq.frames:
            frame['temporal']['seams']['terrain']['openings_excluded'] = False
        self.seq.write()
        self.assertEqual(self.metric('seam_persistence')['status'], 'ineligible')
        for value in ({}, {'id': '../escape', 'mask_version': 'v1', 'openings_excluded': True},
                {'id': 'world-x-0', 'mask_version': '', 'openings_excluded': True},
                {'id': 'world-x-0', 'mask_version': 'v1', 'openings_excluded': True, 'unknown': 1}):
            seq = Sequence(self.new_root())
            for frame in seq.frames:
                frame['temporal']['seams']['terrain'] = value
            seq.write()
            self.assertEqual(self.metric('seam_persistence', seq.measure())['status'], 'ineligible')
        seq = Sequence(self.new_root(), 120, True)
        for frame in seq.frames:
            frame['temporal'].pop('smooth_exposure')
        seq.write()
        self.assertEqual(self.metric('flicker', seq.measure())['status'], 'ineligible')

    def test_classified_motion_is_separate_from_static_deltas(self):
        for frame in self.seq.frames:
            frame['temporal']['classes']['1'] = 'animal'
        self.seq.frames[1]['light'][0] = 1.
        self.seq.write()
        event = self.metric('delta')['events'][0]
        self.assertEqual(event['rgb']['mean'], 0.)
        self.assertEqual(event['classified']['animal']['maximum'], .75)
        pop = self.metric('pops')['events'][0]
        self.assertEqual(pop['interpretation'], 'expected_animal')
        for frame in self.seq.frames:
            frame['temporal']['classes']['2'] = 'animal'
        self.seq.write()
        classified = self.metric('delta')
        self.assertEqual(classified['status'], 'ineligible')
        self.assertEqual(classified['companions']['classified']['animal']['maximum'], .75)
        self.assertEqual(classified['companions']['unmasked']['maximum'], .75)

    def test_invalid_sequence_and_payloads_refuse(self):
        for defect in ('hash', 'partial', 'duplicate-index', 'plan-hash', 'version', 'order', 'escape',
                       'camera', 'nonfinite', 'binary-mask', 'missing', 'extra', 'nonregular',
                       'legacy-lighting', 'schema4-no-light'):
            with self.subTest(defect=defect):
                seq = Sequence(self.new_root())
                if defect == 'hash':
                    path = seq.root / 'frame-0.ppm.lighting.pfm'
                    path.write_bytes(path.read_bytes()[:-4] + struct.pack('<f', .75))
                    os.utime(path, ns=(1000000000, 1000000000))
                elif defect == 'partial':
                    path = seq.root / 'frame-0.ppm.lighting.pfm'
                    path.write_bytes(path.read_bytes()[:-1])
                elif defect == 'duplicate-index':
                    seq.document['frames'][1]['index'] = 0
                    seq.write_manifest()
                elif defect == 'plan-hash':
                    seq.document['plan_sha256'] = '0' * 64
                    seq.path.write_text(json.dumps(seq.document), encoding='utf-8')
                elif defect == 'version':
                    seq.document['version'] = 2
                    seq.write_manifest()
                elif defect == 'order':
                    seq.document['plan']['frames'][1]['tick'] = 99
                    seq.document['frames'][1]['tick'] = 99
                    seq.write_manifest()
                elif defect == 'escape':
                    seq.document['frames'][0]['payloads'][0]['path'] = '../outside.ppm'
                    seq.write_manifest()
                elif defect == 'legacy-lighting':
                    seq.frames[0]['schema'] = 3
                    seq.write()
                elif defect == 'schema4-no-light':
                    seq.omit.add('lighting')
                    for p in seq.root.glob('*.lighting.pfm'):
                        p.unlink()
                    for f in seq.frames:
                        f['schema'] = 4
                    seq.write()
                elif defect == 'camera':
                    seq.frames[1]['camera_change'] = True
                    seq.write()
                elif defect == 'nonfinite':
                    seq.frames[1]['light'][0] = float('nan')
                    seq.write()
                elif defect == 'binary-mask':
                    seq.frames[1]['mask'] = .5
                    # Fixture float mask is deliberately preserved below.
                    seq.write()
                elif defect == 'missing':
                    (seq.root / 'frame-0.ppm.depth.pfm').unlink()
                elif defect == 'extra':
                    (seq.root / 'frame-999.ppm').write_bytes(b'extra')
                else:
                    path = seq.root / 'frame-0.ppm.lighting.pfm'
                    path.unlink()
                    path.mkdir()
                if defect == 'hash':
                    with self.assertRaisesRegex(ValueError, 'payload hash mismatch'):
                        seq.measure()
                else:
                    with self.assertRaises((ValueError, OSError)):
                        seq.measure()

    def test_duplicate_json_keys_unreadable_and_partial_metadata(self):
        duplicate = ('{"status":"complete",' + json.dumps(self.seq.document)[1:]).encode()
        for content in (duplicate, b'{', b'[]'):
            self.seq.path.write_bytes(content)
            with self.assertRaises(ValueError):
                self.seq.measure()
        with mock.patch.dict(C['D'], open_unlinked=mock.Mock(side_effect=PermissionError('denied'))):
            with self.assertRaises(PermissionError):
                self.seq.measure()

    def test_budget_checks_and_ambiguous_producer_refusal(self):
        meta = dict(width=16384, height=1024)
        with self.assertRaisesRegex(ValueError, 'staging budget'):
            C['frame_data'](meta, {}, {}, {}, {}, self.seq.regions)
        meta = dict(width=1, height=1)
        with self.assertRaisesRegex(ValueError, 'producer/ROI budget'):
            C['frame_data'](meta, {}, dict(draws=[None] * 1025), {}, {}, self.seq.regions)
        hist = C['Histogram']()
        hist.add(2.)
        self.assertEqual(hist.values()['p99_upper'], 2.)
        with mock.patch.dict(C['flicker'].__globals__, MAX_WINDOW_SAMPLES=1):
            seq = Sequence(self.new_root(), 120, True)
            with self.assertRaisesRegex(ValueError, 'window producer sample budget'):
                seq.measure()

        seq = Sequence(self.new_root(), 120, True)
        seq.frames[1]['producer_offset'] = 1
        seq.write()
        with mock.patch.dict(C['frame_data'].__globals__, MAX_GROUPS=2):
            with self.assertRaisesRegex(ValueError, 'steady-window track budget'):
                seq.measure()

    def test_json_cli_determinism_advisory_exit_and_event_bound(self):
        self.seq.regions *= 9
        for i, roi in enumerate(self.seq.regions):
            self.seq.regions[i] = dict(roi, name='terrain-' + str(i))
        self.seq.write()
        for frame in self.seq.frames:
            frame['light'][0] = .75
        self.seq.write()
        command = [sys.executable, '-I', '-B', str(PATH), str(self.seq.path), '--rois', str(self.seq.rois)]
        first = subprocess.run(command, capture_output=True, text=True, timeout=20)
        second = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertEqual(first.returncode, 0, first.stderr)
        diagnostic = subprocess.run([sys.executable, '-I', '-B', str(PATH.with_name('check-diagnostic-capture.py')),
            str(self.seq.root / 'frame-0.ppm.diagnostics.json'), '--rois', str(self.seq.rois)],
            capture_output=True, text=True, timeout=20)
        self.assertEqual(diagnostic.returncode, 0, diagnostic.stderr)
        self.assertEqual(first.stdout, second.stdout)
        report = json.loads(first.stdout)
        self.assertFalse(report['qualified'])
        for record in report['results']:
            if record['status'] == 'advisory':
                self.assertIsNotNone(record['values'])
                self.assertIn('index', record['worst_sample'])
        self.assertLessEqual(sum(len(r['events']) for r in report['results']), 40)
        self.assertEqual(first.stdout.strip(), json.dumps(report, sort_keys=True, allow_nan=False, separators=(',', ':')))
        self.seq.path.write_bytes(b'partial')
        failed = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(failed.stdout, '')
        self.assertEqual(json.loads(failed.stderr)['status'], 'invalid')

    def test_partial_masks_exclude_pixels_and_report_unsupported_coverage(self):
        for frame in self.seq.frames:
            frame['mask_pixels'] = [int(x % 2 == 0) for y in range(8) for x in range(16)]
        self.seq.frames[1]['light'][0] = .75
        self.seq.write()
        event = self.metric('delta')['events'][0]
        self.assertEqual(event['changed_area'], .25)
        self.assertEqual(self.metric('pops')['events'][0]['area'], .25)
        self.assertEqual(self.metric('delta')['observed_coverage']['unsupported'], .5)

    def test_numeric_metadata_unknown_and_large_tick_derivatives(self):
        for value in (True, -1, 1e300, 'unknown', None):
            for frame in self.seq.frames:
                frame['temporal']['exposure'] = value
            self.seq.write()
            self.assertEqual(self.metric('lighting_continuity')['status'], 'ineligible')
        self.seq = Sequence(self.new_root())
        offset = (1 << 61)
        for i, frame in enumerate(self.seq.frames):
            frame['light'] = [.25 + i * .125, .5 + i * .125]
        self.seq.write()
        for frame in self.seq.document['plan']['frames'] + self.seq.document['frames']:
            frame['tick'] += offset
        self.seq.document['plan']['start_tick'] += offset
        self.seq.document['plan']['end_tick'] += offset
        self.seq.document['plan']['boundary_ticks'] = [v + offset for v in self.seq.document['plan']['boundary_ticks']]
        self.seq.write_manifest()
        self.assertEqual(self.metric('lighting_continuity')['events'][0]['light']['first'], [.0625, .0625])

    def test_descriptor_identity_links_and_per_frame_json_budget(self):
        with mock.patch.dict(C['D']['validate'].__globals__,
                verify_payload=mock.Mock(side_effect=PermissionError('denied'))):
            with self.assertRaises(PermissionError):
                self.seq.measure()
        # Denied Windows symlinks are covered through the portable reparse
        # metadata refusal; this does not depend on symlink privilege.
        for mode, attributes in ((0o120777, 0), (0o100644, 0x400), (0o010644, 0)):
            info = type('Info', (), dict(st_mode=mode, st_file_attributes=attributes))()
            with mock.patch.object(os, 'lstat', return_value=info):
                with self.assertRaisesRegex(ValueError, 'nonregular'):
                    with D['open_unlinked'](self.root, self.seq.path):
                        self.fail('linked input opened')
        original = self.seq.document['frames'][0]['payloads']
        for p in original:
            if p['path'].endswith('.json'):
                p['bytes'] = 1024 * 1024
        self.seq.write_manifest()
        with self.assertRaisesRegex(ValueError, 'per-frame JSON budget'):
            self.seq.measure()

    def test_plan_metadata_is_closed_and_bounded(self):
        for field, value in (('unknown', []), ('span', 3), ('ticks_per_day', 0),
                ('ticks_per_day', 10000001), ('ticks_per_second', False), ('ticks_per_second', 1001),
                ('tick_limit', 10000001), ('tick_limit', 1), ('frame_limit', 4097), ('frame_limit', 1),
                ('night_moon_phase_milli', 1001), ('settling', {}),
                ('dawn_tick', 0), ('dusk_tick', 20), ('dusk_tick', 100), ('strike_tick', 1),
                ('span', 1), ('end_tick', 99), ('boundary_ticks', [99]),
                ('boundary_ticks', [100, 100]), ('boundary_ticks', [106, 100]), ('presentation_steps', [])):
            with self.subTest(field=field):
                seq = Sequence(self.new_root())
                seq.document['plan'][field] = value
                seq.write_manifest()
                with self.assertRaisesRegex(ValueError, 'plan'):
                    seq.measure()
        self.seq.document['plan'].pop('settling')
        self.seq.write_manifest()
        with self.assertRaisesRegex(ValueError, 'missing plan'):
            self.seq.measure()
        seq = Sequence(self.new_root())
        seq.document['plan'].update(span=1, strike_tick=1500)
        seq.write_manifest()
        self.assertEqual(seq.measure()['status'], 'advisory')
        seq.document['plan']['strike_tick'] = 2700
        seq.write_manifest()
        with self.assertRaisesRegex(ValueError, 'plan strike'):
            seq.measure()

    def test_registered_in_all_consumers(self):
        root = PATH.resolve().parents[2]
        paths = runpy.run_path(str(root / 'tools/ci/ci_paths.py'))
        self.assertEqual(paths['PYTHON_TESTS'].count('tools/gates/test_temporal_capture.py'), 1)
        for path in ('tools/gates/test_temporal_capture.py', 'tools/gates/check-temporal-capture.py'):
            self.assertTrue(paths['needs_python'](path))
        self.assertIn('tools/gates/check-temporal-capture.py', paths['CI_ENTRYPOINTS'])
        self.assertIn('tools/gates/test_temporal_capture.py', (root / 'test/CMakeLists.txt').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Synthetic receipt controls only; no hardware measurements or engine launch."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gates'))

import far_world_report as report
from test_render_contract import artifact, QUALIFIED, session_authority
from test_perf import far_capture, update_far_summary
import test_far_ladder_oracle as ladder_fixtures
from far_ladder_oracle import STATIONS


def inputs(preset):
    benchmark = artifact()
    benchmark['workload']['preset'] = preset
    benchmark['far_world'] = far_capture()['far_world']
    work = dict.fromkeys(report.WORK, 12)
    work['source_ms'] = 1.25
    for row in benchmark['far_world']['frames']:
        row.update(session_authority(), refresh_work=work)
    stations = ladder_fixtures.SessionValidatorTests.session()
    original = stations['stations'][0]
    snapshot = original['snapshot']
    snapshot.update(content_identity=0, world_epoch=12, bake_generation=9,
                    committed_generation=9, serving_committed_generation=9,
                    serving_serial=7, target_serial=7, manifest_fingerprint=2**64-1,
                    refresh_work=work)
    for tile in snapshot['tiles']:
        for field in ('desired', 'built'):
            tile[field].update(world_epoch=12, bake_generation=9)
    stations.update(world_preset=preset, expected_stations=5, stations=[])
    for index, (name, yaw) in enumerate(zip(STATIONS, (0, 90, 180, 270, 45))):
        row = copy.deepcopy(original)
        row['name'] = name
        row['camera'].update(yaw_degrees=yaw, pitch_degrees=-20 if index == 4 else 0)
        if index == 4:
            row['camera']['world_y'] += 78.2
            row['snapshot']['camera'][1] += 78.2
        row['snapshot']['frame_id'] += index
        stations['stations'].append(row)
    refresh = dict(schema='luminumbra.far_world_refresh.v1', samples=[dict(
        world_epoch=12, committed_generation=9, target_serial=7,
        manifest_fingerprint=str(2**64-1), serving_target_overlap_bytes=64, refresh_latency_ms=4.5)])
    return benchmark, stations, refresh


def write_receipt(root, preset, data):
    receipt = dict(schema='luminumbra.far_world_receipt.v1')
    for name, document in zip(('benchmark', 'stations', 'refresh'), data):
        raw = json.dumps(document, sort_keys=True).encode()
        reference = f'{preset}-{name}.json'
        (root / reference).write_bytes(raw)
        receipt[name] = dict(artifact=reference, sha256=hashlib.sha256(raw).hexdigest())
    reference = preset + '.json'
    (root / reference).write_text(json.dumps(receipt), encoding='utf-8')
    return reference, receipt


class FarWorldReportTests(unittest.TestCase):
    def test_complete_session_receipts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            references = [write_receipt(root, preset, inputs(preset))[0] for preset in report.PRESETS]
            result = report.build_report(root, references, QUALIFIED)
            self.assertTrue(result['passed'], result['failures'])
            self.assertEqual([row['preset'] for row in result['runs']], list(report.PRESETS))
            run = result['runs'][0]
            self.assertEqual(run['source_work']['source_read_bytes']['sample_count'], 1)
            self.assertEqual(run['source_work']['source_read_bytes']['max'], 12)
            self.assertEqual(run['serving_target_overlap_bytes']['max'], 64)
            self.assertEqual(run['refresh_latency_ms']['max'], 4.5)
            self.assertEqual(run['station_settle_seconds']['sample_count'], 5)
            self.assertEqual(run['distributions']['gpu_frame_ms']['sample_count'], 4)
            self.assertEqual(len(run['pool_peaks']), 5)
            self.assertNotIn('recommendations', result)
            duplicate = report.build_report(root, [references[0], references[0]], QUALIFIED)
            self.assertFalse(duplicate['passed'])
            self.assertTrue(any('duplicate preset' in item['reason'] for item in duplicate['failures']))
            command = [sys.executable, '-I', '-B', str(Path(report.__file__)),
                       '--root', str(root), '--qualified-renderer', QUALIFIED]
            for reference in references:
                command.extend(('--receipt', reference))
            completed = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(completed.stdout), result)

    def test_fallback_stale_and_missing_samples_fail(self):
        mutations = (
            lambda b, s, r: b['far_world']['frames'][1].update(effective_mode='legacy'),
            lambda b, s, r: b['far_world']['frames'][1].update(serving_serial=6),
            lambda b, s, r: b['far_world']['frames'][1].update(latest_target_ready=False),
            lambda b, s, r: b['far_world']['frames'][1].update(suppressed=True),
            lambda b, s, r: b['far_world']['frames'][1].update(refresh_work=None),
            lambda b, s, r: b['far_world']['frames'][1].update(manifest_fingerprint='2'),
            lambda b, s, r: b['far_world']['frames'][1].update(target_serial=6, serving_serial=6),
            lambda b, s, r: b['far_world']['frames'][1].update(world_epoch=13),
            lambda b, s, r: b['frame_samples'].pop(),
            lambda b, s, r: b['frame_samples'][1].update(gpu_frame_ms=None),
            lambda b, s, r: b.update(gpu_timers_supported=False),
            lambda b, s, r: s['stations'][1]['snapshot'].update(effective_mode='legacy'),
            lambda b, s, r: s['stations'][1]['snapshot'].update(authority_kind='unknown'),
            lambda b, s, r: s['stations'][1]['snapshot'].update(refresh_work=None),
            lambda b, s, r: s['stations'][1].update(settle_seconds=91),
            lambda b, s, r: s['stations'].pop(),
            lambda b, s, r: s.update(world_preset='archipelago'),
            lambda b, s, r: r.update(samples=[]),
            lambda b, s, r: r['samples'][0].pop('serving_target_overlap_bytes'),
            lambda b, s, r: r['samples'][0].pop('refresh_latency_ms'),
            lambda b, s, r: r['samples'][0].update(refresh_latency_ms=-1),
            lambda b, s, r: r['samples'][0].update(refresh_latency_ms=True),
            lambda b, s, r: r['samples'][0].update(target_serial=8),
            lambda b, s, r: r['samples'].append(copy.deepcopy(r['samples'][0])),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, mutate in enumerate(mutations):
                data = inputs('default')
                mutate(*data)
                update_far_summary(data[0])
                _, receipt = write_receipt(root, 'default', data)
                with self.subTest(index=index), self.assertRaises((ValueError, KeyError, TypeError)):
                    report.report_run(root, receipt, QUALIFIED)
            reference, _ = write_receipt(root, 'default', inputs('default'))
            result = report.build_report(root, [reference], QUALIFIED)
            self.assertFalse(result['passed'])
            self.assertEqual(len(result['failures']), 3)

    def test_traversal_snapshot_and_input_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = inputs('default')
            script = Path(__file__).with_name('fixtures') / 'surface-flight.traversal'
            text = script.read_text().replace('duration_seconds 60', 'duration_seconds 0.13333333333333333')
            text = text.replace('expected_ticks 1800', 'expected_ticks 4')
            payload = text.replace('\n', '\r\n').encode()
            manifest = report.render_contract.traversal_manifest(payload.decode())
            data[0]['workload'].update({key: value for key, value in manifest.items() if key != 'path'})
            data[0]['workload'].update(kind='traversal', actual_ticks=4, measured_duration_seconds=0.01,
                                      script=payload.decode(),
                                      camera_samples=[report.render_contract.camera_sample(manifest, tick)
                                                      for tick in range(1, 5)])
            _, receipt = write_receipt(root, 'default', data)
            (root / 'traversal.txt').write_bytes(payload)
            receipt['traversal'] = dict(artifact='traversal.txt', sha256=hashlib.sha256(payload).hexdigest())
            validator = report.render_contract.validate
            observed = []
            def inspect_snapshot(data, renderer, **kwargs):
                snapshot = kwargs['traversal']
                self.assertEqual(snapshot.read_bytes(), payload)
                self.assertNotEqual(snapshot, root / 'traversal.txt')
                observed.append(snapshot)
                return validator(data, renderer, **kwargs)
            with mock.patch.object(report.render_contract, 'validate', side_effect=inspect_snapshot):
                report.report_run(root, receipt, QUALIFIED)
            self.assertFalse(observed[0].exists())
            for pin in ({}, dict(artifact='traversal.txt', sha256='0' * 64),
                        dict(artifact='../escape.txt', sha256='0' * 64)):
                receipt['traversal'] = pin
                with self.assertRaises(ValueError):
                    report.report_run(root, receipt, QUALIFIED)
            with mock.patch.object(report, 'MAX_BYTES', 8), self.assertRaisesRegex(ValueError, 'oversized'):
                report.document(root, 'default.json')

    def test_report_order_is_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            references = [write_receipt(root, preset, inputs(preset))[0] for preset in report.PRESETS]
            first = report.build_report(root, references, QUALIFIED)
            self.assertEqual(first, report.build_report(root, list(reversed(references)), QUALIFIED))
            self.assertEqual(json.dumps(first, sort_keys=True),
                             json.dumps(report.build_report(root, references, QUALIFIED), sort_keys=True))

    def test_unreadable_partial_wrong_hash_and_escaping_inputs_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference, receipt = write_receipt(root, 'default', inputs('default'))
            for text in ('', '{', '{"schema":1,"schema":2}', '{"schema":NaN}', '[]'):
                (root / reference).write_text(text, encoding='utf-8')
                self.assertFalse(report.build_report(root, [reference], QUALIFIED)['passed'])
            self.assertFalse(report.build_report(root, ['absent.json'], QUALIFIED)['passed'])
            for path in ('../escape.json', 'C:escape.json', '/escape.json', ''):
                receipt['benchmark']['artifact'] = path
                with self.subTest(path=path), self.assertRaises(ValueError):
                    report.report_run(root, receipt, QUALIFIED)
            _, receipt = write_receipt(root, 'default', inputs('default'))
            (root / receipt['benchmark']['artifact']).write_bytes(b'{}')
            with self.assertRaisesRegex(ValueError, 'sha256'):
                report.report_run(root, receipt, QUALIFIED)
            with self.assertRaises(ValueError):
                report.build_report(root, [], QUALIFIED)
            with self.assertRaises(ValueError):
                report.build_report(root, ['a'] * 5, QUALIFIED)


if __name__ == '__main__':
    unittest.main()

#!/usr/bin/env python3
"""Measurement validity tests, run in the existing cross-platform Python CI lane."""
import argparse
import copy
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import perf
import render_contract as contract
import validate_render_capture as capture

FIXTURES = Path(__file__).parent / 'fixtures'
QUALIFIED = 'NVIDIA GeForce RTX 5070 Ti'


def isolated_script(path, *arguments):
    return [sys.executable, '-I', '-B', '-c',
            'import runpy,sys; sys.path.insert(0,sys.argv.pop(1)); script=sys.argv.pop(1); sys.argv[0]=script; runpy.run_path(script,run_name="__main__")',
            str(Path(path).parent), str(path), *arguments]


def artifact():
    """Synthetic positive control; every number is an oracle input, not a capture."""
    rows = []
    for frame, gpu in enumerate([3.0, 4.0, 6.0, 12.0], 2):
        passes = {name: {'issued': False, 'ms': None} for name in contract.PASSES}
        passes['gbuffer'] = {'issued': True, 'ms': gpu - 1}
        rows.append(dict(frame=frame, gpu_frame=frame, frame_wall_ms=gpu + 2,
                         cpu_submit_ms=2.0, present_ms=gpu, gpu_frame_ms=gpu,
                         gpu_pass_sum_ms=gpu - 1, passes=passes))
    data = {'schema': contract.SCHEMA, 'frames': len(rows), 'warmup_frames': 2,
            'width': 1280, 'height': 720, 'render_scale': 1.0,
            'internal_width': 1280, 'internal_height': 720, 'gpu_timers_supported': True,
            'frame_samples': rows,
            'adapter': {'vendor': 'NVIDIA Corporation', 'renderer': QUALIFIED, 'version': '4.5 synthetic fixture'},
            'profile': {'name': 'quality', 'render_scale': 1.0, 'width': 1280, 'height': 720},
            'debug_overrides': {'active': {}, 'declared': {}},
            'excluded_windows': [{'first_frame': 0, 'last_frame': 1, 'reason': 'warmup'},
                                 {'first_frame': 6, 'last_frame': 6, 'reason': 'report and optional screenshot'}],
            'workload': {'kind': 'fixed_view', 'seed': 424242, 'preset': 'default',
                         'preset_revision': 6, 'preset_identity': '0000000000000000',
                         'content_identity': '0', 'expected_frames': len(rows)},
            'avg': {}, 'avg_ms': {}, 'distribution': {}}
    for name in contract.METRICS:
        summary = perf.summarize([row[name] for row in rows], 'ms')
        data['distribution'][name] = {key: summary[key] for key in contract.STATS}
        data['distribution'][name].update(unit='ms', sample_count=len(rows), unavailable_count=0)
        data['avg'][name] = sum(row[name] for row in rows) / len(rows)
    for name in contract.PASSES:
        data['avg_ms'][name] = sum(row['passes'][name]['ms'] or 0 for row in rows) / len(rows)
    data['avg_ms']['total'] = data['avg']['gpu_pass_sum_ms']
    data['avg_ms']['ssao_total'] = 0
    return data


def args(**overrides):
    result = dict(position=None, yaw=None, pitch=None, tod=None, fov=None,
                  qualified_renderer=QUALIFIED, qualified_vendor=None,
                  traversal=None, workload_manifest=None, require_controller=False,
                  require_distinct_controller=False, require_geometry=False,
                  require_settled=False, require_camera_chunk=False)
    result.update(overrides)
    return argparse.Namespace(**result)


class RenderContractTests(unittest.TestCase):
    def test_valid_capture(self):
        self.assertEqual(capture.validate(artifact(), args())['verdict'], 'PASS')

    def test_whole_frame_brackets_pass_sum(self):
        data = artifact()
        data['frame_samples'][0]['gpu_frame_ms'] = 1.0
        with self.assertRaisesRegex(ValueError, 'whole-frame GPU'):
            contract.validate(data, QUALIFIED)

    def test_incomplete_timer_coverage(self):
        for field in contract.METRICS:
            data = artifact()
            data['frame_samples'][0][field] = None
            with self.subTest(field=field), self.assertRaises(ValueError):
                contract.validate(data, QUALIFIED)
        data = artifact()
        data['frame_samples'][0]['passes']['gbuffer']['ms'] = None
        with self.assertRaises(ValueError):
            contract.validate(data, QUALIFIED)
        data = artifact()
        data['distribution']['gpu_frame_ms']['unavailable_count'] = 1
        with self.assertRaisesRegex(ValueError, 'incomplete timer'):
            contract.validate(data, QUALIFIED)

    def test_adapter_mismatch_committed_fixture(self):
        data = json.loads((FIXTURES / 'adapter-mismatch.v3.json').read_text(encoding="utf-8"))
        with self.assertRaisesRegex(ValueError, 'adapter does not match'):
            contract.validate(data, QUALIFIED)
        data['adapter']['renderer'] = QUALIFIED
        self.assertEqual(contract.validate(data, QUALIFIED)['verdict'], 'PASS')

    def test_adapter_mismatch_cli_fails(self):
        result = subprocess.run(isolated_script(capture.__file__, str(FIXTURES / 'adapter-mismatch.v3.json'),
                                               '--qualified-renderer', QUALIFIED), capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn('adapter does not match', json.loads(result.stdout)['reason'])

    def test_adapter_identity_required(self):
        data = artifact()
        data['gpu_timers_supported'] = False
        with self.assertRaisesRegex(ValueError, 'GPU timers unavailable'):
            contract.validate(data, QUALIFIED)
        with self.assertRaisesRegex(ValueError, 'qualified-renderer'):
            contract.validate(artifact())
        data = artifact()
        data['adapter']['version'] = None
        with self.assertRaises(ValueError):
            contract.validate(data, QUALIFIED)

    def test_undeclared_render_debug_override(self):
        data = artifact()
        data['debug_overrides']['active']['env:LUMIN_RENDER_SCALE'] = '0.67'
        with self.assertRaisesRegex(ValueError, 'undeclared'):
            contract.validate(data, QUALIFIED)
        data['debug_overrides']['declared'] = copy.deepcopy(data['debug_overrides']['active'])
        self.assertEqual(contract.validate(data, QUALIFIED)['verdict'], 'PASS')

    def test_shared_cpp_percentile_fixture(self):
        tokens = (FIXTURES / 'distribution.txt').read_text(encoding="utf-8").split()
        count = int(tokens[0])
        values = [float(v) for v in tokens[1:1 + count]]
        expected = [float(v) for v in tokens[1 + count:]]
        actual = perf.summarize(values, 'ms')
        self.assertEqual(len(expected), len(contract.STATS))
        for stat, value in zip(contract.STATS, expected):
            self.assertAlmostEqual(actual[stat], value, places=12)

    def test_forged_distribution_and_mean(self):
        for group, key in [('distribution', 'p95'), ('avg', 'gpu_frame_ms')]:
            data = artifact()
            if group == 'distribution':
                data[group]['gpu_frame_ms'][key] += 1
            else:
                data[group][key] += 1
            with self.subTest(group=group), self.assertRaises(ValueError):
                contract.validate(data, QUALIFIED)

    def test_late_query_frame_mismatch(self):
        data = artifact()
        data['frame_samples'][0]['gpu_frame'] += 1
        with self.assertRaisesRegex(ValueError, 'originating frame'):
            contract.validate(data, QUALIFIED)

    def test_profile_and_exclusions(self):
        mutations = [lambda d: d['profile'].update(render_scale=0.67),
                     lambda d: d.update(internal_width=100),
                     lambda d: d['excluded_windows'][0].update(reason=''),
                     lambda d: d['excluded_windows'][0].update(last_frame=2),
                     lambda d: d.update(excluded_windows=[])]
        for mutate in mutations:
            data = artifact()
            mutate(data)
            with self.assertRaises(ValueError):
                contract.validate(data, QUALIFIED)

    def test_float_internal_dimension_boundary(self):
        data = artifact()
        # The float product rounds to 25.5; Python's double product is below it.
        scale = contract.float32(0.51)
        data.update(width=50, render_scale=scale, internal_width=26, internal_height=367)
        data['profile'].update(name='legacy', width=50, render_scale=scale)
        self.assertEqual(contract.validate(data, QUALIFIED)['verdict'], 'PASS')

    def test_v2_missing_arguments_cli(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'v2.json'
            path.write_text(json.dumps({'schema': 'luminumbra.render_benchmark.v2'}), encoding="utf-8")
            for supplied, missing in [([], ['--position', '--yaw', '--pitch', '--tod']),
                                      (['--position', '0', '0', '0', '--yaw', '0'], ['--pitch', '--tod'])]:
                result = subprocess.run(isolated_script(capture.__file__, str(path), *supplied),
                                        capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(json.loads(result.stdout)['reason'],
                                 'v2 capture requires missing arguments: ' + ', '.join(missing))

    def test_corrupt_and_future_schema(self):
        for mutation in [lambda d: d.update(schema='luminumbra.render_benchmark.v5'),
                         lambda d: d['frame_samples'][0].update(gpu_frame_ms=float('nan')),
                         lambda d: d['frame_samples'][0].update(gpu_frame_ms=True),
                         lambda d: d['frame_samples'][0]['passes']['water'].update(ms=0)]:
            data = artifact()
            mutation(data)
            with self.assertRaises(ValueError):
                capture.validate(data, args())

    def test_workload_mismatch(self):
        with self.assertRaisesRegex(ValueError, 'workload manifest'):
            contract.validate(artifact(), QUALIFIED, workload={'seed': 1})

    def test_current_and_legacy_preset_revisions(self):
        text = (FIXTURES / 'surface-flight.traversal').read_text()
        for revision in (6, 7):
            parsed = contract.traversal_manifest(text.replace('preset_revision 7', f'preset_revision {revision}'))
            self.assertEqual(parsed['preset_revision'], revision)
        for revision in (5, 8):
            with self.assertRaises(ValueError):
                contract.traversal_manifest(text.replace('preset_revision 7', f'preset_revision {revision}'))

    def test_traversal_manifest_and_camera_oracle(self):
        manifest = contract.traversal_manifest((FIXTURES / 'surface-flight.traversal').read_bytes().decode('utf-8'))
        first = [contract.camera_sample(manifest, tick) for tick in range(1801)]
        second = [contract.camera_sample(manifest, tick) for tick in range(1801)]
        self.assertEqual(first, second)
        self.assertEqual(first[0]['position'], [8, 80, 8])
        self.assertEqual(first[900]['position'], [248, 80, 8])
        self.assertEqual(first[1800]['position'], [488, 80, 8])
        with self.assertRaises(ValueError):
            contract.traversal_manifest((FIXTURES / 'surface-flight.traversal').read_bytes().decode('utf-8').replace('expected_ticks 1800', 'expected_ticks 1799'))

    def test_traversal_capture_and_refusals(self):
        with tempfile.TemporaryDirectory() as directory:
            for newline in ('\n', '\r\n'):
                script = Path(directory) / 'short.traversal'
                text = (FIXTURES / 'surface-flight.traversal').read_bytes().decode('utf-8')
                text = text.replace('duration_seconds 60', 'duration_seconds 0.13333333333333333')
                text = text.replace('expected_ticks 1800', 'expected_ticks 4')
                script.write_bytes(text.replace('\r\n', '\n').replace('\n', newline).encode('utf-8'))
                with self.subTest(newline=newline):
                    self.check_traversal_capture(script)

    def check_traversal_capture(self, script):
        text = script.read_bytes().decode('utf-8')
        manifest = contract.traversal_manifest(text)
        data = artifact()
        data['workload'].update({key: value for key, value in manifest.items() if key != 'path'})
        data['workload'].update(kind='traversal', actual_ticks=4, measured_duration_seconds=0.01,
                                script=text, script_path=str(script),
                                camera_samples=[contract.camera_sample(manifest, tick) for tick in (1, 2, 3, 4)])
        for row in data['frame_samples']:
            row['frame_wall_ms'] = 15000
        summary = perf.summarize([15000] * 4, 'ms')
        data['distribution']['frame_wall_ms'].update({key: summary[key] for key in contract.STATS})
        data['avg']['frame_wall_ms'] = 15000
        self.assertEqual(contract.validate(data, QUALIFIED, traversal=script)['verdict'], 'PASS')
        for mutate in [lambda d: d['workload'].update(actual_ticks=1799),
                       lambda d: d['workload'].update(preset_identity='1111111111111111'),
                       lambda d: d['workload'].update(content_identity='1'),
                       lambda d: d['workload'].update(measured_duration_seconds=61),
                       lambda d: d['workload'].update(script='luminumbra.traversal.v2'),
                       lambda d: d['workload']['camera_samples'][1].update(position=[0, 0, 0]),
                       lambda d: d['workload']['camera_samples'][-1].update(tick=1799)]:
            candidate = copy.deepcopy(data)
            mutate(candidate)
            with self.assertRaises(ValueError):
                contract.validate(candidate, QUALIFIED, traversal=script)
        with self.assertRaisesRegex(ValueError, 'manifest does not match'):
            contract.validate(data, QUALIFIED)

    def test_v2_existing_report_checks_unchanged(self):
        # A complete synthetic v2 linked-uniform capture, without any v3 keys.
        data = {'schema': 'luminumbra.render_benchmark.v2', 'width': 1280, 'height': 720,
                'camera': {'position': [0, 0, 0], 'yaw': 0, 'pitch': 0, 'fov': 90},
                'capture_context': {'time_of_day_at_report': 0, 'last_world_streaming_position': [0, 0, 0]},
                'render_uniforms': {'matrix_layout': 'column_major', 'observation': 'report_time_shader_state'}}
        right, up, forward = capture.camera_basis(0, 0)
        view = [right[0], up[0], -forward[0], 0, right[1], up[1], -forward[1], 0,
                right[2], up[2], -forward[2], 0, 0, 0, 0, 1]
        projection = [0.0] * 16
        projection[0], projection[5] = 720 / 1280, 1.0
        for name in ('geometry', 'instanced_static_mesh'):
            data['render_uniforms'][name] = dict(available=True, view=view, projection=projection)
        data['render_uniforms']['lighting'] = {'available': True, 'u_viewPos': [0, 0, 0],
            'u_inverseView': right + [0] + up + [0] + [-v for v in forward] + [0] + [0, 0, 0, 1],
            'u_sun.direction': [0, -1, 0]}
        before = copy.deepcopy(data)
        result = capture.validate(data, args(position=[0, 0, 0], yaw=0, pitch=0, tod=0, qualified_renderer=None))
        self.assertEqual(result['verdict'], 'PASS')
        self.assertEqual(data, before)
        data.update(artifact())
        result = capture.validate(data, args(position=[0, 0, 0], yaw=0, pitch=0, tod=0))
        self.assertEqual(result['measurement']['verdict'], 'PASS')
        self.assertNotIn('measurement', result['max_absolute_errors'])
        self.assertTrue(all(isinstance(value, (int, float)) for value in result['max_absolute_errors'].values()))


def workload_artifact(kind, script):
    """Synthetic state evidence; never a native timing or scene qualification."""
    manifest = contract.traversal_manifest(script.read_bytes().decode('utf-8'))
    data = artifact()
    count = manifest['expected_ticks']
    data['schema'] = contract.WORKLOAD_SCHEMA
    data['frames'] = count
    original = data['frame_samples']
    data['frame_samples'] = [dict(copy.deepcopy(original[index % len(original)]), frame=index + 2,
                                  gpu_frame=index + 2) for index in range(count)]
    for metric in contract.METRICS:
        values = [row[metric] for row in data['frame_samples']]
        summary = perf.summarize(values, 'ms')
        data['distribution'][metric] = {key: summary[key] for key in contract.STATS}
        data['distribution'][metric].update(unit='ms', sample_count=count, unavailable_count=0)
        data['avg'][metric] = sum(values) / count
    for name in contract.PASSES:
        data['avg_ms'][name] = sum(row['passes'][name]['ms'] or 0 for row in data['frame_samples']) / count
    data['avg_ms']['total'] = data['avg']['gpu_pass_sum_ms']
    data['excluded_windows'][-1].update(first_frame=count + 2, last_frame=count + 2)
    observed = data['workload']
    observed.update({key: value for key, value in manifest.items()
                     if key not in ('path', 'point_ticks', 'actions')})
    observed.update(expected_frames=count, actual_ticks=count, script=script.read_bytes().decode('utf-8'),
                    script_path=script.name, measured_duration_seconds=0.001,
                    camera_samples=[contract.camera_sample(manifest, tick) for tick in range(1, count + 1)],
                    actions=[dict(tick=a['tick'], name=a['name'], succeeded=True,
                                  modified_chunks=1 if a['name'] in ('dig', 'fill') else 0)
                             for a in manifest['actions']], observations=[], reload=None)
    state, paused = 'gameplay', False
    document = 'hud'
    for tick in range(1, count + 1):
        for action in manifest['actions']:
            if action['tick'] == tick:
                if action['name'] == 'main_menu':
                    state, document = 'main_menu', 'main_menu'
                elif action['name'] == 'enter_world':
                    state, document = 'gameplay', 'hud'
                elif action['name'] in ('menu_open', 'menu_close'):
                    paused = action['name'] == 'menu_open'
                    document = 'pause' if paused else 'hud'
                elif action['name'] in ('settings_open', 'settings_close'):
                    document = 'settings' if action['name'] == 'settings_open' else 'pause'
        position = contract.camera_sample(manifest, tick)['position']
        transitions = []
        if kind == 'region_crossing' and tick in (2, count - 1):
            transitions = [dict(region=[0, 0], **{'from': 0 if tick == 2 else 1,
                                                'to': 1 if tick == 2 else 0})]
        observed['observations'].append(dict(
            tick=tick, frame=tick + 1, state=state, paused=paused,
            presentation_seconds=(2 + tick) / manifest['tick_rate'],
            ui_document=document if kind == 'menu_entry' else 'none',
            region=[math.floor(contract.float32(position[0]) / 512), math.floor(contract.float32(position[2]) / 512)],
            transitions=transitions, far_requested='ladder', far_effective='ladder',
            cave_requested=0, cave_effective=0, underground=kind == 'cave' and tick >= count // 2,
            camera_in_air=kind == 'cave',
            foliage_build_generation=tick, terrain_uploads=0, stream_queue_depth=0,
            terrain_upload_candidates=0, water_upload_candidates=0, foliage_pending=False,
            world_ready=True, settled=state == 'gameplay'))
    if kind == 'save_load':
        request = manifest['actions'][-1]['tick'] + 1
        observed['reload'] = dict(request_frame=request, first_settled_frame=request, elapsed_ms=1.0)
    data['settings'] = dict(gns_enabled=False,
        active_regions=dict(enabled=True, work_limit=0, hold_ticks=30, reduced_shift=1),
        far_mode=dict(requested='ladder', effective='ladder', cave_requested=0, cave_effective=0),
        graphics_preset=None, graphics_preset_status='not_exposed',
        effective_render=dict(render_scale=1.0, width=1280, height=720, vsync=False,
                              taau=True, ssao_quality=0, cloud_quality=2, fov=45.0,
                              ui_scale=1.0,
                              window_mode='windowed', volumetric_quality=1))
    systems = {}
    for name, params in contract.render_registry().items():
        systems[name] = dict(enabled=True, params={
            key: ([contract.float32(v) for v in param['default']] if param['type'] == 'vec3'
                  else contract.float32(param['default'])) for key, param in params.items()})
    data['settings']['effective_render']['systems'] = systems
    data['settings']['render_overrides'] = copy.deepcopy(data['debug_overrides']['active'])
    data['settings']['simulation_identity'] = '0000000000000000'
    data['settings']['content_files'] = {
        name: 'fnv1a64:0000000000000000' for name in (
            'data/common/systems.json', 'data/common/foliage/scatter_set.json',
            'data/common/creatures/species/example.json', 'data/common/materials.json',
            'data/ui/main_menu.rml', 'res/shaders/water.frag', 'config/game-asset-packs.json',
            'assets/audio/music/themes/main_menu_theme.mp3',
            'game-assets/tree-small-02/1.0.0/data/models/trees/tree.lmesh',
            'inputs/systems_config.json', 'inputs/preset.json', 'inputs/user_settings.json')}
    data['render_identity'] = contract.render_identity(data['settings'])
    foliage = dict(instances=12, candidates=2048, candidate_slots=4096, backend='gpu', observation='window_end')
    warm = perf.summarize([1.0, 2.0], 'ms')
    warm = {key: warm[key] for key in contract.STATS}
    warm.update(unit='ms', sample_count=2, unavailable_count=0)
    data['windows'] = dict(startup=dict(elapsed_ms=50.0, foliage=copy.deepcopy(foliage)),
        warmup=dict(frame_wall_samples_ms=[1.0, 2.0], distribution=warm, foliage=copy.deepcopy(foliage)),
        measured=dict(distribution=copy.deepcopy(data['distribution']['frame_wall_ms']), foliage=foliage))
    return data


class ScriptedWorkloadTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.scripts = {}
        for kind in contract.WORKLOADS:
            path = Path(self.directory.name) / (kind + '.traversal')
            lines = (FIXTURES / (kind.replace('_', '-') + '.traversal')).read_text(encoding='utf-8').splitlines()
            for index, line in enumerate(lines):
                tokens = line.split()
                if tokens[0] == 'duration_seconds':
                    tokens[1] = '1'
                elif tokens[0] == 'expected_ticks':
                    tokens[1] = '30'
                elif tokens[0] in ('point', 'action'):
                    self.assertEqual(int(tokens[1]) % 60, 0)
                    tokens[1] = str(int(tokens[1]) // 60)
                lines[index] = ' '.join(tokens)
            path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
            self.scripts[kind] = path

    def test_scenario_definitions_are_well_formed_and_deterministic(self):
        self.assertEqual(set(contract.WORKLOADS), {'cave', 'edit', 'region_crossing', 'save_load', 'menu_entry'})
        for kind in contract.WORKLOADS:
            with self.subTest(kind=kind):
                text = (FIXTURES / (kind.replace('_', '-') + '.traversal')).read_bytes().decode('utf-8')
                first = contract.script_steps(text)
                self.assertEqual(first, contract.script_steps(text))
                self.assertEqual(first, contract.script_steps(text.replace('\n', '\r\n')))
                self.assertEqual(len(first), 1801)
                self.assertEqual([step['camera']['tick'] for step in first], list(range(1801)))
                self.assertEqual(contract.traversal_manifest(text)['kind'], kind)
                self.assertEqual(sum(len(step['actions']) for step in first),
                                 len(contract.traversal_manifest(text)['actions']))

    def test_new_workloads_validate_with_both_gns_settings(self):
        for kind, script in self.scripts.items():
            for gns in (False, True):
                with self.subTest(kind=kind, gns=gns):
                    data = workload_artifact(kind, script)
                    data['settings']['gns_enabled'] = gns
                    data['render_identity'] = contract.render_identity(data['settings'])
                    self.assertEqual(capture.validate(data, args(traversal=script))['verdict'], 'PASS')
                    self.assertEqual(contract.validate(data, QUALIFIED, traversal=script)['frames'], 30)

    def test_optional_audio_absence_is_validated_and_changes_identity(self):
        script = self.scripts['save_load']
        data = workload_artifact('save_load', script)
        original_identity = data['render_identity']
        content = data['settings']['content_files']
        audio_path = 'assets/audio/music/themes/main_menu_theme.mp3'
        del content[audio_path]
        content['assets/audio'] = None
        data['render_identity'] = contract.render_identity(data['settings'])
        self.assertNotEqual(data['render_identity'], original_identity)
        self.assertEqual(capture.validate(data, args(traversal=script))['verdict'], 'PASS')
        absent_identity = data['render_identity']
        del content['assets/audio']
        data['render_identity'] = contract.render_identity(data['settings'])
        self.assertNotEqual(data['render_identity'], absent_identity)
        self.assertEqual(capture.validate(data, args(traversal=script))['verdict'], 'PASS')
        for path, value in (('assets/audio', 'absent'),
                            ('assets/private.mp3', 'fnv1a64:0000000000000000'),
                            ('assets/audio/../private.mp3', 'fnv1a64:0000000000000000'),
                            ('assets/audio/missing.ogg', None)):
            with self.subTest(path=path):
                invalid = copy.deepcopy(data)
                invalid['settings']['content_files'][path] = value
                invalid['render_identity'] = contract.render_identity(invalid['settings'])
                with self.assertRaises(ValueError):
                    capture.validate(invalid, args(traversal=script))
        content['assets/audio'] = None
        content[audio_path] = 'fnv1a64:0000000000000000'
        data['render_identity'] = contract.render_identity(data['settings'])
        with self.assertRaisesRegex(ValueError, 'absent audio tree'):
            capture.validate(data, args(traversal=script))

    def test_all_new_required_fields_are_enforced(self):
        script = self.scripts['save_load']
        original = workload_artifact('save_load', script)
        paths = [('settings',), ('render_identity',), ('windows',), ('workload', 'actions'),
                 ('workload', 'observations'), ('workload', 'reload')]
        for key in original['settings']:
            paths.append(('settings', key))
        for group in ('active_regions', 'effective_render', 'far_mode'):
            paths += [('settings', group, key) for key in original['settings'][group]]
        for window in ('startup', 'warmup', 'measured'):
            paths += [('windows', window, key) for key in original['windows'][window]]
            paths += [('windows', window, 'foliage', key) for key in original['windows'][window]['foliage']]
        paths += [('workload', 'observations', 0, key) for key in original['workload']['observations'][0]]
        paths += [('workload', 'actions', 0, key) for key in original['workload']['actions'][0]]
        paths += [('workload', 'reload', key) for key in original['workload']['reload']]
        for path in paths:
            data = copy.deepcopy(original)
            target = data
            for part in path[:-1]:
                target = target[part]
            del target[path[-1]]
            with self.subTest(path=path), self.assertRaises((ValueError, KeyError, TypeError)):
                capture.validate(data, args(traversal=script))

    def test_bad_settings_counts_and_window_statistics(self):
        mutations = [lambda d: d['settings'].update(gns_enabled=1),
                     lambda d: d['settings']['active_regions'].update(enabled=1),
                     lambda d: d['settings']['active_regions'].update(work_limit=-1),
                     lambda d: d['settings']['active_regions'].update(work_limit=2**64),
                     lambda d: d['settings']['effective_render'].update(vsync=True),
                     lambda d: d['settings']['effective_render'].update(taau=1),
                     lambda d: d['settings']['effective_render'].update(render_scale=0.67),
                     lambda d: d['settings']['effective_render'].update(ui_scale=3.0),
                     lambda d: d['settings'].update(graphics_preset='quality'),
                     lambda d: d['settings']['far_mode'].update(effective='legacy'),
                     lambda d: d['settings']['far_mode'].update(cave_effective=False),
                     lambda d: d['windows']['warmup']['distribution'].update(p99=0),
                     lambda d: d['windows']['measured']['distribution'].update(sample_count=29),
                     lambda d: d['windows']['measured']['foliage'].update(instances=5000),
                     lambda d: d['windows']['measured']['foliage'].update(candidates=5000),
                     lambda d: d['workload']['observations'][0].update(stream_queue_depth=1),
                     lambda d: d['windows']['measured']['foliage'].update(candidate_slots=True),
                     lambda d: d['windows']['measured']['foliage'].update(observation='capacity'),
                     lambda d: d['workload']['actions'][0].update(succeeded=False),
                     lambda d: d['workload']['observations'][0].update(region=[9, 0]),
                     lambda d: d['workload']['observations'][0].update(tick=0)]
        script = self.scripts['edit']
        for mutate in mutations:
            data = workload_artifact('edit', script)
            mutate(data)
            data['render_identity'] = contract.render_identity(data['settings'])
            with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                capture.validate(data, args(traversal=script))

    def test_workload_evidence_cannot_be_relabelled(self):
        mutations = {
            'cave': [lambda d: [r.update(underground=False) for r in d['workload']['observations']],
                     lambda d: [r.update(camera_in_air=False) for r in d['workload']['observations']],
                     lambda d: [r.update(far_effective='legacy', cave_effective=None) for r in d['workload']['observations']]],
            'edit': [lambda d: d['workload']['actions'][0].update(modified_chunks=0)],
            'region_crossing': [lambda d: [r.update(transitions=[]) for r in d['workload']['observations']],
                                lambda d: d['settings']['active_regions'].update(enabled=False)],
            'save_load': [lambda d: d['workload']['reload'].update(first_settled_frame=20),
                          lambda d: d['workload']['reload'].update(elapsed_ms=1e9),
                          lambda d: d['workload']['reload'].update(request_frame=2)],
            'menu_entry': [lambda d: d['workload']['observations'][4].update(ui_document='pause'),
                           lambda d: d['workload']['observations'][2].update(paused=False),
                           lambda d: d['workload']['observations'][12].update(state='gameplay')],
        }
        for kind, cases in mutations.items():
            script = self.scripts[kind]
            for mutate in cases:
                data = workload_artifact(kind, script)
                mutate(data)
                with self.subTest(kind=kind), self.assertRaises(ValueError):
                    capture.validate(data, args(traversal=script))

    def test_region_uses_native_float_at_positive_and_negative_boundaries(self):
        script = self.scripts['edit']
        original = script.read_text(encoding='utf-8')
        for x, z, expected in ((511.99999, -512.00001, [1, -1]),
                               (511.9999, -512.0001, [0, -2])):
            text = original.replace('24 28 24 -135 -25', f'{x} 28 {z} -135 -25')
            script.write_text(text, encoding='utf-8')
            data = workload_artifact('edit', script)
            self.assertEqual(data['workload']['observations'][0]['region'], expected)
            self.assertEqual(capture.validate(data, args(traversal=script))['verdict'], 'PASS')
            data['workload']['observations'][0]['region'] = [0, 0]
            with self.assertRaisesRegex(ValueError, 'region does not match'):
                capture.validate(data, args(traversal=script))

    def test_resolved_registry_and_identity_are_required(self):
        script = self.scripts['edit']
        original = workload_artifact('edit', script)
        for name, entry in original['settings']['effective_render']['systems'].items():
            paths = [('enabled',)] + [('params', key) for key in entry['params']]
            for path in paths:
                data = copy.deepcopy(original)
                target = data['settings']['effective_render']['systems'][name]
                for part in path[:-1]:
                    target = target[part]
                del target[path[-1]]
                # A recomputed hash must not conceal missing registry evidence.
                data['render_identity'] = contract.render_identity(data['settings'])
                with self.subTest(name=name, path=path), self.assertRaises((KeyError, ValueError)):
                    capture.validate(data, args(traversal=script))
        for name in ('sky_lut_gpu', 'live_weather'):
            data = copy.deepcopy(original)
            data['settings']['effective_render']['systems'][name]['enabled'] = False
            with self.assertRaisesRegex(ValueError, 'identity does not match'):
                capture.validate(data, args(traversal=script))

    def test_comparison_rejects_changed_defaults_and_parameters(self):
        script = self.scripts['edit']
        original = workload_artifact('edit', script)
        reference = Path(self.directory.name) / 'reference.json'
        reference.write_text(json.dumps(original), encoding='utf-8')
        pair_args = args(traversal=script, reference_capture=reference)
        self.assertEqual(capture.validate(original, pair_args)['verdict'], 'PASS')
        systems = original['settings']['effective_render']['systems']
        mutations = [(name, None) for name in systems]
        mutations += [(name, param) for name, entry in systems.items() for param in entry['params']]
        for name, param in mutations:
            data = copy.deepcopy(original)
            entry = data['settings']['effective_render']['systems'][name]
            if param is None:
                entry['enabled'] = not entry['enabled']
            elif isinstance(entry['params'][param], list):
                entry['params'][param][0] += 0.125
            else:
                entry['params'][param] += 0.125
            # Default-file changes carry no CLI/env override; each capture is
            # individually valid, but the comparison must fail closed.
            data['render_identity'] = contract.render_identity(data['settings'])
            self.assertEqual(data['debug_overrides']['active'], {})
            self.assertEqual(capture.validate(data, args(traversal=script))['verdict'], 'PASS')
            with self.subTest(name=name, param=param), self.assertRaisesRegex(ValueError, 'identity differs'):
                capture.validate(data, pair_args)
        with self.assertRaisesRegex(ValueError, 'complete v4'):
            contract.require_matching_render_identity(artifact(), artifact())

    def test_render_identity_canonical_oracle(self):
        settings = {'b': [True, False, None, 1, 18446744073709551615, 1.5, -0.0], 'a': 'snow ☃'}
        self.assertEqual(contract.render_identity(settings), 'fnv1a64:bd4e71012aed102e')
        reordered = dict(reversed(list(settings.items())))
        self.assertEqual(contract.render_identity(settings), contract.render_identity(reordered))
        self.assertEqual(contract.render_identity(settings), contract.render_identity(json.loads(json.dumps(settings))))
        for value in (float('nan'), float('inf'), float('-inf')):
            with self.assertRaises(ValueError):
                contract.render_identity({'value': value})

    def test_comparison_rejects_simulation_and_content_changes(self):
        script = self.scripts['edit']
        original = workload_artifact('edit', script)
        reference = Path(self.directory.name) / 'reference.json'
        reference.write_text(json.dumps(original), encoding='utf-8')
        pair_args = args(traversal=script, reference_capture=reference)
        changes = [('simulation_identity', 'sim.water_high_res'),
                   ('simulation_identity', 'sim.ecology')]
        changes += [('content_files', path) for path in original['settings']['content_files']]
        for field, name in changes:
            data = copy.deepcopy(original)
            if field == 'simulation_identity':
                data['settings'][field] = ('1111111111111111' if name == 'sim.water_high_res'
                                           else '2222222222222222')
            else:
                data['settings'][field][name] = 'fnv1a64:1111111111111111'
            data['render_identity'] = contract.render_identity(data['settings'])
            with self.subTest(input=name):
                self.assertEqual(capture.validate(data, args(traversal=script))['verdict'], 'PASS')
                with self.assertRaisesRegex(ValueError, 'identity differs'):
                    capture.validate(data, pair_args)

    def test_simulation_and_content_identity_evidence_is_required(self):
        script = self.scripts['edit']
        mutations = [lambda s: s.update(simulation_identity=''),
                     lambda s: s.update(simulation_identity=0),
                     lambda s: s.update(content_files={}),
                     lambda s: s['content_files'].pop('data/common/systems.json'),
                     lambda s: s['content_files'].pop('data/common/foliage/scatter_set.json'),
                     lambda s: s['content_files'].pop('res/shaders/water.frag'),
                     lambda s: s['content_files'].pop('config/game-asset-packs.json'),
                     lambda s: s['content_files'].pop('game-assets/tree-small-02/1.0.0/data/models/trees/tree.lmesh'),
                     lambda s: s['content_files'].update({'data/../outside': 'fnv1a64:0000000000000000'}),
                     lambda s: s['content_files'].update({'assets/private.mp3': 'fnv1a64:0000000000000000'}),
                     lambda s: s['content_files'].update({'assets/audio/../private.mp3': 'fnv1a64:0000000000000000'}),
                     lambda s: s['content_files'].update({'data/common/materials.json': 'invalid'})]
        for mutate in mutations:
            data = workload_artifact('edit', script)
            mutate(data['settings'])
            data['render_identity'] = contract.render_identity(data['settings'])
            with self.subTest(settings=data['settings']), self.assertRaises(ValueError):
                capture.validate(data, args(traversal=script))

    def test_presentation_time_is_derived_from_script_ticks(self):
        # Recorded presentation phases must ignore time spent in startup/reload.
        for kind in ('edit', 'save_load', 'menu_entry'):
            script = self.scripts[kind]
            original = workload_artifact(kind, script)
            for delay in (0.25, 10.0, 600.0):
                data = copy.deepcopy(original)
                data['windows']['startup']['elapsed_ms'] += delay * 1000
                self.assertEqual(capture.validate(data, args(traversal=script))['verdict'], 'PASS')
                data['workload']['observations'][0]['presentation_seconds'] += delay
                with self.subTest(kind=kind, delay=delay), self.assertRaisesRegex(ValueError, 'presentation time'):
                    capture.validate(data, args(traversal=script))

    def test_reload_duration_covers_every_complete_frame_interval(self):
        script = FIXTURES / 'save-load.traversal'
        original = workload_artifact('save_load', script)
        # The committed script requests frame 301 with the synthetic two-frame
        # warmup; complete frames 302 and 303 below total exactly 19 ms.
        request = original['workload']['reload']['request_frame']
        self.assertEqual(request, 301)
        settled = request + 3
        for row in original['workload']['observations']:
            if request <= row['frame'] < settled:
                row.update(settled=False, stream_queue_depth=1)
        original['workload']['reload']['first_settled_frame'] = settled
        samples = original['frame_samples']
        samples[request - 2 + 1]['frame_wall_ms'] = 9.0
        samples[request - 2 + 2]['frame_wall_ms'] = 10.0
        values = [row['frame_wall_ms'] for row in samples]
        summary = perf.summarize(values, 'ms')
        original['distribution']['frame_wall_ms'].update({key: summary[key] for key in contract.STATS})
        original['avg']['frame_wall_ms'] = sum(values) / len(values)
        original['windows']['measured']['distribution'] = copy.deepcopy(original['distribution']['frame_wall_ms'])
        complete = sum(row['frame_wall_ms'] for row in samples[request - 2 + 1:settled - 2])
        self.assertEqual(complete, 19.0)
        for elapsed in (0.0, complete - 0.01):
            data = copy.deepcopy(original)
            data['workload']['reload']['elapsed_ms'] = elapsed
            with self.subTest(elapsed=elapsed), self.assertRaisesRegex(ValueError, 'complete frame intervals'):
                capture.validate(data, args(traversal=script))
        for elapsed in (complete, complete - 0.0000005, complete + 0.01):
            data = copy.deepcopy(original)
            data['workload']['reload']['elapsed_ms'] = elapsed
            self.assertEqual(capture.validate(data, args(traversal=script))['verdict'], 'PASS')

    def test_reference_capture_cli_enforces_render_identity(self):
        script = self.scripts['edit']
        data = workload_artifact('edit', script)
        reference = Path(self.directory.name) / 'reference.json'
        reference.write_text(json.dumps(data), encoding='utf-8')
        candidate = Path(self.directory.name) / 'candidate.json'
        command = isolated_script(capture.__file__, str(candidate), '--qualified-renderer', QUALIFIED,
                                  '--traversal', str(script), '--reference-capture', str(reference))
        for changed in (False, True):
            if changed:
                data['settings']['effective_render']['systems']['live_weather']['enabled'] = False
                data['render_identity'] = contract.render_identity(data['settings'])
            candidate.write_text(json.dumps(data), encoding='utf-8')
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1 if changed else 0, result.stdout + result.stderr)
            verdict = json.loads(result.stdout)
            if changed:
                self.assertIn('render identity differs', verdict['reason'])
            else:
                self.assertEqual(verdict['reference_render_identity'], data['render_identity'])

    def test_script_refusals(self):
        text = self.scripts['edit'].read_text(encoding='utf-8')
        for candidate in (text.replace('traversal.v2', 'traversal.v3'),
                          text.replace('workload edit', 'workload unknown'),
                          text.replace('actions 2', 'actions 65'),
                          text.replace('action 2 dig', 'action 0 dig'),
                          text.replace('action 5 fill', 'action 2 fill'),
                          text.replace('point 30', 'point 29'),
                          text.replace(' 8 12 8 8', ' 8 12 8 nan'),
                          text.replace(' 8 12 8 8', ' 8 12 8 17'),
                          text.replace('point 0', 'point 1'), text + 'unknown 1',
                          text[:-3], text + (' ' * (1024 * 1024))):
            with self.subTest(candidate=candidate[:80]), self.assertRaises(ValueError):
                contract.traversal_manifest(candidate)

    def test_versions_and_older_reports(self):
        self.assertEqual(contract.WORKLOAD_SCHEMA, 'luminumbra.render_benchmark.v4')
        self.assertEqual(capture.validate(artifact(), args())['verdict'], 'PASS')
        script = self.scripts['edit']
        for schema in (contract.SCHEMA, 'luminumbra.render_benchmark.v5'):
            data = workload_artifact('edit', script)
            data['schema'] = schema
            with self.assertRaises(ValueError):
                capture.validate(data, args(traversal=script))

    def test_all_versions_join_terrain_capture_without_graphics_dependencies(self):
        manifest = dict(color_dimensions=[1280, 720], gbuffer_dimensions=[1280, 720],
                        render_frame=40, far={'wanted': 1})
        benchmark = dict(width=1280, height=720, internal_width=1280, internal_height=720,
                         terrain_coverage=dict(capture_complete=True, frame_observations=[
                             dict(render_frame=40, benchmark_phase='capture', far={'wanted': 1})]))
        for schema in ('luminumbra.render_benchmark.v2', *contract.SCHEMAS):
            with self.subTest(schema=schema):
                benchmark['schema'] = schema
                self.assertEqual(contract.validate_terrain_benchmark(manifest, benchmark)['capture_frame'], 40)
                bad = copy.deepcopy(benchmark)
                bad['terrain_coverage']['frame_observations'] *= 2
                with self.assertRaises(ValueError):
                    contract.validate_terrain_benchmark(manifest, bad)
                bad = copy.deepcopy(benchmark)
                bad['terrain_coverage']['frame_observations'][0]['far']['wanted'] = 2
                with self.assertRaises(ValueError):
                    contract.validate_terrain_benchmark(manifest, bad)
        benchmark['schema'] = 'luminumbra.render_benchmark.v5'
        with self.assertRaisesRegex(ValueError, 'Unknown benchmark schema'):
            contract.validate_terrain_benchmark(manifest, benchmark)

    def test_new_schema_cli_and_opt_in_budget(self):
        import render_budget
        script = self.scripts['menu_entry']
        data = workload_artifact('menu_entry', script)
        path = Path(self.directory.name) / 'capture.json'
        path.write_text(json.dumps(data), encoding='utf-8')
        command = isolated_script(capture.__file__, str(path), '--qualified-renderer', QUALIFIED,
                                  '--traversal', str(script))
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)['verdict'], 'PASS')
        self.assertEqual(render_budget.evaluate(data)['workload_kind'], 'menu_entry')


if __name__ == '__main__':
    unittest.main()

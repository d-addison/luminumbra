#!/usr/bin/env python3
"""Device-free regressions for shader scene evidence, metrics and CI routing."""
import argparse
import copy
import importlib.util
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


scene = module('shader_scenes', 'tools/gates/shader-scenes.py')
paths = module('scene_paths', 'tools/ci/ci_paths.py')
software = module('scene_software', 'tools/static_preview/run_software_gl.py')
MANIFEST = ROOT / 'test/rendering/shader_scenes/scenes.json'


class ShaderScenesTests(unittest.TestCase):
    def capture(self, directory, value=.25):
        for name in ('static_asset.vert', 'static_asset.frag', 'tree_impostor.vert', 'tree_impostor.frag'):
            (directory / name).write_bytes((ROOT / 'res/shaders' / name).read_bytes())
        fixture = (ROOT / 'test/rendering/shader_scene_render_test.cpp').read_bytes().replace(b'\r\n', b'\n')
        (directory / 'fixture-sha256.txt').write_text(scene.digest(fixture), encoding='ascii')
        (directory / 'material.pfm').write_bytes(b'PF\n128 128\n-1.0\n' +
                                               struct.pack('<f', value) * scene.CHANNELS)
        (directory / 'tree-cutout.pfm').write_bytes(
            b'PF\n128 128\n-1.0\n' + struct.pack('<' + 'f' * scene.CHANNELS, *self.cutout()))
        (directory / 'renderer.txt').write_text('llvmpipe (test fixture)\n', encoding='utf-8')
        (directory / 'version.txt').write_text('4.5 Mesa fixture\n', encoding='utf-8')

    @staticmethod
    def cutout():
        return [value for y in range(128) for x in range(128)
                for value in ((.0625, .125, .1875) if (x // 32 + y // 32) % 4 < 2
                              else (.25, .5, .75))]

    def test_inventory_and_analytical_reference(self):
        manifest, data = scene.load_manifest(MANIFEST)
        values = scene.rgb(data['material'], False)
        self.assertEqual(len(values), scene.CHANNELS)
        self.assertEqual(scene.metrics(values, values, manifest['fixtures'][0]['mask']), (0, 0))
        self.assertIn(manifest['reference_capture']['origin'], ('analytical', 'llvmpipe'))
        tree = scene.rgb(data['tree-cutout'], False)
        self.assertLessEqual(max(abs(a - b) for a, b in zip(tree, self.cutout())), .5 / 255)
        self.assertEqual(set(data), {'material', 'tree-cutout'})

    def test_exact_thresholds_and_mask(self):
        zeros = (0.,) * scene.CHANNELS
        for value, passes in ((.004, True), (.005, True), (.0051, False), (.021, False)):
            mean, percentile = scene.metrics((value,) * scene.CHANNELS, zeros, [0, 0, 128, 128])
            self.assertEqual(mean <= .005 and percentile <= .02, passes)
        sparse = list(zeros)
        sparse[:1000] = [.021] * 1000
        mean, percentile = scene.metrics(sparse, zeros, [0, 0, 128, 128])
        self.assertLess(mean, .005)
        self.assertGreater(percentile, .02)
        self.assertEqual(scene.metrics(sparse, zeros, [64, 64, 128, 128]), (0, 0))
        for mask in ([0, 0, 0, 0], [-1, 0, 128, 128], [0, 0, 129, 128], [0., 0, 1, 1], []):
            with self.subTest(mask=mask), self.assertRaises(ValueError):
                scene.metrics(zeros, zeros, mask)
        for value in (math.nan, math.inf, -math.inf):
            invalid = list(zeros)
            invalid[0] = value
            with self.assertRaises(ValueError):
                scene.metrics(invalid, zeros, [64, 64, 128, 128])
        with self.assertRaises(ValueError):
            scene.metrics([], zeros, [0, 0, 1, 1])

    def test_image_refusals(self):
        good = b'PF\n128 128\n-1.0\n' + struct.pack('<f', .25) * scene.CHANNELS
        self.assertEqual(scene.rgb(good, True)[0], .25)
        for bad in (b'', good[:-1], good + b'\0', good.replace(b'128 128', b'129 128'),
                    good.replace(b'-1.0', b'1.0'), good.replace(b'PF', b'Pf')):
            with self.subTest(header=bad[:20]), self.assertRaises(ValueError):
                scene.rgb(bad, True)
        for value in (math.nan, math.inf, -math.inf, -.01, 1.01):
            with self.subTest(value=value), self.assertRaises(ValueError):
                scene.rgb(good[:len(b'PF\n128 128\n-1.0\n')] + struct.pack('<f', value) * scene.CHANNELS, True)

    def test_bounded_regular_reads_and_path_refusals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / 'small'
            target.write_bytes(b'1234')
            self.assertEqual(scene.read_file(target, 4), b'1234')
            with self.assertRaises(ValueError):
                scene.read_file(target, 3)
            with patch.object(scene.os, 'fstat', return_value=SimpleNamespace(
                    st_mode=target.stat().st_mode, st_size=0)):
                with self.assertRaisesRegex(ValueError, 'exceeds bound'):
                    scene.read_file(target, 3)
            with patch.object(scene.os, 'fstat', return_value=SimpleNamespace(st_mode=0, st_size=0)):
                with self.assertRaisesRegex(ValueError, 'regular file'):
                    scene.read_file(target)
            for path in (root, root / 'absent'):
                with self.assertRaises((OSError, ValueError)):
                    scene.read_file(path)
            for name in ('../outside', '/absolute', 'C:/drive', 'a\\b', '', './small', 'x//small'):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    scene.local_file(root, name)
            self.assertEqual(scene.local_file(root, 'small'), target)
            with patch.object(scene.os, 'open', side_effect=PermissionError('denied')):
                with self.assertRaises(PermissionError):
                    scene.read_file(target)
            with patch.object(Path, 'resolve', side_effect=[root.parent / 'escape', root]):
                with self.assertRaisesRegex(ValueError, 'escapes'):
                    scene.local_file(root, 'small')

    def test_manifest_refuses_partial_unknown_and_changed_inputs(self):
        original = json.loads(MANIFEST.read_text(encoding='utf-8'))
        mutations = [dict(schema=1), dict(schema=3), dict(schema=True), dict(size=[128., 128]), dict(size=[129, 128]), dict(defines=['UNKNOWN 1']),
                     dict(encoding='srgb'), dict(fixtures=[]), dict(inputs={}),
                     dict(reference_sha256='0' * 64), dict(reference_sha256={}),
                     dict(reference_sha256={'material': '0' * 64, 'tree-cutout': '0' * 64}), dict(reference_capture={}), dict(inputs=None),
                     dict(reference_capture=None),
                     dict(reference_capture={'origin': 'llvmpipe', 'renderer': 'hardware', 'version': '4.5'})]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'references').mkdir()
            for fixture in scene.FIXTURES:
                (root / fixture['reference']).write_bytes(
                    (MANIFEST.parent / fixture['reference']).read_bytes())
            for changed in mutations:
                data = dict(original, **changed)
                path = root / 'scenes.json'
                path.write_text(json.dumps(data), encoding='utf-8')
                with self.subTest(changed=changed), self.assertRaises(ValueError):
                    scene.load_manifest(path)
            data = copy.deepcopy(original)
            data['inputs']['res/shaders/static_asset.frag'] = '0' * 64
            path.write_text(json.dumps(data), encoding='utf-8')
            with self.assertRaisesRegex(ValueError, 'input hash changed'):
                scene.load_manifest(path)
            with self.assertRaises((ValueError, OSError)):
                scene.local_file(root, 'missing/reference.ppm')

    def test_compare_and_regenerate_require_complete_evidence_before_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / 'capture'
            capture.mkdir()
            self.capture(capture)
            output = root / 'candidate'
            args = argparse.Namespace(command='regenerate', manifest=MANIFEST, capture=capture, output=output)
            (capture / 'renderer.txt').write_text('hardware GPU', encoding='utf-8')
            with self.assertRaises(ValueError):
                scene.run(args)
            self.assertFalse(output.exists())
            self.capture(capture)
            for filename, content in (('fixture-sha256.txt', b'0' * 64),
                                      ('fixture-sha256.txt', b'unknown'),
                                      ('static_asset.frag', b'old shader')):
                (capture / filename).write_bytes(content)
                with self.subTest(filename=filename), self.assertRaises(ValueError):
                    scene.run(args)
                self.assertFalse(output.exists())
                self.capture(capture)
            self.assertEqual(scene.run(args), 0)
            regenerated, unused = scene.load_manifest(output / 'scenes.json')
            self.assertEqual(regenerated['reference_capture']['origin'], 'llvmpipe')
            with self.assertRaises(FileExistsError):
                scene.run(args)
            args.command = 'compare'
            args.manifest = output / 'scenes.json'
            previous = copy.deepcopy(regenerated)
            previous['inputs']['res/shaders/static_asset.frag'] = '0' * 64
            with patch.object(scene, 'load_manifest', return_value=(previous, unused)):
                with self.assertRaisesRegex(ValueError, 'capture inputs differ'):
                    scene.run(args)
            self.assertEqual(scene.run(args), 0)
            self.capture(capture, .9)
            self.assertEqual(scene.run(args), 1)
            (capture / 'version.txt').unlink()
            with self.assertRaises(OSError):
                scene.run(args)
            self.assertTrue((output / 'scenes.json').is_file())

    def test_regeneration_io_failure_leaves_baseline_and_completion_metadata_unchanged(self):
        baseline = MANIFEST.read_bytes()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / 'capture'
            capture.mkdir()
            self.capture(capture)
            output = root / 'candidate'
            args = argparse.Namespace(command='regenerate', manifest=MANIFEST, capture=capture, output=output)
            with patch.object(Path, 'write_bytes', side_effect=OSError('short write')):
                with self.assertRaises(OSError):
                    scene.run(args)
            self.assertFalse((output / 'scenes.json').exists())
            self.assertEqual(MANIFEST.read_bytes(), baseline)
            args.output = None
            with self.assertRaisesRegex(ValueError, 'fresh --output'):
                scene.run(args)

    def test_second_reference_inventory_refusals_and_late_write_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'references').mkdir()
            original = json.loads(MANIFEST.read_text(encoding='utf-8'))
            for fixture in scene.FIXTURES:
                (root / fixture['reference']).write_bytes((MANIFEST.parent / fixture['reference']).read_bytes())
            for mutation in ('duplicate', 'unknown', 'float-mask', 'missing-hash', 'corrupt-reference'):
                manifest = copy.deepcopy(original)
                if mutation == 'duplicate':
                    manifest['fixtures'].append(manifest['fixtures'][0])
                elif mutation == 'unknown':
                    manifest['fixtures'][1]['name'] = 'unknown'
                elif mutation == 'float-mask':
                    manifest['fixtures'][1]['mask'][0] = 0.0
                elif mutation == 'missing-hash':
                    del manifest['reference_sha256']['tree-cutout']
                else:
                    manifest['reference_sha256']['tree-cutout'] = '0' * 64
                (root / 'scenes.json').write_text(json.dumps(manifest), encoding='utf-8')
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    scene.load_manifest(root / 'scenes.json')
            tree_reference = root / 'references/tree-cutout.ppm'
            good_reference = tree_reference.read_bytes()
            for payload in (None, b'', good_reference[:-1], good_reference + b'0'):
                manifest = copy.deepcopy(original)
                if payload is None:
                    tree_reference.unlink()
                else:
                    tree_reference.write_bytes(payload)
                    manifest['reference_sha256']['tree-cutout'] = scene.digest(payload)
                (root / 'scenes.json').write_text(json.dumps(manifest), encoding='utf-8')
                with self.subTest(payload_size=None if payload is None else len(payload)), \
                        self.assertRaises((OSError, ValueError)):
                    scene.load_manifest(root / 'scenes.json')
                tree_reference.write_bytes(good_reference)
            capture = root / 'capture'
            capture.mkdir()
            self.capture(capture)
            output = root / 'candidate'
            write = Path.write_bytes

            def fail_second(path, payload):
                if path.name == 'tree-cutout.ppm':
                    raise OSError('second reference failed')
                return write(path, payload)

            args = argparse.Namespace(command='regenerate', manifest=MANIFEST, capture=capture, output=output)
            with patch.object(Path, 'write_bytes', fail_second), self.assertRaises(OSError):
                scene.run(args)
            self.assertTrue((output / 'references/material.ppm').is_file())
            self.assertFalse((output / 'scenes.json').exists())

    def test_cutout_categories_and_failure_samples(self):
        actual = self.cutout()
        self.assertEqual(scene.tree_cutout_error(actual), 0)
        for index in (0, 32 * 3, 64 * 3, 96 * 3, scene.CHANNELS - 3):
            # Transparent, below cutoff, exact cutoff and opaque texels, with
            # an error at even one pixel: a whole-image MAE can miss this.
            for delta, passes in ((.0000009, True), (.0000011, False), (.1, False)):
                changed = actual.copy()
                changed[index] += delta
                self.assertEqual(scene.tree_cutout_error(changed) <= .000001, passes)
        for invalid in ([], actual[:-1], actual + [0],
                        [math.nan] + actual[1:], [math.inf] + actual[1:],
                        [-math.inf] + actual[1:]):
            with self.assertRaises(ValueError):
                scene.tree_cutout_error(invalid)

    def test_second_scene_evidence_is_mandatory_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            capture = root / 'capture'
            capture.mkdir()
            output = root / 'candidate'
            args = argparse.Namespace(command='regenerate', manifest=MANIFEST,
                                      capture=capture, output=output)
            for filename in ('tree_impostor.vert', 'tree_impostor.frag', 'tree-cutout.pfm'):
                for bad in (None, b'', b'partial'):
                    self.capture(capture)
                    if bad is None:
                        (capture / filename).unlink()
                    else:
                        (capture / filename).write_bytes(bad)
                    with self.subTest(filename=filename, bad=bad), self.assertRaises((OSError, ValueError)):
                        scene.run(args)
                    self.assertFalse(output.exists())
            self.capture(capture)
            # An opaque rectangle must fail both comparison and regeneration,
            # independently of the reference score or baseline regeneration.
            (capture / 'tree-cutout.pfm').write_bytes(
                b'PF\n128 128\n-1.0\n' + struct.pack('<f', .25) * scene.CHANNELS)
            for command in ('compare', 'regenerate'):
                args.command = command
                with self.assertRaisesRegex(ValueError, 'cutout/background invariant'):
                    scene.run(args)
                self.assertFalse(output.exists())

    def test_every_rendering_and_harness_path_routes_and_docs_do_not(self):
        sensitive = ('res/shaders/new.frag', 'src/luminumbra_client/rendering/new.cpp',
                     'test/rendering/shader_scenes/references/material.ppm',
                     'test/rendering/shader_scene_render_test.cpp', 'test/CMakeLists.txt',
                     'tools/gates/shader-scenes.py', 'tools/gates/test_shader_scenes.py',
                     'tools/static_preview/run_software_gl.py', 'tools/ci/ci_paths.py',
                     'tools/ci/test_ci_paths.py', 'tools/ci/compile_shaders.py',
                     'tools/ci/test_compile_shaders.py', 'tools/ci/shader_variants.json',
                     '.github/workflows/ci.yml', 'CMakeLists.txt', '.gitattributes')
        for path in sensitive:
            with self.subTest(path=path), patch.object(paths, 'changed_paths', return_value=[path]):
                self.assertTrue(paths.classify_shader_scenes(ROOT, 'pull_request', 'base', 'head'))
                self.assertTrue(paths.needs_cpp(path))
        with patch.object(paths, 'changed_paths', return_value=['docs/readme.md']):
            self.assertFalse(paths.classify_shader_scenes(ROOT, 'pull_request', 'base', 'head'))
        for revisions in (('', 'head'), ('base', '')):
            with self.assertRaises(ValueError):
                paths.classify_shader_scenes(ROOT, 'pull_request', *revisions)
        for result in ([], None):
            with patch.object(paths, 'changed_paths', return_value=result), self.assertRaises(ValueError):
                paths.classify_shader_scenes(ROOT, 'pull_request', 'base', 'head')
        with patch.object(paths, 'changed_paths', side_effect=subprocess.CalledProcessError(1, 'git')):
            with self.assertRaises(subprocess.CalledProcessError):
                paths.classify_shader_scenes(ROOT, 'pull_request', 'base', 'head')
        self.assertTrue(paths.classify_shader_scenes(ROOT, 'push', '', ''))

    def test_runner_exact_scene_inventory_rejects_missing_substituted_and_skipped_cases(self):
        source = (ROOT / 'test/rendering/shader_scene_render_test.cpp').read_text(encoding='utf-8')
        registered = re.findall(r'^TEST_F\(\s*(\w+)\s*,\s*(\w+)\s*\)', source, re.MULTILINE)
        self.assertEqual(set(registered), software.SHADER_CASES)
        root = ET.Element('testsuites')
        for classname, name in registered:
            ET.SubElement(root, 'testcase', classname=classname, name=name, status='run')
        software.validate_report(root, software.SHADER_CASES)
        for tag in ('failure', 'error', 'skipped'):
            bad = copy.deepcopy(root)
            ET.SubElement(bad[0], tag)
            with self.assertRaises(RuntimeError):
                software.validate_report(bad, software.SHADER_CASES)
        for field, value in (('name', 'substitute'), ('classname', 'other'), ('status', 'notrun'),
                             ('result', 'suppressed')):
            bad = copy.deepcopy(root)
            bad[0].set(field, value)
            with self.assertRaises(RuntimeError):
                software.validate_report(bad, software.SHADER_CASES)
        for bad in (ET.Element('testsuites'), ET.fromstring(ET.tostring(root))):
            if len(bad):
                bad.append(copy.deepcopy(bad[0]))
            with self.assertRaises(RuntimeError):
                software.validate_report(bad, software.SHADER_CASES)

    def test_runner_profiles_forward_output_and_force_software_without_native_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            for profile, expected in (('static-preview', software.EXPECTED_CASES),
                                      ('shader-scenes', software.SHADER_CASES)):
                output = Path(temporary) / profile

                def execute(command, **kwargs):
                    env = kwargs['env']
                    self.assertEqual(env['LIBGL_ALWAYS_SOFTWARE'], '1')
                    self.assertEqual(env['GALLIUM_DRIVER'], 'llvmpipe')
                    self.assertEqual(env['DISPLAY'], ':42')
                    if profile == 'shader-scenes':
                        self.assertEqual(env['LUMINUMBRA_SHADER_SCENE_OUTPUT'], str(output.resolve()))
                    root = ET.Element('testsuites')
                    for classname, name in sorted(expected):
                        ET.SubElement(root, 'testcase', classname=classname, name=name, status='run')
                    self.assertEqual(command, ['fixture-binary', '--gtest_output=xml:' + str(output / 'tests.xml')])
                    (output / 'tests.xml').write_bytes(ET.tostring(root))
                    return SimpleNamespace(returncode=0, stdout=b'fixture output')

                with patch.object(sys, 'argv', ['run_software_gl.py', '--binary', 'fixture-binary',
                                               '--output', str(output), '--profile', profile]), \
                        patch.object(software.subprocess, 'Popen') as server, \
                        patch.object(software.subprocess, 'run', side_effect=execute), \
                        patch.object(software.selectors, 'DefaultSelector') as selector, \
                        patch.object(software.os, 'read', return_value=b'42\n'):
                    server.return_value.poll.return_value = 0
                    selector.return_value.select.return_value = [True]
                    software.main()
                    self.assertTrue((output / 'tests.log').is_file())

    def test_ci_registration_and_gate_share_reference_and_consumer(self):
        self.assertIn(('tools/gates/test_shader_scenes.py',), paths.python_test_commands(ROOT))
        cmake = (ROOT / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('add_test(NAME ShaderScenesPython', cmake)
        self.assertIn('add_test(NAME ShaderSceneSoftwareGl', cmake)
        self.assertLess(cmake.index('add_test(NAME ShaderScenesPython'), cmake.index('add_test(NAME RenderBenchmarkAssets'))
        workflow = (ROOT / '.github/workflows/ci.yml').read_text(encoding='utf-8').split('  fast-compile:', 1)[1].split('  tidy-shard:', 1)[0]
        for step in ('Render and compare software shader scenes', 'Upload software shader scenes'):
            block = workflow.split('- name: ' + step, 1)[1].split('      - name:', 1)[0]
            self.assertIn("steps.shader-scenes.outputs.needs_shader_scenes == 'true'", block)
        self.assertIn('python3 -I -B tools/ci/ci_paths.py classify-shader-scenes', workflow)
        self.assertIn('python3 -I -B tools/gates/test_shader_scenes.py', workflow)
        configure_calls = [line for line in workflow.splitlines()
                           if 'cmake ' in line and ' -S . ' in line]
        self.assertEqual(len(configure_calls), 2)
        for call in configure_calls:
            self.assertIn('"-DPython3_EXECUTABLE=$(python3 -I -B -c '
                          "'import sys; print(sys.executable)')\"", call)
        self.assertIn('--target shader_scene_render_test', workflow)
        self.assertEqual(workflow.count('-DLUMINUMBRA_SHADER_SCENES="${{ steps.shader-scenes.outputs.needs_shader_scenes }}"'), 2)
        self.assertIn('tools/gates/shader-scenes.py compare', workflow)
        self.assertNotIn('continue-on-error', workflow.split('- name: Render and compare', 1)[1].split('- name: Replay', 1)[0])

    def test_scene_registration_shares_graphics_lock_only_when_enabled(self):
        source = (ROOT / 'test/CMakeLists.txt').read_text(encoding='utf-8')
        start = source.index('    add_test(NAME ShaderSceneSoftwareGl')
        end = source.index('\nendif()', start)
        registration = source[start:end]
        with tempfile.TemporaryDirectory(prefix='shader scene registration ') as temporary:
            for enabled in ('OFF', 'ON'):
                with self.subTest(enabled=enabled):
                    project = Path(temporary) / enabled
                    project.mkdir()
                    build = project / 'build with spaces'
                    (project / 'CMakeLists.txt').write_text(
                        'cmake_minimum_required(VERSION 3.20)\n'
                        'project(ShaderSceneRegistration LANGUAGES NONE)\n'
                        'enable_testing()\n'
                        f'set(CMAKE_SOURCE_DIR [=[{ROOT.as_posix()}]=])\n'
                        f'set(Python3_EXECUTABLE [=[{Path(sys.executable).as_posix()}]=])\n'
                        'add_executable(shader_scene_render_test IMPORTED)\n'
                        'set_target_properties(shader_scene_render_test PROPERTIES\n'
                        f'    IMPORTED_LOCATION [=[{Path(sys.executable).as_posix()}]=])\n'
                        f'if({enabled})\n{registration}\nendif()\n', encoding='utf-8')
                    subprocess.run(['cmake', '-S', str(project), '-B', str(build), '-G', 'Ninja'],
                                   check=True, capture_output=True, text=True)
                    # Inspect registration metadata; never execute the native scene.
                    result = subprocess.run(['ctest', '--test-dir', str(build), '--show-only=json-v1'],
                                            check=True, capture_output=True, text=True)
                    tests = json.loads(result.stdout)['tests']
                    if enabled == 'OFF':
                        self.assertEqual(tests, [])
                        continue
                    self.assertEqual([test['name'] for test in tests], ['ShaderSceneSoftwareGl'])
                    properties = {item['name']: item['value'] for item in tests[0]['properties']}
                    self.assertEqual(properties.get('RESOURCE_LOCK'), ['luminumbra_gl_context'])
                    self.assertEqual(properties['TIMEOUT'], 180)
                    self.assertEqual(tests[0]['command'], [
                        Path(sys.executable).as_posix(), '-I', '-B',
                        (ROOT / 'tools/static_preview/run_software_gl.py').as_posix(),
                        '--binary', Path(sys.executable).as_posix(), '--profile', 'shader-scenes',
                        '--output', (build / 'shader-scene-capture').as_posix()])
                    for forbidden in ('DISABLED', 'SKIP_RETURN_CODE', 'SKIP_REGULAR_EXPRESSION',
                                      'LABELS', 'RUN_SERIAL'):
                        self.assertNotIn(forbidden, properties)


if __name__ == '__main__':
    unittest.main()

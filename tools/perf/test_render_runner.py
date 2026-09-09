#!/usr/bin/env python3
"""Client-process regression tests for the opt-in render measurement runner."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import render_contract as contract


class RenderRunnerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=OPTIONS.artifacts)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        # Runtime discovery starts at cwd. Isolate saves and settings; shared asset
        # inputs are read-only, and presets get independent copies for mutations.
        def link_or_copy(source, target):
            try:
                return os.link(source, target)
            except OSError:
                return shutil.copy2(source, target)
        for name in ('data', 'res', 'config'):
            # Do not link the shipping manifest: only the synthetic root's
            # manifest describes these test inputs, and the source stays intact.
            ignore = shutil.ignore_patterns('game-asset-packs.json') if name == 'config' else None
            shutil.copytree(OPTIONS.source / name, self.root / name,
                            copy_function=link_or_copy, ignore=ignore)
        shutil.copy2(OPTIONS.assets / 'config/game-asset-packs.json',
                     self.root / 'config/game-asset-packs.json')
        shutil.copytree(OPTIONS.assets / 'game-assets', self.root / 'game-assets',
                        copy_function=link_or_copy)
        shutil.copytree(OPTIONS.source / 'worlds/atlas', self.root / 'worlds/atlas')
        self.environment = os.environ.copy()
        for name in list(self.environment):
            if name.startswith(('LUMIN_', 'LUMINUMBRA_')):
                del self.environment[name]
        self.environment.update(XDG_CONFIG_HOME=str(self.root / 'settings'),
                                APPDATA=str(self.root / 'settings'))
        self.script = self.root / 'short.traversal'
        text = (OPTIONS.source / 'tools/perf/fixtures/surface-flight.traversal').read_bytes().decode('utf-8')
        text = text.replace('duration_seconds 60', 'duration_seconds 0.13333333333333333')
        text = text.replace('expected_ticks 1800', 'expected_ticks 4')
        self.script.write_bytes(text.encode('utf-8'))

    def run_client(self, name, extra=(), environment=None, expected_exit=0):
        artifact = self.root / (name + '.json')
        log = OPTIONS.artifacts / (self._testMethodName + '-' + name + '.log')
        command = [str(OPTIONS.client), '--no-audio', '--no-menu-backdrop',
                   '--render-benchmark', str(artifact), '--render-benchmark-schema', 'v3',
                   '--capture-size', '160x90', '--perf-profile', 'quality',
                   '--render-benchmark-warmup', '2', *extra]
        with log.open('wb') as output:
            result = subprocess.run(command, cwd=self.root, env=environment or self.environment,
                                    stdout=output, stderr=subprocess.STDOUT, timeout=180, check=False)
        self.assertEqual(result.returncode, expected_exit, log.read_text(errors='replace')[-6000:])
        if expected_exit:
            self.assertFalse(artifact.exists())
            return log.read_text(errors='replace')
        # A completed capture alone could hide a mesh/material fallback. Require
        # the real loaders to accept the synthetic inputs on every success path.
        output = log.read_text(errors='replace')
        self.assertIn('Static-model textures registered (3 models).', output)
        for part in ('trunk', 'branches', 'leaves'):
            self.assertIn(f"tree_small_02_{part}.lmesh' (3 verts, 3 indices)", output)
        data = json.loads(artifact.read_bytes())
        (OPTIONS.artifacts / (self._testMethodName + '-' + name + '.json')).write_bytes(artifact.read_bytes())
        return data

    def test_traversal_twice(self):
        first = self.run_client('first', ['--traversal', str(self.script)])
        second = self.run_client('second', ['--traversal', str(self.script)])
        for data in (first, second):
            contract.validate(data, data['adapter']['renderer'], traversal=self.script)
            self.assertEqual(data['workload']['actual_ticks'], 4)
            self.assertEqual([row['tick'] for row in data['workload']['camera_samples']], [1, 2, 3, 4])
        self.assertEqual(first['workload']['camera_samples'], second['workload']['camera_samples'])
        self.assertEqual(first['workload']['actual_ticks'], second['workload']['actual_ticks'])

    def test_implicit_creation_failure(self):
        log = self.run_client('missing', ['--world-preset', 'nonexistent_render_test_preset'], expected_exit=2)
        self.assertIn('World open refused', log)
        self.assertIn('nonexistent_render_test_preset', log)

    def test_gl_debug_override_collection(self):
        environment = dict(self.environment, LUMIN_GL_DEBUG='1')
        data = self.run_client('undeclared', ['--render-benchmark-frames', '4'], environment)
        self.assertEqual(data['debug_overrides']['active']['env:LUMIN_GL_DEBUG'], '1')
        with self.assertRaisesRegex(ValueError, 'undeclared'):
            contract.validate(data, data['adapter']['renderer'])
        declaration = self.root / 'overrides.json'
        declaration.write_text(json.dumps({'env:LUMIN_GL_DEBUG': '1'}))
        data = self.run_client('declared', ['--render-benchmark-frames', '4',
                                          '--declare-render-overrides', str(declaration)], environment)
        contract.validate(data, data['adapter']['renderer'])

    def test_embedded_preset_refusal(self):
        original = self.run_client('original', ['--render-benchmark-frames', '4'])
        saves = list((self.root / 'worlds/saves').iterdir())
        self.assertEqual(len(saves), 1)
        saved = saves[0]
        metadata = json.loads((saved / 'world_info.json').read_text())
        self.assertEqual(metadata['worldType'], 'default')
        preset = json.loads((self.root / 'worlds/atlas/presets/default.json').read_text())
        preset['generation_params']['terrain']['height_offset'] += 1
        (saved / 'preset.json').write_text(json.dumps(preset))
        altered = self.run_client('altered', ['--load-world', saved.name, '--render-benchmark-frames', '4'])
        self.assertEqual(altered['workload']['preset'], original['workload']['preset'])
        for key in ('preset_identity', 'content_identity'):
            self.assertNotEqual(altered['workload'][key], original['workload'][key])
            with self.assertRaisesRegex(ValueError, 'workload manifest'):
                contract.validate(altered, altered['adapter']['renderer'],
                                  workload={key: original['workload'][key]})
        log = self.run_client('refused', ['--load-world', saved.name, '--traversal', str(self.script)], expected_exit=2)
        self.assertIn('content identity mismatch', log)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--client', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--assets', type=Path, required=True,
                        help='Synthetic content root containing config/ and game-assets/')
    parser.add_argument('--artifacts', type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    OPTIONS.artifacts.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[__file__, *remaining])

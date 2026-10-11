#!/usr/bin/env python3
"""Client-process regression tests for the opt-in render measurement runner."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

import render_contract as contract


def _file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _atomic_write(path, payload):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + '.',
                                         suffix='.tmp', delete=False) as output:
            temporary = Path(output.name)
            output.write(payload)
        temporary.replace(path)
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass  # Cleanup must not replace the original write/replace error.


def _retain_process_evidence(receipt, artifact, retained, receipt_path):
    errors = receipt['evidence_errors']
    try:
        payload = artifact.read_bytes()
    except FileNotFoundError:
        receipt['capture_status'] = 'missing'
    except OSError as error:
        receipt['capture_status'] = 'read_error'
        errors.append(f'capture read: {error}')
    else:
        try:
            json.loads(payload)
        except (ValueError, UnicodeError, RecursionError):
            receipt['capture_status'] = 'invalid_json'
        else:
            try:
                _atomic_write(retained, payload)
            except OSError as error:
                receipt['capture_status'] = 'write_error'
                errors.append(f'capture retention: {error}')
            else:
                receipt.update(capture_status='retained', capture_path=str(retained),
                               capture_sha256=hashlib.sha256(payload).hexdigest())
    try:
        _atomic_write(receipt_path, (json.dumps(receipt, indent=2) + '\n').encode('utf-8'))
    except OSError as error:
        errors.append(f'process receipt: {error}')
    if errors:
        try:
            print('Render runner evidence error: ' + '; '.join(errors), file=sys.stderr)
        except (OSError, ValueError):
            pass  # A closed diagnostic stream must not hide a process failure.
    return errors


def _run_process(command, *, cwd, environment, log, artifact, retained, receipt_path):
    # Retire only this case's exact outputs, including a repeated invocation in
    # the same runtime root. Never let an earlier capture appear current.
    for path in (artifact, retained, receipt_path, log):
        path.unlink(missing_ok=True)
    receipt = dict(schema='luminumbra.render_runner_process.v1', command=command,
                   cwd=str(cwd), binary_path=command[0], binary_sha256=None,
                   timeout_seconds=180, elapsed_seconds=None, outcome='spawn_error',
                   return_code=None, capture_status='missing', capture_path=None,
                   capture_sha256=None, evidence_errors=[])
    try:
        receipt['binary_sha256'] = _file_sha256(Path(command[0]))
        with log.open('wb') as output:
            started = time.monotonic()
            try:
                result = subprocess.run(command, cwd=cwd, env=environment,
                                        stdout=output, stderr=subprocess.STDOUT,
                                        timeout=180, check=False)
            finally:
                receipt['elapsed_seconds'] = time.monotonic() - started
            receipt.update(outcome='returned', return_code=result.returncode)
    except (subprocess.TimeoutExpired, OSError) as error:
        receipt.update(outcome='timed_out' if isinstance(error, subprocess.TimeoutExpired)
                       else receipt['outcome'], exception_type=type(error).__name__,
                       exception_message=str(error))
        _retain_process_evidence(receipt, artifact, retained, receipt_path)
        raise
    errors = _retain_process_evidence(receipt, artifact, retained, receipt_path)
    return result, errors


def _require_evidence(errors):
    if errors:
        raise RuntimeError('Render runner evidence could not be retained: ' + '; '.join(errors))


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
        retained = OPTIONS.artifacts / (self._testMethodName + '-' + name + '.json')
        receipt_path = retained.with_suffix('.process.json')
        result, evidence_errors = _run_process(
            command, cwd=self.root, environment=environment or self.environment,
            log=log, artifact=artifact, retained=retained, receipt_path=receipt_path)
        self.assertEqual(result.returncode, expected_exit, log.read_text(errors='replace')[-6000:])
        if expected_exit:
            self.assertFalse(artifact.exists())
            _require_evidence(evidence_errors)
            return log.read_text(errors='replace')
        # A completed capture alone could hide a mesh/material fallback. Require
        # the real loaders to accept the synthetic inputs on every success path.
        output = log.read_text(errors='replace')
        self.assertIn('Static-model textures registered (3 models).', output)
        for part in ('trunk', 'branches', 'leaves'):
            self.assertIn(f"tree_small_02_{part}.lmesh' (3 verts, 3 indices)", output)
        data = json.loads(artifact.read_bytes())
        _require_evidence(evidence_errors)
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

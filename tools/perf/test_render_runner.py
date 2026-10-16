#!/usr/bin/env python3
"""Client-process regression tests for the opt-in render measurement runner."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
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


TIMEOUT_LOG_TAIL_BYTES = 8 * 1024
SURFACE_READY_PHASE = re.compile(r'EnsureSurfaceReadyNear[:/]')


def _report_timeout(receipt, log):
    truncated = False
    try:
        with log.open('rb') as source:
            source.seek(0, os.SEEK_END)
            offset = max(0, source.tell() - TIMEOUT_LOG_TAIL_BYTES)
            truncated = offset > 0
            source.seek(offset)
            payload = source.read(TIMEOUT_LOG_TAIL_BYTES)
    except OSError as error:
        payload = b''
        receipt['evidence_errors'].append(f'timeout log read: {error}')
    tail = payload.decode('utf-8', errors='replace')
    # Phase discovery is limited to the retained raw-byte window. Its first
    # line may be a fragment, so it cannot establish a complete phase record.
    complete_lines = tail.splitlines(keepends=True)
    if truncated:
        complete_lines = complete_lines[1:]
    complete_lines = [line for line in complete_lines if line.endswith('\n')]
    phases = [line for line in complete_lines
              if SURFACE_READY_PHASE.search(line)]
    last_phase = phases[-1].strip() if phases else None
    receipt.update(log_tail=tail, last_observed_phase=last_phase)
    # Diagnostics are best effort: a closed or failing stderr must not replace
    # the TimeoutExpired or prevent receipt retention.
    try:
        print(f"render runner: timed out after {receipt['timeout_seconds']} s", file=sys.stderr)
        print(tail, file=sys.stderr)
        print('last EnsureSurfaceReadyNear phase: ' + (last_phase or '(none observed)'),
              file=sys.stderr)
    except (OSError, ValueError):
        pass


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
        try:
            if isinstance(error, subprocess.TimeoutExpired):
                _report_timeout(receipt, log)
        finally:
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
        audio = OPTIONS.source / 'assets/audio'
        if audio.is_dir():
            shutil.copytree(audio, self.root / 'assets/audio', copy_function=link_or_copy)
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

    def run_client(self, name, extra=(), environment=None, expected_exit=0, schema='v3',
                   audio=False):
        artifact = self.root / (name + '.json')
        log = OPTIONS.artifacts / (self._testMethodName + '-' + name + '.log')
        command = [str(OPTIONS.client), *([] if audio else ['--no-audio']), '--no-menu-backdrop',
                   '--render-benchmark', str(artifact), '--render-benchmark-schema', schema,
                   '--capture-size', '160x90', '--perf-profile', 'quality',
                   '--render-benchmark-warmup', '2', *extra]
        retained = OPTIONS.artifacts / (self._testMethodName + '-' + name + '.json')
        receipt_path = retained.with_suffix('.process.json')
        result, evidence_errors = _run_process(
            command, cwd=self.root, environment=environment or self.environment,
            log=log, artifact=artifact, retained=retained, receipt_path=receipt_path)
        self.assertEqual(result.returncode, expected_exit, log.read_text(errors='replace', encoding="utf-8")[-6000:])
        if expected_exit:
            self.assertFalse(artifact.exists())
            _require_evidence(evidence_errors)
            return log.read_text(errors='replace', encoding="utf-8")
        # A completed capture alone could hide a mesh/material fallback. Require
        # the real loaders to accept the synthetic inputs on every success path.
        output = log.read_text(errors='replace', encoding="utf-8")
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

    def short_save_load_script(self):
        script = self.root / 'save-load.traversal'
        text = (OPTIONS.source / 'tools/perf/fixtures/save-load.traversal').read_text(encoding='utf-8')
        text = text.split('points ', 1)[0]
        text = text.replace('duration_seconds 60', 'duration_seconds 0.13333333333333333')
        text = text.replace('expected_ticks 1800', 'expected_ticks 4')
        text += ('points 2\npoint 0 8 80 8 0 -6\npoint 4 9 80 8 0 -6\n'
                 'actions 2\naction 1 save 0 0 0 0\naction 2 reload 0 0 0 0\n')
        script.write_bytes(text.encode('utf-8'))
        return script

    def test_v4_save_load_success(self):
        self.validate_save_load_capture(audio=True)

    def test_v4_no_audio_success(self):
        audio = self.root / 'assets/audio'
        if audio.exists():
            shutil.rmtree(audio)
        self.assertFalse(audio.exists())
        data = self.validate_save_load_capture(audio=False)
        self.assertIn('assets/audio', data['settings']['content_files'])
        self.assertIsNone(data['settings']['content_files']['assets/audio'])

    def validate_save_load_capture(self, audio):
        script = self.short_save_load_script()
        data = self.run_client('save-load', ['--traversal', str(script)], schema='v4', audio=audio)
        contract.validate(data, data['adapter']['renderer'], traversal=script)
        self.assertEqual(data['schema'], contract.WORKLOAD_SCHEMA)
        self.assertEqual(data['workload']['actual_ticks'], 4)
        self.assertEqual([(row['name'], row['succeeded']) for row in data['workload']['actions']],
                         [('save', True), ('reload', True)])
        self.assertIsNotNone(data['workload']['reload'])
        return data

    def test_escaping_audio_refusal(self):
        bank = self.root / 'data/audio/sfx_main.bank.json'
        document = json.loads(bank.read_bytes())
        event = next(iter(document['events'].values()))
        external = self.root / 'assets/private.mp3'
        external.parent.mkdir(exist_ok=True)
        external.write_bytes(b'outside the audio content tree')
        event['files'] = ['assets/audio/../private.mp3']
        _atomic_write(bank, json.dumps(document).encode('utf-8'))
        log = self.run_client('refused-audio',
                              ['--traversal', str(self.short_save_load_script())],
                              expected_exit=2, schema='v4', audio=True)
        self.assertIn('content reference escapes inventoried trees', log)

    def external_texture_refusal(self, absolute):
        manifest = self.root / 'data/models/trees/tree_textures.json'
        document = json.loads(manifest.read_bytes())
        external = self.root / 'external.ltex'
        shutil.copy2(self.root / document['models'][0]['albedo'], external)
        document['models'][0]['albedo'] = (str(external) if absolute
                                           else 'data/../external.ltex')
        # The fixture may share a hard link with the source; replace its inode.
        _atomic_write(manifest, json.dumps(document).encode('utf-8'))
        log = self.run_client('refused', ['--traversal', str(self.short_save_load_script())],
                              expected_exit=2, schema='v4')
        self.assertIn('content reference escapes inventoried trees', log)
        self.assertNotIn('Traversal owns the measured duration', log)

    def test_absolute_texture_refusal(self):
        self.external_texture_refusal(absolute=True)

    def test_parent_relative_texture_refusal(self):
        self.external_texture_refusal(absolute=False)

    def scene_config_refusal(self, assignment):
        scene = self.root / 'external-scene.json'
        scene.write_text(json.dumps({'moon': 0.75, 'weather': {'type': 'rain'},
                                     'clouds': {'coverage': 0.9}}), encoding='utf-8')
        option = [f'--scene-config={scene}'] if assignment else ['--scene-config', str(scene)]
        log = self.run_client('refused', ['--traversal', str(self.short_save_load_script()),
                                         *option], expected_exit=2, schema='v4')
        self.assertIn('schema v4 refuses --scene-config', log)
        self.assertNotIn('Scene-config loaded:', log)
        self.assertNotIn('Scene-config parse failed:', log)
        self.assertFalse((self.root / 'worlds/saves').exists())

    def test_scene_config_separate_refusal(self):
        self.scene_config_refusal(assignment=False)

    def test_scene_config_assignment_refusal(self):
        self.scene_config_refusal(assignment=True)

    def test_existing_world_option_forms_refusal(self):
        for option in (['--load-world', 'existing-world'], ['--load-world=existing-world'],
                       ['--load-world=']):
            with self.subTest(option=option):
                log = self.run_client('refused', ['--traversal', str(self.short_save_load_script()),
                                                 *option], expected_exit=2, schema='v4')
                self.assertIn('schema v4 refuses --load-world', log)
                self.assertNotIn('Static-model textures registered', log)
                self.assertFalse((self.root / 'worlds/saves').exists())

    def test_traversal_duration_option_forms_refusal(self):
        for option in (['--render-benchmark-frames', '4'], ['--render-benchmark-frames=4']):
            with self.subTest(option=option):
                log = self.run_client('refused', ['--traversal', str(self.script), *option],
                                      expected_exit=2)
                self.assertIn('Traversal owns the measured duration', log)

    def test_capture_option_missing_values_refusal(self):
        for name in ('--render-benchmark-schema', '--capture-size', '--perf-profile',
                     '--traversal', '--declare-render-overrides'):
            for option in ([name, '--no-audio'], [f'{name}=']):
                with self.subTest(option=option):
                    log = self.run_client('refused', option, expected_exit=2)
                    self.assertIn(f'missing value for {name}', log)

    def test_traversal_owner_option_forms_refusal(self):
        for name, value in (('--cam-pos', '1,2,3'), ('--cam-yaw', '10'), ('--cam-pitch', '5'),
                            ('--debug-goto', 'spawn'), ('--profile-fly', '4'),
                            ('--timelapse-frames', '4')):
            for option in ([name, value], [f'{name}={value}']):
                with self.subTest(option=option):
                    log = self.run_client('refused', ['--traversal', str(self.script), *option],
                                          expected_exit=2)
                    self.assertIn('exclusive camera/world ownership', log)

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
        declaration.write_text(json.dumps({'env:LUMIN_GL_DEBUG': '1'}), encoding="utf-8")
        data = self.run_client('declared', ['--render-benchmark-frames', '4',
                                          '--declare-render-overrides', str(declaration)], environment)
        contract.validate(data, data['adapter']['renderer'])

    def test_embedded_preset_refusal(self):
        original = self.run_client('original', ['--render-benchmark-frames', '4'])
        saves = list((self.root / 'worlds/saves').iterdir())
        self.assertEqual(len(saves), 1)
        saved = saves[0]
        metadata = json.loads((saved / 'world_info.json').read_text(encoding="utf-8"))
        self.assertEqual(metadata['worldType'], 'default')
        preset = json.loads((self.root / 'worlds/atlas/presets/default.json').read_text(encoding="utf-8"))
        preset['generation_params']['terrain']['height_offset'] += 1
        (saved / 'preset.json').write_text(json.dumps(preset), encoding="utf-8")
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

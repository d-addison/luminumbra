#!/usr/bin/env python3
"""Capture issue #617 evidence at the original process deadlines; never qualify a head."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_sim_budget as populated
from fixture_hash import fixture_hash
from fixture_paths import path_exists, resolve_reference
from resolved_fixture import resolved_fixture_hash, unique_object
import render_runner_process as runner
import summarize_process_receipts as receipts
import sim_budget_io
import perf
import test_render_runner as render_tests

SCHEMA = 'luminumbra.deferred_stack_capture.v1'
RENDER_DEADLINE = 180
PARITY_DEADLINE = 1800
RENDER_CASES = {
    'RenderTraversalIntegrated': 'test_traversal_twice',
    'RenderBenchmarkIsolationSubmission': 'test_isolation_submission_reporting',
    'RenderBenchmarkOverrideCollection': 'test_gl_debug_override_collection',
    'RenderTraversalEmbeddedPresetRefusal': 'test_embedded_preset_refusal',
}
RENDER_PROCESSES = {
    # test_traversal_twice also runs two expected frame-option refusals.
    'RenderTraversalIntegrated': {'first': 0, 'second': 0, 'separate': 2, 'assigned': 2},
    'RenderBenchmarkIsolationSubmission': {'isolated': 0},
    'RenderBenchmarkOverrideCollection': {'undeclared': 0, 'declared': 0},
    'RenderTraversalEmbeddedPresetRefusal': {'original': 0, 'loaded': 0, 'altered': 2, 'refused': 2},
}
PARITY_PREFIX = 'ActiveRegions/SimBudgetWorldParity.Populated3600TicksPreserveWorldHashAndReplayWork/'
CASES = tuple(RENDER_CASES) + tuple(PARITY_PREFIX + switch for switch in ('Disabled', 'Enabled'))
TARGETS = ('SaveTransaction::commit', 'WorldSaveService::member_digest', 'DecodeChunkRecord',
           'quiesce_streaming_jobs_for_save')
# All JSON/log inputs inherit the receipt reader's 16 MiB cap. Per bundle:
# six cases, <=1,000 process receipts, <=4,096 profile stacks of <=128 frames,
# and <=64 profiler artifacts. Hashing streams at most 1 MiB per read.
MAX_STACKS = 4096
MAX_FRAMES = 128
MAX_ARTIFACTS = 64


def encode(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode('utf-8')


def fingerprint(value):
    return hashlib.sha256(encode(value)).hexdigest()


def payload_record(path, payload):
    return dict(path=str(path), sha256=hashlib.sha256(payload).hexdigest())


def read_json_record(path, root=None):
    path = Path(os.path.abspath(path))
    with receipts.open_regular_file(path, root or path.parent) as source:
        payload = receipts.bounded_bytes(path, source=source)
    return json.loads(payload, object_pairs_hook=unique_object), payload_record(path, payload)


def read_json(path, root=None):
    return read_json_record(path, root)[0]


def file_record(path, root=None):
    path = Path(os.path.abspath(path))
    digest = hashlib.sha256()
    # Validate and hash the owning handle, refusing links, escapes and devices.
    with receipts.open_regular_file(path, root or path.parent) as source:
        total = 0
        for block in iter(lambda: source.read(1024 * 1024), b''):
            total += len(block)
            if total > receipts.MAX_FILE_BYTES:
                raise ValueError('artifact exceeds byte cap')
            digest.update(block)
    return dict(path=str(path), sha256=digest.hexdigest())


def source_sha(source):
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=source, text=True).strip()
    revision = git('rev-parse', 'HEAD')
    if re.fullmatch('[0-9a-f]{40}', revision) is None or git('status', '--porcelain'):
        raise ValueError('capture requires a clean source SHA')
    return revision


def identity(source, assets, binaries, build_record):
    if source != Path(__file__).resolve().parents[2]:
        raise ValueError('source must be the checkout containing this capture harness')
    record, record_artifact = read_json_record(build_record)
    revision = source_sha(source)
    # Parity fixtures use the source root compiled into the test executable.
    build_source = record.get('source_root')
    if (not isinstance(build_source, str) or not build_source
            or runner.resolve_path(build_source, strict=True) != source):
        raise ValueError('build record source root must match the fixture checkout')
    flags = record.get('build_flags')
    if (record.get('source_sha') != revision or not isinstance(flags, dict)
            or set(flags) != perf.BUILD_CONFIGURATION_FIELDS
            or not all(isinstance(value, str) and value for value in flags.values())
            or re.fullmatch('[0-9a-f]{64}', flags['effective_flags_sha256']) is None
            or any(flags[key] not in ('ON', 'OFF') for key in
                   perf.BUILD_CONFIGURATION_FIELDS - {'build_type', 'effective_flags_sha256'})):
        raise ValueError('build record must bind source SHA and the complete performance build flags')
    hashes = {role: runner._binary_identity(path)[1] for role, path in sorted(binaries.items())}
    if record.get('binaries') != hashes:
        raise ValueError('build record binary identity mismatch')
    # Include both historical parity presets and every render runtime input.
    fixtures = {name: fixture_hash(resolve_reference(source, name), reference_root=source / 'data')
                for name in ('data', 'res', 'config', 'worlds/atlas')}
    audio = resolve_reference(source, 'assets/audio')
    fixtures['assets/audio'] = fixture_hash(audio) if path_exists(audio) else None
    fixtures['render_assets'] = fixture_hash(assets)
    fixtures['populated'] = resolved_fixture_hash(source)
    fixtures['render_harness'] = file_record(source / 'tools/perf/test_render_runner.py')['sha256']
    return dict(source_sha=revision, source_root=str(source), binaries=hashes, build_flags=flags,
                build_record_sha256=record_artifact['sha256'], fixtures=fixtures)


def render_case(name, source, assets, client, folder):
    """Run the unchanged public regression, retaining its individual process receipts."""
    previous_options = getattr(render_tests, 'OPTIONS', None)
    previous_deadline = render_tests.PROCESS_TIMEOUT_SECONDS
    render_tests.OPTIONS = SimpleNamespace(source=source, assets=assets, client=client,
                                          artifacts=folder)
    render_tests.PROCESS_TIMEOUT_SECONDS = RENDER_DEADLINE
    result = unittest.TestResult()
    try:
        render_tests.RenderRunnerTests(RENDER_CASES[name]).run(result)
    finally:
        render_tests.OPTIONS = previous_options
        render_tests.PROCESS_TIMEOUT_SECONDS = previous_deadline
    return dict(passed=result.wasSuccessful() and result.testsRun == 1 and not result.skipped,
                errors=[detail for _test, detail in result.errors + result.failures],
                skipped=[reason for _test, reason in result.skipped])


def parity_case(name, source, binary, folder):
    command = [str(binary), '--gtest_filter=' + name,
               '--gtest_output=json:' + str(folder / 'gtest.json')]
    errors = []
    try:
        result, errors = runner.run_process(
            command, cwd=source, environment=os.environ.copy(), log=folder / 'parity.log',
            artifact=folder / 'gtest.json', retained=folder / 'parity.json',
            receipt_path=folder / 'parity.process.json', timeout_seconds=PARITY_DEADLINE)
        passed = result.returncode == 0 and not errors
    except (OSError, subprocess.TimeoutExpired) as error:
        passed = False
        errors.append(str(error))
    return dict(passed=passed, errors=errors, skipped=[])


def capture(source, assets, binaries, build_record, evidence):
    if evidence.resolve().is_relative_to(source):
        raise ValueError('evidence directory must be outside the source checkout')
    bound = identity(source, assets, binaries, build_record)
    # Exclusive creation refuses overwriting any earlier successful or failed run.
    evidence.mkdir(parents=True, exist_ok=False)
    manifest = dict(schema=SCHEMA, identity=bound, runs=[], complete=False)
    runner.atomic_write(evidence / 'capture.json', encode(manifest))
    for index, name in enumerate(CASES):
        folder = evidence / str(index)
        folder.mkdir()
        result = (render_case(name, source, assets, binaries['client'], folder)
                  if name in RENDER_CASES else parity_case(name, source, binaries['parity'], folder))
        process_paths = receipts.collect_receipts([folder])
        for path, root in process_paths:
            receipt = read_json(path, root)
            receipt.update(source_sha=bound['source_sha'], capture_identity_sha256=fingerprint(bound),
                           case=name)
            runner.atomic_write(path, encode(receipt))
        manifest['runs'].append(dict(case=name, result=result,
                                    receipts=[path.relative_to(evidence).as_posix()
                                              for path, _root in process_paths]))
        # Publish after every case so timeout/assertion failures remain inspectable.
        runner.atomic_write(evidence / 'capture.json', encode(manifest))
    if identity(source, assets, binaries, build_record) != bound:
        raise ValueError('source, binaries, build flags or fixtures changed during capture')
    manifest['complete'] = True
    runner.atomic_write(evidence / 'capture.json', encode(manifest))
    return manifest


def relative_file(root, name):
    if (not isinstance(name, str) or not name or '\\' in name or ':' in name
            or Path(name).is_absolute() or '..' in Path(name).parts):
        raise ValueError('artifact must be a relative contained path')
    return root / name


def log_report(path, root):
    try:
        with receipts.open_regular_file(path, root) as source:
            payload = receipts.bounded_bytes(path, source=source)
    except FileNotFoundError:
        return dict(log_path=str(path), log_missing=True, last_shutdown_stage=None,
                    placement=None, log=None, phase='unknown')
    stage = receipts.last_shutdown_stage(path, io.BytesIO(payload))
    if stage in ('world_save_started', 'world_save_finished'):
        phase = 'save'
    elif stage in ('world_readers_drain_started', 'far_lod_drained', 'world_readers_drained'):
        phase = 'streaming_wait'
    elif stage is None and any('EnsureSurfaceReadyNear' in line
                               for line in receipts.read_log(path, source=io.BytesIO(payload))):
        phase = 'boot'
    else:
        phase = 'unknown'
    return dict(log_path=str(path), log_missing=False, last_shutdown_stage=stage,
                placement=receipts.NO_BREADCRUMBS if stage is None else 'in_' + stage,
                log=payload_record(path, payload), phase=phase)


def process_report(path, root, bound, role, deadline, case):
    with receipts.open_regular_file(path, root) as source:
        payload = receipts.bounded_bytes(path, source=source)
    json.loads(payload, object_pairs_hook=unique_object)
    receipt = receipts.load_receipt(path, io.BytesIO(payload))
    unverified_spawn = (receipt['outcome'] == 'spawn_error' and receipt['binary_sha256'] is None
                        and receipt.get('binary_identity_status') is None)
    if ((not unverified_spawn and (receipt['binary_sha256'] != bound['binaries'][role]
                                  or receipt.get('binary_identity_status') != 'verified'))
            or receipt.get('source_sha') != bound['source_sha']
            or receipt.get('capture_identity_sha256') != fingerprint(bound)
            or receipt.get('case') != case
            or receipt['timeout_seconds'] != deadline
            or receipt['command'][0] != receipt['binary_path']):
        raise ValueError('process binary identity or fixed deadline mismatch')
    entry = dict(classification=receipt['outcome'], return_code=receipt['return_code'],
                 capture_status=receipt['capture_status'], timeout_seconds=receipt['timeout_seconds'],
                 elapsed_seconds=receipt['elapsed_seconds'], evidence_errors=receipt['evidence_errors'])
    entry['label'] = path.name[:-len('.process.json')].rsplit('-', 1)[-1]
    # Classify breadcrumbs for every outcome; absence is unknown, never proof of boot.
    location = log_report(path.with_name(path.name[:-len('.process.json')] + '.log'), root)
    entry.update(location, command=receipt['command'],
                 receipt=payload_record(path, payload), measurement_ms=None,
                 binary_verified=not unverified_spawn)
    if receipt['capture_status'] == 'retained':
        # Render receipts retain absolute capture paths; permit only this bundle.
        retained = Path(receipt['capture_path'])
        if not retained.is_absolute():
            raise ValueError('retained capture path must be absolute')
        data, artifact = read_json_record(retained, root)
        if artifact['sha256'] != receipt['capture_sha256']:
            raise ValueError('retained capture digest mismatch')
        entry['capture'] = artifact
        if role == 'parity':
            entry['parity_completed'] = parity_completed(data, case)
    return entry


def profile_report(path, bound, commands):
    profile = read_json(path)
    if profile.get('schema') != 'luminumbra.deferred_stack_profile.v1' or profile.get('identity') != bound:
        raise ValueError('profile source or binary identity mismatch')
    rows = profile.get('runs')
    if not isinstance(rows, list) or len(rows) > len(CASES):
        raise ValueError('invalid profile run count')
    output = {}
    for row in rows:
        case = row['case']
        if case in output or case not in commands or row.get('commands_sha256') != fingerprint(commands[case]):
            raise ValueError('profile command identity mismatch')
        artifacts = row.get('artifacts')
        samples = row.get('samples')
        if (not isinstance(artifacts, list) or not 0 < len(artifacts) <= MAX_ARTIFACTS
                or any(not isinstance(item, str) for item in artifacts)
                or len(set(artifacts)) != len(artifacts)
                or not isinstance(samples, list) or len(samples) > MAX_STACKS):
            raise ValueError('invalid profile artifacts or sample count')
        records = [file_record(relative_file(path.parent, item), path.parent) for item in sorted(artifacts)]
        counts = dict.fromkeys(TARGETS, 0) if samples else dict.fromkeys(TARGETS, None)
        for sample in samples:
            stack, count = sample.get('stack'), sample.get('samples')
            if (not isinstance(stack, list) or not 0 < len(stack) <= MAX_FRAMES
                    or any(not isinstance(frame, str) or len(frame) > 4096 for frame in stack)
                    or type(count) is not int or not 0 < count < 2**64):
                raise ValueError('invalid profile sample')
            for target in TARGETS:
                if any(target in frame for frame in stack):
                    counts[target] += count
        output[case] = dict(artifacts=records, inclusive_samples=counts)
    return output


def populated_report(path, bound):
    """Validate existing capture_sim_budget captures without substituting for parity."""
    manifest, manifest_record = read_json_record(path)
    sim_budget_io.validate_manifest(manifest)
    sim_budget_io.validate_workload_integer(manifest['seed'], 'seed')
    sim_budget_io.validate_workload_integer(manifest['ticks'], 'ticks')
    if (manifest.get('schema') != 'luminumbra.sim_budget_capture.v1'
            or manifest.get('revision') != bound['source_sha'] or manifest.get('source_dirty') is not False
            or manifest.get('binary_sha256') != bound['binaries'].get('server')
            or manifest.get('fixture_hash') != bound['fixtures']['populated']):
        raise ValueError('populated capture source, binary or fixture identity mismatch')
    runs = manifest.get('runs')
    if not isinstance(runs, list) or len(runs) != len(populated.PRESETS):
        raise ValueError('incomplete populated captures')
    output = []
    for row, preset in zip(runs, populated.PRESETS):
        if row.get('preset') != preset:
            raise ValueError('populated preset mismatch')
        artifact = relative_file(path.parent, row['artifact'])
        data, artifact_record = read_json_record(artifact, path.parent)
        populated.validate_artifact(data, preset, manifest['seed'], manifest['ticks'])
        output.append(dict(preset=preset, artifact=artifact_record, manifest=manifest_record,
                           command=row['command'], sim_budget=data['sim_budget']))
    return output


def report(evidence, profile=None, populated_capture=None):
    evidence = Path(os.path.abspath(evidence))
    profile = Path(os.path.abspath(profile)) if profile is not None else None
    populated_capture = Path(os.path.abspath(populated_capture)) if populated_capture is not None else None
    manifest = read_json(evidence / 'capture.json', evidence)
    if manifest.get('schema') != SCHEMA or manifest.get('complete') is not True:
        raise ValueError('incomplete deferred capture')
    bound = manifest['identity']
    runs = manifest['runs']
    if not isinstance(runs, list) or [row['case'] for row in runs] != list(CASES):
        raise ValueError('missing, duplicate or reordered cases')
    output, commands = [], {}
    seen = set()
    for row in runs:
        name = row['case']
        paths = row['receipts']
        if not isinstance(paths, list) or len(paths) + len(seen) > receipts.MAX_RECEIPTS:
            raise ValueError('invalid process receipt count')
        entries = []
        for item in paths:
            if item in seen or not item.endswith('.process.json'):
                raise ValueError('duplicate process receipt')
            seen.add(item)
            entries.append(process_report(relative_file(evidence, item), evidence, bound,
                                          'client' if name in RENDER_CASES else 'parity',
                                          RENDER_DEADLINE if name in RENDER_CASES else PARITY_DEADLINE, name))
        commands[name] = [entry['command'] for entry in entries]
        passed = (row['result']['passed'] is True and not row['result'].get('skipped') and bool(entries)
                  and all(entry['classification'] == 'returned' and entry['binary_verified']
                          and entry['elapsed_seconds'] <= entry['timeout_seconds']
                          and not entry['evidence_errors'] for entry in entries))
        if name in RENDER_CASES:
            expected = RENDER_PROCESSES[name]
            passed = (passed and len(entries) == len(expected)
                      and {entry['label']: entry['return_code'] for entry in entries} == expected
                      and all(entry['capture_status'] == ('missing' if entry['return_code'] else 'retained')
                              for entry in entries))
        else:
            passed = passed and len(entries) == 1 and entries[0]['return_code'] == 0
            if passed:
                passed = entries[0].get('parity_completed') is True
        output.append(dict(case=name, passed=passed, result=row['result'], processes=entries,
                           profile=None))
    profiles = profile_report(profile, bound, commands) if profile is not None else {}
    for row in output:
        row['profile'] = profiles.get(row['case'])
    return dict(schema='luminumbra.deferred_stack_report.v1', identity=bound, runs=output,
                passed=all(row['passed'] for row in output),
                native_qualification='pending',
                populated=None if populated_capture is None else populated_report(populated_capture, bound))


def parity_completed(data, name):
    """A zero exit with an empty/mistyped gtest filter is not parity evidence."""
    suite, method = name.split('.', 1)
    suites = data.get('testsuites')
    if (data.get('tests') != 1 or data.get('failures') != 0 or data.get('disabled') != 0
            or not isinstance(suites, list) or len(suites) != 1):
        return False
    tests = suites[0].get('testsuite')
    return (suites[0].get('name') == suite and isinstance(tests, list) and len(tests) == 1
            and tests[0].get('name') == method and tests[0].get('status') == 'RUN'
            and tests[0].get('result') == 'COMPLETED' and not tests[0].get('failures'))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='mode', required=True)
    collect = commands.add_parser('capture')
    for flag in ('source', 'assets', 'client', 'parity', 'build-record', 'evidence-dir'):
        collect.add_argument('--' + flag, type=Path, required=True)
    collect.add_argument('--server', type=Path, help='identity for optional populated captures')
    summarize = commands.add_parser('report')
    summarize.add_argument('--evidence-dir', type=Path, required=True)
    summarize.add_argument('--profile', type=Path)
    summarize.add_argument('--populated-capture', type=Path)
    args = parser.parse_args(argv)
    try:
        if args.mode == 'capture':
            binaries = {key: runner.resolve_path(value, strict=True)
                        for key in ('client', 'parity', 'server')
                        if (value := getattr(args, key)) is not None}
            capture(args.source.resolve(strict=True), args.assets.resolve(strict=True), binaries,
                    args.build_record, args.evidence_dir.absolute())
            return 0 if report(args.evidence_dir.absolute())['passed'] else 1
        else:
            result = report(args.evidence_dir.absolute(), args.profile, args.populated_capture)
            print(encode(result).decode('utf-8'), end='')
            return 0 if result['passed'] else 1
    except (OSError, ValueError, KeyError, TypeError, AttributeError, RecursionError,
            receipts.InputError, subprocess.SubprocessError) as error:
        print(f'deferred evidence refused: {error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

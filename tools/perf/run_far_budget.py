#!/usr/bin/env python3
"""Capture or inspect the far budget matrix; publish measurements outside the tree."""
import argparse
import json
import math
from pathlib import Path
import subprocess
import sys

# Support the documented isolated invocation without ambient Python imports.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import far_budget as budget
import render_runner_process as process
import run_qualification as qualification


def outside_tree(path):
    selected = process.resolve_path(path)
    if selected == budget.ROOT or selected.is_relative_to(budget.ROOT):
        raise ValueError('evidence output must be outside the source tree')
    return selected


def paths(root, cell):
    base = Path(root) / 'cells' / cell['id']
    return {key: base.with_name(base.name + suffix) for key, suffix in
            (('capture', '.json'), ('retained', '.artifact.json'),
             ('receipt', '.process.json'), ('log', '.log'))}


def custody(root, cell, snapshots):
    """Reuse process custody while hashing only the already opened snapshots."""
    selected = paths(root, cell)
    receipt = budget.decode(snapshots['receipt'])
    if not isinstance(receipt, dict):
        raise ValueError('process receipt must be an object')
    digests = {selected[key]: budget.digest(snapshots[key]) for key in ('capture', 'retained')}
    reason = qualification.capture_ownership(receipt, selected['retained'], selected['capture'], root,
                                             hash_file=lambda path: digests[path])
    if reason is not None:
        raise ValueError(reason)
    client = receipt.get('client')
    if (not isinstance(client, dict) or client.get('kind') != 'native' or
            client.get('native_build') is not True or client.get('sha256_verified') is not True or
            client.get('sha256') != client.get('build_manifest_client_sha256') or
            client.get('sha256') != receipt.get('binary_sha256')):
        raise ValueError('native build identity unavailable or mismatched')
    budget.sha(client.get('sha256'), 'client')
    budget.sha(client.get('build_manifest_sha256'), 'build manifest')
    config = receipt.get('systems_config')
    if not isinstance(config, dict) or config.get('sha256_verified') is not True:
        raise ValueError('systems configuration identity unavailable')
    budget.sha(config.get('sha256'), 'systems configuration')
    return dict(client=client, systems_config=config,
                receipt_sha256=budget.digest(snapshots['receipt']))


def inspect_cell(root, matrix, cell, world, reasons, traversal=None):
    result = dict(cell, fixture_sha256=budget.fixture_identity(world), state='incomplete',
                  reasons=list(reasons), capture_sha256=None, sources={})
    snapshots = {}
    for key in ('capture', 'retained', 'receipt'):
        relative = paths(root, cell)[key].relative_to(root).as_posix()
        try:
            payload = budget.read(root, relative, qualification.MAX_CAPTURE_BYTES
                                  if key != 'receipt' else qualification.MAX_FILE_BYTES)
        except FileNotFoundError:
            result['reasons'].append('missing ' + key)
            continue
        except (OSError, ValueError) as error:
            result.update(state='invalid')
            result['reasons'].append(str(error))
            return result
        snapshots[key] = payload
        result['sources'][key] = dict(path=relative, sha256=budget.digest(payload))
        if key == 'capture':
            result['capture_sha256'] = budget.digest(payload)
    if len(snapshots) != 3:
        return result
    try:
        result['process'] = custody(root, cell, snapshots)
        data = budget.decode(snapshots['capture'])
        if not isinstance(data, dict):
            raise ValueError('capture must be an object')
        # Missing dependency pins cannot bind a measurement, even with a valid receipt.
        if reasons:
            return result
        result.update(budget.evaluate(data, matrix, cell, world, traversal))
    except (OSError, ValueError, KeyError, TypeError, OverflowError, RecursionError) as error:
        result.update(state='invalid')
        result['reasons'].append(str(error))
    return result


def command(cell, world, matrix, client, capture, script):
    argv = [str(client), '--no-audio', '--no-menu-backdrop', '--render-benchmark', str(capture),
            '--render-benchmark-schema', 'v3', '--capture-size', cell['display'],
            '--perf-profile', cell['profile'], '--render-benchmark-warmup', str(matrix['warmup_frames']),
            '--world-preset', world['preset']]
    if cell['workload'] == 'traversal':
        argv += ['--traversal', str(script)]
    else:
        pose = world['pose']
        argv += ['--render-benchmark-frames', str(matrix['measured_frames']),
                 '--cam-pos', ','.join(repr(pose[key]) for key in ('x', 'y', 'z')),
                 '--cam-yaw', repr(pose['yaw']), '--cam-pitch', repr(pose['pitch'])]
    return argv


def snapshot_script(work, world, source_root=budget.ROOT):
    payload = budget.read(source_root, world['script'], budget.MAX_SCRIPT_BYTES)
    text = budget.fixture_paths.decode_text(payload)
    if budget.digest(text.encode('utf-8')) != world['script_sha256']:
        raise ValueError('traversal changed before inspection or launch')
    script = work / 'workload.traversal'
    process.atomic_write(script, text.encode('utf-8'))
    return script


def launch_cell(out, matrix, cell, world, *, client, runtime_root, build_manifest, timeout):
    selected = paths(out, cell)
    work = out / 'work' / cell['id']
    work.mkdir(parents=True)
    save_root = work / 'storage'
    save_root.mkdir()
    script = None
    if cell['workload'] == 'traversal':
        script = snapshot_script(work, world)
    argv = command(cell, world, matrix, client, selected['capture'], script)
    systems = runtime_root / 'data/common/systems.json'
    argv += qualification.runtime_options(systems, runtime_root, save_root)
    declaration = work / 'render-overrides.json'
    process.atomic_write(declaration, budget.canonical({'cli:--systems-config': str(systems)}))
    argv += ['--declare-render-overrides', str(declaration)]
    environment = qualification.isolated_environment(work)
    qualification.reset_settings(work)
    if str(client).endswith('.py'):
        argv = [sys.executable, '-I', '-B', str(client)] + argv[1:]
    try:
        completed, errors, _ = qualification.run_client_process(
            argv, client=client, build_manifest=build_manifest, systems_config=systems,
            runtime_root=runtime_root, save_root=save_root, cwd=work,
            environment=environment, log=selected['log'], artifact=selected['capture'],
            retained=selected['retained'], receipt_path=selected['receipt'], timeout_seconds=timeout)
    except subprocess.TimeoutExpired:
        return ['process deadline expired']
    except OSError as error:
        return ['process failed: ' + str(error)]
    if completed.returncode != 0:
        errors.append('process exit ' + str(completed.returncode))
    return errors


def build_index(matrix, results):
    expected = [cell['id'] for cell in budget.cells(matrix)]
    if [cell['id'] for cell in results] != expected:
        raise ValueError('evidence index requires every world/profile/display/workload cell')
    complete = all(cell['state'] == 'complete' for cell in results)
    return dict(schema=budget.INDEX_SCHEMA, matrix_sha256=budget.digest(budget.canonical(matrix)),
                scope='measurement only; no acceptance receipt', state='complete' if complete else 'incomplete',
                group_count=16, cells=results,
                candidate_caps=dict(status='awaiting measured proposal and owner approval',
                    tiers=[dict(tier=tier, resident_bytes=None, builds=None, uploads=None, upload_bytes=None)
                           for tier in range(1, 6)],
                    global_caps=dict(builds=None, uploads=None, upload_bytes=None),
                    separately_measured_reserve_bytes=None))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--matrix', type=Path, default=budget.DEFAULT_MATRIX)
    parser.add_argument('--out-dir', type=Path, required=True, help='New directory outside the source tree')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--evidence-root', type=Path, help='Inspect an existing runner directory without launching')
    mode.add_argument('--client', type=Path, help='Capture ready cells using the existing process-custody runner')
    parser.add_argument('--runtime-root', type=Path)
    parser.add_argument('--build-manifest', type=Path)
    parser.add_argument('--timeout-seconds', type=float, default=qualification.DEFAULT_TIMEOUT_SECONDS)
    args = parser.parse_args(argv)
    try:
        if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
            raise ValueError('timeout must be positive and finite')
        matrix = budget.load_matrix(args.matrix)
        expanded = budget.cells(matrix)
        worlds = {world['id']: world for world in matrix['worlds']}
        pending = {cell['id']: budget.dependencies(worlds[cell['world']], cell) for cell in expanded}
        out = outside_tree(args.out_dir)
        evidence = outside_tree(args.evidence_root) if args.evidence_root else out
        if args.evidence_root is not None and out == evidence:
            raise ValueError('report output must differ from the source evidence directory')
        build = qualification.load_build_manifest(args.build_manifest)
        if args.client is not None and (args.runtime_root is None or build is None):
            raise ValueError('capture requires --runtime-root and --build-manifest')
        client = None if args.client is None else qualification.resolve_client(str(args.client))
        runtime = None if args.runtime_root is None else process.resolve_path(args.runtime_root)
        # Refuse mismatched runtime fixtures before creating any output.
        if client is not None:
            for cell in expanded:
                if not pending[cell['id']]:
                    world = worlds[cell['world']]
                    for reference, expected in sorted(world['inputs'].items()):
                        payload = budget.read(runtime, reference, budget.MAX_MATRIX_BYTES)
                        if budget.digest(budget.canonical(budget.decode(payload))) != expected:
                            raise ValueError('runtime fixture differs: ' + reference)
        # An exclusive new directory is the publication/launch lock. Existing evidence
        # is never retired or replaced. The evidence index is the final commit marker.
        out.mkdir(parents=True, exist_ok=False)
        (out / 'cells').mkdir()
        results = []
        for cell in expanded:
            world = worlds[cell['world']]
            reasons = list(pending[cell['id']])
            script = None
            if client is not None and not reasons:
                reasons.extend(launch_cell(out, matrix, cell, world, client=client, runtime_root=runtime,
                                           build_manifest=build, timeout=args.timeout_seconds))
                script = out / 'work' / cell['id'] / 'workload.traversal' if cell['workload'] == 'traversal' else None
            elif not reasons and cell['workload'] == 'traversal' and world['script'] is not None:
                work = out / 'work' / cell['id']
                work.mkdir(parents=True)
                script = snapshot_script(work, world)
            results.append(inspect_cell(evidence, matrix, cell, world, reasons, script))
        index = build_index(matrix, results)
        index.update(matrix=matrix, evidence_root=str(evidence))
        process.atomic_write(out / 'review.md', budget.review(index))
        process.atomic_write(out / 'index.json', budget.canonical(index))
        print(json.dumps(dict(state=index['state'], groups=index['group_count'], cells=len(results))))
        return 0 if index['state'] == 'complete' else 3
    except (OSError, ValueError, KeyError, TypeError, OverflowError, RecursionError) as error:
        print('run_far_budget: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())

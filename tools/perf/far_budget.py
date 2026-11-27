#!/usr/bin/env python3
"""Bounded world-matrix measurements and independently approved receipt evidence."""
import hashlib
import json
import re
import tempfile
from argparse import Namespace
from pathlib import Path

import fixture_paths
import perf
import render_budget
import render_contract
import run_qualification as qualification
import validate_render_capture

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MATRIX = ROOT / 'tools/perf/qualification/far-budgets.json'
SCHEMA = 'luminumbra.far_budget_matrix.v1'
INDEX_SCHEMA = 'luminumbra.far_budget_evidence.v1'
WORLDS = ('default', 'mountains', 'cave-dense', 'edited')
PROFILES = ('quality', 'performance')
DISPLAYS = ('3840x1600', '3440x1440')
WORKLOADS = ('settled', 'traversal')
WORK = ('build_attempts', 'dispatched', 'completed', 'upload_attempts', 'uploaded',
        'upload_bytes', 'successful_upload_bytes', 'rejected', 'evicted')
# 32 captures, each with at most 36000 budget rows and five tiers. JSON depth,
# capture byte limits and frame limits are shared with the existing render tools.
MAX_MATRIX_BYTES = 1024 * 1024
MAX_SCRIPT_BYTES = 64 * 1024
# Index summaries include distributions for 32 cells; source captures are read
# one cell at a time under the existing capture bound, never accumulated.
MAX_INDEX_BYTES = 16 * 1024 * 1024
APPROVAL_SCHEMA = 'luminumbra.far_budget_approval.v1'
APPROVAL_HASHES = ('matrix_sha256', 'baseline_sha256', 'limits_sha256',
                   'client_sha256', 'build_manifest_sha256', 'systems_config_sha256')


def validate_approval(approval, revision):
    """The operator supplies owner-reviewed authority separately from the receipt.

    The reviewed executable/configuration pair identifies the installed limits;
    a receipt cannot approve its own profile or substitute candidate cap values.
    """
    exact(approval, ('schema', 'revision', 'profile_id', *APPROVAL_HASHES), 'budget approval')
    if approval['schema'] != APPROVAL_SCHEMA:
        raise ValueError('unsupported budget approval schema')
    if (not isinstance(revision, str) or re.fullmatch('[0-9a-f]{40}', revision) is None or
            approval['revision'] != revision):
        raise ValueError('budget approval revision differs from expected candidate')
    profile = approval['profile_id']
    if (not isinstance(profile, str) or re.fullmatch('[a-z0-9][a-z0-9._-]{0,127}', profile) is None or
            profile in ('legacy', 'development', 'unavailable')):
        raise ValueError('approved budget profile identity unavailable')
    for key in APPROVAL_HASHES:
        sha(approval[key], 'budget approval ' + key)


def validate_index(index, matrix):
    """Reject omitted, duplicated, reordered or failed cells before reading sources."""
    if not isinstance(index, dict) or index.get('schema') != INDEX_SCHEMA:
        raise ValueError('missing or unsupported far budget evidence index')
    if (index.get('state') != 'complete' or type(index.get('group_count')) is not int or
            index['group_count'] != 16 or index.get('matrix') != matrix or
            index.get('matrix_sha256') != digest(canonical(matrix)) or
            digest(canonical(index['matrix'])) != index['matrix_sha256']):
        raise ValueError('incomplete or stale far budget evidence matrix')
    expected = cells(matrix)
    observed = index.get('cells')
    if not isinstance(observed, list) or len(observed) != len(expected):
        raise ValueError('far budget evidence requires every matrix cell')
    for actual, cell in zip(observed, expected):
        if (not isinstance(actual, dict) or any(actual.get(key) != value for key, value in cell.items()) or
                actual.get('state') != 'complete' or actual.get('reasons') != []):
            raise ValueError('failed, incomplete or mismatched far budget cell: ' + cell['id'])
        verdict = actual.get('verdict')
        floor = verdict.get('floor') if isinstance(verdict, dict) else None
        if (not isinstance(floor, dict) or verdict.get('schema') != render_budget.VERDICT_SCHEMA or
                floor.get('limit_ms') != render_budget.FLOOR_MS or
                type(floor.get('p99_ms')) not in (int, float) or
                not 0 <= floor['p99_ms'] <= render_budget.FLOOR_MS or
                not render_budget.verdict_passes(verdict)):
            raise ValueError('far budget timing check failed: ' + cell['id'])


def qualify_receipt(section, root, approval, revision):
    """Recheck reporter inputs; summary claims alone never qualify acceptance.

    Baseline bytes and all profile identities are pinned by independent owner
    approval. No budget numbers are selected here. Missing dependency contracts
    (including committed edits/all-tier bake) retain the measurement refusals.
    """
    # Local import shares the runner's custody checks without changing its CLI.
    import run_far_budget as runner

    validate_approval(approval, revision)
    matrix = load_matrix()
    if approval['matrix_sha256'] != digest(canonical(matrix)):
        raise ValueError('approved matrix differs from current qualification matrix')
    for key in ('profile_id', 'limits_sha256', 'baseline_sha256'):
        if section.get(key) != approval[key]:
            raise ValueError('budget profile or approval identity mismatch: ' + key)
    if section.get('approval_sha256') != digest(canonical(approval)):
        raise ValueError('budget approval identity mismatch')
    baseline = read(root, section.get('baseline'), MAX_INDEX_BYTES)
    if digest(baseline) != approval['baseline_sha256']:
        raise ValueError('approved baseline identity mismatch')
    validate_index(decode(baseline), matrix)
    index = decode(read(root, section.get('evidence'), MAX_INDEX_BYTES))
    validate_index(index, matrix)
    # The reporter records an absolute original source root. It may name only a
    # subtree of the receipt evidence root; never follow it to arbitrary storage.
    source_reference = index.get('evidence_root')
    if not isinstance(source_reference, str) or not source_reference:
        raise ValueError('missing far budget source root')
    source_root = Path(source_reference)
    if not source_root.is_absolute():
        raise ValueError('far budget source root must identify the original runner directory')
    try:
        relative = source_root.relative_to(root)
    except ValueError as error:
        raise ValueError('far budget source root outside receipt evidence root') from error
    # Preserve lexical spelling for custody and use the shared contained reader
    # for every source. Junction/symlink escapes are refused on both platforms.
    worlds = {world['id']: world for world in matrix['worlds']}
    for actual, cell in zip(index['cells'], cells(matrix)):
        world = worlds[cell['world']]
        if actual.get('fixture_sha256') != fixture_identity(world):
            raise ValueError('far budget fixture identity mismatch: ' + cell['id'])
        sources = actual.get('sources')
        exact(sources, ('capture', 'retained', 'receipt'), 'far budget sources')
        snapshots = {}
        selected = runner.paths(source_root, cell)
        for key in ('capture', 'retained', 'receipt'):
            source = sources[key]
            exact(source, ('path', 'sha256'), 'far budget source')
            reference = selected[key].relative_to(source_root).as_posix()
            if source['path'] != reference:
                raise ValueError('far budget source path mismatch')
            payload = read(root, (relative / reference).as_posix(),
                           qualification.MAX_FILE_BYTES if key == 'receipt' else qualification.MAX_CAPTURE_BYTES)
            if digest(payload) != source['sha256']:
                raise ValueError('far budget source identity mismatch')
            snapshots[key] = payload
        process = runner.custody(source_root, cell, snapshots)
        if (actual.get('process') != process or
                actual.get('capture_sha256') != digest(snapshots['capture'])):
            raise ValueError('far budget process or capture summary mismatch')
        for key, observed in (('client_sha256', process['client']['sha256']),
                              ('build_manifest_sha256', process['client']['build_manifest_sha256']),
                              ('systems_config_sha256', process['systems_config']['sha256'])):
            if observed != approval[key]:
                raise ValueError('unapproved executable/configuration identity: ' + key)
        receipt = decode(snapshots['receipt'])
        work = source_root / 'work' / cell['id']
        command = runner.command(cell, world, matrix, receipt['client_path'], selected['capture'],
                                 work / 'workload.traversal' if cell['workload'] == 'traversal' else None)
        command += qualification.runtime_options(receipt['systems_config']['path'],
                                                  receipt['runtime_root'], receipt['save_root'])
        command += ['--declare-render-overrides', str(work / 'render-overrides.json')]
        if receipt['command'] != command:
            raise ValueError('unapproved far budget capture command or override')
        reasons = dependencies(world, cell)
        if reasons:
            raise ValueError('far budget dependency unavailable: ' + '; '.join(reasons))
        data = decode(snapshots['capture'])
        if not isinstance(data, dict):
            raise ValueError('far budget capture must be an object')
        expected_overrides = {'cli:--systems-config': receipt['systems_config']['path']}
        if data.get('debug_overrides') != {'active': expected_overrides,
                                          'declared': expected_overrides}:
            raise ValueError('unapproved far budget capture overrides')
        # Snapshot the verified traversal bytes for the path-based validator.
        # A single private, closed file of at most 64 KiB is live at a time;
        # Windows never reads an open NamedTemporaryFile or removes open files.
        if cell['workload'] == 'traversal':
            text = fixture_paths.decode_text(read(ROOT, world['script'], MAX_SCRIPT_BYTES))
            workload = data.get('workload')
            if (digest(text.encode('utf-8')) != world['script_sha256'] or
                    not isinstance(workload, dict) or workload.get('script') != text):
                raise ValueError('far budget embedded traversal differs from pinned fixture')
            with tempfile.TemporaryDirectory(prefix='far-budget-traversal-') as directory:
                traversal = Path(directory) / 'workload.traversal'
                traversal.write_bytes(text.encode('utf-8'))
                checked = evaluate(data, matrix, cell, world, traversal)
        else:
            checked = evaluate(data, matrix, cell, world)
        if checked['state'] != 'complete' or any(actual.get(key) != value for key, value in checked.items()):
            raise ValueError('far budget timing/coverage evidence failed or summary differs: ' + cell['id'])


def digest(payload):
    return hashlib.sha256(payload).hexdigest()


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'),
                       ensure_ascii=True, allow_nan=False) + '\n').encode('utf-8')


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def decode(payload):
    text = payload.decode('utf-8-sig')
    qualification.check_host_json_depth(text)
    def refuse_constant(value):
        raise ValueError('non-finite JSON constant: ' + value)
    return json.loads(text, object_pairs_hook=unique_object, parse_constant=refuse_constant)


def read(root, reference, limit):
    path = fixture_paths.resolve_reference(root, reference, max_depth=32)
    return fixture_paths.read_bytes(root, path, limit=limit)


def exact(value, keys, label):
    if not isinstance(value, dict) or set(value) != set(keys):
        raise ValueError(label + ': missing or unknown fields')


def sha(value, label):
    if not isinstance(value, str) or re.fullmatch('[0-9a-f]{64}', value) is None:
        raise ValueError(label + ': expected SHA-256')


def validate_matrix(matrix):
    exact(matrix, ('schema', 'renderer', 'profiles', 'displays', 'worlds', 'workloads',
                   'warmup_frames', 'measured_frames'), 'matrix')
    if (matrix['schema'] != SCHEMA or matrix['renderer'] != qualification.CANONICAL_RENDERER or
            matrix['profiles'] != list(PROFILES) or matrix['displays'] != list(DISPLAYS) or
            matrix['workloads'] != list(WORKLOADS) or
            type(matrix['warmup_frames']) is not int or matrix['warmup_frames'] != 400 or
            type(matrix['measured_frames']) is not int or matrix['measured_frames'] != 240):
        raise ValueError('matrix must retain every contracted profile, display and workload')
    worlds = matrix['worlds']
    if not isinstance(worlds, list) or len(worlds) != len(WORLDS):
        raise ValueError('matrix requires every world/profile/display group')
    if [world.get('id') if isinstance(world, dict) else None for world in worlds] != list(WORLDS):
        raise ValueError('matrix requires every world/profile/display group in canonical order')
    for world in worlds:
        exact(world, ('id', 'preset', 'preset_revision', 'preset_identity', 'content_identity',
                      'seed', 'inputs', 'script', 'script_sha256', 'pose', 'blocked_by'), 'world')
        if world['preset'] != {'cave-dense': 'cave_dense', 'edited': 'default'}.get(world['id'], world['id']):
            raise ValueError('world preset differs')
        if type(world['preset_revision']) is not int or world['preset_revision'] != 8:
            raise ValueError('world preset revision differs')
        render_contract.integer(world['seed'], 'seed', 0, 2**32 - 1)
        for key, pattern in (('preset_identity', '[0-9a-f]{16}'),
                             ('content_identity', '[1-9][0-9]{0,19}')):
            value = world[key]
            if value is not None and (not isinstance(value, str) or re.fullmatch(pattern, value) is None):
                raise ValueError('invalid world ' + key)
        if world['content_identity'] is not None and int(world['content_identity']) > 2**64 - 1:
            raise ValueError('world content identity exceeds uint64')
        inputs = world['inputs']
        if not isinstance(inputs, dict) or not 1 <= len(inputs) <= 32:
            raise ValueError('missing or excessive fixture inputs')
        required = {'worlds/atlas/presets/' + world['preset'] + '.json', 'data/common/biomes.json'}
        if not required.issubset(inputs):
            raise ValueError('fixture must pin preset and referenced biome data')
        for path, identity in inputs.items():
            fixture_paths.reference_parts(path)
            sha(identity, 'fixture input')
        script, identity = world['script'], world['script_sha256']
        if script is not None:
            fixture_paths.reference_parts(script)
            sha(identity, 'traversal')
        elif identity is not None:
            raise ValueError('missing script cannot have an identity')
        reason = world['blocked_by']
        if reason is not None and (not isinstance(reason, str) or not reason.strip() or len(reason) > 1024):
            raise ValueError('invalid dependency reason')
        pose = world['pose']
        if pose is not None:
            exact(pose, ('x', 'y', 'z', 'yaw', 'pitch'), 'pose')
            for key, value in pose.items():
                render_contract.number(value, 'pose ' + key, -32000)
                if abs(value) > (89.99 if key == 'pitch' else 32000):
                    raise ValueError('pose outside supported bounds')


def load_matrix(path=DEFAULT_MATRIX):
    path = Path(path).absolute()
    matrix = decode(fixture_paths.read_bytes(path.parent, path, limit=MAX_MATRIX_BYTES))
    validate_matrix(matrix)
    return matrix


def cells(matrix):
    validate_matrix(matrix)
    return [dict(id='.'.join((world['id'], profile, display, workload)),
                 world=world['id'], profile=profile, display=display, workload=workload,
                 phases=['cold', 'settled' if workload == 'settled' else 'traversal'])
            for world in matrix['worlds'] for profile in PROFILES for display in DISPLAYS
            for workload in WORKLOADS]


def fixture_identity(world):
    return digest(canonical(world))


def dependencies(world, cell, root=ROOT):
    """Missing pins are incomplete; present but invalid/escaping inputs are refused."""
    reasons = []
    for reference, expected in sorted(world['inputs'].items()):
        try:
            payload = read(root, reference, MAX_MATRIX_BYTES)
        except FileNotFoundError:
            reasons.append('missing fixture input: ' + reference)
            continue
        if digest(canonical(decode(payload))) != expected:
            raise ValueError('fixture input identity differs: ' + reference)
    if world['blocked_by'] is not None:
        reasons.append(world['blocked_by'])
    if any(world[key] is None for key in ('preset_identity', 'content_identity')):
        reasons.append('native preset/content identity unavailable')
    if cell['workload'] == 'settled':
        if world['pose'] is None:
            reasons.append('settled pose unavailable')
    elif world['script'] is None:
        reasons.append('traversal unavailable')
    else:
        try:
            payload = read(root, world['script'], MAX_SCRIPT_BYTES)
        except FileNotFoundError:
            reasons.append('traversal missing')
        else:
            # Match pinned text on CRLF checkouts without changing the script itself.
            text = fixture_paths.decode_text(payload)
            if digest(text.encode('utf-8')) != world['script_sha256']:
                raise ValueError('traversal identity differs')
            manifest = render_contract.traversal_manifest(text)
            for key in ('preset', 'preset_revision', 'preset_identity', 'content_identity', 'seed'):
                if manifest[key] != world[key]:
                    raise ValueError('traversal fixture differs: ' + key)
    # The current traversal reports dig/fill, not a committed authority snapshot.
    # Do not infer an issue #132 commit from a bake-generation counter alone.
    if world['id'] == 'edited':
        reasons.append('issue #132 commit/all-tier bake evidence contract unavailable')
    return list(dict.fromkeys(reasons))


def distribution(values, unit):
    result = perf.summarize(values, unit)
    result.pop('samples')
    return result


def summarize_rows(rows):
    result = {'frames': len(rows), 'render_wall_ms': distribution([r['render_wall_ms'] for r in rows], 'ms'),
              'tiers': [], 'combined_resident_peak_bytes': max(sum(t['memory']['total_bytes'] for t in r['tiers']) for r in rows)}
    result['resident_peaks'] = {key: max(sum(t['memory'][key] for t in row['tiers']) for row in rows)
                                for key in ('cpu_bytes', 'gpu_bytes', 'total_bytes')}
    for index in range(5):
        tiers = [row['tiers'][index] for row in rows]
        result['tiers'].append(dict(tier=index + 1,
            memory_peaks={key: max(t['memory'][key] for t in tiers)
                          for key in ('cpu_bytes', 'gpu_bytes', 'total_bytes', 'peak_bytes', 'admission_refusals')},
            work={key: distribution([t['work'][key] for t in tiers], 'bytes' if 'bytes' in key else 'count') for key in WORK},
            draws={key: distribution([t[key] for t in tiers], 'count')
                   for key in ('draws', 'indices', 'draw_refusals')}))
    result['work'] = {key: distribution([sum(t['work'][key] for t in r['tiers']) for r in rows],
                                        'bytes' if 'bytes' in key else 'count') for key in WORK}
    return result


def measurements(data):
    if not isinstance(data.get('far_budget'), dict):
        raise ValueError('far budget evidence is missing')
    render_contract.validate_far_budget(data)
    rows = data['far_budget']['frames']
    phases = {}
    for phase in ('cold_or_settling', 'measured'):
        selected = [row for row in rows if row['phase'] == phase]
        phases[phase] = summarize_rows(selected) if selected else None
    # Timing results can arrive later. Only timing_frame identifies the originating
    # sample; render_frame or list position must never substitute for this key.
    timings = {row['frame']: row for row in data['frame_samples']}
    hitches = []
    for row in rows:
        timing = timings.get(row['timing_frame'])
        if timing is not None and timing['frame_wall_ms'] > render_budget.FLOOR_MS:
            hitches.append(dict(timing_frame=timing['frame'], render_frame=row['render_frame'],
                frame_wall_ms=timing['frame_wall_ms'], event=row['event'],
                tiers=[dict(tier=tier['tier'], work=tier['work'], draws=tier['draws']) for tier in row['tiers']]))
    fills = []
    for fill in data['far_budget']['fills']:
        origin, accepted = fill['origin_ms'], fill['accepted_ms']
        fills.append(dict(fill, elapsed_from_origin_ms=None if fill['completed_ms'] is None else fill['completed_ms'] - origin,
                          elapsed_from_acceptance_ms=None if accepted is None or fill['completed_ms'] is None else fill['completed_ms'] - accepted,
                          tier_elapsed_ms=[None if value is None else value - origin for value in fill['tier_completed_ms']]))
    return dict(phases=phases, fills=fills, hitches=hitches)


def evaluate(data, matrix, cell, world, traversal=None):
    """Full render validity and floor, plus binding and far-mode checks."""
    if world['id'] == 'edited':
        raise ValueError('issue #132 commit/all-tier bake evidence contract unavailable')
    width, height = map(int, cell['display'].split('x'))
    if (data['profile']['name'] != cell['profile'] or data['width'] != width or data['height'] != height or
            data['warmup_frames'] != matrix['warmup_frames']):
        raise ValueError('capture profile/display/warmup differs from cell')
    expected = {key: world[key] for key in ('preset', 'preset_revision', 'preset_identity', 'content_identity', 'seed')}
    if any(value is None for value in expected.values()):
        raise ValueError('capture cannot bind unavailable native fixture identities')
    expected['kind'] = ('fixed_view' if cell['workload'] == 'settled' else
                        render_contract.traversal_manifest(data['workload']['script']).get('kind', 'traversal'))
    if cell['workload'] == 'settled' and data['frames'] != matrix['measured_frames']:
        raise ValueError('settled frame count differs')
    render_contract.validate(data, matrix['renderer'], traversal=traversal, workload=expected,
                             ladder_baseline=cell['workload'] == 'settled')
    if cell['workload'] == 'settled':
        pose = world['pose']
        if pose is None:
            raise ValueError('settled pose unavailable')
        validate_render_capture.validate(data, Namespace(
            position=[pose[key] for key in ('x', 'y', 'z')], yaw=pose['yaw'], pitch=pose['pitch'],
            tod=0.04, fov=qualification.EXPECTED_FOV, qualified_renderer=matrix['renderer'],
            traversal=None, require_geometry=True, require_settled=True, require_camera_chunk=True,
            require_controller=False, require_distinct_controller=False, ladder_baseline=True))
    far_rows = data.get('far_world', {}).get('frames')
    if not far_rows:
        raise ValueError('missing far mode evidence')
    for row in far_rows:
        if (row['requested_mode'] != 'ladder' or row['effective_mode'] != 'ladder' or
                row['refusal'] != 'None' or row['submission_refusal'] != 'None' or row['suppressed'] or
                int(row['content_identity']) == 0):
            raise ValueError('Legacy fallback or refused far submission cannot qualify')
    metrics = measurements(data)
    if metrics['phases']['cold_or_settling'] is None:
        raise ValueError('cold/settling phase is missing')
    for row in data['far_budget']['frames']:
        for tier in row['tiers']:
            memory = tier['memory']
            if (memory['cpu_bytes'] + memory['gpu_bytes'] != memory['total_bytes'] or
                    memory['peak_bytes'] < memory['total_bytes']):
                raise ValueError('tier memory components or peak do not reconcile')
    floor = render_budget.evaluate(data)
    incomplete = any(fill['status'] != 'complete' for fill in metrics['fills'])
    return dict(metrics=metrics, verdict=floor,
                state=('fail' if not render_budget.verdict_passes(floor) else
                       'incomplete' if incomplete else 'complete'),
                reasons=['incomplete or timed out fill'] if incomplete else [])


def review(index):
    lines = ['# Far streaming measurements', '',
             'Measurement evidence only. Native qualification, budget approval, merge and release remain pending.', '',
             '| Cell | State | Source SHA-256 | p99 ms | Combined peak bytes | First fill ms |',
             '| --- | --- | --- | --- | --- | --- |']
    for cell in index['cells']:
        metrics = cell.get('metrics')
        available = [] if metrics is None else [p for p in metrics['phases'].values() if p is not None]
        peak = max((p['combined_resident_peak_bytes'] for p in available), default=None)
        first = None if metrics is None else metrics['fills'][0]['elapsed_from_origin_ms']
        verdict = cell.get('verdict')
        p99 = None if verdict is None else verdict['floor']['p99_ms']
        lines.append(f"| {cell['id']} | {cell['state']} | {cell.get('capture_sha256') or 'unavailable'} | {p99 if p99 is not None else 'unavailable'} | {peak if peak is not None else 'unavailable'} | {first if first is not None else 'unavailable'} |")
    lines += ['', 'Candidate per-tier ceilings, global caps and separately measured reserve: unavailable; owner review required.', '']
    return '\n'.join(lines).encode('utf-8')

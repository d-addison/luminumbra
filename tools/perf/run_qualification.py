#!/usr/bin/env python3
"""Execution runner and aggregate for the render qualification matrix.

Cells run in matrix order. A ready cell is launched through
render_runner_process.run_process, its capture is bound to the cell, and then it is
judged by validate_render_capture.py with --enforce-floor; the checker's verdict must
reconcile with the capture. An unready cell is recorded as unevaluated and never
launched. The result is written to <out-dir>/qualification.json
(luminumbra.render_qualification.v1).

A --client path ending in .py runs with the current interpreter; the tests use this
to drive a fake client. Importing this module has no side effects.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import plan_qualification as planner
import qualification_matrix as matrix_lib
import render_budget
import render_runner_process as runner

SCHEMA = 'luminumbra.render_qualification.v1'
VERDICT_SCHEMA = 'luminumbra.render_verdict.v1'
VERDICT_KEYS = ('schema', 'workload_kind', 'profile', 'adapter', 'frames', 'floor', 'target')
FLOOR_KEYS = ('limit_ms', 'p99_ms', 'verdict')
STATES = ('pass', 'fail', 'invalid', 'process_failed', 'unevaluated')
DEFAULT_TIMEOUT_SECONDS = 900
CHECKER_TIMEOUT_SECONDS = 120
FLOOR_FAILURE_EXIT = 4
QUALIFICATION_NAME = 'qualification.json'
EXPANDED_REASON = 'requires far_coverage evidence from the far-world runtime workstream'
REFRESH_TEXT = re.compile(r'[0-9]{1,4}')
SETTINGS_NAME = 'settings'
SETTINGS_KEYS = ('HOME', 'USERPROFILE', 'APPDATA', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME')
EXPECTED_FOV = 45.0  # The client default from SystemConfig.
FIXED_VIEW_CHECK_FLAGS = ('--require-settled', '--require-geometry', '--require-camera-chunk')

# The canonical qualification universe. A run qualifies only against exactly these.
CANONICAL_RENDERER = 'NVIDIA GeForce RTX 5070 Ti'
CANONICAL_KINDS = {
    'forest-settled': 'fixed_view',
    'ground-settled': 'fixed_view',
    'high-altitude-settled': 'fixed_view',
    'cave-air-settled': 'fixed_view',
    'shoreline-settled': 'fixed_view',
    'mountain-vista-settled': 'fixed_view',
    'surface-flight': 'traversal',
    'cave-walk': 'traversal',
    'edited-world-tour': 'traversal',
}
CANONICAL_WORKLOADS = tuple(CANONICAL_KINDS)
CANONICAL_PROFILES = ('quality', 'performance')
CANONICAL_DISPLAYS = ('3840x1600', '3440x1440')
CANONICAL_WARMUP = 400
CANONICAL_FIXED_FRAMES = 240
CANONICAL_SURFACE_SCRIPT = 'tools/perf/fixtures/surface-flight.traversal'
# The traversal script each traversal workload must carry to be canonical. cave-walk and
# edited-world-tour are deliberately absent: they stay non-canonical (never qualify) until
# AC8 commits their scripts and binds them here.
BOUND_TRAVERSAL_SCRIPTS = {'surface-flight': CANONICAL_SURFACE_SCRIPT}


class UsageError(ValueError):
    """Operator input error; reported on stderr with exit code 2."""


def canonical_cell_ids():
    return frozenset(f'{workload}.{profile}.{display}'
                     for workload in CANONICAL_WORKLOADS
                     for profile in CANONICAL_PROFILES
                     for display in CANONICAL_DISPLAYS)


def sanitized_environment():
    """Copy of os.environ without LUMIN_ and LUMINUMBRA_ variables."""
    return {key: value for key, value in os.environ.items()
            if not key.startswith(('LUMIN_', 'LUMINUMBRA_'))}


def isolated_environment(out_dir, environment=None):
    """Client environment with user-settings locations redirected into out-dir/settings."""
    settings = Path(out_dir) / SETTINGS_NAME
    result = sanitized_environment() if environment is None else dict(environment)
    for key in SETTINGS_KEYS:
        result[key] = str(settings)
    return result


def reset_settings(out_dir):
    """Remove whatever an earlier run left in out-dir/settings, then recreate it empty."""
    settings = Path(out_dir) / SETTINGS_NAME
    if settings.is_dir():
        shutil.rmtree(settings, ignore_errors=False)
    elif settings.exists():
        settings.unlink()
    settings.mkdir(parents=True)


def set_aside(path):
    """Rename an existing file to the smallest unused <name>.prev-<n>; never delete it."""
    if not path.exists():
        return
    number = 1
    while path.with_name(f'{path.name}.prev-{number}').exists():
        number += 1
    path.rename(path.with_name(f'{path.name}.prev-{number}'))


def read_json_object(path):
    """Parsed JSON object from path, or None when missing, unreadable or not an object."""
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError, UnicodeError):
        return None
    return data if isinstance(data, dict) else None


def finite(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))


def adapter_triple(data):
    adapter = data.get('adapter') if isinstance(data, dict) else None
    if not isinstance(adapter, dict):
        return None
    return {key: adapter.get(key) for key in ('vendor', 'renderer', 'version')}


def finish(cell, state, reason, artifact, verdict=None, *, checker_canonical):
    """Cell result; the artifact path and adapter are reported only when the file exists."""
    present = Path(artifact).is_file()
    return dict(id=cell['id'], workload=cell['workload'], kind=cell['kind'],
                warmup_frames=cell['warmup_frames'], measured_frames=cell['measured_frames'],
                script=cell['script'], state=state, reason=reason,
                adapter=adapter_triple(read_json_object(artifact)) if present else None,
                artifact=str(artifact) if present else None, verdict=verdict,
                checker_canonical=checker_canonical)


def binding_mismatch(cell, data):
    """Name the first capture field that disagrees with the cell, or None when all match."""
    profile = data.get('profile')
    if not isinstance(profile, dict) or profile.get('name') != cell['profile']:
        return 'profile'
    if data.get('width') != cell['width']:
        return 'width'
    if data.get('height') != cell['height']:
        return 'height'
    if data.get('warmup_frames') != cell['warmup_frames']:
        return 'warmup_frames'
    if cell['kind'] == 'fixed_view' and data.get('frames') != cell['measured_frames']:
        return 'frames'
    return None


def verdict_reason(verdict, data, cell):
    """None when a checker-passed verdict reconciles with the capture and cell, else the reason."""
    if not isinstance(verdict, dict) or any(key not in verdict for key in VERDICT_KEYS):
        return 'verdict missing/malformed'
    floor = verdict['floor']
    if (verdict['schema'] != VERDICT_SCHEMA or not isinstance(floor, dict)
            or any(key not in floor for key in FLOOR_KEYS)
            or not isinstance(verdict['target'], dict)):
        return 'verdict missing/malformed'
    if (floor['verdict'] not in ('pass', 'fail') or floor['limit_ms'] != render_budget.FLOOR_MS
            or not finite(floor['p99_ms'])):
        return 'verdict missing/malformed'
    if floor['verdict'] == 'fail':
        return 'verdict-disagrees-with-checker'
    if floor['p99_ms'] > floor['limit_ms']:
        return 'verdict-does-not-match-capture: p99_ms'
    distribution = data.get('distribution')
    block = distribution.get('frame_wall_ms') if isinstance(distribution, dict) else None
    capture_p99 = block.get('p99') if isinstance(block, dict) else None
    if not finite(capture_p99):
        return 'capture-lacks-frame-wall-p99'
    profile = data.get('profile')
    name = profile.get('name') if isinstance(profile, dict) else None
    if verdict['profile'] != name:
        return 'verdict-does-not-match-capture: profile'
    # The checker's workload_kind is the capture's workload.kind, which uses the same
    # 'fixed_view' / 'traversal' vocabulary as the matrix cell kind, so no mapping is needed.
    capture_workload = data.get('workload')
    capture_kind = capture_workload.get('kind') if isinstance(capture_workload, dict) else None
    if verdict['workload_kind'] != cell['kind'] or capture_kind not in (None, cell['kind']):
        return 'verdict-does-not-match-capture: workload_kind'
    if verdict['adapter'] != adapter_triple(data):
        return 'verdict-does-not-match-capture: adapter'
    if verdict['frames'] != data.get('frames'):
        return 'verdict-does-not-match-capture: frames'
    if floor['p99_ms'] != capture_p99:
        return 'verdict-does-not-match-capture: p99_ms'
    return None


def verdict_passes(verdict):
    return (isinstance(verdict, dict) and verdict.get('schema') == VERDICT_SCHEMA
            and isinstance(verdict.get('floor'), dict)
            and verdict['floor'].get('verdict') == 'pass')


def valid_adapter(adapter):
    return (isinstance(adapter, dict)
            and all(isinstance(adapter.get(key), str) and adapter.get(key)
                    for key in ('vendor', 'renderer', 'version')))


def canonical_record(result):
    """True when the cell carries exactly the canonical workload semantics."""
    if CANONICAL_KINDS.get(result['workload']) != result['kind']:
        return False
    if result['warmup_frames'] != CANONICAL_WARMUP:
        return False
    if result['kind'] == 'fixed_view':
        return result['measured_frames'] == CANONICAL_FIXED_FRAMES
    return (result['script'] is not None
            and BOUND_TRAVERSAL_SCRIPTS.get(result['workload']) == result['script'])


def cell_qualifies(result):
    """A cell counts toward qualification only with a complete, canonical, passing record
    that was checked by the canonical checker."""
    return (result['state'] == 'pass' and valid_adapter(result['adapter'])
            and result['adapter']['renderer'] == CANONICAL_RENDERER
            and result['artifact'] is not None and verdict_passes(result['verdict'])
            and result['checker_canonical'] is True and canonical_record(result))


def check_command(cell, *, matrix, artifact, verdict_path, root, python, checker):
    """Checker argv: the matrix checker command with the script swapped and floor flags."""
    command = matrix_lib.checker_command(
        cell, artifact=artifact, qualified_renderer=matrix['qualified_renderer'],
        root=root, python=python)
    command[1] = str(checker)
    if cell['kind'] == 'fixed_view':
        command += list(FIXED_VIEW_CHECK_FLAGS) + ['--fov', str(EXPECTED_FOV)]
    return command + ['--enforce-floor', '--verdict-out', str(verdict_path)]


def run_cell(cell, matrix, *, client, out_dir, root, python, checker, timeout_seconds,
             environment, checker_canonical):
    state, reason = matrix_lib.readiness(cell, root)
    cells_dir = out_dir / 'cells'
    cell_id = cell['id']
    artifact = cells_dir / f'{cell_id}.json'
    canonical = checker_canonical
    if state != 'ready':
        return finish(cell, 'unevaluated', reason, artifact, checker_canonical=canonical)
    work = out_dir / 'work' / cell_id
    work.mkdir(parents=True, exist_ok=True)
    cells_dir.mkdir(parents=True, exist_ok=True)
    set_aside(artifact)
    command = matrix_lib.build_command(cell, client=client, artifact=artifact, root=root)
    if str(client).endswith('.py'):
        command = [sys.executable, str(client)] + command[1:]
    try:
        completed, evidence = runner.run_process(
            command, cwd=work, environment=environment, log=cells_dir / f'{cell_id}.log',
            artifact=artifact, retained=cells_dir / f'{cell_id}.artifact.json',
            receipt_path=cells_dir / f'{cell_id}.process.json',
            timeout_seconds=timeout_seconds)
    except subprocess.TimeoutExpired:
        return finish(cell, 'process_failed', 'timed_out', artifact, checker_canonical=canonical)
    except OSError as error:
        return finish(cell, 'process_failed', f'spawn-failed: {error}', artifact,
                      checker_canonical=canonical)
    if completed.returncode != 0:
        return finish(cell, 'process_failed', f'exit {completed.returncode}', artifact,
                      checker_canonical=canonical)
    if evidence:
        return finish(cell, 'process_failed', 'evidence: ' + '; '.join(evidence), artifact,
                      checker_canonical=canonical)
    if not artifact.is_file():
        return finish(cell, 'process_failed', 'no-artifact', artifact,
                      checker_canonical=canonical)
    data = read_json_object(artifact)
    field = None if data is None else binding_mismatch(cell, data)
    if field is not None:
        return finish(cell, 'invalid', f'capture-does-not-match-cell: {field}', artifact,
                      checker_canonical=canonical)
    verdict_path = cells_dir / f'{cell_id}.verdict.json'
    set_aside(verdict_path)
    check = check_command(cell, matrix=matrix, artifact=artifact, verdict_path=verdict_path,
                          root=root, python=python, checker=checker)
    try:
        with (cells_dir / f'{cell_id}.check.log').open('wb') as log:
            checked = subprocess.run(check, cwd=work, env=environment, stdout=log,
                                     stderr=subprocess.STDOUT,
                                     timeout=CHECKER_TIMEOUT_SECONDS, check=False)
    except subprocess.TimeoutExpired:
        return finish(cell, 'invalid',
                      f'checker timed out after {CHECKER_TIMEOUT_SECONDS} s', artifact,
                      checker_canonical=canonical)
    except OSError as error:
        return finish(cell, 'invalid', f'checker spawn-failed: {error}', artifact,
                      checker_canonical=canonical)
    verdict = read_json_object(verdict_path)
    if checked.returncode == 0:
        reason = verdict_reason(verdict, data or {}, cell)
        if reason is not None:
            return finish(cell, 'invalid', reason, artifact, verdict,
                          checker_canonical=canonical)
        return finish(cell, 'pass', None, artifact, verdict, checker_canonical=canonical)
    if checked.returncode == FLOOR_FAILURE_EXIT:
        return finish(cell, 'fail', 'floor', artifact, verdict, checker_canonical=canonical)
    return finish(cell, 'invalid', f'checker exit {checked.returncode}', artifact, verdict,
                  checker_canonical=canonical)


def run_cells(matrix, *, client, out_dir, root=planner.ROOT, python=sys.executable,
              only=None, timeout_seconds=DEFAULT_TIMEOUT_SECONDS, checker=None,
              environment=None):
    """Run every selected cell in matrix order; return one result dict per cell."""
    out_dir = Path(out_dir)
    if checker is None:
        checker = Path(root) / matrix_lib.VALIDATOR
    # A replacement checker may run, but its results never count toward qualification.
    checker_canonical = Path(checker).resolve() == (Path(root) / matrix_lib.VALIDATOR).resolve()
    reset_settings(out_dir)
    environment = isolated_environment(out_dir, environment)
    return [run_cell(cell, matrix, client=client, out_dir=out_dir, root=root, python=python,
                     checker=checker, timeout_seconds=timeout_seconds,
                     environment=environment, checker_canonical=checker_canonical)
            for cell in planner.select(matrix_lib.cells(matrix), only)]


def display_entries(matrix, declared_refresh):
    """Displays with refresh_hz and refresh_source; a matrix value always wins."""
    entries = []
    for display in matrix['displays']:
        if display['refresh_hz'] is not None:
            hz, source = display['refresh_hz'], 'matrix'
        elif display['id'] in declared_refresh:
            hz, source = declared_refresh[display['id']], 'operator'
        else:
            hz, source = None, 'undeclared'
        entries.append(dict(id=display['id'], refresh_hz=hz, refresh_source=source))
    return entries


def aggregate(matrix, results, *, partial, declared_refresh):
    """Build the luminumbra.render_qualification.v1 document from cell results."""
    declared = dict(declared_refresh)
    displays = display_entries(matrix, declared)
    summary = {state: 0 for state in STATES}
    for result in results:
        summary[result['state']] += 1
    # Identity is decided over every cell, so a missing adapter breaks it.
    adapters = [result['adapter'] for result in results]
    adapter_identical = (bool(adapters) and adapters[0] is not None
                         and all(item == adapters[0] for item in adapters))
    reported = [item for item in adapters if item is not None]
    qualified = matrix['qualified_renderer']
    adapter_matches = all(item['renderer'] == qualified for item in reported)
    harness_complete = (not partial and summary['process_failed'] == 0
                        and summary['invalid'] == 0)
    ids = [result['id'] for result in results]
    canonical = canonical_cell_ids()
    complete_universe = (not partial and len(ids) == len(canonical)
                         and set(ids) == canonical)
    undeclared = any(item['refresh_source'] == 'undeclared' for item in displays)
    render_floor = (complete_universe and qualified == CANONICAL_RENDERER
                    and all(cell_qualifies(result) for result in results)
                    and adapter_identical and adapter_matches and not undeclared)
    return dict(
        schema=SCHEMA, matrix_schema=matrix['schema'], partial=partial,
        qualified_renderer=qualified, declared_refresh=declared, displays=displays,
        cells=[dict(id=result['id'], workload=result['workload'], kind=result['kind'],
                    warmup_frames=result['warmup_frames'],
                    measured_frames=result['measured_frames'], script=result['script'],
                    state=result['state'], reason=result['reason'],
                    adapter=result['adapter'], artifact=result['artifact'],
                    verdict=result['verdict'], checker_canonical=result['checker_canonical'])
               for result in results],
        summary=summary, adapter_identical=adapter_identical,
        adapter_matches_qualified=adapter_matches, harness_complete=harness_complete,
        render_floor_qualified=render_floor, expanded_qualified=False,
        expanded_qualified_reason=EXPANDED_REASON)


def parse_declarations(values, matrix):
    """Parse repeated <display-id>=<hz>; refuse unknown ids, bad values and conflicts."""
    known = {display['id']: display for display in matrix['displays']}
    declared = {}
    for value in values:
        display_id, separator, hz_text = value.partition('=')
        if not separator or display_id not in known:
            raise UsageError(f'--declare-refresh {value!r}: expected <display-id>=<hz> '
                             'naming a matrix display')
        if display_id in declared:
            raise UsageError(f'--declare-refresh {display_id!r} given more than once')
        if known[display_id]['refresh_hz'] is not None:
            raise UsageError(f'--declare-refresh {display_id!r}: the matrix already '
                             'records its refresh')
        if REFRESH_TEXT.fullmatch(hz_text) is None or not 1 <= int(hz_text) <= 1000:
            raise UsageError(f'--declare-refresh {value!r}: hz must be an integer 1..1000')
        declared[display_id] = int(hz_text)
    return declared


def exit_code(summary):
    if summary['fail'] or summary['invalid'] or summary['process_failed']:
        return 1
    if summary['unevaluated']:
        return 3
    return 0


def resolve_client(text):
    """Absolute path when the client is a path that exists from the caller's cwd."""
    if (os.sep in text or (os.altsep and os.altsep in text)) and Path(text).exists():
        return str(Path(text).resolve())
    return text


def resolve_checker(text):
    """Absolute path when the checker script exists from the caller's cwd."""
    return str(Path(text).resolve()) if Path(text).exists() else text


def run_dry(matrix, args, out_dir):
    plan = planner.build_plan(matrix, client=args.client, out_dir=out_dir, root=planner.ROOT,
                              python=sys.executable, only=args.only)
    path = out_dir / planner.PLAN_NAME
    path.write_text(json.dumps(plan, indent=2) + '\n', encoding='utf-8')
    print(f"qualification plan: {plan['cell_count']} cells, "
          f"ready {plan['summary']['ready']}, "
          f"unevaluated {plan['summary']['unevaluated']} -> {path}")
    return 0


def fail_closed(path, error):
    """Replace the aggregate with an unqualified error record; exit 1."""
    message = str(error) or type(error).__name__
    payload = dict(schema=SCHEMA, render_floor_qualified=False, harness_complete=False,
                   expanded_qualified=False, error=message)
    try:
        runner.atomic_write(path, (json.dumps(payload, indent=2) + '\n').encode('utf-8'))
    except OSError as write_error:
        print(f'run_qualification: cannot write {path}: {write_error}', file=sys.stderr)
    print(f'run_qualification: unexpected error: {message}', file=sys.stderr)
    return 1


def build_parser():
    parser = argparse.ArgumentParser(
        description='Run render qualification cells and write qualification.json.')
    parser.add_argument('--client', required=True,
                        help='Render benchmark client executable; a path ending in .py is '
                             'run with the current interpreter (used by tests)')
    parser.add_argument('--out-dir', required=True, type=Path,
                        help='Directory receiving cells/, work/ and qualification.json')
    parser.add_argument('--matrix', type=Path, default=planner.DEFAULT_MATRIX,
                        help='Qualification matrix JSON (default: committed matrix)')
    parser.add_argument('--only', action='append', metavar='CELL_ID',
                        help='Select one cell id; repeatable; makes the run partial')
    parser.add_argument('--timeout-seconds', type=int, default=DEFAULT_TIMEOUT_SECONDS,
                        help='Client process timeout (default: 900)')
    parser.add_argument('--declare-refresh', action='append', default=[],
                        metavar='DISPLAY_ID=HZ',
                        help='Operator refresh rate for a display the matrix leaves null')
    parser.add_argument('--checker', type=Path, default=None,
                        help='Checker script run in place of validate_render_capture.py')
    parser.add_argument('--dry-run', action='store_true',
                        help='Write qualification-plan.json and launch nothing')
    parser.add_argument('--format', choices=('json',), default='json',
                        help='Output format (only json)')
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.timeout_seconds < 1:
            raise UsageError('--timeout-seconds must be at least 1')
        matrix = matrix_lib.load_matrix(args.matrix)
        declared = parse_declarations(args.declare_refresh, matrix)
        planner.select(matrix_lib.cells(matrix), args.only)
    except (matrix_lib.MatrixError, UsageError) as error:
        print(f'run_qualification: {error}', file=sys.stderr)
        return 2
    out_dir = args.out_dir.resolve()
    client = resolve_client(str(args.client))
    checker = None if args.checker is None else resolve_checker(str(args.checker))
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
        if args.dry_run:
            return run_dry(matrix, args, out_dir)
    except OSError as error:
        print(f'run_qualification: cannot write under {out_dir}: {error}', file=sys.stderr)
        return 2
    path = out_dir / QUALIFICATION_NAME
    try:
        set_aside(path)
        results = run_cells(matrix, client=client, out_dir=out_dir,
                            only=args.only, timeout_seconds=args.timeout_seconds,
                            checker=checker)
        payload = aggregate(matrix, results, partial=args.only is not None,
                            declared_refresh=declared)
    except Exception as error:  # Any unexpected failure must leave an unqualified record.
        return fail_closed(path, error)
    try:
        runner.atomic_write(path, (json.dumps(payload, indent=2) + '\n').encode('utf-8'))
    except OSError as error:
        print(f'run_qualification: cannot write {path}: {error}', file=sys.stderr)
        return 2
    summary = payload['summary']
    print(f"run_qualification: {len(results)} cells, pass {summary['pass']}, "
          f"fail {summary['fail']}, invalid {summary['invalid']}, "
          f"process_failed {summary['process_failed']}, "
          f"unevaluated {summary['unevaluated']}, "
          f"render_floor_qualified {str(payload['render_floor_qualified']).lower()} "
          f"-> {path}")
    return exit_code(summary)


if __name__ == '__main__':
    sys.exit(main())

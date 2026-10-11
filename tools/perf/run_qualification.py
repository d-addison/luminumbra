#!/usr/bin/env python3
"""Execution runner and aggregate for the render qualification matrix.

Cells run in matrix order. A ready cell is launched through
render_runner_process.run_process and then judged by validate_render_capture.py with
--enforce-floor; an unready cell is recorded as unevaluated and never launched.
The result is written to <out-dir>/qualification.json (luminumbra.render_qualification.v1).

A --client path ending in .py runs with the current interpreter; the tests use this
to drive a fake client. Importing this module has no side effects.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import plan_qualification as planner
import qualification_matrix as matrix_lib
import render_runner_process as runner

SCHEMA = 'luminumbra.render_qualification.v1'
STATES = ('pass', 'fail', 'invalid', 'process_failed', 'unevaluated')
DEFAULT_TIMEOUT_SECONDS = 900
CHECKER_TIMEOUT_SECONDS = 120
FLOOR_FAILURE_EXIT = 4
QUALIFICATION_NAME = 'qualification.json'
EXPANDED_REASON = 'requires far_coverage evidence from the far-world runtime workstream'
REFRESH_TEXT = re.compile(r'[0-9]{1,4}')


class UsageError(ValueError):
    """Operator input error; reported on stderr with exit code 2."""


def sanitized_environment():
    """Copy of os.environ without LUMIN_ and LUMINUMBRA_ variables."""
    return {key: value for key, value in os.environ.items()
            if not key.startswith(('LUMIN_', 'LUMINUMBRA_'))}


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


def adapter_of(artifact):
    data = read_json_object(artifact)
    adapter = None if data is None else data.get('adapter')
    if not isinstance(adapter, dict):
        return None
    return {key: adapter.get(key) for key in ('vendor', 'renderer', 'version')}


def finish(cell_id, state, reason, artifact, verdict=None):
    """Cell result; the artifact path and adapter are reported only when the file exists."""
    present = Path(artifact).is_file()
    return dict(id=cell_id, state=state, reason=reason,
                adapter=adapter_of(artifact) if present else None,
                artifact=str(artifact) if present else None, verdict=verdict)


def run_cell(cell, matrix, *, client, out_dir, root, python, checker, timeout_seconds,
             environment):
    cell_id = cell['id']
    state, reason = matrix_lib.readiness(cell, root)
    cells_dir = out_dir / 'cells'
    artifact = cells_dir / f'{cell_id}.json'
    if state != 'ready':
        return finish(cell_id, 'unevaluated', reason, artifact)
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
        return finish(cell_id, 'process_failed', 'timed_out', artifact)
    except OSError as error:
        return finish(cell_id, 'process_failed', f'spawn-failed: {error}', artifact)
    if completed.returncode != 0:
        return finish(cell_id, 'process_failed', f'exit {completed.returncode}', artifact)
    if evidence:
        return finish(cell_id, 'process_failed', 'evidence: ' + '; '.join(evidence), artifact)
    if not artifact.is_file():
        return finish(cell_id, 'process_failed', 'no-artifact', artifact)
    verdict_path = cells_dir / f'{cell_id}.verdict.json'
    set_aside(verdict_path)
    check = matrix_lib.checker_command(
        cell, artifact=artifact, qualified_renderer=matrix['qualified_renderer'],
        root=root, python=python)
    check[1] = str(checker)
    check += ['--enforce-floor', '--verdict-out', str(verdict_path)]
    try:
        with (cells_dir / f'{cell_id}.check.log').open('wb') as log:
            checked = subprocess.run(check, cwd=work, env=environment, stdout=log,
                                     stderr=subprocess.STDOUT,
                                     timeout=CHECKER_TIMEOUT_SECONDS, check=False)
    except subprocess.TimeoutExpired:
        return finish(cell_id, 'invalid',
                      f'checker timed out after {CHECKER_TIMEOUT_SECONDS} s', artifact)
    except OSError as error:
        return finish(cell_id, 'invalid', f'checker spawn-failed: {error}', artifact)
    verdict = read_json_object(verdict_path)
    if checked.returncode == 0:
        return finish(cell_id, 'pass', None, artifact, verdict)
    if checked.returncode == FLOOR_FAILURE_EXIT:
        return finish(cell_id, 'fail', 'floor', artifact, verdict)
    return finish(cell_id, 'invalid', f'checker exit {checked.returncode}', artifact, verdict)


def run_cells(matrix, *, client, out_dir, root=planner.ROOT, python=sys.executable,
              only=None, timeout_seconds=DEFAULT_TIMEOUT_SECONDS, checker=None,
              environment=None):
    """Run every selected cell in matrix order; return one result dict per cell."""
    out_dir = Path(out_dir)
    if checker is None:
        checker = Path(root) / matrix_lib.VALIDATOR
    if environment is None:
        environment = sanitized_environment()
    return [run_cell(cell, matrix, client=client, out_dir=out_dir, root=root, python=python,
                     checker=checker, timeout_seconds=timeout_seconds,
                     environment=environment)
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
    adapters = [result['adapter'] for result in results if result['adapter'] is not None]
    adapter_identical = bool(adapters) and all(item == adapters[0] for item in adapters)
    qualified = matrix['qualified_renderer']
    adapter_matches = all(item['renderer'] == qualified for item in adapters)
    harness_complete = (not partial and summary['process_failed'] == 0
                        and summary['invalid'] == 0)
    total = len(matrix_lib.cells(matrix))
    undeclared = any(item['refresh_source'] == 'undeclared' for item in displays)
    render_floor = (not partial and len(results) == total and summary['pass'] == total
                    and adapter_identical and adapter_matches and not undeclared)
    return dict(
        schema=SCHEMA, matrix_schema=matrix['schema'], partial=partial,
        qualified_renderer=qualified, declared_refresh=declared, displays=displays,
        cells=[dict(id=result['id'], state=result['state'], reason=result['reason'],
                    adapter=result['adapter'], artifact=result['artifact'],
                    verdict=result['verdict']) for result in results],
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


def run_dry(matrix, args, out_dir):
    plan = planner.build_plan(matrix, client=args.client, out_dir=out_dir, root=planner.ROOT,
                              python=sys.executable, only=args.only)
    path = out_dir / planner.PLAN_NAME
    path.write_text(json.dumps(plan, indent=2) + '\n', encoding='utf-8')
    print(f"qualification plan: {plan['cell_count']} cells, "
          f"ready {plan['summary']['ready']}, "
          f"unevaluated {plan['summary']['unevaluated']} -> {path}")
    return 0


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
    out_dir = args.out_dir
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
        if args.dry_run:
            return run_dry(matrix, args, out_dir)
    except OSError as error:
        print(f'run_qualification: cannot write under {out_dir}: {error}', file=sys.stderr)
        return 2
    checker = None if args.checker is None else os.path.abspath(args.checker)
    results = run_cells(matrix, client=os.path.abspath(args.client), out_dir=out_dir,
                        only=args.only, timeout_seconds=args.timeout_seconds,
                        checker=checker)
    payload = aggregate(matrix, results, partial=args.only is not None,
                        declared_refresh=declared)
    path = out_dir / QUALIFICATION_NAME
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

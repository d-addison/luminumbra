#!/usr/bin/env python3
"""Dry-run planner for the render qualification matrix; never launches the client.

Writes <out-dir>/qualification-plan.json: one entry per selected cell with its
readiness, render-client argv, checker argv and the artifact path a later runner
must write. Ready cells carry argv lists; unevaluated cells carry null.
"""
import argparse
import json
from pathlib import Path
import sys

import qualification_matrix as matrix_lib
import render_runner_process as runner

ROOT = runner.resolve_path(__file__).parents[2]
DEFAULT_MATRIX = runner.resolve_path(__file__).parent / 'qualification' / 'matrix.json'
PLAN_SCHEMA = 'luminumbra.qualification_plan.v1'
PLAN_NAME = 'qualification-plan.json'


def select(cells, only):
    """Return cells in matrix order, filtered to `only` ids when given."""
    if only is None:
        return cells
    known = {cell['id'] for cell in cells}
    unknown = [cell_id for cell_id in only if cell_id not in known]
    if unknown:
        raise matrix_lib.MatrixError(f'unknown cell id {unknown[0]!r}')
    wanted = set(only)
    return [cell for cell in cells if cell['id'] in wanted]


def build_plan(matrix, *, client, out_dir, root, python, only=None):
    counts = {'ready': 0, 'unevaluated': 0}
    entries = []
    selected = select(matrix_lib.cells(matrix), only)
    for cell in selected:
        state, reason = matrix_lib.readiness(cell, root)
        counts[state] += 1
        artifact = Path(out_dir) / 'cells' / f"{cell['id']}.json"
        command = checker = None
        if state == 'ready':
            command = matrix_lib.build_command(cell, client=client, artifact=artifact,
                                               root=root)
            checker = matrix_lib.checker_command(
                cell, artifact=artifact, qualified_renderer=matrix['qualified_renderer'],
                root=root, python=python)
        entries.append(dict(id=cell['id'], workload=cell['workload'], kind=cell['kind'],
                            profile=cell['profile'], display=cell['display'], state=state,
                            reason=reason, command=command, checker_command=checker,
                            artifact=str(artifact)))
    return dict(schema=PLAN_SCHEMA, matrix_schema=matrix['schema'],
                partial=only is not None, cell_count=len(entries), summary=counts,
                cells=entries)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Plan render qualification cells (dry run).')
    parser.add_argument('--client', required=True, help='Render benchmark client executable')
    parser.add_argument('--out-dir', required=True, type=Path,
                        help='Directory receiving qualification-plan.json')
    parser.add_argument('--matrix', type=Path, default=DEFAULT_MATRIX,
                        help='Qualification matrix JSON (default: committed matrix)')
    parser.add_argument('--only', action='append', metavar='CELL_ID',
                        help='Select one cell id; repeatable; makes the plan partial')
    parser.add_argument('--format', choices=('json',), default='json',
                        help='Plan format (only json)')
    args = parser.parse_args(argv)
    try:
        matrix = matrix_lib.load_matrix(args.matrix)
        plan = build_plan(matrix, client=args.client, out_dir=args.out_dir, root=ROOT,
                          python=sys.executable, only=args.only)
    except matrix_lib.MatrixError as error:
        print(f'plan_qualification: {error}', file=sys.stderr)
        return 2
    path = args.out_dir / PLAN_NAME
    try:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(plan, indent=2) + '\n', encoding='utf-8')
    except OSError as error:
        print(f'plan_qualification: cannot write {path}: {error}', file=sys.stderr)
        return 1
    print(f"qualification plan: {plan['cell_count']} cells, "
          f"ready {plan['summary']['ready']}, "
          f"unevaluated {plan['summary']['unevaluated']} -> {path}")
    return 0


if __name__ == '__main__':
    sys.exit(main())

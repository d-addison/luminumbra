#!/usr/bin/env python3
"""Classify render runner process receipts (*.process.json).

Each receipt (luminumbra.render_runner_process.v1) is classified as returned,
timed_out or spawn_error. For a timed_out receipt the sibling log (the receipt name with
.process.json replaced by --log-suffix, default .log) is searched for the last
'Shutdown stage: <name>' line. That stage marks where the wait happened: placement is
'in_<stage>'. With no stage line the placement is
'before_shutdown_or_no_breadcrumbs'; with no log the entry carries log_missing.

This only classifies. It never changes a limit or a verdict. Exit 0 on success, 2 for
invalid input (unreadable path, bad JSON, wrong schema, unknown outcome).
"""
import argparse
import json
import re
import sys
from pathlib import Path

import render_runner_process as runner

SCHEMA = 'luminumbra.process_receipt_summary.v1'
RECEIPT_SUFFIX = '.process.json'
OUTCOMES = ('returned', 'timed_out', 'spawn_error')
STAGES = ('query_cleanup_started', 'query_cleanup_finished', 'world_readers_drain_started',
          'far_lod_drained', 'world_readers_drained', 'world_save_started',
          'world_save_finished')
STAGE_LINE = re.compile(r'Shutdown stage: (' + '|'.join(STAGES) + r')(?![A-Za-z_])')
NO_BREADCRUMBS = 'before_shutdown_or_no_breadcrumbs'


class InputError(Exception):
    """Invalid input; reported on stderr with exit code 2."""


def load_receipt(path):
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise InputError(f'{path}: unreadable: {error}') from error
    try:
        data = json.loads(payload)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise InputError(f'{path}: not valid JSON') from error
    if not isinstance(data, dict) or data.get('schema') != runner.RECEIPT_SCHEMA:
        raise InputError(f'{path}: not a {runner.RECEIPT_SCHEMA} receipt')
    if data.get('outcome') not in OUTCOMES:
        raise InputError(f'{path}: unknown outcome {data.get("outcome")!r}')
    return data


def collect_receipts(inputs):
    found = {}
    for item in inputs:
        path = Path(item)
        if path.is_dir():
            candidates = [p for p in path.rglob('*' + RECEIPT_SUFFIX) if p.is_file()]
        elif path.is_file():
            if not path.name.endswith(RECEIPT_SUFFIX):
                raise InputError(f'{item}: not a {RECEIPT_SUFFIX} receipt')
            candidates = [path]
        else:
            raise InputError(f'{item}: no such file or directory')
        for candidate in candidates:
            resolved = candidate.resolve()
            found[str(resolved)] = resolved
    return [found[key] for key in sorted(found)]


def last_shutdown_stage(log_path):
    payload = log_path.read_bytes()
    if payload[:2] in (b'\xff\xfe', b'\xfe\xff'):
        text = payload.decode('utf-16', errors='replace')
    else:
        text = payload.decode('utf-8', errors='replace')
    stage = None
    for line in text.replace('\x00', '').splitlines():
        match = STAGE_LINE.search(line)
        if match is not None:
            stage = match.group(1)
    return stage


def timed_out_fields(receipt_path, log_suffix):
    log_path = receipt_path.with_name(receipt_path.name[:-len(RECEIPT_SUFFIX)] + log_suffix)
    if not log_path.is_file():
        return dict(log_path=str(log_path), log_missing=True, last_shutdown_stage=None,
                    placement=None)
    stage = last_shutdown_stage(log_path)
    placement = NO_BREADCRUMBS if stage is None else 'in_' + stage
    return dict(log_path=str(log_path), log_missing=False, last_shutdown_stage=stage,
                placement=placement)


def summarize(receipt_path, receipt, log_suffix):
    entry = dict(path=str(receipt_path), classification=receipt['outcome'],
                 binary_path=receipt.get('binary_path'),
                 timeout_seconds=receipt.get('timeout_seconds'),
                 elapsed_seconds=receipt.get('elapsed_seconds'))
    if receipt['outcome'] == 'returned':
        entry.update(return_code=receipt.get('return_code'),
                     capture_status=receipt.get('capture_status'))
    elif receipt['outcome'] == 'spawn_error':
        entry.update(exception_type=receipt.get('exception_type'))
    else:
        entry.update(timed_out_fields(receipt_path, log_suffix))
    return entry


def build_parser():
    parser = argparse.ArgumentParser(
        description='Classify render runner process receipts and locate timed-out waits.')
    parser.add_argument('inputs', nargs='+', metavar='dir-or-receipt',
                        help='Receipt file, or directory searched recursively')
    parser.add_argument('--log-suffix', default='.log',
                        help='Suffix of the sibling log that replaces .process.json '
                             '(default: .log)')
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        entries = []
        counts = {outcome: 0 for outcome in OUTCOMES}
        for receipt_path in collect_receipts(args.inputs):
            receipt = load_receipt(receipt_path)
            entries.append(summarize(receipt_path, receipt, args.log_suffix))
            counts[receipt['outcome']] += 1
    except InputError as error:
        print(f'summarize_process_receipts: {error}', file=sys.stderr)
        return 2
    print(json.dumps(dict(schema=SCHEMA, receipts=entries, counts=counts), indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())

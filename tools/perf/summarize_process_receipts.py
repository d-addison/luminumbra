#!/usr/bin/env python3
"""Classify render runner process receipts (*.process.json).

Each receipt (luminumbra.render_runner_process.v1) is classified as returned,
timed_out or spawn_error. For a timed_out receipt the sibling log (the receipt name with
.process.json replaced by --log-suffix, default .log) is searched for the last
'Shutdown stage: <name>' line. That stage marks where the wait happened: placement is
'in_<stage>'. With no stage line the placement is
'before_shutdown_or_no_breadcrumbs'; with no log the entry carries log_missing.

This only classifies. It never changes a limit or a verdict. Exit 0 on success, 2 for
invalid input (paths, JSON/schema/field types, decoding, links or limits).
Limits: 16 MiB per file, 1,000 receipts, 100,000 discovery entries/log lines,
65,536 characters per log line. Over-limit input exits 2 without a summary.
Retained diagnostic strings are limited to 300 characters. Logs are streamed and
strictly decoded as UTF-8 or BOM-marked UTF-16. Receipts/logs must be regular files
within the supplied evidence directory (or an explicit receipt's parent).
"""
import argparse
import json
import math
import os
import re
import stat
import sys
from pathlib import Path

import render_runner_process as runner
from check_gl_debug_log import InputError, bounded_bytes, read_log, MAX_RECORDS

SCHEMA = 'luminumbra.process_receipt_summary.v1'
RECEIPT_SUFFIX = '.process.json'
OUTCOMES = ('returned', 'timed_out', 'spawn_error')
STAGES = ('query_cleanup_started', 'query_cleanup_finished', 'world_readers_drain_started',
          'far_lod_drained', 'world_readers_drained', 'world_save_started',
          'world_save_finished')
STAGE_LINE = re.compile(r'Shutdown stage: (' + '|'.join(STAGES) + r')(?![A-Za-z_])')
NO_BREADCRUMBS = 'before_shutdown_or_no_breadcrumbs'
MAX_RECEIPTS = 1_000
MAX_DISCOVERY_ENTRIES = 100_000
MAX_EXCERPT_CHARS = 300


def validate_path(path):
    if '\x00' in str(path):
        raise InputError('path contains NUL')


def validate_log_suffix(suffix):
    if not suffix or any(char in suffix for char in ('/', '\\', '\x00', ':')):
        raise InputError('--log-suffix must be a nonempty filename suffix without separators')


def regular_file(path, root):
    """lstat first: never resolve a receipt/log link and read its target."""
    validate_path(path)
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise InputError(f'{path}: receipt/log must be a regular file, not a symlink')
    resolved = path.resolve(strict=True)
    if not resolved.is_relative_to(root):
        raise InputError(f'{path}: evidence escapes supplied directory')
    return resolved


def load_receipt(path):
    payload = bounded_bytes(path)
    try:
        data = json.loads(payload)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise InputError(f'{path}: not valid JSON') from error
    if not isinstance(data, dict) or data.get('schema') != runner.RECEIPT_SCHEMA:
        raise InputError(f'{path}: not a {runner.RECEIPT_SCHEMA} receipt')
    if data.get('outcome') not in OUTCOMES:
        raise InputError(f'{path}: unknown outcome')

    def require(name, predicate):
        if name not in data or not predicate(data[name]):
            raise InputError(f'{path}: missing or invalid {name}')

    def text(value):
        return isinstance(value, str) and bool(value) and '\x00' not in value

    def digest(value):
        return isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None

    def number(value):
        return (type(value) in (int, float) and value >= 0
                and (type(value) is int or math.isfinite(value)))

    require('command', lambda v: isinstance(v, list) and 0 < len(v) <= MAX_RECORDS
            and all(text(item) for item in v))
    for name in ('cwd', 'binary_path'):
        require(name, text)
    spawn_error = data['outcome'] == 'spawn_error'
    require('evidence_errors', lambda v: isinstance(v, list) and len(v) <= MAX_RECORDS
            and all(isinstance(item, str) for item in v))
    changed_binary = (data.get('binary_identity_status') == 'changed'
                      and any(error.startswith('executable verification: ')
                              for error in data['evidence_errors']))
    require('binary_sha256', lambda v: digest(v) or
            ((spawn_error or changed_binary) and v is None))
    require('timeout_seconds', lambda v: number(v) and v > 0)
    require('elapsed_seconds', lambda v: number(v) or (spawn_error and v is None))
    require('return_code', lambda v: type(v) is int if data['outcome'] == 'returned'
            else v is None)
    require('capture_status', lambda v: v in ('missing', 'read_error', 'invalid_json',
                                             'write_error', 'retained'))
    require('capture_path', lambda v: v is None or text(v))
    require('capture_sha256', lambda v: v is None or digest(v))
    if data['outcome'] != 'returned':
        require('exception_type', text)
        require('exception_message', lambda v: isinstance(v, str))
    for name in ('log_tail', 'last_observed_phase'):
        if name in data:
            require(name, lambda v: v is None or isinstance(v, str))
    return data


def collect_receipts(inputs):
    found = {}
    scanned = 0

    def add(candidate, root):
        resolved = regular_file(candidate, root)
        found[str(resolved)] = resolved
        if len(found) > MAX_RECEIPTS:
            raise InputError(f'input exceeds {MAX_RECEIPTS} receipt cap')

    for item in inputs:
        validate_path(item)
        path = Path(item)
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode):
            raise InputError(f'{path}: symlinked input is not allowed')
        if stat.S_ISDIR(info.st_mode):
            root = path.resolve(strict=True)
            pending = [root]
            while pending:
                folder = pending.pop()
                # scandir streams directory entries; never follow directory links.
                with os.scandir(folder) as candidates:
                    for candidate in candidates:
                        scanned += 1
                        if scanned > MAX_DISCOVERY_ENTRIES:
                            raise InputError(f'input exceeds {MAX_DISCOVERY_ENTRIES} discovery entry cap')
                        candidate_path = Path(candidate.path)
                        if candidate.name.endswith(RECEIPT_SUFFIX):
                            add(candidate_path, root)
                        elif candidate.is_dir(follow_symlinks=False):
                            if not candidate_path.resolve(strict=True).is_relative_to(root):
                                raise InputError('directory escapes supplied evidence root')
                            pending.append(candidate_path)
        elif stat.S_ISREG(info.st_mode):
            if not path.name.endswith(RECEIPT_SUFFIX):
                raise InputError(f'{item}: not a {RECEIPT_SUFFIX} receipt')
            add(path, path.parent.resolve(strict=True))
        else:
            raise InputError(f'{item}: not a regular file or directory')
    return [found[key] for key in sorted(found)]


def last_shutdown_stage(log_path):
    stage = None
    for line_number, line in enumerate(read_log(log_path), 1):
        if line_number > MAX_RECORDS:
            raise InputError(f'log exceeds {MAX_RECORDS} line cap')
        match = STAGE_LINE.search(line)
        if match is not None:
            stage = match.group(1)
    return stage


def timed_out_fields(receipt_path, log_suffix):
    validate_log_suffix(log_suffix)
    log_path = receipt_path.with_name(receipt_path.name[:-len(RECEIPT_SUFFIX)] + log_suffix)
    try:
        regular_file(log_path, receipt_path.parent)
    except FileNotFoundError:
        return dict(log_path=str(log_path), log_missing=True, last_shutdown_stage=None,
                    placement=None)
    stage = last_shutdown_stage(log_path)
    placement = NO_BREADCRUMBS if stage is None else 'in_' + stage
    return dict(log_path=str(log_path), log_missing=False, last_shutdown_stage=stage,
                placement=placement)


def summarize(receipt_path, receipt, log_suffix):
    entry = dict(path=str(receipt_path), classification=receipt['outcome'],
                 binary_path=receipt['binary_path'][:MAX_EXCERPT_CHARS],
                 timeout_seconds=receipt['timeout_seconds'],
                 elapsed_seconds=receipt['elapsed_seconds'])
    if receipt['outcome'] == 'returned':
        entry.update(return_code=receipt['return_code'],
                     capture_status=receipt['capture_status'])
    elif receipt['outcome'] == 'spawn_error':
        entry.update(exception_type=receipt['exception_type'][:MAX_EXCERPT_CHARS])
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
        validate_log_suffix(args.log_suffix)
        entries = []
        counts = {outcome: 0 for outcome in OUTCOMES}
        for receipt_path in collect_receipts(args.inputs):
            receipt = load_receipt(receipt_path)
            entries.append(summarize(receipt_path, receipt, args.log_suffix))
            counts[receipt['outcome']] += 1
    except (InputError, OSError, ValueError, RuntimeError) as error:
        print(f'summarize_process_receipts: {str(error)[:MAX_EXCERPT_CHARS]}', file=sys.stderr)
        return 2
    print(json.dumps(dict(schema=SCHEMA, receipts=entries, counts=counts), indent=2))
    return 0


if __name__ == '__main__':
    sys.exit(main())

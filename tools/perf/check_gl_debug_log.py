#!/usr/bin/env python3
"""Verdict on the GL KHR_debug output recorded in a client log.

The client logs driver messages as lines containing
[GL][<Source>][<Type>][<SEVERITY>] id=<n> <message>. A line is a GL error when Type is
'Error' or SEVERITY is 'HIGH', the same rule the client's is_error uses. Debug output
must have been installed: a missing install marker, or a 'LUMIN_GL_DEBUG set but' line
(debug output not installed), makes the verdict unevaluated. Absence of evidence is
never a pass.

Exit codes: 0 pass, 1 fail (GL errors or malformed records), 2 invalid input,
3 unevaluated. Each file is limited to 16 MiB; logs to 100,000 lines/records and
65,536 characters per line. Over-limit or invalid text/path input exits 2.
Only the first 20 error excerpts (300 characters each) are retained.
With --artifact, the file must be a JSON object with schema
luminumbra.render_benchmark.v3; it is validated only and does not change the verdict.
"""
import argparse
import codecs
from contextlib import nullcontext
import io
import json
import os
import re
import stat
import sys
from pathlib import Path

SCHEMA = 'luminumbra.gl_debug_verdict.v1'
ARTIFACT_SCHEMA = 'luminumbra.render_benchmark.v3'
INSTALL_MARKER = 'GL KHR_debug output installed (synchronous'
NOT_INSTALLED = 'LUMIN_GL_DEBUG set but'
MESSAGE = re.compile(r'\[GL\]\[([^\[\]\r\n]+)\]\[([^\[\]\r\n]+)\]\[([^\[\]\r\n]+)\] id=\d+ ')
MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_RECORDS = 100_000
MAX_LINE_CHARS = 65_536
MAX_ERRORS = 20
MAX_ERROR_CHARS = 300
PASS = 'pass'
FAIL = 'fail'
UNEVALUATED = 'unevaluated'
EXIT_CODES = {PASS: 0, FAIL: 1, UNEVALUATED: 3}


class InputError(Exception):
    """Unreadable or invalid input; reported on stderr with exit code 2."""


def open_input(path):
    """Open a regular file with a byte cap; reject malformed path arguments."""
    try:
        candidate = Path(path)
        if not stat.S_ISREG(candidate.stat().st_mode):
            raise InputError(f'input is not a regular file: {path}')
        source = candidate.open('rb')
        try:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode):
                raise InputError(f'input is not a regular file: {path}')
            if info.st_size > MAX_FILE_BYTES:
                raise InputError(f'input exceeds {MAX_FILE_BYTES} byte cap: {path}')
        except (OSError, InputError):
            source.close()
            raise
        return source
    except (OSError, ValueError) as error:
        raise InputError(f'input unreadable: {path}: {str(error)[:MAX_ERROR_CHARS]}') from error


def bounded_bytes(path, *, source=None):
    """Bound JSON bytes; an optional verified source stays owned by the caller."""
    try:
        with open_input(path) if source is None else nullcontext(source) as source:
            payload = source.read(MAX_FILE_BYTES + 1)
    except OSError as error:
        raise InputError(f'input unreadable: {path}: {str(error)[:MAX_ERROR_CHARS]}') from error
    if len(payload) > MAX_FILE_BYTES:
        raise InputError(f'input exceeds {MAX_FILE_BYTES} byte cap: {path}')
    return payload


def read_log(path, *, source=None):
    """Stream bounded UTF-8/UTF-16 lines, optionally from a caller-owned source."""
    try:
        with open_input(path) if source is None else nullcontext(source) as source:
            prefix = source.read(2)
            encoding = ('utf-16' if prefix in (codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)
                        else 'utf-8')
            decoder = codecs.getincrementaldecoder(encoding)(errors='strict')
            pending = ''
            total = 0
            block = prefix
            while True:
                total += len(block)
                if total > MAX_FILE_BYTES:
                    raise InputError(f'input exceeds {MAX_FILE_BYTES} byte cap: {path}')
                pending += decoder.decode(block, final=not block)
                while '\n' in pending:
                    line, pending = pending.split('\n', 1)
                    if len(line) > MAX_LINE_CHARS:
                        raise InputError(f'log line exceeds {MAX_LINE_CHARS} character cap: {path}')
                    if '\x00' in line:
                        raise InputError(f'log contains NUL: {path}')
                    yield line.rstrip('\r')
                if len(pending) > MAX_LINE_CHARS:
                    raise InputError(f'log line exceeds {MAX_LINE_CHARS} character cap: {path}')
                if not block:
                    if '\x00' in pending:
                        raise InputError(f'log contains NUL: {path}')
                    if pending:
                        yield pending
                    break
                block = source.read(64 * 1024)
    except (OSError, ValueError, UnicodeError) as error:
        raise InputError(f'log is not valid readable text: {path}: {str(error)[:MAX_ERROR_CHARS]}') from error


def check_artifact(path):
    payload = bounded_bytes(path)
    try:
        data = json.loads(payload)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise InputError(f'artifact is not valid JSON: {path}') from error
    if not isinstance(data, dict) or data.get('schema') != ARTIFACT_SCHEMA:
        raise InputError(f'artifact schema is not {ARTIFACT_SCHEMA}: {path}')


def evaluate(lines):
    # String callers are supported without materializing splitlines().
    if isinstance(lines, str):
        lines = io.StringIO(lines)
    installed = False
    not_installed = False
    errors = []
    error_count = malformed_count = record_count = 0
    warning_count = notification_count = 0
    for line_number, line in enumerate(lines, 1):
        if line_number > MAX_RECORDS:
            raise InputError(f'log exceeds {MAX_RECORDS} line/record cap')
        installed |= INSTALL_MARKER in line
        not_installed |= NOT_INSTALLED in line
        for marker in re.finditer(r'\[GL(?=\]|\[|$)', line):
            record_count += 1
            if record_count > MAX_RECORDS:
                raise InputError(f'log exceeds {MAX_RECORDS} record cap')
            match = MESSAGE.match(line, marker.start())
            malformed = match is None
            if malformed:
                malformed_count += 1
            if malformed or match.group(2) == 'Error' or match.group(3) == 'HIGH':
                error_count += 1
                if len(errors) < MAX_ERRORS:
                    errors.append(line[:MAX_ERROR_CHARS].rstrip())
            elif match.group(3) == 'NOTIFICATION':
                notification_count += 1
            else:
                warning_count += 1
    installed &= not not_installed
    if malformed_count:
        verdict = FAIL
    elif not installed:
        verdict = UNEVALUATED
    elif error_count:
        verdict = FAIL
    else:
        verdict = PASS
    return dict(schema=SCHEMA, verdict=verdict, installed=installed,
                error_count=error_count, malformed_count=malformed_count,
                warning_count=warning_count, notification_count=notification_count,
                errors=errors)


def build_parser():
    parser = argparse.ArgumentParser(
        description='Verdict on GL KHR_debug output recorded in a client log.')
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument('--log', type=Path,
                        help='Client log path (UTF-8 or UTF-16 with BOM)')
    inputs.add_argument('--stdin', action='store_true',
                        help='Read bounded binary client log bytes from standard input')
    parser.add_argument('--artifact', type=Path, default=None,
                        help='Optional render benchmark v3 capture to validate')
    parser.add_argument('--format', choices=('json',), default='json',
                        help='Output format (only json)')
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.artifact is not None:
            check_artifact(args.artifact)
        result = evaluate(read_log('<stdin>' if args.stdin else args.log,
                                   source=sys.stdin.buffer if args.stdin else None))
    except InputError as error:
        print(f'check_gl_debug_log: {str(error)[:MAX_ERROR_CHARS]}', file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return EXIT_CODES[result['verdict']]


if __name__ == '__main__':
    sys.exit(main())

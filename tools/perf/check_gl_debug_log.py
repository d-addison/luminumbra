#!/usr/bin/env python3
"""Verdict on the GL KHR_debug output recorded in a client log.

The client logs driver messages as lines containing
[GL][<Source>][<Type>][<SEVERITY>] id=<n> <message>. A line is a GL error when Type is
'Error' or SEVERITY is 'HIGH', the same rule the client's is_error uses. Debug output
must have been installed: a missing install marker, or a 'LUMIN_GL_DEBUG set but' line
(debug output not installed), makes the verdict unevaluated. Absence of evidence is
never a pass.

Exit codes: 0 pass, 1 fail (GL error lines present), 2 invalid input, 3 unevaluated.
With --artifact, the file must be a JSON object with schema
luminumbra.render_benchmark.v3; it is validated only and does not change the verdict.
"""
import argparse
import codecs
import json
import re
import sys
from pathlib import Path

SCHEMA = 'luminumbra.gl_debug_verdict.v1'
ARTIFACT_SCHEMA = 'luminumbra.render_benchmark.v3'
INSTALL_MARKER = 'GL KHR_debug output installed (synchronous'
NOT_INSTALLED = 'LUMIN_GL_DEBUG set but'
MESSAGE = re.compile(r'\[GL\]\[([^\]]*)\]\[([^\]]*)\]\[([^\]]*)\]')
MAX_ERRORS = 20
MAX_ERROR_CHARS = 300
PASS = 'pass'
FAIL = 'fail'
UNEVALUATED = 'unevaluated'
EXIT_CODES = {PASS: 0, FAIL: 1, UNEVALUATED: 3}


class InputError(Exception):
    """Unreadable or invalid input; reported on stderr with exit code 2."""


def decode_text(payload):
    """Decode log bytes; UTF-16 when BOM-marked, NUL characters removed."""
    if payload[:2] in (codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE):
        text = payload.decode('utf-16', errors='replace')
    else:
        text = payload.decode('utf-8', errors='replace')
    return text.replace('\x00', '')


def read_log(path):
    try:
        payload = Path(path).read_bytes()
    except OSError as error:
        raise InputError(f'log unreadable: {error}') from error
    text = decode_text(payload)
    if '\ufffd' in text:
        raise InputError(f'log is not valid text: {path}')
    return text


def check_artifact(path):
    try:
        payload = Path(path).read_bytes()
    except OSError as error:
        raise InputError(f'artifact unreadable: {error}') from error
    try:
        data = json.loads(payload)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise InputError(f'artifact is not valid JSON: {path}') from error
    if not isinstance(data, dict) or data.get('schema') != ARTIFACT_SCHEMA:
        raise InputError(f'artifact schema is not {ARTIFACT_SCHEMA}: {path}')


def evaluate(text):
    installed = INSTALL_MARKER in text and NOT_INSTALLED not in text
    errors = []
    warning_count = 0
    notification_count = 0
    for line in text.splitlines():
        match = MESSAGE.search(line)
        if match is None:
            continue
        _, kind, severity = match.groups()
        if kind == 'Error' or severity == 'HIGH':
            errors.append(line.rstrip())
        elif severity == 'NOTIFICATION':
            notification_count += 1
        else:
            warning_count += 1
    if not installed:
        verdict = UNEVALUATED
    elif errors:
        verdict = FAIL
    else:
        verdict = PASS
    return dict(schema=SCHEMA, verdict=verdict, installed=installed,
                error_count=len(errors), warning_count=warning_count,
                notification_count=notification_count,
                errors=[line[:MAX_ERROR_CHARS] for line in errors[:MAX_ERRORS]])


def build_parser():
    parser = argparse.ArgumentParser(
        description='Verdict on GL KHR_debug output recorded in a client log.')
    parser.add_argument('--log', required=True, type=Path,
                        help='Client log path (UTF-8 or UTF-16 with BOM)')
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
        result = evaluate(read_log(args.log))
    except InputError as error:
        print(f'check_gl_debug_log: {error}', file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return EXIT_CODES[result['verdict']]


if __name__ == '__main__':
    sys.exit(main())

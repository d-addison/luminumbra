#!/usr/bin/env python3
"""Parametrised client-process runner for render benchmark captures.

Copy of the _file_sha256, _atomic_write, _retain_process_evidence and _run_process
helpers from test_render_runner.py, with the timeout passed in and recorded in the
receipt. test_render_runner.py keeps its own copy with its fixed 180 s contract limit
so its evidence tests stay unchanged; keep the two in step by hand.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

RECEIPT_SCHEMA = 'luminumbra.render_runner_process.v1'


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def atomic_write(path, payload):
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + '.',
                                         suffix='.tmp', delete=False) as output:
            temporary = Path(output.name)
            output.write(payload)
        temporary.replace(path)
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass  # Cleanup must not replace the original write/replace error.


def retain_process_evidence(receipt, artifact, retained, receipt_path):
    errors = receipt['evidence_errors']
    try:
        payload = Path(artifact).read_bytes()
    except FileNotFoundError:
        receipt['capture_status'] = 'missing'
    except OSError as error:
        receipt['capture_status'] = 'read_error'
        errors.append(f'capture read: {error}')
    else:
        try:
            json.loads(payload)
        except (ValueError, UnicodeError, RecursionError):
            receipt['capture_status'] = 'invalid_json'
        else:
            try:
                atomic_write(retained, payload)
            except OSError as error:
                receipt['capture_status'] = 'write_error'
                errors.append(f'capture retention: {error}')
            else:
                receipt.update(capture_status='retained', capture_path=str(retained),
                               capture_sha256=hashlib.sha256(payload).hexdigest())
    try:
        atomic_write(receipt_path, (json.dumps(receipt, indent=2) + '\n').encode('utf-8'))
    except OSError as error:
        errors.append(f'process receipt: {error}')
    if errors:
        try:
            print('Render runner evidence error: ' + '; '.join(errors), file=sys.stderr)
        except (OSError, ValueError):
            pass  # A closed diagnostic stream must not hide a process failure.
    return errors


def resolve_executable(program, cwd, environment=None):
    """Return the absolute executable that a spawn of `program` would run.

    A name with a path separator resolves against `cwd` when relative; a bare name
    resolves through PATH from `environment`. Raises FileNotFoundError when unresolved.
    """
    program = str(program)
    if os.sep in program or (os.altsep and os.altsep in program):
        candidate = Path(program)
        if not candidate.is_absolute():
            candidate = Path(cwd) / candidate
        if candidate.is_file():
            return Path(os.path.abspath(candidate))
        raise FileNotFoundError(f'executable not found: {candidate}')
    found = shutil.which(program, path=environment.get('PATH') if environment else None)
    if found is None:
        raise FileNotFoundError(f'executable not found on PATH: {program}')
    return Path(os.path.abspath(found))


def run_process(command, *, cwd, environment, log, artifact, retained, receipt_path,
                timeout_seconds=180):
    """Run one client process; return (CompletedProcess, evidence_errors).

    The executable is resolved once; the hash and the spawned argv[0] use that
    same file. Raises FileNotFoundError before any process starts when unresolved,
    and subprocess.TimeoutExpired or OSError after writing the receipt.
    """
    artifact, retained, receipt_path, log = (Path(artifact), Path(retained),
                                             Path(receipt_path), Path(log))
    executable = resolve_executable(command[0], cwd, environment)
    command = [str(executable)] + list(command[1:])
    # Retire only this case's exact outputs, including a repeated invocation in
    # the same runtime root. Never let an earlier capture appear current.
    for path in (artifact, retained, receipt_path, log):
        path.unlink(missing_ok=True)
    receipt = dict(schema=RECEIPT_SCHEMA, command=command, cwd=str(cwd),
                   binary_path=str(executable), binary_sha256=None,
                   timeout_seconds=timeout_seconds, elapsed_seconds=None,
                   outcome='spawn_error', return_code=None, capture_status='missing',
                   capture_path=None, capture_sha256=None, evidence_errors=[])
    try:
        receipt['binary_sha256'] = file_sha256(executable)
        with log.open('wb') as output:
            started = time.monotonic()
            try:
                result = subprocess.run(command, cwd=cwd, env=environment,
                                        stdout=output, stderr=subprocess.STDOUT,
                                        timeout=timeout_seconds, check=False)
            finally:
                receipt['elapsed_seconds'] = time.monotonic() - started
            receipt.update(outcome='returned', return_code=result.returncode)
    except (subprocess.TimeoutExpired, OSError) as error:
        receipt.update(outcome='timed_out' if isinstance(error, subprocess.TimeoutExpired)
                       else receipt['outcome'], exception_type=type(error).__name__,
                       exception_message=str(error))
        retain_process_evidence(receipt, artifact, retained, receipt_path)
        raise
    errors = retain_process_evidence(receipt, artifact, retained, receipt_path)
    return result, errors

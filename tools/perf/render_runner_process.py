#!/usr/bin/env python3
"""Parametrised client-process runner for render benchmark captures.

The timeout is passed in and recorded in the receipt. Shared output paths are
locked through process execution and evidence publication. The device regression
runner in test_render_runner.py keeps its fixed 180 s contract limit separately.
"""
from contextlib import contextmanager
import errno
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time

if os.name == 'nt':
    import msvcrt
else:
    import fcntl

RECEIPT_SCHEMA = 'luminumbra.render_runner_process.v1'
OUTPUT_LOCK_SUFFIX = '.lock'


def _windows_device_name(component):
    # Devices remain reserved with extensions or spaces before an extension.
    stem = component.partition('.')[0].rstrip(' ').casefold()
    return stem in ('con', 'prn', 'aux', 'nul', 'conin$', 'conout$') or (
        len(stem) == 4 and stem[:3] in ('com', 'lpt')
        and stem[3] in '123456789\u00b9\u00b2\u00b3')


def path_without_extended_prefix(path, *, require_ordinary=False):
    """Strip a Windows drive/UNC prefix only when ordinary spelling is equivalent.

    Comparison callers require ordinary spelling so an alias cannot bypass their
    identity checks. Other callers may retain an unsafe path's extended spelling.
    """
    text = str(path)
    if os.name == 'nt':
        # Handle paths and explicitly extended input can retain this prefix.
        # Strip only drive/UNC prefixes; other device namespaces are distinct.
        if text[:8].casefold() == '\\\\?\\unc\\':
            ordinary = '\\\\' + text[8:]
        elif (text.startswith('\\\\?\\') and len(text) >= 7
              and text[4] in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
              and text[5:7] == ':\\'):
            ordinary = text[4:]
        else:
            return type(path)(text)
        # Win32 normalizes names only without the prefix. MAX_PATH includes the
        # terminating NUL and counts UTF-16 code units, not Python characters.
        safe = (len(ordinary.encode('utf-16-le', errors='surrogatepass')) // 2 < 260
                and '/' not in ordinary
                and all(not component.endswith(('.', ' '))
                        and not _windows_device_name(component)
                        for component in ordinary.split('\\') if component))
        if safe:
            text = ordinary
        elif require_ordinary:
            raise OSError(errno.EINVAL,
                          'Windows path cannot be safely normalized for comparison', text)
    return type(path)(text)


def resolve_path(path):
    """Resolve aliases; refuse ambiguous extended Windows comparison paths."""
    # Check input too: lexical resolution must not erase an unsafe component.
    path_without_extended_prefix(Path(path), require_ordinary=True)
    return path_without_extended_prefix(Path(path).resolve(), require_ordinary=True)


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


TIMEOUT_LOG_TAIL_BYTES = 8 * 1024
SURFACE_READY_PHASE = re.compile(r'EnsureSurfaceReadyNear[:/]')


def report_timeout(receipt, log):
    truncated = False
    try:
        with log.open('rb') as source:
            source.seek(0, os.SEEK_END)
            offset = max(0, source.tell() - TIMEOUT_LOG_TAIL_BYTES)
            truncated = offset > 0
            source.seek(offset)
            payload = source.read(TIMEOUT_LOG_TAIL_BYTES)
    except OSError as error:
        payload = b''
        receipt['evidence_errors'].append(f'timeout log read: {error}')
    tail = payload.decode('utf-8', errors='replace')
    # Phase discovery is limited to the retained raw-byte window. Its first
    # line may be a fragment, so it cannot establish a complete phase record.
    complete_lines = tail.splitlines(keepends=True)
    if truncated:
        complete_lines = complete_lines[1:]
    complete_lines = [line for line in complete_lines if line.endswith('\n')]
    phases = [line for line in complete_lines
              if SURFACE_READY_PHASE.search(line)]
    last_phase = phases[-1].strip() if phases else None
    receipt.update(log_tail=tail, last_observed_phase=last_phase)
    # Diagnostics are best effort: a closed or failing stderr must not replace
    # the TimeoutExpired or prevent receipt retention.
    try:
        print(f"render runner: timed out after {receipt['timeout_seconds']} s", file=sys.stderr)
        print(tail, file=sys.stderr)
        print('last EnsureSurfaceReadyNear phase: ' + (last_phase or '(none observed)'),
              file=sys.stderr)
    except (OSError, ValueError):
        pass


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
        path_without_extended_prefix(candidate, require_ordinary=True)
        if candidate.is_file():
            return path_without_extended_prefix(Path(os.path.abspath(candidate)),
                                                require_ordinary=True)
        raise FileNotFoundError(f'executable not found: {candidate}')
    search = environment.get('PATH') if environment else None
    if search is None:
        search = os.environ.get('PATH', os.defpath)
    entries = []
    for entry in search.split(os.pathsep):
        base = Path(entry) if entry else Path('.')
        entries.append(str(base if base.is_absolute() else Path(cwd) / base))
    found = shutil.which(program, path=os.pathsep.join(entries))
    if found is None:
        raise FileNotFoundError(f'executable not found on PATH: {program}')
    path_without_extended_prefix(Path(found), require_ordinary=True)
    return path_without_extended_prefix(Path(os.path.abspath(found)), require_ordinary=True)


def _output_identity(path):
    """Return the shared destination identity after portable name validation."""
    def validate_components(candidate):
        for component in candidate.parts:
            if component in (candidate.anchor, '.', '..'):
                continue
            if component.endswith(('.', ' ')):
                raise OSError(errno.EINVAL, 'output path component ends in a dot or space',
                              str(path))
            if any(ord(char) < 32 or char in '<>:"|?*\\' for char in component):
                raise OSError(errno.EINVAL, 'output path contains an invalid Windows character',
                              str(path))
            if _windows_device_name(component):
                raise OSError(errno.EINVAL, 'output path contains a reserved Windows device name',
                              str(path))

    # Validate before resolving so that an unsafe component followed by '..'
    # cannot disappear. Also validate the resolved parent of a directory symlink.
    validate_components(path)
    path_without_extended_prefix(path, require_ordinary=True)
    parent = resolve_path(path.parent)
    validate_components(parent)
    # Preserve the directory's actual spelling on case-sensitive filesystems;
    # fold the destination name for every use of its identity, including locks.
    identity = parent / path.name.casefold()
    if identity.name.endswith(OUTPUT_LOCK_SUFFIX):
        raise OSError(errno.EINVAL, 'output overlaps a lock sidecar', str(path))
    return identity


def _sidecar_stat(lock):
    try:
        metadata = lock.lstat()
    except FileNotFoundError:
        return None
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
        raise OSError(errno.EINVAL, 'lock sidecar must be a regular file with one link',
                      str(lock))
    return metadata


def _same_file(left, right):
    return (left.st_dev, left.st_ino) == (right.st_dev, right.st_ino)


def _open_sidecar(lock):
    before = _sidecar_stat(lock)
    flags = os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
    if before is None:
        # Exclusive creation also refuses a symlink installed after lstat.
        flags |= os.O_CREAT | os.O_EXCL
    descriptor = os.open(lock, flags, 0o600)
    try:
        opened = os.fstat(descriptor)
        after = _sidecar_stat(lock)
        if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1
                or after is None or not _same_file(opened, after)
                or (before is not None and not _same_file(opened, before))):
            raise OSError(errno.EINVAL, 'lock sidecar changed during acquisition', str(lock))
        handle = os.fdopen(descriptor, 'r+b')
    except BaseException:
        os.close(descriptor)
        raise
    return handle


def _lock_handle(handle, path):
    # Windows locks a byte range at the current position, including beyond EOF.
    handle.seek(0)
    try:
        if os.name == 'nt':
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as error:
        if error.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
            raise
        raise FileExistsError(errno.EEXIST, 'output is locked by another run',
                              str(path)) from error


def _check_output(path):
    try:
        before = path.lstat()
    except FileNotFoundError:
        return
    if stat.S_ISLNK(before.st_mode):
        raise OSError(errno.ELOOP, 'output must not be a symlink', str(path))
    if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
        raise OSError(errno.EINVAL, 'output must be a regular file with one link', str(path))
    # Probe the inode too: an older invocation may already hold this output as
    # its lock through a sidecar symlink in an entirely different directory.
    flags = (os.O_RDWR if os.name == 'nt' else os.O_RDONLY)
    flags |= getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
    descriptor = os.open(path, flags)
    with os.fdopen(descriptor, 'rb') as handle:
        opened = os.fstat(handle.fileno())
        after = path.lstat()
        if (not _same_file(opened, before) or not _same_file(opened, after)
                or not stat.S_ISREG(after.st_mode)
                or opened.st_nlink != 1 or after.st_nlink != 1):
            raise OSError(errno.EINVAL, 'output changed during lock check', str(path))
        _lock_handle(handle, path)
        if os.name == 'nt':
            handle.seek(0)
            msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)


@contextmanager
def _output_locks(paths):
    # Never follow the destination itself: replacing an output must not change
    # its lock identity. Validate every path before any write-capable open.
    paths = [(Path(path), _output_identity(Path(path))) for path in paths]
    identities = [identity for path, identity in paths]
    if len(set(identities)) != len(identities):
        raise OSError(errno.EINVAL, 'output paths must be distinct')
    paths.sort(key=lambda item: item[1])
    for path, identity in paths:
        _sidecar_stat(Path(str(identity) + OUTPUT_LOCK_SUFFIX))
    for path, identity in paths:
        _check_output(path)
    locks = []
    completed = False
    try:
        for path, identity in paths:
            lock = Path(str(identity) + OUTPUT_LOCK_SUFFIX)
            handle = _open_sidecar(lock)
            locks.append((handle, False))
            _lock_handle(handle, lock)
            locks[-1] = (handle, True)
            current = _sidecar_stat(lock)
            if current is None or not _same_file(os.fstat(handle.fileno()), current):
                raise OSError(errno.EINVAL, 'lock sidecar changed during acquisition', str(lock))
        for path, identity in paths:
            _check_output(path)
        yield
        completed = True
    finally:
        cleanup_error = None
        for handle, acquired in reversed(locks):
            try:
                if os.name == 'nt' and acquired:
                    handle.seek(0)
                    msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError as error:
                cleanup_error = error
            try:
                handle.close()
            except OSError as error:
                cleanup_error = error
        if cleanup_error is not None and completed:
            raise cleanup_error
        # Never unlink lock files: contenders must keep opening the same inode.
        # OS locks are released above or by the OS on process death.


def run_process(command, *, cwd, environment, log, artifact, retained, receipt_path,
                timeout_seconds=180):
    """Run one client process; return (CompletedProcess, evidence_errors).

    The executable is resolved once; the hash and the spawned argv[0] use that
    same file. Resolution and lock-acquisition refusals leave outputs unchanged.
    Spawn and timeout failures attempt receipt publication before re-raising the
    original exception. An active OS lock refuses the invocation; leftover lock
    files are reusable after a crash. Symlink and overlapping outputs are refused;
    the lock-sidecar suffix is reserved globally. Locks cover all outputs through
    receipt publication.
    """
    artifact, retained, receipt_path, log = (Path(artifact), Path(retained),
                                             Path(receipt_path), Path(log))
    executable = resolve_executable(command[0], cwd, environment)
    command = [str(executable)] + list(command[1:])
    with _output_locks((artifact, retained, receipt_path, log)):
        return _run_process(command, cwd=cwd, environment=environment, log=log,
                            artifact=artifact, retained=retained,
                            receipt_path=receipt_path, timeout_seconds=timeout_seconds)


def _run_process(command, *, cwd, environment, log, artifact, retained, receipt_path,
                 timeout_seconds):
    # Retire only this case's exact outputs, including a repeated invocation in
    # the same runtime root. Never let an earlier capture appear current.
    # The output-lock guard excludes every name in the reserved sidecar namespace.
    for path in (artifact, retained, receipt_path, log):
        path.unlink(missing_ok=True)
    receipt = dict(schema=RECEIPT_SCHEMA, command=command, cwd=str(cwd),
                   binary_path=command[0], binary_sha256=None,
                   timeout_seconds=timeout_seconds, elapsed_seconds=None,
                   outcome='spawn_error', return_code=None, capture_status='missing',
                   capture_path=None, capture_sha256=None, evidence_errors=[])
    try:
        receipt['binary_sha256'] = file_sha256(command[0])
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
        try:
            if isinstance(error, subprocess.TimeoutExpired):
                report_timeout(receipt, log)
        finally:
            retain_process_evidence(receipt, artifact, retained, receipt_path)
        raise
    errors = retain_process_evidence(receipt, artifact, retained, receipt_path)
    return result, errors

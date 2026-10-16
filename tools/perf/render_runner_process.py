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
            components = text[8:].split('\\')
            # Server and share names are not subject to Win32 filename rules.
            valid_root = len(components) >= 2 and all(components[:2])
            components = components[2:]
        elif (text.startswith('\\\\?\\') and len(text) >= 7
              and text[4] in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
              and text[5:7] == ':\\'):
            ordinary = text[4:]
            valid_root = True
            components = ordinary.split('\\')
        else:
            return type(path)(text)
        # Win32 normalizes names only without the prefix. MAX_PATH includes the
        # terminating NUL and counts UTF-16 code units, not Python characters.
        safe = (valid_root
                and len(ordinary.encode('utf-16-le', errors='surrogatepass')) // 2 < 260
                and '/' not in ordinary
                and all(not component.endswith(('.', ' '))
                        and not _windows_device_name(component)
                        for component in components if component))
        if safe:
            text = ordinary
        elif require_ordinary:
            raise OSError(errno.EINVAL,
                          'Windows path cannot be safely normalized for comparison', text)
    return type(path)(text)


def resolve_path(path, *, strict=False):
    """Resolve aliases; refuse ambiguous extended Windows comparison paths."""
    # Check input too: lexical resolution must not erase an unsafe component.
    path_without_extended_prefix(Path(path), require_ordinary=True)
    resolved = Path(path).resolve(strict=True) if strict else Path(path).resolve()
    return path_without_extended_prefix(resolved, require_ordinary=True)


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def _executable_metadata(metadata):
    """Comparable snapshot for repeated calls of the *same* stat kind.

    Windows CPython 3.12's path stat copies birth time into st_ctime for
    compatibility, whereas handle fstat can expose ChangeTime. Do not use
    Windows ctime as a portable modification clock. Keep POSIX ctime, and
    keep device, inode, size and mtime on both platforms, comparing path
    snapshots separately from handle snapshots (their precision can differ).
    The digest independently detects edits that restore size and mtime.
    """
    fields = (metadata.st_dev, metadata.st_ino, metadata.st_size,
              metadata.st_mtime_ns)
    return fields if os.name == 'nt' else fields + (metadata.st_ctime_ns,)


def _open_binary(path):
    # A replacement FIFO must not block before the regular-file/identity check.
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NONBLOCK', 0)
    descriptor = os.open(path, flags)
    try:
        return os.fdopen(descriptor, 'rb')
    except BaseException:
        os.close(descriptor)
        raise


def _binary_identity(path):
    """Hash one open file and verify that its path still names that identity."""
    named_before = _executable_metadata(os.stat(path))

    digest = hashlib.sha256()
    with _open_binary(path) as source:
        before = os.fstat(source.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise OSError(errno.EINVAL, 'executable must be a regular file', str(path))
        # Hash in bounded 1 MiB blocks, independently of executable size.
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
        after = os.fstat(source.fileno())
        # Bind the current name to the hashed handle with another handle stat,
        # never by comparing the platform-dependent path/handle stat fields.
        with _open_binary(path) as current:
            reopened = os.fstat(current.fileno())
        named_after = _executable_metadata(os.stat(path))
    opened = _executable_metadata(after)
    if (not stat.S_ISREG(reopened.st_mode)
            or _executable_metadata(before) != opened
            or opened != _executable_metadata(reopened)
            or named_before != named_after):
        raise OSError(errno.EIO, 'executable changed while hashing', str(path))
    return (opened, named_after), digest.hexdigest()


@contextmanager
def verified_binary(receipt):
    """Fail closed if executable metadata or bytes change across execution.

    Keep the original location so adjacent runtime libraries remain discoverable.
    A process exception retains priority, but an unverified hash is never published.
    """
    path = receipt['binary_path']
    before = _binary_identity(path)
    receipt.update(binary_sha256=before[1], binary_identity_status='pending')
    failed = False
    try:
        yield
    except BaseException:
        failed = True
        raise
    finally:
        try:
            if _binary_identity(path) != before:
                raise OSError(errno.EIO, 'executable changed during execution', path)
        except OSError as error:
            receipt.update(binary_sha256=None, binary_identity_status='changed')
            receipt['evidence_errors'].append(f'executable verification: {error}')
            if not failed:
                raise
        else:
            receipt['binary_identity_status'] = 'verified'


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
    searches only absolute, nonempty PATH entries. All checks use the resolved path.
    """
    program = str(program)
    if os.sep in program or (os.altsep and os.altsep in program):
        candidate = Path(program)
        if not candidate.is_absolute():
            candidate = Path(cwd) / candidate
        candidate = resolve_path(candidate, strict=True)
        if candidate.is_file():
            return candidate
        raise FileNotFoundError(f'executable not found: {candidate}')
    search = (environment if environment is not None else os.environ).get('PATH', os.defpath)
    for entry in search.split(os.pathsep):
        if not entry or not Path(entry).is_absolute():
            continue
        # Passing a directory-qualified name prevents Windows which() from
        # implicitly prepending the current directory to the supplied search path.
        found = shutil.which(str(Path(entry) / program), path=entry)
        if found is not None:
            candidate = resolve_path(found, strict=True)
            if candidate.is_file():
                return candidate
    raise FileNotFoundError(f'executable not found on PATH: {program}')


def _output_identity(path):
    """Return the resolved destination spelling after portable name validation."""
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
    if os.name == 'nt':
        # Resolve existing destination short names as well as parent aliases.
        # Refuse symlinks before resolving: they cannot acquire a new identity.
        if path.is_symlink():
            raise OSError(errno.ELOOP, 'output must not be a symlink', str(path))
        canonical = resolve_path(parent / path.name)
        validate_components(canonical)
        # Deleting an 8.3 leaf alias removes its mapping to the long name.
        # Recreating that spelling could then publish under a different lock.
        # Refuse before mutation rather than rewrite executable-specific argv.
        if canonical.name.casefold() != path.name.casefold():
            raise OSError(errno.EINVAL, 'output leaf alias must use its long name', str(path))
        parent, path = canonical.parent, canonical
    identity = parent / path.name
    if identity.name.casefold().endswith(OUTPUT_LOCK_SUFFIX):
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
def _output_locks(paths, *, normcase=None):
    """Lock comparison keys using the platform's injectable case normalization."""
    # Refuse destination symlinks and short-name leaf aliases: replacing an
    # output must not change its lock identity. Validate before write-capable opens.
    # Keep resolved spelling separate from comparison and sidecar keys. Windows
    # normcase folds aliases; POSIX normcase preserves distinct case-sensitive names.
    normcase = os.path.normcase if normcase is None else normcase
    paths = [(Path(path), normcase(str(_output_identity(Path(path))))) for path in paths]
    keys = [key for path, key in paths]
    if len(set(keys)) != len(keys):
        raise OSError(errno.EINVAL, 'output paths must be distinct')
    paths.sort(key=lambda item: item[1])
    for path, key in paths:
        _sidecar_stat(Path(key + OUTPUT_LOCK_SUFFIX))
    for path, key in paths:
        _check_output(path)
    locks = []
    completed = False
    try:
        for path, key in paths:
            lock = Path(key + OUTPUT_LOCK_SUFFIX)
            handle = _open_sidecar(lock)
            locks.append((handle, False))
            _lock_handle(handle, lock)
            locks[-1] = (handle, True)
            current = _sidecar_stat(lock)
            if current is None or not _same_file(os.fstat(handle.fileno()), current):
                raise OSError(errno.EINVAL, 'lock sidecar changed during acquisition', str(lock))
        for path, key in paths:
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

    The executable is resolved once and its identity and digest are verified
    before and after execution. Resolution and lock refusals leave outputs unchanged.
    Spawn and timeout failures attempt receipt publication before re-raising the
    original exception. An active OS lock refuses the invocation; leftover lock
    files are reusable after a crash. Symlink, short-name leaf alias and overlapping
    outputs are refused; the lock-sidecar suffix is reserved globally. Locks cover
    all outputs through receipt publication.
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
        with verified_binary(receipt), log.open('wb') as output:
            started = time.perf_counter()
            try:
                result = subprocess.run(command, cwd=cwd, env=environment,
                                        stdout=output, stderr=subprocess.STDOUT,
                                        timeout=timeout_seconds, check=False)
            finally:
                receipt['elapsed_seconds'] = time.perf_counter() - started
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

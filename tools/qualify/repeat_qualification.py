#!/usr/bin/env python3
"""Run a gtest executable repeatedly and write a qualification receipt.

Each iteration runs in its own process group (session on POSIX) with output captured
to a per-iteration log. An iteration that exceeds the timeout is killed together with
its whole process tree, and cleanup is bounded. Every invocation writes into a fresh run
directory, so earlier evidence is never overwritten.

An iteration passes only when the process exits with status 0 and the gtest JSON report
shows exactly the expected number of executed tests with no failures and no skips. Any
other outcome counts as a failure. The receipt is rewritten atomically after every
iteration, and the script exits non-zero when any iteration failed.

Crash capture: the child receives LUMINUMBRA_CRASH_DIR pointing at its iteration's crash
directory. On POSIX the child's RLIMIT_CORE soft limit is raised to its hard limit (the hard limit
is never changed), and new core files under the directory the kernel writes to, resolved
against the child's working directory, and whose names match core_pattern, are copied into the
iteration. A missing core directory is created only when it lies under the child's working
directory, and it must be writable. A piped core_pattern or
an undeterminable core directory is a preflight failure. On Windows the executable must have
a WER LocalDumps DumpFolder that matches --wer-dump-folder, and new dumps are copied into the
iteration. Without that setup the run fails closed with status PREFLIGHT_FAILED, unless
--allow-no-dumps is given, which is recorded in the receipt.

Invocations sharing a core or WER directory serialize the snapshot, child execution,
dump settling and copying under an OS file lock. The persistent lock file is excluded
from artifact collection; closing the lock handle releases ownership even after a crash.
"""

from __future__ import annotations

import argparse
import errno
import functools
import hashlib
import json
import math
import os
import platform
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
from collections import Counter, deque
from contextlib import ExitStack, contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, BinaryIO, Callable, ContextManager, Iterator, NamedTuple

try:
    import resource as _resource_module
except ImportError:  # Windows
    _resource_module = None  # type: ignore[assignment]
resource: Any = _resource_module

SCHEMA = "luminumbra.repeat_qualification.v1"
LABEL = "process repetitions"
DEFAULT_GTEST_FILTER = (
    "WorldOpenRefusal.CurrentServerBootKeepsLoadedAuthorityAndSkipsWaterSettle"
)
DEFAULT_TIMEOUT_SECONDS = 120.0
CLEANUP_TIMEOUT_SECONDS = 5.0
DUMP_SETTLE_SECONDS = 30.0
DUMP_STABLE_SECONDS = 1.0
SNAPSHOT_FILE_READ_BYTES = 64 * 1024
SNAPSHOT_TOTAL_READ_BYTES = 1024 * 1024
COPY_CHUNK_BYTES = 1024 * 1024  # Bound every transient copy and verification buffer to 1 MiB.
REGISTRY_QUERY_TIMEOUT_SECONDS = 30.0
PREFLIGHT_EXIT_CODE = 3
CRASH_DIR_ENV = "LUMINUMBRA_CRASH_DIR"
DUMP_LOCK_NAME = ".luminumbra-qualification-dumps.lock"
DUMP_LOCK_TIMEOUT_SECONDS = 300.0
IS_WINDOWS = os.name == "nt"
SCRUBBED_GTEST_ENV = (
    "GTEST_SHARD_INDEX",
    "GTEST_TOTAL_SHARDS",
    "GTEST_REPEAT",
    "GTEST_FILTER",
    "GTEST_ALSO_RUN_DISABLED_TESTS",
)
RESERVED_GTEST_FLAGS = (
    "--gtest_filter",
    "--gtest_output",
    "--gtest_catch_exceptions",
    "--gtest_repeat",
    "--gtest_also_run_disabled_tests",
)
WER_KEY_ROOT = r"HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps"

DUMP_EPILOG = r"""Windows crash dumps: unless --allow-no-dumps is given, --wer-dump-folder must equal the
LocalDumps DumpFolder configured for this executable (or for all executables). Configure it
once from an elevated prompt, replacing <exe name> with the executable file name and
<dump folder> with the folder passed to --wer-dump-folder:

  reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe name>" /v DumpFolder /t REG_EXPAND_SZ /d "<dump folder>" /f
  reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe name>" /v DumpType /t REG_DWORD /d 2 /f
"""


def positive_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"not an integer: {text!r}") from exc
    if value < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return value


def positive_finite_float(text: str) -> float:
    try:
        value = float(text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"not a number: {text!r}") from exc
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("must be a positive, finite number")
    return value


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Repeat a gtest executable and write a qualification receipt. "
        "Arguments after '--' are passed to the executable.",
        epilog=DUMP_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--exe", required=True, type=Path, help="executable to run")
    parser.add_argument(
        "--gtest-filter",
        default=DEFAULT_GTEST_FILTER,
        help="value for --gtest_filter (default: %(default)s)",
    )
    parser.add_argument("--cwd", type=Path, default=None, help="working directory for each run")
    parser.add_argument("--count", type=positive_int, required=True, help="number of iterations (>= 1)")
    parser.add_argument(
        "--timeout-seconds",
        type=positive_finite_float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="per-iteration timeout, positive and finite (default: %(default)s)",
    )
    parser.add_argument(
        "--expected-tests",
        type=positive_int,
        default=1,
        help="number of tests each iteration must execute (default: %(default)s)",
    )
    parser.add_argument("--receipt", required=True, type=Path, help="receipt JSON path")
    parser.add_argument(
        "--wer-dump-folder",
        type=Path,
        default=None,
        help="Windows: LocalDumps DumpFolder that must be configured for the executable",
    )
    parser.add_argument(
        "--allow-no-dumps",
        action="store_true",
        help="Windows: run even when no matching LocalDumps configuration exists (recorded)",
    )
    return parser.parse_args(argv)


def split_extra_args(argv: list[str]) -> tuple[list[str], list[str]]:
    if "--" in argv:
        index = argv.index("--")
        return argv[:index], argv[index + 1 :]
    return argv, []


def reserved_extra_flag(extra: list[str]) -> str | None:
    for argument in extra:
        if argument.startswith(RESERVED_GTEST_FLAGS):
            return argument
    return None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def stage_executable(exe: Path, run_dir: Path, expected_sha256: str, cleanup: ExitStack) -> Path:
    """Own a private copy for the whole run; never launch the mutable source path."""
    directory = Path(cleanup.enter_context(tempfile.TemporaryDirectory(
        prefix="executable-", dir=run_dir,
    )))
    snapshot = directory / exe.name
    shutil.copy2(exe, snapshot)
    problem = executable_identity_problem(snapshot, expected_sha256, "snapshot staging")
    if problem is not None:
        raise OSError(problem)
    # Windows searches the executable directory before cwd/PATH; POSIX packages
    # may use $ORIGIN/@loader_path. Keep adjacent runtime libraries beside the
    # copy. The exe basename also preserves per-executable WER settings.
    for sibling in exe.parent.iterdir():
        library = (sibling.suffix.lower() == ".dll" if IS_WINDOWS else
                   sibling.name.endswith((".so", ".dylib")) or ".so." in sibling.name)
        if sibling != exe and library and sibling.is_file():
            shutil.copy2(sibling, directory / sibling.name)
    return snapshot


def make_run_directory(parent: Path, stamp: str, pid: int) -> Path:
    """Create run-<stamp>-<pid> under parent; refuses (FileExistsError) if it already exists."""
    run_dir = parent / f"run-{stamp}-{pid}"
    run_dir.mkdir(parents=True, exist_ok=False)
    return run_dir


class FileStamp(NamedTuple):
    size: int
    mtime_ns: int
    ctime_ns: int
    identity: tuple[int, int]
    # At most two range digests per file; raw sample bytes are never retained.
    fingerprints: tuple[tuple[int, int, str], ...]
    # Budget exhaustion is valid empty coverage; a failed read is not.
    read_complete: bool = True


class DumpWriterBusyError(OSError):
    """An incompatible open handle requires another settle attempt."""


def check_settle_deadline(deadline: float | None, clock: Callable[[], float]) -> None:
    if deadline is not None and clock() >= deadline:
        raise TimeoutError("dump settle deadline expired")


def snapshot_files(
    root: Path | None,
    recursive: bool,
    skip: Callable[[Path], bool] | None = None,
    errors: list[str] | None = None,
    *,
    baseline: dict[str, FileStamp] | None = None,
    deadline: float | None = None,
    clock: Callable[[], float] | None = None,
    writer_probe: Callable[[Path], bool] | None = None,
) -> dict[str, FileStamp]:
    """Record metadata and bounded range hashes; never omit an oversized file.

    Preserve baseline coverage for files with unchanged size and identity, even
    when they have no samples. Read those ranges first; their total already fits
    SNAPSHOT_TOTAL_READ_BYTES. Allocate the remainder in sorted path order.
    Deadline expiry always propagates, even when inspection errors are collected.
    When given, writer_probe must succeed for the inventory before any reads;
    busy handles are retryable, while probe errors and failed reads are fatal.
    """
    clock = clock or time.monotonic
    check_settle_deadline(deadline, clock)
    if root is None:
        return {}
    result: dict[str, FileStamp] = {}
    remaining = SNAPSHOT_TOTAL_READ_BYTES

    def inspection_error(path: Path, exc: OSError) -> None:
        check_settle_deadline(deadline, clock)
        if errors is None:
            raise exc
        errors.append(f"{path}: {type(exc).__name__}: {exc}")

    try:
        root_stat = root.stat()
    except OSError as exc:
        inspection_error(root, exc)
        return result
    check_settle_deadline(deadline, clock)
    if not stat.S_ISDIR(root_stat.st_mode):
        return result

    def traversal_error(exc: OSError) -> None:
        inspection_error(Path(exc.filename) if exc.filename else root, exc)

    check_settle_deadline(deadline, clock)

    # os.walk's onerror reports directory traversal failures that rglob may hide.
    paths = (
        (Path(directory) / name
         for directory, _directories, names in os.walk(root, onerror=traversal_error)
         for name in names)
        if recursive else root.iterdir()
    )
    try:
        for path in paths:
            check_settle_deadline(deadline, clock)
            if path.name == DUMP_LOCK_NAME:
                continue
            if skip is not None and skip(path):
                continue
            try:
                file_stat = path.stat()
            except OSError as exc:
                inspection_error(path, exc)
                continue
            check_settle_deadline(deadline, clock)
            if stat.S_ISREG(file_stat.st_mode):
                result[str(path)] = FileStamp(
                    file_stat.st_size, file_stat.st_mtime_ns, file_stat.st_ctime_ns,
                    (file_stat.st_dev, file_stat.st_ino), (),
                )
    except OSError as exc:
        if errors is None:
            raise
        traversal_error(exc)

    if writer_probe is not None:
        for source in result:
            check_settle_deadline(deadline, clock)
            try:
                closed = writer_probe(Path(source))
            except Exception as exc:
                # Platform imports and ctypes calls can also fail without an
                # OSError. Unknown completion must never become settled evidence.
                raise OSError(f"{source} writer probe: {type(exc).__name__}: {exc}") from exc
            check_settle_deadline(deadline, clock)
            if not closed:
                raise DumpWriterBusyError(f"{source}: dump writer still open")

    def pinned(path: str) -> bool:
        previous = baseline.get(path) if baseline is not None else None
        return (previous is not None and previous.size == result[path].size and
                previous.identity == result[path].identity)

    for source in sorted(result, key=lambda path: (not pinned(path), path)):
        check_settle_deadline(deadline, clock)
        stamp = result[source]
        if baseline is not None and pinned(source):
            ranges = [(offset, length) for offset, length, _digest in baseline[source].fingerprints]
        else:
            allowance = min(stamp.size, SNAPSHOT_FILE_READ_BYTES, remaining)
            prefix = (allowance + 1) // 2
            ranges = [(0, prefix)] if allowance else []
            if allowance > prefix:
                ranges.append((stamp.size - (allowance - prefix), allowance - prefix))
        fingerprints: list[tuple[int, int, str]] = []
        if ranges:
            # Reserve all ranges, including reused baseline samples, even if
            # opening or reading fails before any bytes can be fingerprinted.
            remaining -= sum(length for _offset, length in ranges)
            try:
                with Path(source).open("rb", buffering=0) as handle:
                    for offset, length in ranges:
                        check_settle_deadline(deadline, clock)
                        handle.seek(offset)
                        # Each transient chunk is at most SNAPSHOT_FILE_READ_BYTES.
                        chunk = handle.read(length)
                        check_settle_deadline(deadline, clock)
                        if len(chunk) != length:
                            raise OSError(errno.EIO, "short dump fingerprint read")
                        fingerprints.append((offset, length, hashlib.sha256(chunk).hexdigest()))
            except OSError as exc:
                inspection_error(Path(source), exc)
                fingerprints = []
                stamp = stamp._replace(read_complete=False)
        result[source] = stamp._replace(fingerprints=tuple(fingerprints))
    check_settle_deadline(deadline, clock)
    return result


def acquire_windows_dump_lock(handle: Any) -> None:
    """Lock byte zero (Windows permits locking beyond EOF), then initialize it."""
    import msvcrt

    fd = handle.fileno()
    deadline = time.monotonic() + DUMP_LOCK_TIMEOUT_SECONDS
    while True:
        locked = False
        try:
            handle.seek(0)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            locked = True
            if os.fstat(fd).st_size == 0:
                if handle.write(b"\0") != 1:
                    raise OSError(errno.EIO, "short dump lock initialization write")
            handle.seek(0)
            if handle.read(1) != b"\0":
                raise OSError(errno.EIO, "dump lock initialization verification failed")
            return
        except OSError as exc:
            if locked:
                handle.seek(0)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
            if not isinstance(exc, PermissionError) and exc.errno not in (
                    errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                raise
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(errno.ETIMEDOUT, "timed out acquiring shared dump lock") from exc
            time.sleep(min(0.1, remaining))


@contextmanager
def shared_dump_directory_locks(*folders: Path | None) -> Iterator[None]:
    """Hold advisory OS locks until all collection completes; never unlink their files.

    Canonical paths and a fixed acquisition order also handle directory aliases. The
    lock files must persist: unlinking one lets a contender lock a different inode.
    Use the host OS here rather than IS_WINDOWS, which tests override for WER coverage.
    """
    directories = sorted({folder.resolve() for folder in folders if folder is not None})
    with ExitStack() as handles:
        for directory in directories:
            fd = os.open(directory / DUMP_LOCK_NAME, os.O_RDWR | os.O_CREAT, 0o600)
            handle = handles.enter_context(os.fdopen(fd, "r+b", buffering=0))
            if os.name == "nt":
                acquire_windows_dump_lock(handle)
            else:
                import fcntl

                fcntl.flock(fd, fcntl.LOCK_EX)
        yield


def receipt_ownership_path(receipt: Path) -> Path:
    """Use the canonical parent, preserving the receipt name even if it is a symlink."""
    return receipt.parent.resolve() / f".{receipt.name}.owner"


def executable_identity_problem(exe: Path, expected_sha256: str, phase: str) -> str | None:
    try:
        actual = sha256_file(exe)
    except OSError as exc:
        return f"executable identity {phase}: {exe}: {type(exc).__name__}: {exc}"
    if actual != expected_sha256:
        return (f"executable identity {phase}: {exe}: SHA-256 mismatch "
                f"(expected {expected_sha256}, observed {actual})")
    return None


def harness_output_skip(run_dir: Path, receipt: Path) -> Callable[[Path], bool]:
    """Skip this harness's run directory, receipt, ownership file, and receipt temp files."""
    ownership = receipt_ownership_path(receipt)

    def skip(path: Path) -> bool:
        if path == receipt or path == ownership or path == run_dir or run_dir in path.parents:
            return True
        if path.name == ownership.name and path.parent.resolve() == ownership.parent:
            return True
        return (
            path.parent == receipt.parent
            and path.name.startswith(f".{receipt.name}.")
            and path.name.endswith(".tmp")
        )

    return skip


def core_name_regex(pattern: str) -> re.Pattern[str]:
    """Regex for the file names core_pattern can produce, matched against the pattern's basename.

    %p becomes digits, %% a literal percent sign, and every other % specifier one path-free run of
    characters. When the pattern has no %p, the kernel appends .<pid> whenever core_uses_pid is set,
    so an optional .<digits> suffix is accepted; a pattern containing %p already carries the pid.
    """
    name = pattern.rsplit("/", 1)[-1]
    parts: list[str] = []
    has_pid = False
    index = 0
    while index < len(name):
        char = name[index]
        if char == "%" and index + 1 < len(name):
            specifier = name[index + 1]
            index += 2
            if specifier == "%":
                parts.append(re.escape("%"))
            elif specifier == "p":
                has_pid = True
                parts.append(r"\d+")
            else:
                parts.append(r"[^/]+")
            continue
        parts.append(re.escape(char))
        index += 1
    if not has_pid:
        parts.append(r"(?:\.\d+)?")
    return re.compile("".join(parts))


def core_collection_skip(
    run_dir: Path, receipt: Path, pattern: str
) -> Callable[[Path], bool]:
    """Skip harness output, and any file whose name core_pattern cannot produce."""
    harness_skip = harness_output_skip(run_dir, receipt)
    names = core_name_regex(pattern)

    def skip(path: Path) -> bool:
        return harness_skip(path) or names.fullmatch(path.name) is None

    return skip


def file_rewritten(before: FileStamp, after: FileStamp) -> bool:
    if not before.read_complete or not after.read_complete:
        return False
    if before[:4] != after[:4]:
        return True
    # Only equal byte-range coverage can establish a content change. Two empty
    # samples rely on metadata; the settle writer probe covers deferred-timestamp
    # rewrites when there is no sampling budget.
    before_ranges = [(offset, length) for offset, length, _digest in before.fingerprints]
    after_ranges = [(offset, length) for offset, length, _digest in after.fingerprints]
    return before_ranges == after_ranges and before.fingerprints != after.fingerprints


def changed_files(before: dict[str, FileStamp], after: dict[str, FileStamp]) -> list[str]:
    return sorted(path for path, stamp in after.items()
                  if stamp.read_complete and
                  (path not in before or file_rewritten(before[path], stamp)))


@contextmanager
def open_dump_for_copy(path: Path) -> Iterator[BinaryIO]:
    """Select the platform source transport for the verified copier."""
    if os.name == "nt":
        with windows_dump_for_copy(path) as reader:
            yield reader
    else:
        with path.open("rb", buffering=0) as reader:
            yield reader


@contextmanager
def windows_dump_for_copy(path: Path) -> Iterator[BinaryIO]:
    """On Windows, retain a read-only share-mode handle through copy and validation.

    The CRT descriptor owns the original Win32 handle, so reads (including the
    verification pass) never reopen a path or bypass mandatory byte-range locks.
    """
    try:
        import msvcrt

        kernel32, handle = open_windows_dump_handle(path)
        try:
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        except BaseException:
            kernel32.CloseHandle(handle)
            raise
        try:
            reader = os.fdopen(descriptor, "rb", buffering=0)
        except BaseException:
            os.close(descriptor)
            raise
    except OSError:
        raise
    except Exception as exc:
        raise OSError(f"{path} copy open: {type(exc).__name__}: {exc}") from exc
    with reader:
        yield reader


def copy_stable_file(
    source: Path,
    target: Path,
    expected: FileStamp,
    *,
    opener: Callable[[Path], ContextManager[BinaryIO]],
    deadline: float,
    clock: Callable[[], float],
) -> None:
    """Publish only bytes matching a full post-copy source read and metadata.

    An opener must retain the same readable file through both passes. Windows'
    default opener additionally prevents writers from reopening during them.
    A temporary destination is closed before publication or removal on all hosts.
    """
    def metadata(info: os.stat_result) -> tuple[int, int, tuple[int, int]]:
        # These fields agree between path stat and handle fstat on every host.
        # Windows stat uses creation time for ctime, while fstat uses change time.
        return info.st_size, info.st_mtime_ns, (info.st_dev, info.st_ino)

    temporary_path: Path | None = None
    try:
        check_settle_deadline(deadline, clock)
        with opener(source) as reader:
            initial = os.fstat(reader.fileno())
            if metadata(initial) != (expected.size, expected.mtime_ns, expected.identity):
                raise DumpWriterBusyError(f"{source}: dump changed before copy")
            # Validate the final snapshot's samples using the retained handle.
            for offset, length, digest in expected.fingerprints:
                check_settle_deadline(deadline, clock)
                reader.seek(offset)
                if hashlib.sha256(reader.read(length)).hexdigest() != digest:
                    raise DumpWriterBusyError(f"{source}: dump changed before copy")
            reader.seek(0)
            copied_digest = hashlib.sha256()
            with tempfile.NamedTemporaryFile(dir=target.parent, prefix=f".{target.name}.",
                                             suffix=".tmp", delete=False) as temporary:
                temporary_path = Path(temporary.name)
                remaining = initial.st_size
                while remaining:
                    check_settle_deadline(deadline, clock)
                    chunk = reader.read(min(remaining, COPY_CHUNK_BYTES))
                    if not chunk:
                        raise DumpWriterBusyError(f"{source}: dump shortened during copy")
                    if temporary.write(chunk) != len(chunk):
                        raise OSError(errno.EIO, "short dump copy write")
                    copied_digest.update(chunk)
                    remaining -= len(chunk)
                if reader.read(1):
                    raise DumpWriterBusyError(f"{source}: dump grew during copy")

            # Hash the source again after the copy, through the same handle. A
            # path stat also detects replacement of the name on POSIX. Bound
            # this pass to the copied size so a growing file cannot extend it.
            post_copy = source.stat()
            reader.seek(0)
            source_digest = hashlib.sha256()
            remaining = initial.st_size
            while remaining:
                check_settle_deadline(deadline, clock)
                chunk = reader.read(min(remaining, COPY_CHUNK_BYTES))
                if not chunk:
                    raise DumpWriterBusyError(f"{source}: dump shortened after copy")
                source_digest.update(chunk)
                remaining -= len(chunk)
            final = os.fstat(reader.fileno())
            if (reader.read(1) or
                    any(metadata(info) != metadata(initial)
                        for info in (post_copy, final, source.stat())) or
                    source_digest.digest() != copied_digest.digest()):
                raise DumpWriterBusyError(f"{source}: dump changed during copy")
            check_settle_deadline(deadline, clock)
            os.utime(temporary_path, ns=(initial.st_atime_ns, initial.st_mtime_ns))
            os.replace(temporary_path, target)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def copy_changed_files(
    root: Path | None,
    before: dict[str, FileStamp],
    destination: Path,
    recursive: bool = False,
    skip: Callable[[Path], bool] | None = None,
    errors: list[str] | None = None,
    *,
    after: dict[str, FileStamp] | None = None,
    opener: Callable[[Path], ContextManager[BinaryIO]] | None = None,
    deadline: float | None = None,
    clock: Callable[[], float] | None = None,
    pause: Callable[[float], None] | None = None,
) -> list[str]:
    """Copy new or rewritten files under root, keeping their path relative to root.

    A file that cannot be copied, or a directory that cannot be listed, is described in errors when a
    list is given, so one unreadable entry does not discard the rest of the iteration. Without a list
    the error propagates. Changed sources and incompatible writer handles restart
    settling within the original deadline; an unverified copy is never published.
    opener is the platform source opener, injectable for share-mode transports.
    """
    clock = clock or time.monotonic
    deadline = deadline if deadline is not None else clock() + DUMP_SETTLE_SECONDS
    opener = opener or open_dump_for_copy
    copies: list[str] = []
    try:
        after = after if after is not None else snapshot_files(
            root, recursive, skip, errors, baseline=before, deadline=deadline, clock=clock,
        )
        pending = deque(changed_files(before, after))
    except OSError as exc:
        if errors is None:
            raise
        errors.append(f"{root} listing: {type(exc).__name__}: {exc}")
        return copies
    completed: set[str] = set()
    while pending:
        source = pending.popleft()
        relative = Path(source).relative_to(root) if root is not None else Path(source).name
        target = destination / relative
        try:
            target.parent.mkdir(parents=True, exist_ok=True)
            copy_stable_file(Path(source), target, after[source], opener=opener,
                             deadline=deadline, clock=clock)
        except DumpWriterBusyError:
            try:
                # Keep probe-before-read ordering, including baseline entries,
                # and require a fresh full stable window after a copy race.
                assert root is not None
                after = wait_for_dump_settle(
                    root, before, max(0.0, deadline - clock()), errors, skip,
                    clock=clock, pause=pause, recursive=recursive,
                )
            except OSError as exc:
                if errors is None:
                    raise
                errors.append(f"{source} copy settle: {type(exc).__name__}: {exc}")
                return copies
            pending = deque(path for path in changed_files(before, after) if path not in completed)
            continue
        except OSError as exc:
            if errors is None:
                raise
            errors.append(f"{source}: {type(exc).__name__}: {exc}")
            completed.add(source)
            continue
        copies.append(str(target))
        completed.add(source)
    return copies


def signal_name(signum: int) -> str:
    try:
        return signal.Signals(signum).name
    except ValueError:
        return f"SIG{signum}"


def core_limit_target(current: tuple[int, int]) -> tuple[int, int]:
    """Limits the child runs with: soft raised to the hard limit, the highest value it may take.

    The hard limit is left unchanged. A finite hard limit caps the soft limit, an infinite hard
    limit yields an unlimited soft limit, and a hard limit of zero leaves the child with zero.
    """
    _soft, hard = current
    return hard, hard


def apply_core_limit(target: tuple[int, int]) -> None:
    """Runs in the child before exec. An error here surfaces in the parent as a launch error."""
    resource.setrlimit(resource.RLIMIT_CORE, target)


def limit_text(value: int) -> int | str:
    if resource is not None and value == resource.RLIM_INFINITY:
        return "unlimited"
    return value


def read_core_pattern() -> str | None:
    pattern_file = Path("/proc/sys/kernel/core_pattern")
    return pattern_file.read_text(encoding="utf-8").strip() if pattern_file.is_file() else None


def core_directory_for_pattern(pattern: str | None, cwd: Path) -> str | None:
    """Directory the kernel writes cores into, or None when it cannot be determined.

    The kernel resolves a relative core_pattern against the crashing process's working
    directory, so a relative parent is resolved against the child's cwd. Piped patterns and
    directories still holding % specifiers are reported as None.
    """
    if not pattern or pattern.startswith("|"):
        return None
    parent = Path(pattern).parent
    if "%" in str(parent):
        return None
    directory = parent if parent.is_absolute() else cwd / parent
    return os.path.normpath(str(directory))


def posix_crash_environment(
    cwd: Path,
    pattern: str | None,
    current: tuple[int, int],
    child: tuple[int, int],
) -> dict[str, Any]:
    return {
        "core_pattern": pattern,
        "core_pattern_piped": bool(pattern and pattern.startswith("|")),
        "core_directory": core_directory_for_pattern(pattern, cwd),
        "rlimit_core_parent": {"soft": limit_text(current[0]), "hard": limit_text(current[1])},
        "rlimit_core_child": {"soft": limit_text(child[0]), "hard": limit_text(child[1])},
    }


def posix_preflight_problems(posix: dict[str, Any], child_soft: int) -> list[str]:
    """Reasons the POSIX run cannot collect crash evidence; empty when it can."""
    problems: list[str] = []
    pattern = posix["core_pattern"]
    if pattern is None:
        problems.append("core_pattern could not be read from /proc/sys/kernel/core_pattern")
    elif posix["core_pattern_piped"]:
        problems.append(f"core_pattern {pattern!r} pipes cores to a handler the harness cannot collect from")
    elif posix["core_directory"] is None:
        problems.append(f"the core directory for core_pattern {pattern!r} cannot be determined")
    if child_soft == 0:
        problems.append("the child's effective core soft limit is 0 because the RLIMIT_CORE hard limit is 0")
    return problems


def core_directory_preflight(
    directory: Path,
    cwd: Path,
    access: Callable[[str, int], bool] = os.access,
) -> dict[str, Any]:
    """Make sure the kernel's core directory exists and is writable before any child runs.

    A missing directory is created only when it lies under the child's working directory, which
    the harness controls. Anywhere else a missing directory is reported as a problem.
    """
    result: dict[str, Any] = {"path": str(directory), "created": False, "writable": False, "problem": None}
    if not directory.exists():
        try:
            directory.relative_to(cwd)
        except ValueError:
            result["problem"] = (
                f"core directory {directory} does not exist and is outside the child working directory {cwd}"
            )
            return result
        try:
            os.makedirs(directory, exist_ok=True)
        except OSError as exc:
            result["problem"] = f"core directory {directory} could not be created: {type(exc).__name__}: {exc}"
            return result
        result["created"] = True
    if not directory.is_dir():
        result["problem"] = f"core directory {directory} is not a directory"
        return result
    if not access(str(directory), os.W_OK | os.X_OK):
        result["problem"] = f"core directory {directory} is not writable"
        return result
    result["writable"] = True
    return result


def parse_reg_values(output: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        match = re.match(r"\s*(DumpFolder|DumpType)\s+REG_\w+\s+(.*?)\s*$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def query_registry_key(key: str) -> tuple[int, str]:
    """Return (exit code, stdout) of `reg query key`. Failure to run counts as not found."""
    try:
        result = subprocess.run(
            ["reg", "query", key],
            capture_output=True,
            text=True,
            check=False,
            timeout=REGISTRY_QUERY_TIMEOUT_SECONDS,
        )
    except (OSError, subprocess.TimeoutExpired):
        return 1, ""
    return result.returncode, result.stdout


def same_directory(first: str, second: str) -> bool:
    return os.path.normcase(os.path.normpath(first)) == os.path.normcase(os.path.normpath(second))


WER_DEFAULTS = {"DumpFolder": "", "DumpType": "1"}
VALID_WER_DUMP_TYPES = (0, 1, 2)


def parse_dump_type(text: str) -> int | None:
    """Parse a REG_DWORD as reg.exe prints it ("0x2" or decimal). None when it is not a number."""
    try:
        if text.lower().startswith("0x"):
            return int(text, 16)
        return int(text, 10)
    except ValueError:
        return None


def evaluate_wer_preflight(
    exe_name: str,
    expected_folder: Path,
    query: Callable[[str], tuple[int, str]] = query_registry_key,
) -> dict[str, Any]:
    """Merge defaults, the global LocalDumps values and the executable's values, then validate.

    The application key overrides each global value individually, so an application key that
    sets only DumpType still takes DumpFolder from the global key.
    """
    layers: list[tuple[str, dict[str, str]]] = []
    for key in (f"{WER_KEY_ROOT}\\{exe_name}", WER_KEY_ROOT):
        returncode, output = query(key)
        if returncode == 0:
            layers.append((key, parse_reg_values(output)))

    effective = dict(WER_DEFAULTS)
    sources: dict[str, str] = {name: "default" for name in WER_DEFAULTS}
    for name in WER_DEFAULTS:
        for key, values in layers:
            if name in values:
                effective[name] = values[name]
                sources[name] = key
                break

    folder = os.path.expandvars(effective["DumpFolder"])
    dump_type_text = effective["DumpType"]
    dump_type = parse_dump_type(dump_type_text)
    problems: list[str] = []
    if not folder:
        if sources["DumpFolder"] == "default":
            problems.append(f"no LocalDumps DumpFolder under {WER_KEY_ROOT} for {exe_name} or globally")
        else:
            problems.append("effective DumpFolder is empty")
    if dump_type not in VALID_WER_DUMP_TYPES:
        problems.append(f"effective DumpType {dump_type_text!r} is not one of 0, 1, 2")

    base = {
        "key": None if sources["DumpFolder"] == "default" else sources["DumpFolder"],
        "sources": sources,
        "dump_folder": folder or None,
        "dump_type": dump_type_text,
        "dump_type_value": dump_type,
    }
    if problems:
        return {**base, "configured": False, "reason": "; ".join(problems)}
    configured = same_directory(folder, str(expected_folder))
    reason = None
    if not configured:
        reason = f"DumpFolder {folder!r} does not match --wer-dump-folder {str(expected_folder)!r}"
    return {**base, "configured": configured, "reason": reason}


def windows_dump_preflight(
    exe: Path,
    dump_folder: Path | None,
    query: Callable[[str], tuple[int, str]] = query_registry_key,
) -> dict[str, Any]:
    if dump_folder is None:
        return {
            "configured": False,
            "key": None,
            "dump_folder": None,
            "dump_type": None,
            "reason": "--wer-dump-folder was not given",
        }
    return evaluate_wer_preflight(exe.name, Path(os.path.abspath(dump_folder)), query)


def build_command(exe: Path, gtest_filter: str, extra: list[str], json_path: Path) -> list[str]:
    """The argument vector, as recorded in the receipt."""
    return [
        str(exe),
        f"--gtest_filter={gtest_filter}",
        f"--gtest_output=json:{json_path}",
        "--gtest_catch_exceptions=0",
        *extra,
    ]


CMD_SPECIAL_CHARACTERS = frozenset("&|<>^()%!,;=")


def quote_cmd_argument(argument: str) -> str:
    """Quote one argument for a line that cmd.exe parses under /s /c.

    list2cmdline leaves characters such as & | < > ^ unquoted, and cmd.exe would then split or
    redirect the command. An argument containing whitespace or any of those characters is wrapped
    in double quotes, which cmd.exe keeps literal. A percent sign, a double quote, or a trailing
    backslash cannot be escaped reliably in a batch invocation, so they are rejected.
    """
    if "%" in argument:
        raise ValueError(f"cannot pass a percent sign through cmd.exe reliably: {argument!r}")
    if '"' in argument:
        raise ValueError(f"cannot pass a double quote through cmd.exe reliably: {argument!r}")
    if argument and not any(ch.isspace() or ch in CMD_SPECIAL_CHARACTERS for ch in argument):
        return argument
    if argument.endswith("\\"):
        raise ValueError(f"a trailing backslash cannot be quoted reliably: {argument!r}")
    return f'"{argument}"'


def spawn_target(command: list[str]) -> str | list[str]:
    """What Popen receives. A batch launcher must run through cmd.exe as one command string.

    Passing a list would make subprocess quote the cmd.exe /c text a second time, and cmd.exe
    cannot parse that. With /s and one enclosing quote pair, cmd.exe strips exactly that pair.
    Raises ValueError when an argument cannot be passed through cmd.exe.
    """
    if IS_WINDOWS and Path(command[0]).suffix.lower() in (".cmd", ".bat"):
        line = " ".join(quote_cmd_argument(argument) for argument in command)
        return f'cmd.exe /d /s /c "{line}"'
    return command


def build_child_environment(base: dict[str, str], crash_dir: Path) -> dict[str, str]:
    env = {name: value for name, value in base.items() if name not in SCRUBBED_GTEST_ENV}
    env[CRASH_DIR_ENV] = str(crash_dir)
    return env


def terminate_process_tree(process: subprocess.Popen[Any]) -> list[str]:
    """Kill the process tree, then the direct child if needed. Returns cleanup errors."""
    errors: list[str] = []
    if IS_WINDOWS:
        try:
            result = subprocess.run(
                ["taskkill", "/T", "/F", "/PID", str(process.pid)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                timeout=CLEANUP_TIMEOUT_SECONDS,
            )
            if result.returncode != 0:
                errors.append(f"taskkill exited {result.returncode}")
        except (OSError, subprocess.TimeoutExpired) as exc:
            errors.append(f"taskkill failed: {exc}")
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        except OSError as exc:
            errors.append(f"killpg failed: {exc}")
    try:
        process.wait(timeout=CLEANUP_TIMEOUT_SECONDS)
        return errors
    except subprocess.TimeoutExpired:
        errors.append("process tree still running after kill; killing direct child")
    try:
        process.kill()
    except OSError as exc:
        errors.append(f"direct child kill failed: {exc}")
    try:
        process.wait(timeout=CLEANUP_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        errors.append("direct child was not reaped within the cleanup timeout")
    return errors


def run_iteration(
    command: list[str],
    cwd: Path | None,
    env: dict[str, str],
    log_path: Path,
    timeout: float,
    core_target: tuple[int, int] | None = None,
) -> dict[str, Any]:
    popen_options: dict[str, Any] = {}
    if IS_WINDOWS:
        popen_options["creationflags"] = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        popen_options["start_new_session"] = True
        if core_target is not None:
            popen_options["preexec_fn"] = functools.partial(apply_core_limit, core_target)

    started = time.monotonic()
    timed_out = False
    cleanup_errors: list[str] = []
    try:
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                spawn_target(command),
                cwd=cwd,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=subprocess.STDOUT,
                **popen_options,
            )
            try:
                returncode = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                cleanup_errors = terminate_process_tree(process)
                returncode = process.returncode
            except KeyboardInterrupt as exc:
                try:
                    errors = terminate_process_tree(process)
                except Exception as cleanup_exc:
                    errors = [f"{type(cleanup_exc).__name__}: {cleanup_exc}"]
                if errors:
                    original = str(exc)
                    detail = "process-tree cleanup errors: " + "; ".join(errors)
                    exc.args = (f"{original}; {detail}" if original else detail,)
                raise
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        return {
            "exit_code": None,
            "signal": None,
            "timed_out": False,
            "duration_seconds": round(time.monotonic() - started, 3),
            "status": "launch_error",
            "launch_error": f"{type(exc).__name__}: {exc}",
            "cleanup_errors": [],
        }
    duration = round(time.monotonic() - started, 3)

    if timed_out:
        return {
            "exit_code": None,
            "signal": None,
            "timed_out": True,
            "duration_seconds": duration,
            "status": "timeout",
            "launch_error": None,
            "cleanup_errors": cleanup_errors,
        }
    if not IS_WINDOWS and returncode < 0:
        name = signal_name(-returncode)
        return {
            "exit_code": None,
            "signal": name,
            "timed_out": False,
            "duration_seconds": duration,
            "status": f"signal:{name}",
            "launch_error": None,
            "cleanup_errors": [],
        }
    return {
        "exit_code": returncode,
        "signal": None,
        "timed_out": False,
        "duration_seconds": duration,
        "status": f"exit:{returncode}",
        "launch_error": None,
        "cleanup_errors": [],
    }


def gtest_report_problem(path: Path, expected: int) -> str | None:
    """Return why the gtest JSON does not show a clean run of `expected` tests, or None."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return "gtest JSON report was not written"
    except (OSError, ValueError) as exc:
        return f"gtest JSON report unreadable: {exc}"
    if not isinstance(data, dict):
        return "gtest JSON report is not an object"
    cases: list[dict[str, Any]] = []
    for suite in data.get("testsuites") or []:
        if not isinstance(suite, dict):
            continue
        for case in suite.get("testsuite") or []:
            if isinstance(case, dict):
                cases.append(case)
    tests = data.get("tests")
    failures = data.get("failures")
    errors = data.get("errors")
    skipped = sum(1 for case in cases if case.get("result") == "SKIPPED" or case.get("status") == "NOTRUN")
    if tests != expected or len(cases) != expected:
        return f"expected {expected} executed test(s), gtest reported tests={tests} with {len(cases)} entries"
    if failures != 0 or errors != 0:
        return f"gtest reported failures={failures} errors={errors}"
    if skipped:
        return f"{skipped} test(s) were skipped"
    return None


def write_receipt(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(dir=str(path.parent), prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(data, handle, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temp_name, path)
    except BaseException:
        try:
            os.unlink(temp_name)
        except OSError:
            pass
        raise


def failure_reason(result: dict[str, Any], problem: str | None) -> str | None:
    if result.get("executable_identity_error"):
        return result["executable_identity_error"]
    if problem is not None:
        return f"gtest: {problem}"
    if result["status"] == "exit:0":
        return None
    if result.get("launch_error"):
        return f"{result['status']}: {result['launch_error']}"
    return result["status"]


def run_indexed_iteration(
    index: int,
    run_dir: Path,
    exe: Path,
    expected_sha256: str,
    gtest_filter: str,
    extra: list[str],
    cwd: Path | None,
    timeout: float,
    expected_tests: int,
    dump_folder: Path | None,
    core_directory: Path | None,
    core_target: tuple[int, int] | None,
    core_skip: Callable[[Path], bool] | None,
    dump_skip: Callable[[Path], bool] | None,
) -> dict[str, Any]:
    # Acquire before the baseline snapshot and release only after all dumps are copied.
    # Distinct receipts can otherwise attribute another invocation's changed files.
    try:
        with shared_dump_directory_locks(core_directory, dump_folder):
            return run_locked_iteration(
                index, run_dir, exe, expected_sha256, gtest_filter, extra, cwd, timeout,
                expected_tests, dump_folder, core_directory, core_target, core_skip, dump_skip,
            )
    except OSError as exc:
        result = {
            "exit_code": None,
            "signal": None,
            "timed_out": False,
            "duration_seconds": 0.0,
            "status": "harness_error",
            "launch_error": f"shared dump directory lock: {type(exc).__name__}: {exc}",
            "cleanup_errors": [],
        }
        iter_dir = run_dir / f"iter-{index}"
        return build_entry(
            index, run_dir / f"iter-{index}.log", iter_dir / "gtest.json", iter_dir / "crash",
            [], result, None,
        )


def run_locked_iteration(
    index: int,
    run_dir: Path,
    exe: Path,
    expected_sha256: str,
    gtest_filter: str,
    extra: list[str],
    cwd: Path | None,
    timeout: float,
    expected_tests: int,
    dump_folder: Path | None,
    core_directory: Path | None,
    core_target: tuple[int, int] | None,
    core_skip: Callable[[Path], bool] | None,
    dump_skip: Callable[[Path], bool] | None,
) -> dict[str, Any]:
    iter_dir = run_dir / f"iter-{index}"
    log_path = run_dir / f"iter-{index}.log"
    crash_dir = iter_dir / "crash"
    gtest_json = iter_dir / "gtest.json"
    try:
        crash_dir.mkdir(parents=True, exist_ok=False)
    except OSError as exc:
        result = {
            "exit_code": None,
            "signal": None,
            "timed_out": False,
            "duration_seconds": 0.0,
            "status": "harness_error",
            "launch_error": f"{type(exc).__name__}: {exc}",
            "cleanup_errors": [],
        }
        return build_entry(index, log_path, gtest_json, crash_dir, [], result, None)

    collection_errors: list[str] = []
    dump_before = snapshot_files(dump_folder, recursive=False, skip=dump_skip, errors=collection_errors)
    core_before = snapshot_files(core_directory, recursive=True, skip=core_skip, errors=collection_errors)
    command = build_command(exe, gtest_filter, extra, gtest_json)
    env = build_child_environment(dict(os.environ), crash_dir)
    identity_error = executable_identity_problem(exe, expected_sha256, "before launch")
    if identity_error is not None:
        result = {
            "exit_code": None,
            "signal": None,
            "timed_out": False,
            "duration_seconds": 0.0,
            "status": "executable_identity_error",
            "executable_identity_error": identity_error,
            "launch_error": None,
            "cleanup_errors": [],
        }
        return build_entry(index, log_path, gtest_json, crash_dir, [], result, None, collection_errors)
    result = run_iteration(command, cwd, env, log_path, timeout, core_target)
    # Preserve the child's actual exit fields, including its exit classification.
    result["executable_identity_error"] = executable_identity_problem(exe, expected_sha256, "after child")

    problem: str | None = None
    if result["status"] == "exit:0":
        problem = gtest_report_problem(gtest_json, expected_tests)
        if problem is not None:
            result["status"] = "gtest_invalid"

    # Collection problems are recorded on the iteration, which keeps its classification.
    artifacts: list[str] = []
    try:
        artifacts = sorted(snapshot_files(crash_dir, recursive=True, errors=collection_errors))
    except OSError as exc:
        collection_errors.append(f"crash directory listing: {type(exc).__name__}: {exc}")
    dump_ready = True
    dump_deadline = time.monotonic() + DUMP_SETTLE_SECONDS
    settled: dict[str, FileStamp] | None = None
    if IS_WINDOWS and dump_folder is not None and result["status"] not in ("exit:0", "timeout"):
        try:
            settled = wait_for_dump_settle(
                dump_folder, dump_before, max(0.0, dump_deadline - time.monotonic()),
                collection_errors, dump_skip,
            )
        except OSError as exc:
            collection_errors.append(f"dump folder settle: {type(exc).__name__}: {exc}")
            dump_ready = False
    if dump_ready:
        artifacts += copy_changed_files(
            dump_folder, dump_before, iter_dir / "wer", skip=dump_skip,
            errors=collection_errors, after=settled, deadline=dump_deadline,
        )
    artifacts += copy_changed_files(
        core_directory, core_before, iter_dir / "core", recursive=True, skip=core_skip,
        errors=collection_errors,
    )
    return build_entry(
        index, log_path, gtest_json, crash_dir, sorted(artifacts), result, problem, collection_errors
    )


def open_windows_dump_handle(path: Path) -> tuple[Any, int]:
    """Open for reading with FILE_SHARE_READ only; the caller owns the handle.

    CreateFileW checks sharing in both directions: FILE_SHARE_READ permits an
    existing reader that shares our GENERIC_READ access, while omitting
    FILE_SHARE_WRITE rejects existing writers even if they permit read sharing.
    Readers that deny read sharing remain incompatible and must also be deferred.
    """
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    create_file = kernel32.CreateFileW
    create_file.argtypes = (
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    )
    create_file.restype = wintypes.HANDLE
    close_handle = kernel32.CloseHandle
    close_handle.argtypes = (wintypes.HANDLE,)
    close_handle.restype = wintypes.BOOL
    handle = create_file(str(path), 0x80000000, 0x1, None, 3, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        error = ctypes.get_last_error()
        if error in (32, 33):  # ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION
            raise DumpWriterBusyError(f"{path}: dump writer still open")
        raise ctypes.WinError(error)
    return kernel32, handle


def windows_dump_writer_closed(path: Path) -> bool:
    """Probe with the same sharing contract as the retained copy handle."""
    import ctypes

    try:
        kernel32, handle = open_windows_dump_handle(path)
    except DumpWriterBusyError:
        return False
    close_handle = kernel32.CloseHandle
    if not close_handle(handle):
        raise ctypes.WinError(ctypes.get_last_error())
    return True


def dump_writer_closed(path: Path) -> bool:
    """Probe WER handles on Windows, cooperative advisory writers on POSIX.

    A shared advisory probe allows cooperative readers and rejects exclusive
    writers. POSIX has no mandatory share-mode equivalent for arbitrary writers. WER
    collection uses Windows; the advisory fallback and injectable probe allow
    deterministic portable tests without claiming native WER qualification.
    """
    if os.name == "nt":
        return windows_dump_writer_closed(path)
    import fcntl

    with path.open("rb", buffering=0) as handle:
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_SH | fcntl.LOCK_NB)
        except OSError as exc:
            if exc.errno in (errno.EACCES, errno.EAGAIN):
                return False
            raise
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
    return True


def wait_for_dump_settle(
    folder: Path,
    before: dict[str, FileStamp],
    timeout: float,
    errors: list[str] | None = None,
    skip: Callable[[Path], bool] | None = None,
    *,
    probe: Callable[[Path], bool] | None = None,
    clock: Callable[[], float] | None = None,
    pause: Callable[[float], None] | None = None,
    recursive: bool = False,
) -> dict[str, FileStamp]:
    """Require closed writers and unchanged snapshots for the full stable window.

    Byte sampling alone cannot rule out a middle-only rewrite with deferred file
    timestamps. The writer probe is required on both ends of the stable window.
    Expiry during collection or probing must never be accepted as stability.
    """
    clock = clock or time.monotonic
    pause = pause or time.sleep
    probe = probe or dump_writer_closed
    deadline = clock() + timeout
    previous: dict[str, FileStamp] | None = None
    stable_since: float | None = None
    if any(not stamp.read_complete for stamp in before.values()):
        raise OSError("incomplete baseline dump fingerprint coverage")
    while True:
        check_settle_deadline(deadline, clock)
        try:
            # Probe even baseline files before reading: Windows may defer their
            # timestamps until close. Compatible readers do not block the probe.
            snapshot = snapshot_files(folder, recursive, skip, errors, baseline=before,
                                      deadline=deadline, clock=clock, writer_probe=probe)
        except DumpWriterBusyError:
            snapshot = {}
            closed = False
        else:
            if any(not stamp.read_complete for stamp in snapshot.values()):
                raise OSError("incomplete dump fingerprint coverage")
            closed = True
        current = {path: snapshot[path] for path in changed_files(before, snapshot)}
        closed = closed and bool(current)
        check_settle_deadline(deadline, clock)
        now = clock()
        if not closed:
            stable_since = None
        elif current != previous or stable_since is None:
            stable_since = now
        elif now - stable_since >= DUMP_STABLE_SECONDS:
            check_settle_deadline(deadline, clock)
            return snapshot
        previous = current
        pause(min(DUMP_STABLE_SECONDS, max(0.0, deadline - clock())))


def build_entry(
    index: int,
    log_path: Path,
    gtest_json: Path,
    crash_dir: Path,
    artifacts: list[str],
    result: dict[str, Any],
    problem: str | None,
    collection_errors: list[str] | None = None,
) -> dict[str, Any]:
    # Collection errors never turn a passing iteration into a failure, and never change a failure's reason.
    reason = failure_reason(result, problem)
    return {
        "index": index,
        "log_path": str(log_path),
        "gtest_json": str(gtest_json),
        "crash_dir": str(crash_dir),
        "crash_artifacts": artifacts,
        "collection_errors": list(collection_errors or []),
        "failed": reason is not None,
        "failure_reason": reason,
        **result,
    }


def run_qualification(
    args: argparse.Namespace,
    extra: list[str],
    query: Callable[[str], tuple[int, str]] = query_registry_key,
    core_limits: Callable[[], tuple[int, int]] | None = None,
    read_pattern: Callable[[], str | None] | None = None,
) -> int:
    exe = Path(os.path.abspath(args.exe))
    if not exe.is_file():
        print(f"error: executable not found: {exe}", file=sys.stderr)
        return 2
    reserved = reserved_extra_flag(extra)
    if reserved is not None:
        print(f"error: {reserved} is set by this harness; pass it through --gtest-filter instead", file=sys.stderr)
        return 2

    receipt_path = Path(os.path.abspath(args.receipt))
    receipt_path.parent.mkdir(parents=True, exist_ok=True)
    receipt_path = receipt_path.parent.resolve() / receipt_path.name
    ownership = receipt_ownership_path(receipt_path)
    try:
        ownership_fd = os.open(ownership, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except OSError as exc:
        print(f"error: cannot own receipt {receipt_path}: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    try:
        try:
            receipt_path.lstat()
        except FileNotFoundError:
            pass
        else:
            print(f"error: receipt already exists; refusing to overwrite: {receipt_path}", file=sys.stderr)
            return 2
        with ExitStack() as cleanup:
            return run_owned_qualification(
                args, extra, exe, receipt_path, query, core_limits, read_pattern, cleanup,
            )
    finally:
        try:
            os.close(ownership_fd)
        finally:
            ownership.unlink()


def run_owned_qualification(
    args: argparse.Namespace,
    extra: list[str],
    exe: Path,
    receipt_path: Path,
    query: Callable[[str], tuple[int, str]],
    core_limits: Callable[[], tuple[int, int]] | None,
    read_pattern: Callable[[], str | None] | None,
    cleanup: ExitStack,
) -> int:
    """Run only while this invocation holds exclusive receipt ownership."""
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    try:
        run_dir = make_run_directory(receipt_path.parent, stamp, os.getpid())
    except FileExistsError:
        print(f"error: run directory already exists; refusing to reuse it: {receipt_path.parent}", file=sys.stderr)
        return 2
    cwd = Path(os.path.abspath(args.cwd)) if args.cwd is not None else None
    # Staging changes only the executable path. Preserve --cwd (or the caller's
    # cwd), including package-relative assets and relative kernel core paths.
    base_cwd = cwd if cwd is not None else Path.cwd()

    environment: dict[str, Any] = {
        "scrubbed_gtest_env": [name for name in SCRUBBED_GTEST_ENV if name in os.environ],
        "allow_no_dumps": bool(args.allow_no_dumps),
    }
    dump_folder: Path | None = None
    core_directory: Path | None = None
    core_target: tuple[int, int] | None = None
    core_skip: Callable[[Path], bool] | None = None
    preflight_reason: str | None = None
    if IS_WINDOWS:
        preflight = windows_dump_preflight(exe, args.wer_dump_folder, query)
        environment["wer"] = preflight
        if preflight["configured"]:
            dump_folder = Path(os.path.abspath(args.wer_dump_folder))
            try:
                dump_folder.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                diagnostic = f"{dump_folder}: {type(exc).__name__}: {exc}"
                preflight["dump_folder_error"] = diagnostic
                preflight["configured"] = False
                preflight["reason"] = f"dump folder could not be created: {diagnostic}"
                dump_folder = None
                if not args.allow_no_dumps:
                    preflight_reason = preflight["reason"]
        elif not args.allow_no_dumps:
            preflight_reason = preflight["reason"]
    else:
        current = core_limits() if core_limits is not None else resource.getrlimit(resource.RLIMIT_CORE)
        core_target = core_limit_target(current)
        pattern = (read_pattern or read_core_pattern)()
        posix = posix_crash_environment(base_cwd, pattern, current, core_target)
        environment.update(posix)
        problems = posix_preflight_problems(posix, core_target[0])
        if posix["core_directory"] is not None and pattern is not None:
            directory_check = core_directory_preflight(Path(posix["core_directory"]), base_cwd)
            environment["core_directory_preflight"] = directory_check
            if directory_check["problem"] is not None:
                problems.append(directory_check["problem"])
        environment["core_problems"] = problems
        if problems and not args.allow_no_dumps:
            preflight_reason = "; ".join(problems) + (
                "; set `sysctl kernel.core_pattern=core.%p` or use --allow-no-dumps"
            )
        if posix["core_directory"] is not None and pattern is not None and not problems:
            core_directory = Path(posix["core_directory"])
            core_skip = core_collection_skip(run_dir, receipt_path, pattern)

    try:
        expected_sha256: str | None = sha256_file(exe)
    except OSError as exc:
        expected_sha256 = None
        preflight_reason = f"executable identity initial hash: {exe}: {type(exc).__name__}: {exc}"

    snapshot: Path | None = None
    if preflight_reason is None and expected_sha256 is not None:
        try:
            snapshot = stage_executable(exe, run_dir, expected_sha256, cleanup)
        except OSError as exc:
            preflight_reason = f"executable snapshot: {type(exc).__name__}: {exc}"

    receipt: dict[str, Any] = {
        "schema": SCHEMA,
        "label": LABEL,
        "command": build_command(snapshot or exe, args.gtest_filter, extra, Path("<iter-N>") / "gtest.json"),
        "exe": {"path": str(exe), "sha256": expected_sha256,
                "snapshot_path": str(snapshot) if snapshot is not None else None},
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "cwd": str(base_cwd),
        "run_dir": str(run_dir),
        "environment": environment,
        "count": args.count,
        "timeout_seconds": args.timeout_seconds,
        "expected_tests": args.expected_tests,
        "status": "running",
        "start_time": utc_now(),
        "end_time": None,
        "iterations": [],
        "exit_code_histogram": {},
        "failure_count": 0,
        "collection_error_count": 0,
        "first_failure": None,
    }
    if preflight_reason is not None:
        receipt["status"] = "PREFLIGHT_FAILED"
        receipt["failure_reason"] = preflight_reason
        receipt["end_time"] = utc_now()
        write_receipt(receipt_path, receipt)
        print(f"error: qualification preflight failed: {preflight_reason}", file=sys.stderr)
        print(f"receipt {receipt_path}", file=sys.stderr)
        return PREFLIGHT_EXIT_CODE

    write_receipt(receipt_path, receipt)
    assert snapshot is not None
    histogram: Counter[str] = Counter()
    failures = 0
    try:
        for index in range(1, args.count + 1):
            entry = run_indexed_iteration(
                index,
                run_dir,
                snapshot,
                receipt["exe"]["sha256"],
                args.gtest_filter,
                extra,
                cwd,
                args.timeout_seconds,
                args.expected_tests,
                dump_folder,
                core_directory,
                core_target,
                core_skip,
                harness_output_skip(run_dir, receipt_path),
            )
            receipt["iterations"].append(entry)
            histogram[entry["status"]] += 1
            receipt["exit_code_histogram"] = dict(sorted(histogram.items()))
            if entry["failed"]:
                failures += 1
                if receipt["first_failure"] is None:
                    receipt["first_failure"] = {
                        "index": index,
                        "log_path": entry["log_path"],
                        "crash_artifacts": entry["crash_artifacts"],
                        "failure_reason": entry["failure_reason"],
                    }
            receipt["failure_count"] = failures
            receipt["collection_error_count"] = sum(len(item["collection_errors"]) for item in receipt["iterations"])
            write_receipt(receipt_path, receipt)
            if entry.get("executable_identity_error"):
                receipt["failure_reason"] = entry["executable_identity_error"]
                break
        receipt["status"] = "failed" if failures else "passed"
    except BaseException as exc:
        receipt["status"] = "aborted"
        receipt["abort_reason"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        receipt["end_time"] = utc_now()
        write_receipt(receipt_path, receipt)

    summary = "failed" if failures else "passed"
    collection_note = ""
    if receipt["collection_error_count"]:
        collection_note = f"; {receipt['collection_error_count']} artifact collection error(s) recorded"
    print(
        f"{LABEL}: {summary} ({len(receipt['iterations']) - failures}/{args.count} iterations clean), "
        f"receipt {receipt_path}{collection_note}"
    )
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    raw = sys.argv[1:] if argv is None else argv
    own_args, extra = split_extra_args(raw)
    args = parse_args(own_args)
    return run_qualification(args, extra)


if __name__ == "__main__":
    sys.exit(main())

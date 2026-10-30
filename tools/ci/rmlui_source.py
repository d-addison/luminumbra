#!/usr/bin/env python3
"""Deliver the reviewed RmlUi source inside an exclusive, fresh build workspace.

The workspace excludes non-cooperating namespace writers. Interrupted or unknown
state requires a new workspace; this tool never adopts, resets or cleans it.
"""
from __future__ import annotations

import argparse
import configparser
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import signal
import stat
import subprocess
import sys
import threading
import time
import uuid

REVISION = "58c751531f4c1a1eb2dc56f409b8e8a323a0c9ad"
PATCH_SHA = "1b49aa929bc7411d7d26059135fd029341c0cc769c4b588388ad66a57fb3f196"
SOURCE_ID = "60304a1ae56d9a21c2e0db0853d02b5946bc4ac2030aa8f21ef84d9ae752f1de"
UPSTREAM = "https://github.com/mikke89/RmlUi.git"
POLICY = "fresh-exclusive-workspace-v1"
FILE_COUNT = 1118
SOURCE_BYTES = 21004671
# Bounds cover the complete pinned source, Git administration and command output.
STORAGE_BYTES = 128 * 1024 * 1024
STORAGE_ENTRIES = 8192
OUTPUT_BYTES = 4 * 1024 * 1024
HEX = re.compile(r"[0-9a-f]{64}\Z")
REPARSE = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)


class Refusal(ValueError):
    """Input or workspace does not satisfy the delivery contract."""


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise Refusal(reason)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ordinary(info: os.stat_result, directory: bool = False) -> bool:
    return (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)) and not (
        getattr(info, "st_file_attributes", 0) & REPARSE)


def read_file(path: Path, bound: int) -> bytes:
    require(ordinary(os.lstat(path)), f"not an ordinary file: {path.name}")
    # Nonblocking open prevents a substituted FIFO from blocking admission.
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0))
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        require(ordinary(info) and info.st_nlink == 1 and info.st_size <= bound,
                f"invalid file size or links: {path.name}")
        data = stream.read(bound + 1)
        require(len(data) == info.st_size and len(data) <= bound, f"partial file: {path.name}")
        return data


def safe_relative(name: str) -> None:
    require(isinstance(name, str) and bool(name) and "\\" not in name and ":" not in name,
            "invalid relative path")
    parts = name.split("/")
    require(all(p and p not in {".", ".."} and not p.endswith((" ", ".")) and "~" not in p
                and not re.fullmatch(r"(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?", p)
                for p in parts), "nonportable relative path")


def canonical(raw: str | Path) -> Path:
    text = str(raw)
    require(bool(text) and len(text) <= 32768, "empty or excessive workspace path")
    windows = PureWindowsPath(text)
    require(not windows.drive or windows.is_absolute(), "drive-relative path")
    require(not {".", ".."}.intersection(text.replace("\\", "/").split("/")), "parent or dot path alias")
    require(os.name == "nt" or (not windows.drive and "\\" not in text), "foreign platform path alias")
    path = Path(os.path.abspath(text))
    require(path == path.resolve(), "workspace path alias")
    for part in [path, *path.parents]:
        try:
            info = os.lstat(part)
        except FileNotFoundError:
            continue
        require(ordinary(info, True) or (part == path and ordinary(info)), "workspace symlink or reparse path")
    if os.name == "nt":
        existing = path
        while not existing.exists():
            existing = existing.parent
        check_windows_spelling(str(existing), windows_long_name(str(existing)))
    return path


def windows_long_name(path: str) -> str:
    import ctypes
    buffer = ctypes.create_unicode_buffer(32768)
    length = ctypes.windll.kernel32.GetLongPathNameW(path, buffer, len(buffer))
    require(0 < length < len(buffer), "unreadable Windows long path")
    return buffer.value


def check_windows_spelling(path: str, long_name: str) -> None:
    require(bool(long_name) and PureWindowsPath(path) == PureWindowsPath(long_name),
            "workspace short-name alias or unreadable long path")


def inventory(root: Path, ignore_git: bool = False, allowed_directories: set[str] | None = None,
              *, live_git: bool = False) -> dict[str, os.stat_result]:
    """Allow transient extra links only below .git during an owned command.

    All admission, verification and post-exit inventories keep sole-link checks.
    """
    result = {}
    pending = [root]
    entries = size = 0
    while pending:
        directory = pending.pop()
        require(ordinary(os.lstat(directory), True), "nonordinary source directory")
        with os.scandir(directory) as scan:
            for entry in scan:
                entries += 1
                require(entries <= STORAGE_ENTRIES, "source entry bound exceeded")
                name = Path(entry.path).relative_to(root).as_posix()
                safe_relative(name)
                info = os.lstat(entry.path)
                if ignore_git and name == ".git":
                    require(ordinary(info, True), "Git administration must be a local directory")
                    continue
                if ordinary(info, True):
                    require(allowed_directories is None or name in allowed_directories, "extra source directory")
                    pending.append(Path(entry.path))
                else:
                    # Git publishes objects by linking a temporary file, then unlinking it.
                    links_ok = (info.st_nlink == 1 or
                                (live_git and name.startswith(".git/") and info.st_nlink > 1))
                    require(ordinary(info) and links_ok, "nonordinary source entry")
                    size += info.st_size
                    require(size <= STORAGE_BYTES, "source storage bound exceeded")
                    result[name] = info
    return result


class Specification:
    def __init__(self, manifest: Path, patch: Path):
        self.manifest_bytes = read_file(canonical(manifest), 1024 * 1024)
        self.patch = canonical(patch)
        self.patch_bytes = read_file(self.patch, 22655)
        require(len(self.patch_bytes) == 22655 and digest(self.patch_bytes) == PATCH_SHA, "patch identity mismatch")
        self.data = json.loads(self.manifest_bytes)
        m = self.data
        require(m["schema"] == 1 and m["revision"] == REVISION and m["patch_sha256"] == PATCH_SHA
                and m["source_identity"] == SOURCE_ID and m["file_count"] == FILE_COUNT
                and m["pristine_bytes"] == SOURCE_BYTES, "manifest identity mismatch")
        require(digest(f"RmlUi-source-v1\n{REVISION}\n{PATCH_SHA}\n".encode()) == SOURCE_ID,
                "source identity mismatch")
        self.files = m["files"]
        require(len(self.files) == FILE_COUNT, "manifest file count mismatch")
        aliases = set()
        for name, entry in self.files.items():
            safe_relative(name)
            require(name.casefold() not in aliases, "manifest path alias")
            aliases.add(name.casefold())
            require(entry["mode"] == "100644" and re.fullmatch(r"[0-9a-f]{40}", entry["blob"])
                    and HEX.fullmatch(entry["pristine_sha256"]) and HEX.fullmatch(entry["patched_sha256"]),
                    "invalid manifest file identity")
            for key in ("size", "patched_size"):
                require(type(entry[key]) is int and 0 <= entry[key] <= SOURCE_BYTES, "invalid manifest size")
        require(sum(e["size"] for e in self.files.values()) == SOURCE_BYTES
                and sum(e["patched_size"] for e in self.files.values()) == m["patched_bytes"], "manifest byte total mismatch")
        self.delivery = dict(schema=1, policy=POLICY, revision=REVISION, source_identity=SOURCE_ID,
                             patch_sha256=PATCH_SHA, manifest_sha256=digest(self.manifest_bytes),
                             helper_sha256=digest(read_file(Path(__file__).resolve(), 128 * 1024)))

    def verify_files(self, source: Path, patched: bool) -> None:
        directories = {parent.as_posix() for name in self.files for parent in Path(name).parents if parent != Path(".")}
        actual = inventory(source, ignore_git=True, allowed_directories=directories)
        require(set(actual) == set(self.files), "extra or missing source files")
        for name, entry in self.files.items():
            size = entry["patched_size" if patched else "size"]
            require(actual[name].st_size == size, f"source size mismatch: {name}")
            if os.name != "nt":
                require(not actual[name].st_mode & 0o111, f"source mode mismatch: {name}")
            data = read_file(source / name, size)
            require(digest(data) == entry["patched_sha256" if patched else "pristine_sha256"],
                    f"source digest mismatch: {name}")


def git(source: Path, *args: str, input_bytes: bytes | None = None,
        cancellation: CommandCancellation | None = None) -> bytes:
    # Exclude inherited Git directories, filters, hooks, credential helpers and config.
    environment = {k.upper(): v for k, v in os.environ.items()
                   if k.upper() in {"PATH", "SYSTEMROOT", "COMSPEC", "PATHEXT", "TEMP", "TMP", "TMPDIR"}}
    # Transport libraries distinguish proxy variable spellings; preserve them.
    environment.update({k: v for k, v in os.environ.items()
                        if k.upper() in {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY"}})
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_CONFIG_SYSTEM=os.devnull, GIT_ATTR_NOSYSTEM="1",
                       GIT_NO_REPLACE_OBJECTS="1", GIT_OPTIONAL_LOCKS="0", GIT_TERMINAL_PROMPT="0")
    path_entries = environment.get("PATH", "").split(os.pathsep)
    require(all(entry and Path(entry).is_absolute() for entry in path_entries), "empty or relative executable search path")
    executable = shutil.which("git", path=environment.get("PATH", ""))
    require(executable is not None and Path(executable).is_absolute(), "Git executable unavailable")
    # Git 2.46+ ends fetch (and merge, commit, am) by spawning a detached
    # `git maintenance run --auto`; the grandchild holds the inherited output
    # pipe for a moment after the owned process exits, which the retained-output
    # check below refuses. Disabling auto maintenance and auto gc keeps every
    # descendant foreground, so the pipe closes exactly when Git exits.
    argv = [str(Path(executable).resolve()), "-c", "core.autocrlf=false", "-c", "core.safecrlf=false",
            "-c", "core.hooksPath=" + os.devnull, "-c", "core.fsmonitor=false",
            "-c", "protocol.file.allow=never", "-c", "maintenance.auto=false", "-c", "gc.auto=0",
            "-C", str(source), *args]
    return run_command(source, argv, environment, input_bytes, cancellation=cancellation)


def shutdown_refusal(primary_error: BaseException | None, shutdown_errors: list[BaseException]) -> Refusal:
    # Callers contribute at most ten failures from their exact owned resources.
    detail = "; ".join(f"{type(error).__name__}: {error}" for error in shutdown_errors)
    reason = (str(primary_error) + "\n" if primary_error is not None else "")
    return Refusal(reason + "Git shutdown refused: " + detail)


class CommandCancellation:
    """Main-thread signal ownership shared explicitly with command workers.

    Handlers record the first cancellation; commands retire their retained tree
    before this scope restores and forwards the original handler. Workers must
    join this scope before spawning. Failed handler restorations retain their
    original callbacks and pending cancellation for explicit recovery.
    """

    def __init__(self):
        self.condition = threading.Condition()
        self.active = 0
        self.open = False
        self.pending = None
        self.handlers = {}

    def __enter__(self):
        require(threading.current_thread() is threading.main_thread(),
                "Git cancellation requires a main-thread owner")
        require(not self.open and not self.handlers, "Git cancellation owner already entered")
        try:
            for signum in (signal.SIGINT, signal.SIGTERM):
                previous = signal.getsignal(signum)
                if previous == signal.SIG_IGN:
                    continue
                self.handlers[signum] = previous
                signal.signal(signum, self.record)
            self.open = True
            return self
        except BaseException as error:
            failures = self.restore_handlers()
            if failures:
                raise shutdown_refusal(error, failures) from error
            raise

    def restore_handlers(self):
        failures = []
        # At most two handlers. An error restoring either must not prevent the
        # other restoration; retain failed entries instead of losing custody.
        for signum, previous in tuple(self.handlers.items()):
            try:
                signal.signal(signum, previous)
            except BaseException as error:
                failures.append(error)
            else:
                del self.handlers[signum]
        return failures

    def record(self, signum, frame):
        # Two handlers, one retained signal/frame. Repeated signals cannot
        # interrupt spawn custody, tree shutdown, or handler restoration.
        if self.pending is None:
            self.pending = (signum, frame, self.handlers[signum])

    def check(self):
        require(self.open and self.pending is None, "Git command cancelled")

    @contextmanager
    def command(self):
        with self.condition:
            self.check()
            # Reuse the storage entry bound for outstanding registrations.
            require(self.active < STORAGE_ENTRIES, "Git command ownership bound exceeded")
            self.active += 1
        try:
            yield
        finally:
            with self.condition:
                self.active -= 1
                self.condition.notify_all()

    def __exit__(self, exc_type, exc_value, traceback):
        with self.condition:
            self.open = False  # Atomically close admission before waiting.
            # Each registered command retains its existing 120-second deadline
            # and bounded shutdown. Cancellation is checked before each scan/read.
            while self.active:
                self.condition.wait(0.01)
        failures = self.restore_handlers()
        if failures:
            raise shutdown_refusal(exc_value, failures) from (exc_value or failures[0])
        pending, self.pending = self.pending, None
        if pending is not None:
            signum, frame, previous = pending
            if previous == signal.SIG_DFL:
                signal.raise_signal(signum)
            else:
                previous(signum, frame)


class WindowsJob:
    """A private kill-on-close job; only its bootstrap may enroll itself."""

    def __init__(self, name: str, existing: bool = False):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = {
            "CreateJobObjectW": ([ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
            "OpenJobObjectW": ([wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
            "SetInformationJobObject": ([wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
            "AssignProcessToJobObject": ([wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
            "GetCurrentProcess": ([], wintypes.HANDLE),
            "TerminateJobObject": ([wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
            "QueryInformationJobObject": ([wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                           wintypes.DWORD, ctypes.c_void_p], wintypes.BOOL),
            "CloseHandle": ([wintypes.HANDLE], wintypes.BOOL),
            "PeekNamedPipe": ([wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p,
                               ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p], wintypes.BOOL),
        }
        for function, (arguments, result) in signatures.items():
            method = getattr(self.api, function)
            method.argtypes, method.restype = arguments, result
        # JOB_OBJECT_ASSIGN_PROCESS | JOB_OBJECT_TERMINATE. No breakaway rights.
        # A successful creation need not clear a stale ERROR_ALREADY_EXISTS.
        if not existing:
            ctypes.set_last_error(0)
        self.handle = (self.api.OpenJobObjectW(0x0001 | 0x0008, False, name) if existing
                       else self.api.CreateJobObjectW(None, name))
        error = ctypes.get_last_error()
        if not self.handle:
            raise ctypes.WinError(error)
        try:
            if not existing:
                require(error != 183, "private process job already exists")
                class BasicLimits(ctypes.Structure):
                    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong),
                                ("PerJobUserTimeLimit", ctypes.c_longlong),
                                ("LimitFlags", wintypes.DWORD),
                                ("MinimumWorkingSetSize", ctypes.c_size_t),
                                ("MaximumWorkingSetSize", ctypes.c_size_t),
                                ("ActiveProcessLimit", wintypes.DWORD),
                                ("Affinity", ctypes.c_size_t),
                                ("PriorityClass", wintypes.DWORD),
                                ("SchedulingClass", wintypes.DWORD)]
                class Counters(ctypes.Structure):
                    _fields_ = [(field, ctypes.c_ulonglong) for field in (
                        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]
                class ExtendedLimits(ctypes.Structure):
                    _fields_ = [("BasicLimitInformation", BasicLimits), ("IoInfo", Counters),
                                ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                                ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]
                limits = ExtendedLimits()
                limits.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                if not self.api.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
                    raise ctypes.WinError(ctypes.get_last_error())
        except BaseException as primary_error:
            try:
                self.close()
            except BaseException as error:
                raise shutdown_refusal(primary_error, [error]) from primary_error
            raise

    def enroll_bootstrap(self) -> None:
        if not self.api.AssignProcessToJobObject(self.handle, self.api.GetCurrentProcess()):
            raise self.ctypes.WinError(self.ctypes.get_last_error())

    def terminate(self) -> None:
        if not self.api.TerminateJobObject(self.handle, 1):
            raise self.ctypes.WinError(self.ctypes.get_last_error())

    def close(self) -> None:
        if self.handle:
            handle, self.handle = self.handle, None
            if not self.api.CloseHandle(handle):
                raise self.ctypes.WinError(self.ctypes.get_last_error())

    def wait_empty(self) -> None:
        from ctypes import wintypes
        class Accounting(self.ctypes.Structure):
            _fields_ = [(field, self.ctypes.c_longlong) for field in (
                "TotalUserTime", "TotalKernelTime", "ThisPeriodTotalUserTime", "ThisPeriodTotalKernelTime")]
            _fields_ += [(field, wintypes.DWORD) for field in (
                "TotalPageFaultCount", "TotalProcesses", "ActiveProcesses", "TotalTerminatedProcesses")]
        deadline = time.monotonic() + 2
        while True:
            info = Accounting()
            if not self.api.QueryInformationJobObject(self.handle, 1, self.ctypes.byref(info),
                                                       self.ctypes.sizeof(info), None):
                raise self.ctypes.WinError(self.ctypes.get_last_error())
            if info.ActiveProcesses == 0:
                return
            require(time.monotonic() < deadline, "Git process tree shutdown deadline exceeded")
            time.sleep(0.01)

    def available(self, stream) -> int:
        import msvcrt
        from ctypes import wintypes
        count = wintypes.DWORD()
        if not self.api.PeekNamedPipe(msvcrt.get_osfhandle(stream.fileno()), None, 0, None,
                                      self.ctypes.byref(count), None):
            error = self.ctypes.get_last_error()
            if error == 109:  # ERROR_BROKEN_PIPE: all writers closed.
                return -1
            raise self.ctypes.WinError(error)
        return count.value


def contained_git(name: str, argv: list[str]) -> int:
    # This trusted, isolated Python bootstrap creates no children before enrollment.
    # Assignment failure (including incompatible outer jobs) refuses before Git.
    require(os.name == "nt" and bool(argv), "unsupported process bootstrap")
    job = WindowsJob(name, existing=True)
    try:
        job.enroll_bootstrap()
    finally:
        # Only the parent retains a job handle when the command starts. Its death
        # therefore kills the complete tree even if Git retains output handles.
        primary_error = sys.exc_info()[1]
        try:
            job.close()
        except BaseException as error:
            if primary_error is not None:
                raise shutdown_refusal(primary_error, [error]) from primary_error
            raise
    return subprocess.call(argv, stdin=sys.stdin, stdout=sys.stdout, stderr=sys.stderr)


def run_command(source: Path, argv: list[str], environment: dict[str, str],
                input_bytes: bytes | None = None, *,
                cancellation: CommandCancellation | None = None) -> bytes:
    """Run one owned tree, with bounded output, storage and shutdown."""
    if cancellation is None:
        with CommandCancellation() as owner:
            with owner.command():
                return owned_command(source, argv, environment, input_bytes, owner)
    with cancellation.command():
        return owned_command(source, argv, environment, input_bytes, cancellation)


def owned_command(source: Path, argv: list[str], environment: dict[str, str],
                  input_bytes: bytes | None, cancellation: CommandCancellation) -> bytes:
    require(input_bytes is None or len(input_bytes) <= 22655, "Git input bound exceeded")
    require(os.name == "nt" or (os.name == "posix" and all(hasattr(os, key) for key in
            ("waitid", "P_PID", "WNOWAIT", "WEXITED", "WNOHANG"))), "process containment unavailable")
    job = None
    process = None
    writer = None
    failures = []
    output = bytearray()  # At most OUTPUT_BYTES; each read is at most 64 KiB.
    def read_output():
        if job is not None:
            available = job.available(process.stdout)
            if available < 0:
                return b""
            if not available:
                return None
            return os.read(process.stdout.fileno(), min(65536, available))
        try:
            return os.read(process.stdout.fileno(), 65536)
        except BlockingIOError:
            return None
    try:
        cancellation.check()
        command = argv
        if os.name == "nt":
            name = "rmlui-source-" + uuid.uuid4().hex
            job = WindowsJob(name)
            command = [sys.executable, "-I", "-B", "-S", str(Path(__file__).resolve()),
                       "--contained-git", name, *argv]
        process = subprocess.Popen(command, env=environment,
                                   stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   start_new_session=os.name == "posix", bufsize=0)
        cancellation.check()
        if input_bytes is not None:
            def send_input() -> None:
                try:
                    assert process.stdin is not None
                    with process.stdin:
                        remaining = memoryview(input_bytes)
                        while remaining:
                            count = process.stdin.write(remaining)
                            require(count is not None and count > 0, "partial Git input")
                            remaining = remaining[count:]
                except (OSError, ValueError) as error:
                    failures.append(str(error))
            sender = threading.Thread(target=send_input, daemon=True)
            sender.start()
            writer = sender

        # Scan synchronously: no monitor can outlive this call or block a join.
        def scan() -> None:
            try:
                inventory(source, live_git=True)
            except FileNotFoundError:
                # Git renames its own locks/packs. The final scan stays strict.
                pass
            except (OSError, Refusal) as error:
                raise Refusal("Git storage refused: " + str(error)) from error
        scan()
        assert process.stdout is not None
        if job is None:
            os.set_blocking(process.stdout.fileno(), False)
        deadline = time.monotonic() + 120
        next_scan = time.monotonic() + 0.1
        eof = False
        while True:
            cancellation.check()
            now = time.monotonic()
            require(now < deadline, "Git command deadline exceeded")
            require(not failures, "Git input refused")
            if now >= next_scan:
                scan()
                next_scan = now + 0.1
            # WNOWAIT retains the session leader's PID until tree termination:
            # an exited parent cannot cause an unrelated reused PGID to be killed.
            if job is not None:
                exited = process.poll() is not None
            else:
                info = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                # WNOHANG may report no exit as either None or a zero PID.
                exited = info is not None and info.si_pid == process.pid
            # Observe exit before reading: an earlier empty read cannot prove
            # that an exited leader left a writer, or omit its final bytes.
            data = None
            if not eof:
                data = read_output()
                if data == b"":
                    eof = True
                elif data:
                    require(len(data) <= OUTPUT_BYTES - len(output), "Git output bound exceeded")
                    output.extend(data)
                    continue
            if exited:
                require(eof, "Git descendant retained output")
                break
            time.sleep(0.01)
    finally:
        primary_error = sys.exc_info()[1]
        shutdown_errors = []
        def attempt(action) -> None:
            try:
                action()
            except BaseException as error:
                shutdown_errors.append(error)
        if job is not None and process is not None:
            # The bootstrap may still be outside the job. Stop that exact
            # retained process and observe exit before final job termination:
            # an empty job alone cannot exclude later enrollment or spawning.
            attempt(process.kill)
        elif process is not None:
            def terminate_session() -> None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            attempt(terminate_session)
        if process is not None:
            attempt(lambda: process.wait(timeout=2))
        if job is not None:
            # Even failed bootstrap shutdown must not bypass remaining cleanup.
            attempt(job.terminate)
            attempt(job.wait_empty)
        if process is not None:
            if writer is not None:
                attempt(lambda: writer.join(timeout=2))
                if writer.is_alive():
                    shutdown_errors.append(Refusal("Git input shutdown deadline exceeded"))
            if process.stdin is not None and (writer is None or not writer.is_alive()):
                attempt(process.stdin.close)
            if process.stdout is not None:
                def drain_output() -> None:
                    if job is None:
                        os.set_blocking(process.stdout.fileno(), False)
                    # Drain already queued bytes before closure, even on refusal.
                    # Both retained output and discarded drain have the 4 MiB cap.
                    drain_deadline = time.monotonic() + 2
                    drained = 0
                    while True:
                        data = read_output()
                        if data == b"":
                            break
                        require(time.monotonic() < drain_deadline, "Git pipe shutdown deadline exceeded")
                        if data:
                            require(len(data) <= OUTPUT_BYTES - drained, "Git pipe drain bound exceeded")
                            drained += len(data)
                            output.extend(data[:max(0, OUTPUT_BYTES - len(output))])
                        else:
                            time.sleep(0.01)
                attempt(drain_output)
                attempt(process.stdout.close)
        if job is not None:
            attempt(job.close)
        if shutdown_errors:
            if primary_error is None and process is not None and process.returncode not in (None, 0):
                primary_error = Refusal("Git command refused\n" +
                                        output[-4096:].decode("utf-8", errors="replace"))
            raise shutdown_refusal(primary_error, shutdown_errors) from (primary_error or shutdown_errors[0])
    assert process is not None
    require(not failures and process.returncode == 0, "Git command refused\n" +
            output[-4096:].decode("utf-8", errors="replace"))
    inventory(source)
    return bytes(output)


def verify_git(source: Path, spec: Specification) -> None:
    admin = source / ".git"
    require(ordinary(os.lstat(admin), True), "external Git administration")
    metadata = inventory(admin)
    for name in metadata:
        require(name in {"HEAD", "config", "description", "index", "FETCH_HEAD", "ORIG_HEAD", "shallow", "logs/HEAD"}
                or re.fullmatch(r"objects/[0-9a-f]{2}/[0-9a-f]{38}", name)
                or re.fullmatch(r"objects/pack/pack-[0-9a-f]{40}\.(pack|idx|rev)", name),
                "unexpected Git administrative file")
    read_file(admin / "index", 1024 * 1024)
    for name in ("commondir", "gitdir", "objects/info/alternates", "objects/info/http-alternates", "info/grafts", "refs/replace"):
        require(not os.path.lexists(admin / name), "shared or substituted Git administration")
    config = configparser.ConfigParser(interpolation=None)
    config.read_string(read_file(admin / "config", 8192).decode("utf-8"))
    require(config.sections() == ["core"] and not config.defaults(), "nonordinary Git configuration")
    allowed = {"repositoryformatversion": {"0"}, "bare": {"false"}, "filemode": {"true", "false"},
               "logallrefupdates": {"true"}, "ignorecase": {"true", "false"}, "symlinks": {"true", "false"}}
    require(all(k in allowed and v.lower() in allowed[k] for k, v in config["core"].items()), "unsafe local Git configuration")
    require(read_file(admin / "HEAD", 128) == (REVISION + "\n").encode(), "HEAD is not the detached pin")
    require(git(source, "rev-parse", "HEAD").strip().decode() == REVISION, "wrong Git revision")
    # An empty path selects the root tree without brace metacharacters.
    require(git(source, "rev-parse", "HEAD:").strip().decode() == spec.data["tree"], "wrong Git tree")
    expected = {name: (e["mode"], e["blob"]) for name, e in spec.files.items()}
    index = {}
    for row in git(source, "ls-files", "--stage", "-z").split(b"\0")[:-1]:
        metadata, name = row.split(b"\t", 1)
        mode, blob, stage = metadata.decode().split()
        require(stage == "0" and name.decode() not in index, "mixed Git index")
        index[name.decode()] = (mode, blob)
    require(index == expected, "Git index differs from pinned manifest")
    flags = git(source, "ls-files", "-v", "-z").split(b"\0")[:-1]
    require(len(flags) == FILE_COUNT and all(r.startswith(b"H ") for r in flags), "hidden index state")
    tree = {}
    for row in git(source, "ls-tree", "-rz", REVISION).split(b"\0")[:-1]:
        metadata, name = row.split(b"\t", 1)
        mode, kind, blob = metadata.decode().split()
        require(kind == "blob", "nonregular Git tree")
        tree[name.decode()] = (mode, blob)
    require(tree == expected, "Git tree differs from manifest")


def write_exclusive(path: Path, data: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def encoded(value: dict) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def root_mismatch_details(root: Path, stored: object) -> str:
    # Refusal only: one lstat of the validated root, no contents or environment.
    # Root inputs are capped at 32768 characters; the owner record at 4096 bytes.
    def identity(path):
        try:
            info = os.lstat(path)
            return dict(device=info.st_dev, inode=info.st_ino, links=info.st_nlink,
                        mode=info.st_mode, attributes=getattr(info, "st_file_attributes", None))
        except (OSError, ValueError) as error:
            return dict(unavailable=type(error).__name__, errno=getattr(error, "errno", None))
    valid = isinstance(stored, str) and 0 < len(stored) <= 32768 and "\0" not in stored
    valid = valid and Path(stored).is_absolute()
    details = dict(expected_root=str(root), expected_identity=identity(root),
                   interpreter=sys.executable[:32768], version=sys.version[:512],
                   platform=sys.platform, os_name=os.name)
    # Malformed relative fields are not root spellings and may contain secrets.
    if valid:
        details["stored_root"] = stored
    else:
        details["stored_root"] = "<invalid absolute root>"
    return json.dumps(details, sort_keys=True, ensure_ascii=True)


class Workspace:
    def __init__(self, root: Path, spec: Specification):
        self.root = canonical(root)
        self.spec = spec
        self.owner_path = self.root / ".rmlui-owner.json"
        self.source = self.root / "_deps" / ("rmlui-" + SOURCE_ID + "-src")
        self.stage = self.source.with_name(self.source.name + ".stage")
        self.population = self.source.with_name("rmlui-" + SOURCE_ID + "-population")
        self.receipt = self.root / ".rmlui-ready.json"
        self.admission = self.root / ".rmlui-admission"

    def initialize(self) -> None:
        require(not os.path.lexists(self.root), "build root already exists; choose a fresh root")
        self.root.parent.mkdir(parents=True, exist_ok=True)
        self.root.mkdir()
        write_exclusive(self.owner_path, encoded(dict(schema=1, root=str(self.root), nonce=uuid.uuid4().hex,
                                                      delivery=self.spec.delivery)))

    def owner(self) -> bytes:
        raw = read_file(self.owner_path, 4096)
        value = json.loads(raw)
        require(raw == encoded(value), "noncanonical ownership record")
        require(set(value) == {"schema", "root", "nonce", "delivery"}, "workspace ownership fields mismatch")
        require(value["schema"] == 1, "workspace ownership schema mismatch")
        if value["root"] != str(self.root):
            raise Refusal("workspace ownership root mismatch: " + root_mismatch_details(self.root, value["root"]))
        require(isinstance(value["nonce"], str) and re.fullmatch(r"[0-9a-f]{32}", value["nonce"]),
                "workspace ownership nonce format mismatch")
        require(isinstance(value["delivery"], dict), "workspace delivery format mismatch")
        require(set(value["delivery"]) == set(self.spec.delivery), "workspace delivery fields mismatch")
        for field, expected in self.spec.delivery.items():
            require(value["delivery"][field] == expected, f"workspace delivery {field} mismatch")
        return raw

    def expected_receipt(self, owner: bytes) -> dict:
        return dict(schema=1, owner_sha256=digest(owner), delivery=self.spec.delivery, source=str(self.source),
                    head_sha256=digest(read_file(self.source / ".git/HEAD", 128)),
                    index_sha256=digest(read_file(self.source / ".git/index", 1024 * 1024)))

    def verify_population(self) -> None:
        # FetchContent may inspect SOURCE_SUBDIR before returning. Only the
        # empty ordinary population directory is eligible for that operation.
        canonical(self.population)
        try:
            info = os.lstat(self.population)
        except FileNotFoundError:
            return
        require(ordinary(info, True), "nonordinary population directory")
        with os.scandir(self.population) as entries:
            require(next(entries, None) is None, "nonempty population directory")

    def verify(self) -> None:
        owner = self.owner()
        self.verify_population()
        require(not os.path.lexists(self.stage) and not os.path.lexists(self.admission), "unfinished source delivery")
        require(canonical(self.source) == self.source, "source path alias")
        receipt_bytes = read_file(self.receipt, 4096)
        receipt = json.loads(receipt_bytes)
        require(receipt_bytes == encoded(receipt), "noncanonical receipt")
        require(receipt == self.expected_receipt(owner), "source receipt mismatch")
        verify_git(self.source, self.spec)
        self.spec.verify_files(self.source, True)

    def preflight(self, disconnected: bool) -> None:
        self.owner()
        canonical(self.source.parent)
        self.verify_population()
        if os.path.lexists(self.receipt):
            self.verify()
            return
        require(not disconnected, "fully disconnected source unavailable")
        require(not any(os.path.lexists(p) for p in (self.source, self.stage, self.admission)),
                "ambiguous or preexisting source; choose a fresh build root")

    def publish(self, owner: bytes) -> None:
        # Caller owns the exclusive admission; all payloads precede readiness.
        require(read_file(self.owner_path, 4096) == owner, "ownership changed during delivery")
        verify_git(self.stage, self.spec)
        self.spec.verify_files(self.stage, False)
        git(self.stage, "apply", "--unidiff-zero", "-p1", "--check", input_bytes=self.spec.patch_bytes)
        git(self.stage, "apply", "--unidiff-zero", "-p1", input_bytes=self.spec.patch_bytes)
        verify_git(self.stage, self.spec)
        self.spec.verify_files(self.stage, True)
        require(not os.path.lexists(self.source) and not os.path.lexists(self.receipt), "publication destination occupied")
        os.rename(self.stage, self.source)
        receipt = self.expected_receipt(owner)
        write_exclusive(self.receipt, encoded(receipt))
        # Only this invocation creates/removes admission, under workspace exclusivity.
        self.admission.unlink()

    def download(self) -> None:
        self.preflight(False)
        if os.path.lexists(self.receipt):
            return
        write_exclusive(self.admission, b"exclusive source delivery\n")
        owner = self.owner()
        require(not any(os.path.lexists(p) for p in (self.source, self.stage, self.receipt)), "source state changed")
        canonical(self.stage.parent)
        self.stage.parent.mkdir(exist_ok=True)
        self.stage.mkdir()
        git(self.stage, "init", "--template=")
        git(self.stage, "fetch", "--depth=1", "--no-tags", UPSTREAM, REVISION)
        git(self.stage, "-c", "advice.detachedHead=false", "checkout", "--detach", REVISION)
        self.publish(owner)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("init", "preflight", "download", "verify"))
    parser.add_argument("--root", required=True)
    parser.add_argument("--fully-disconnected", action="store_true")
    args = parser.parse_args(argv)
    repository = Path(__file__).resolve().parents[2]
    try:
        spec = Specification(repository / "vendor/rmlui/rmlui-6.1-source.json",
                             repository / "cmake/patches/rmlui-6.1-shadow.patch")
        workspace = Workspace(args.root, spec)
        if args.operation == "init":
            workspace.initialize()
        elif args.operation == "preflight":
            workspace.preflight(args.fully_disconnected)
        elif args.operation == "verify":
            workspace.verify()
        else:
            if args.fully_disconnected:
                workspace.preflight(True)
            else:
                workspace.download()
        return 0
    except (OSError, ValueError, KeyError, TypeError, configparser.Error) as error:
        print(f"RmlUi source refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--contained-git":
        try:
            raise SystemExit(contained_git(sys.argv[2], sys.argv[3:]))
        except (OSError, ValueError) as error:
            print(f"RmlUi process containment refused: {error}", file=sys.stderr)
            raise SystemExit(1)
    raise SystemExit(main())

#!/usr/bin/env python3
"""Tests for scripts/tidy_cache.py: a cache hit must be impossible unless the
whole analysis input is identical to an earlier passing run."""

import importlib.util
import concurrent.futures
import contextlib
import ctypes
import io
import json
import ntpath
import os
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
import unittest
from types import SimpleNamespace
from pathlib import Path
from unittest.mock import MagicMock, patch

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tidy_cache.py"
SPEC = importlib.util.spec_from_file_location("tidy_cache", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

FAKE_COMPILER = """
import os, re, sys
args = sys.argv[1:]
if args == ["--version"]:
    print("fakecc version " + os.environ.get("FAKE_CC_VERSION", "1"))
    sys.exit(0)
if "-DBREAK_DEPS" in args:
    sys.exit(1)
dirs, system_dirs, source, i = [], [], None, 0
while i < len(args):
    a = args[i]
    if a == "-I":
        dirs.append(args[i + 1]); i += 2; continue
    if a == "-isystem":
        system_dirs.append(args[i + 1]); i += 2; continue
    if a == "-o":
        i += 2; continue
    if a.startswith("-"):
        i += 1; continue
    source = a; i += 1
source = os.path.abspath(source)
seen, order = set(), []
def visit(path):
    if path in seen:
        return
    seen.add(path); order.append(path)
    active = True
    for line in open(path, encoding="utf-8"):
        if line.startswith("#ifdef __clang_analyzer__"):
            active = "-D__clang_analyzer__" in args
            continue
        if line.startswith("#endif"):
            active = True
            continue
        if not active:
            continue
        m = re.match(r'\\s*#include\\s+([<"])([^>"]+)[>"]', line)
        if not m:
            continue
        kind, name = m.groups()
        search = ([os.path.dirname(path)] if kind == '"' else []) + dirs + system_dirs
        for d in search:
            candidate = os.path.join(d, name)
            if os.path.exists(candidate):
                visit(os.path.abspath(candidate)); break
        else:
            sys.exit(1)
visit(source)
print("main.o: " + " \\\\\\n ".join(p.replace(" ", "\\\\ ") for p in order))
"""

FAKE_TIDY = """
import json, os, sys
args = sys.argv[1:]
if args == ["--version"]:
    print("faketidy " + os.environ.get("FAKE_TIDY_VERSION", "1"))
    sys.exit(0)
target = args[-1]
with open(os.environ["FAKE_TIDY_LOG"], "a", encoding="utf-8") as log:
    log.write(target + "\\n")
database = os.path.join(args[args.index("-p") + 1], "compile_commands.json")
with open(database, encoding="utf-8") as handle:
    entries = json.load(handle)
first = next((entry for entry in entries if os.path.normcase(os.path.abspath(
    os.path.join(entry["directory"], entry["file"]))) == os.path.normcase(os.path.abspath(target))), None)
flag_failure = first is not None and "-DTIDY_FAIL" in first["arguments"]
swap = os.environ.get("FAKE_TIDY_DB_FROM")
if swap:
    import shutil
    shutil.copyfile(swap, os.environ["FAKE_TIDY_DB"])
mutate = os.environ.get("FAKE_TIDY_MUTATE")
if mutate:
    with open(mutate, "a", encoding="utf-8") as handle:
        handle.write("// changed while the analysis was running\\n")
if flag_failure or "TIDY_FAIL" in open(target, encoding="utf-8").read():
    print("fake diagnostic in " + target)
    sys.exit(1)
sys.exit(0)
"""


def assert_tree_timeout(case, launch, error_type=subprocess.TimeoutExpired, *, driver_exits=False,
                        interrupt=False):
    """Exercise a real driver and grandchild, and reap the fixture on failure."""
    with tempfile.TemporaryDirectory(prefix="process-tree-") as temporary:
        directory = Path(temporary)
        ready = directory / "grandchild.pid"
        script = directory / "driver.py"
        script.write_text(
            "import subprocess, sys, time\n"
            'code = ("import os, pathlib, sys, time; "\n'
            '        "ready = pathlib.Path(sys.argv[1]); pending = ready.with_suffix(\\".ready\\"); "\n'
            '        "pending.write_text(str(os.getpid())); pending.replace(ready); time.sleep(60)")\n'
            "subprocess.Popen([sys.executable, '-I', '-B', '-c', code, sys.argv[1]])\n"
            "print('driver output', flush=True)\n"
            + ("sys.exit(0)\n" if driver_exits else "time.sleep(60)\n"), encoding="utf-8")
        grandchild = None
        handle = None
        process = None
        subreaper = None
        terminated = False
        started = threading.Event()
        if sys.platform == "linux":
            # Adopt the orphan so the regression verifies reaping as well as
            # termination, even in containers whose init does not reap zombies.
            libc = ctypes.CDLL(None, use_errno=True)
            subreaper = ctypes.c_int()
            case.assertEqual(libc.prctl(37, ctypes.byref(subreaper), 0, 0, 0), 0)
            case.assertEqual(libc.prctl(36, 1, 0, 0, 0), 0)
        if os.name == "nt":
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = (ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_ulong)
            kernel.TerminateProcess.argtypes = (ctypes.c_void_p, ctypes.c_uint)
            kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
        popen = subprocess.Popen

        def start(command, **kwargs):
            nonlocal process
            process = popen(command, **kwargs)
            communicate = process.communicate

            def wait_until_ready(input=None, timeout=None):
                nonlocal grandchild, handle
                if timeout is not None:
                    deadline = time.monotonic() + 5
                    while not ready.exists() or not ready.read_text():
                        case.assertLess(time.monotonic(), deadline, "grandchild did not start")
                        time.sleep(0.01)
                    grandchild = int(ready.read_text())
                    if os.name == "nt":
                        handle = kernel.OpenProcess(0x100001, False, grandchild)
                        case.assertTrue(handle)
                    started.set()
                    # Start the actual timeout only after the descendant exists.
                    return communicate(input, timeout=0.05)
                return communicate(input)

            process.communicate = wait_until_ready
            return process

        def gone():
            nonlocal terminated
            if terminated:
                return True
            if os.name == "nt":
                terminated = kernel.WaitForSingleObject(handle, 0) == 0
                return terminated
            try:
                waited, _ = os.waitpid(grandchild, os.WNOHANG)
                if waited:
                    terminated = True
                    return True
            except ChildProcessError:
                pass
            try:
                os.kill(grandchild, 0)
            except ProcessLookupError:
                terminated = True
                return True
            return False

        try:
            def driver_only(command, **kwargs):
                return start(command, **kwargs) if command[0] == sys.executable else popen(command, **kwargs)

            future_result = concurrent.futures.Future.result

            def interrupt_result(future, *args, **kwargs):
                deadline = time.monotonic() + 5
                while not started.is_set():
                    # Let non-process preparation finish before interrupting
                    # the analysis pool in the public command regression.
                    if future.done():
                        return future_result(future, *args, **kwargs)
                    case.assertLess(time.monotonic(), deadline, "worker did not start")
                    started.wait(0.001)
                raise KeyboardInterrupt()

            with contextlib.ExitStack() as stack:
                stack.enter_context(patch.object(subprocess, "Popen", side_effect=driver_only))
                if interrupt:
                    stack.enter_context(patch.object(concurrent.futures.Future, "result", interrupt_result))
                with case.assertRaises(error_type):
                    launch([sys.executable, "-I", "-B", str(script), str(ready)], directory)
            case.assertIsNotNone(grandchild)
            case.assertIsNotNone(process.returncode)
            deadline = time.monotonic() + 2
            while not gone():
                case.assertLess(time.monotonic(), deadline, "grandchild survived the timeout")
                time.sleep(0.01)
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait()
            if grandchild is not None and not gone():
                if os.name == "nt":
                    kernel.TerminateProcess(handle, 1)
                    kernel.WaitForSingleObject(handle, 5000)
                else:
                    os.kill(grandchild, signal.SIGKILL)
                    if subreaper is not None:
                        os.waitpid(grandchild, 0)
            if handle is not None:
                kernel.CloseHandle(handle)
            if subreaper is not None:
                case.assertEqual(libc.prctl(36, subreaper.value, 0, 0, 0), 0)


class ProcessRunnerContractTests(unittest.TestCase):
    def setUp(self):
        self.process = MagicMock(pid=123, returncode=0)
        self.process.poll.return_value = None
        self.process.__enter__.return_value = self.process
        self.popen = patch.object(MODULE.subprocess, "Popen", return_value=self.process).start()
        # Fake Popen handles must use a fake job backend on native Windows too.
        patch.object(MODULE, "_WindowsJob").start()
        self.addCleanup(patch.stopall)

    def test_success_output_exit_codes_and_capture_validation(self):
        for output in ((b"out", b"err"), ("out", "err"), (None, None)):
            with self.subTest(output=output):
                self.process.communicate.return_value = output
                result = MODULE.run_process(["tool"], timeout=7)
                self.assertEqual((result.stdout, result.stderr), output)
                self.assertEqual(result.returncode, 0)
        self.process.returncode = 2
        self.assertEqual(MODULE.run_process(["tool"], timeout=7).returncode, 2)
        with self.assertRaises(subprocess.CalledProcessError):
            MODULE.run_process(["tool"], timeout=7, check=True)
        self.popen.reset_mock()
        for name in ("stdout", "stderr"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                MODULE.run_process(["tool"], timeout=7, capture_output=True, **{name: subprocess.PIPE})
        self.popen.assert_not_called()
        self.popen.side_effect = OSError("cannot launch")
        with self.assertRaisesRegex(OSError, "cannot launch"):
            MODULE.run_process(["tool"], timeout=7)

    def test_posix_timeout_and_interruption_kill_group_before_draining(self):
        for error in (subprocess.TimeoutExpired("tool", 7, b"partial"), KeyboardInterrupt()):
            with self.subTest(error=type(error)), patch.object(MODULE.os, "name", "posix"), \
                    patch.object(MODULE.signal, "SIGKILL", 9, create=True), \
                    patch.object(MODULE.os, "killpg", create=True) as killpg:
                self.process.communicate.side_effect = [error, (b"partial", b"diagnostic")]
                with self.assertRaises(type(error)):
                    MODULE.run_process(["tool"], timeout=7, capture_output=True)
                self.assertTrue(self.popen.call_args.kwargs["start_new_session"])
                killpg.assert_called_once_with(123, signal.SIGKILL)
                self.assertEqual(self.process.communicate.call_args_list[-1].kwargs, {})

    def test_posix_missing_group_is_reaped_and_cleanup_errors_propagate(self):
        for failure in (ProcessLookupError(), PermissionError("cannot terminate")):
            with self.subTest(failure=type(failure)), patch.object(MODULE.os, "name", "posix"), \
                    patch.object(MODULE.signal, "SIGKILL", 9, create=True), \
                    patch.object(MODULE.os, "killpg", create=True, side_effect=failure):
                self.process.communicate.side_effect = [subprocess.TimeoutExpired("tool", 7), (b"", b"")]
                expected = subprocess.TimeoutExpired if isinstance(failure, ProcessLookupError) else PermissionError
                with self.assertRaises(expected):
                    MODULE.run_process(["tool"], timeout=7)
                if expected is PermissionError:
                    self.process.kill.assert_called_once()
                    self.process.wait.assert_called_once()

    def test_windows_job_flags_output_and_failure_inputs(self):
        for stage in (None, "create", "launch", "start", "terminate"):
            with self.subTest(stage=stage), patch.object(MODULE.os, "name", "nt"), \
                    patch.object(MODULE.subprocess, "CREATE_NEW_PROCESS_GROUP", 512, create=True), \
                    patch.object(MODULE, "_WindowsJob") as factory:
                self.process.reset_mock()
                self.process.communicate.side_effect = [subprocess.TimeoutExpired("tool", 7),
                                                       (b"partial", b"diagnostic")]
                job = factory.return_value
                failure = OSError("job failure")
                if stage == "create":
                    factory.side_effect = failure
                elif stage == "launch":
                    self.popen.side_effect = failure
                elif stage is not None:
                    getattr(job, stage).side_effect = failure
                    if stage == "start":
                        self.process.communicate.side_effect = [(b"", b"")]
                expected = OSError if stage else subprocess.TimeoutExpired
                with self.assertRaises(expected) as raised:
                    MODULE.run_process(["tool"], timeout=7, capture_output=True)
                if stage == "create":
                    job.close.assert_not_called()
                    continue
                self.assertEqual(self.popen.call_args.kwargs["creationflags"], 516)
                job.close.assert_called_once()
                if stage == "launch":
                    job.start.assert_not_called()
                    self.popen.side_effect = None
                    continue
                job.start.assert_called_once_with(self.process)
                job.terminate.assert_called_once()
                self.process.kill.assert_called_once()
                if stage == "terminate":
                    self.process.wait.assert_called_once()
                elif stage is None:
                    self.assertEqual(raised.exception.stdout, b"partial")
                    self.assertEqual(raised.exception.stderr, b"diagnostic")

    def test_windows_success_releases_job_after_collecting_output(self):
        with patch.object(MODULE.os, "name", "nt"), \
                patch.object(MODULE.subprocess, "CREATE_NEW_PROCESS_GROUP", 512, create=True), \
                patch.object(MODULE, "_WindowsJob") as factory:
            self.process.communicate.return_value = (b"out", b"err")
            result = MODULE.run_process(["tool"], timeout=7)
            self.assertEqual((result.stdout, result.stderr), (b"out", b"err"))
            factory.return_value.start.assert_called_once_with(self.process)
            factory.return_value.close.assert_called_once()
            factory.return_value.terminate.assert_not_called()

    def test_windows_assignment_failure_after_driver_exit_is_reported_and_reaped(self):
        failure = OSError("assignment failed after exit")
        self.process.poll.return_value = 0
        self.process.communicate.return_value = (b"", b"")
        with patch.object(MODULE.os, "name", "nt"), \
                patch.object(MODULE.subprocess, "CREATE_NEW_PROCESS_GROUP", 512, create=True), \
                patch.object(MODULE, "_WindowsJob") as factory:
            factory.return_value.start.side_effect = failure
            with self.assertRaises(OSError) as raised:
                MODULE.run_process(["tool"], timeout=7)
            self.assertIs(raised.exception, failure)
            self.process.kill.assert_not_called()
            self.process.communicate.assert_called_once_with()
            self.process.__exit__.assert_called_once()
            factory.return_value.terminate.assert_called_once()
            factory.return_value.close.assert_called_once()

    def test_parallel_cancellation_terminates_both_backends_and_refuses_new_launches(self):
        for platform in ("posix", "nt"):
            with self.subTest(platform=platform), patch.object(MODULE.os, "name", platform), \
                    patch.object(MODULE.signal, "SIGKILL", 9, create=True), \
                    patch.object(MODULE.os, "killpg", create=True) as killpg, \
                    patch.object(MODULE.subprocess, "CREATE_NEW_PROCESS_GROUP", 512, create=True), \
                    patch.object(MODULE, "_WindowsJob") as factory:
                self.process.reset_mock()
                cancelled = threading.Event()

                def communicate(timeout=None):
                    if timeout is None:
                        return b"partial", b"diagnostic"
                    self.assertLessEqual(timeout, 0.1)
                    cancelled.set()
                    raise subprocess.TimeoutExpired("tool", timeout)

                self.process.communicate.side_effect = communicate
                with patch.object(MODULE._PROCESS_CONTEXT, "cancelled", cancelled, create=True):
                    with self.assertRaises(concurrent.futures.CancelledError):
                        MODULE.run_process(["tool"], timeout=7)
                    self.process.communicate.assert_called_with()
                    self.popen.reset_mock()
                    with self.assertRaises(concurrent.futures.CancelledError):
                        MODULE.run_process(["queued tool"], timeout=7)
                    self.popen.assert_not_called()
                if platform == "nt":
                    factory.return_value.terminate.assert_called_once()
                    factory.return_value.close.assert_called_once()
                else:
                    killpg.assert_called_once_with(123, 9)

    def test_parallel_deadline_preserves_partial_output_and_empty_work(self):
        cancelled = threading.Event()
        self.process.communicate.side_effect = [subprocess.TimeoutExpired("tool", 0, b"partial"),
                                               (b"partial", b"diagnostic")]
        with patch.object(MODULE.os, "name", "posix"), \
                patch.object(MODULE.signal, "SIGKILL", 9, create=True), \
                patch.object(MODULE.os, "killpg", create=True), \
                patch.object(MODULE._PROCESS_CONTEXT, "cancelled", cancelled, create=True):
            with self.assertRaises(subprocess.TimeoutExpired) as raised:
                MODULE.run_process(["tool"], timeout=0)
            self.assertEqual(raised.exception.timeout, 0)
            self.assertEqual(raised.exception.stdout, b"partial")
        self.assertEqual(MODULE.parallel_map(lambda item: self.fail("unexpected work"), [], 2), [])


class WindowsJobTests(unittest.TestCase):
    def make_job(self):
        self.kernel = MagicMock()
        self.kernel.CreateJobObjectW.return_value = 11
        self.kernel.CreateToolhelp32Snapshot.return_value = 22
        self.kernel.OpenThread.return_value = 33
        self.kernel.ResumeThread.return_value = 1
        self.kernel.QueryInformationJobObject.return_value = 1

        def entry(snapshot, pointer):
            pointer._obj.th32OwnerProcessID = 123
            pointer._obj.th32ThreadID = 456
            return True

        self.kernel.Thread32First.side_effect = entry
        patch.object(ctypes, "WinDLL", return_value=self.kernel, create=True).start()
        patch.object(ctypes, "WinError", side_effect=lambda code: OSError("Windows API failure"),
                     create=True).start()
        patch.object(ctypes, "get_last_error", return_value=5, create=True).start()
        self.addCleanup(patch.stopall)
        return MODULE._WindowsJob()

    def test_assign_before_resume_and_wait_for_every_descendant(self):
        job = self.make_job()
        job.start(SimpleNamespace(pid=123, _handle=44))
        self.kernel.AssignProcessToJobObject.assert_called_once_with(11, 44)
        self.kernel.OpenThread.assert_called_once_with(2, False, 456)
        self.kernel.ResumeThread.assert_called_once_with(33)
        calls = [call[0] for call in self.kernel.mock_calls]
        self.assertLess(calls.index("AssignProcessToJobObject"), calls.index("ResumeThread"))
        active = iter((2, 1, 0))

        def query(handle, kind, pointer, size, returned):
            pointer._obj.ActiveProcesses = next(active)
            return True

        self.kernel.QueryInformationJobObject.side_effect = query
        with patch.object(MODULE.time, "sleep") as sleep:
            job.terminate()
        self.assertEqual(self.kernel.QueryInformationJobObject.call_count, 3)
        self.assertEqual(sleep.call_count, 2)
        job.close()
        self.assertEqual([call.args[0] for call in self.kernel.CloseHandle.call_args_list], [33, 22, 11])

    def test_kernel_failure_inputs_release_acquired_handles(self):
        for stage in ("CreateJobObjectW", "AssignProcessToJobObject", "CreateToolhelp32Snapshot",
                      "Thread32First", "Thread32Next", "OpenThread", "ResumeThread",
                      "TerminateJobObject", "QueryInformationJobObject"):
            with self.subTest(stage=stage):
                job = self.make_job()
                function = getattr(self.kernel, stage)
                function.side_effect = None
                function.return_value = (ctypes.c_void_p(-1).value if stage == "CreateToolhelp32Snapshot"
                                         else 0xFFFFFFFF if stage == "ResumeThread" else 0)
                if stage == "Thread32Next":
                    self.kernel.Thread32First.side_effect = None
                    self.kernel.Thread32First.return_value = True
                try:
                    with self.assertRaises(OSError):
                        if stage == "CreateJobObjectW":
                            MODULE._WindowsJob()
                        elif stage in {"TerminateJobObject", "QueryInformationJobObject"}:
                            job.terminate()
                        else:
                            job.start(SimpleNamespace(pid=123, _handle=44))
                finally:
                    job.close()
                handles = [call.args[0] for call in self.kernel.CloseHandle.call_args_list]
                self.assertEqual(handles[-1], 11)
                if stage in {"Thread32First", "Thread32Next", "OpenThread", "ResumeThread"}:
                    self.assertIn(22, handles)
                if stage == "ResumeThread":
                    self.assertIn(33, handles)

    def test_thread_enumeration_ignores_other_processes(self):
        job = self.make_job()
        entry = self.kernel.Thread32First.side_effect

        def unrelated(snapshot, pointer):
            pointer._obj.th32OwnerProcessID = 789
            return True

        self.kernel.Thread32First.side_effect = unrelated
        self.kernel.Thread32Next.side_effect = entry
        job.start(SimpleNamespace(pid=123, _handle=44))
        self.kernel.Thread32Next.assert_called_once()
        self.kernel.OpenThread.assert_called_once_with(2, False, 456)
        job.close()

    def test_partial_thread_information_refuses_resume_and_releases_snapshot(self):
        job = self.make_job()

        def partial(snapshot, pointer):
            pointer._obj.dwSize = 8
            return True

        self.kernel.Thread32First.side_effect = partial
        with self.assertRaisesRegex(OSError, "incomplete tool thread information"):
            job.start(SimpleNamespace(pid=123, _handle=44))
        self.kernel.ResumeThread.assert_not_called()
        self.kernel.CloseHandle.assert_called_once_with(22)
        job.close()


class ProcessTreeTests(unittest.TestCase):
    def test_cache_command_interrupt_cleans_up_preparation_and_analysis(self):
        for phase in ("prepare", "analyze"):
            with self.subTest(phase=phase):
                key_calls, launches, failures = [], [], []

                def launch(command, directory):
                    original_run = MODULE.run_process

                    def run_tool(*args, **kwargs):
                        launches.append(args[0])
                        try:
                            return original_run(command, cwd=directory, capture_output=True, timeout=1)
                        except BaseException as error:
                            failures.append(type(error))
                            raise

                    def key_for(tu, *args):
                        key_calls.append(tu)
                        if phase == "prepare":
                            run_tool(command)
                        return "key"

                    args = SimpleNamespace(files=[str(directory / f"{i}.cpp") for i in range(8)],
                                           tool_file=[], clang_tidy="tidy", build_dir=str(directory),
                                           mode="readwrite", cache_dir=str(directory / "cache"),
                                           jobs=1, config="{}")
                    with patch.object(MODULE, "combined_tool_hash", return_value="hash"), \
                            patch.object(MODULE, "load_entries", return_value={}), \
                            patch.object(MODULE.KeyBuilder, "key_for", side_effect=key_for), \
                            patch.object(MODULE, "run_process", side_effect=run_tool), \
                            contextlib.redirect_stdout(io.StringIO()):
                        MODULE.run_command(args)

                assert_tree_timeout(self, launch, KeyboardInterrupt, interrupt=True)
                self.assertEqual(len(launches), 1)
                self.assertEqual(len(key_calls), 1 if phase == "prepare" else 8)
                self.assertEqual(failures, [concurrent.futures.CancelledError])

    def test_parallel_interrupt_cancels_queue_and_reaps_descendants(self):
        visited = []

        def launch(command, directory):
            def work(index):
                visited.append(index)
                if index == 0:
                    return MODULE.run_process(command, cwd=directory, capture_output=True, timeout=1)
            MODULE.parallel_map(work, range(8), 1)

        assert_tree_timeout(self, launch, KeyboardInterrupt, interrupt=True)
        self.assertEqual(visited, [0])

    def test_timeout_terminates_and_reaps_grandchild(self):
        for driver_exits in (False, True):
            with self.subTest(driver_exits=driver_exits):
                assert_tree_timeout(self, lambda command, directory: MODULE.run_process(
                    command, cwd=directory, capture_output=True, timeout=1), driver_exits=driver_exits)

    def test_dependency_timeout_terminates_grandchild_before_cache_miss(self):
        def launch(command, directory):
            builder = MODULE.KeyBuilder("hash", "tidy", SimpleNamespace(describe=lambda *args: "tool"))
            entry = {"directory": str(directory), "arguments": command}
            with patch.object(MODULE, "dependency_command", return_value=command):
                builder.key_for(str(directory / "unit.cpp"), [entry], "{}")

        assert_tree_timeout(self, launch, MODULE.CacheMiss)


class TidyCacheTests(unittest.TestCase):
    def setUp(self):
        # Match tool identity paths even when Windows supplies an 8.3 temp path.
        self.root = Path(tempfile.mkdtemp(prefix="tidy-cache-test-")).resolve()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.src = self.root / "src"
        self.build = self.root / "build"
        self.system = self.root / "sysinc"
        self.cache = self.root / "cache"
        for directory in (self.src, self.build, self.system):
            directory.mkdir()
        self.cc = self.root / "fakecc.py"
        self.tidy = self.root / "faketidy.py"
        self.cc.write_text(FAKE_COMPILER, encoding="utf-8")
        self.tidy.write_text(FAKE_TIDY, encoding="utf-8")
        self.tool_file = self.root / "tidy.sh"
        self.tool_file.write_text("# check list v1\n", encoding="utf-8")
        self.log = self.root / "tidy.log"
        self.env = dict(os.environ, FAKE_TIDY_LOG=str(self.log))
        (self.src / "util.h").write_text("int util();\n", encoding="utf-8")
        (self.system / "sys.h").write_text("int sys();\n", encoding="utf-8")
        (self.build / "generated.h").write_text("int generated();\n", encoding="utf-8")
        self.main = self.src / "a.cpp"
        self.main.write_text(
            '#include "util.h"\n#include <sys.h>\n#include <generated.h>\nint a() { return 1; } // NOLINT\n',
            encoding="utf-8",
        )
        self.entries = [self.entry(["-DONE"])]
        self.write_db()

    def entry(self, extra, source=None):
        source = source or self.main
        arguments = [
            str(self.cc), "-isystem", str(self.system), "-I", str(self.build),
            *extra, "-o", "a.o", "-c", str(source),
        ]
        return {"directory": str(self.root), "arguments": arguments, "file": str(source)}

    def write_db(self):
        (self.build / "compile_commands.json").write_text(json.dumps(self.entries), encoding="utf-8")

    def run_cache(self, files=None, config="{}", mode="readwrite", tool_file=None):
        files = files or [self.main]
        command = [
            "run", "--build-dir", str(self.build),
            "--cache-dir", str(self.cache), "--mode", mode, "--jobs", "2",
            "--clang-tidy", str(self.tidy), "--config", config,
            "--tool-file", str(tool_file or self.tool_file), "--", *map(str, files),
        ]
        original_run = MODULE.run_process

        def run_fixture(argv, **kwargs):
            # Preserve tool-file hashing, argv, output and exit status; only the
            # fixture launch needs an interpreter instead of a POSIX shebang.
            self.assertIn(Path(argv[0]), (self.cc, self.tidy))
            return original_run([sys.executable, "-I", "-B", *argv], **kwargs)

        out, err = io.StringIO(), io.StringIO()
        with patch.dict(os.environ, self.env, clear=True), patch.object(
            MODULE, "run_process", side_effect=run_fixture
        ), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = MODULE.main(command)
        return subprocess.CompletedProcess(command, status, out.getvalue(), err.getvalue())

    def tidy_runs(self):
        if not self.log.exists():
            return 0
        return len(self.log.read_text(encoding="utf-8").splitlines())

    def assertRan(self, result, expected_total_runs):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.tidy_runs(), expected_total_runs, result.stdout + result.stderr)

    def test_second_identical_run_is_a_hit(self):
        self.assertRan(self.run_cache(), 1)
        result = self.run_cache()
        self.assertRan(result, 1)
        self.assertIn("1 hit(s)", result.stdout)

    def test_first_party_header_edit_misses(self):
        self.assertRan(self.run_cache(), 1)
        (self.src / "util.h").write_text("int util(); // edited\n", encoding="utf-8")
        self.assertRan(self.run_cache(), 2)

    def test_system_header_edit_misses(self):
        self.assertRan(self.run_cache(), 1)
        (self.system / "sys.h").write_text("int sys(); // new toolchain\n", encoding="utf-8")
        self.assertRan(self.run_cache(), 2)

    def test_generated_header_edit_misses(self):
        self.assertRan(self.run_cache(), 1)
        (self.build / "generated.h").write_text("int generated(int);\n", encoding="utf-8")
        self.assertRan(self.run_cache(), 2)

    def test_removed_nolint_comment_misses_and_can_then_fail(self):
        self.assertRan(self.run_cache(), 1)
        self.main.write_text(self.main.read_text(encoding="utf-8").replace(" // NOLINT", " // TIDY_FAIL"), encoding="utf-8")
        result = self.run_cache()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.tidy_runs(), 2)
        # The failing source must never be served from the cache afterwards.
        result = self.run_cache()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.tidy_runs(), 3)

    def test_changed_compile_arguments_miss(self):
        self.assertRan(self.run_cache(), 1)
        self.entries = [self.entry(["-DTWO"])]
        self.write_db()
        self.assertRan(self.run_cache(), 2)

    def test_changed_config_misses(self):
        self.assertRan(self.run_cache(config='{"Checks":"a"}'), 1)
        self.assertRan(self.run_cache(config='{"Checks":"b"}'), 2)
        self.assertRan(self.run_cache(config='{"Checks":"b"}'), 2)

    def test_changed_tool_script_misses(self):
        self.assertRan(self.run_cache(), 1)
        self.tool_file.write_text("# check list v2\n", encoding="utf-8")
        self.assertRan(self.run_cache(), 2)

    def test_changed_tidy_or_compiler_version_misses(self):
        self.assertRan(self.run_cache(), 1)
        self.env["FAKE_TIDY_VERSION"] = "2"
        self.assertRan(self.run_cache(), 2)
        self.env["FAKE_CC_VERSION"] = "2"
        self.assertRan(self.run_cache(), 3)

    def test_every_duplicate_compile_entry_is_part_of_the_key(self):
        self.entries = [self.entry(["-DONE"]), self.entry(["-DTWO"])]
        self.write_db()
        self.assertRan(self.run_cache(), 1)
        self.assertRan(self.run_cache(), 1)
        self.entries = [self.entry(["-DONE"]), self.entry(["-DTHREE"])]
        self.write_db()
        self.assertRan(self.run_cache(), 2)

    def test_reordered_duplicate_commands_cannot_hide_failing_first_flags(self):
        self.entries = [self.entry(["-DONE"]), self.entry(["-DTIDY_FAIL"])]
        self.write_db()
        self.assertRan(self.run_cache(), 1)
        result = self.run_cache()
        self.assertRan(result, 1)
        self.assertIn("1 hit(s)", result.stdout)
        self.entries.reverse()
        self.write_db()
        for runs in (2, 3):
            result = self.run_cache()
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("0 hit(s)", result.stdout)
            self.assertIn("fake diagnostic", result.stdout)
            self.assertEqual(self.tidy_runs(), runs)
            self.assertEqual(len(list(self.cache.rglob("*.json"))), 1)
        self.entries.reverse()
        self.write_db()
        result = self.run_cache()
        self.assertRan(result, 3)
        self.assertIn("1 hit(s)", result.stdout)

    def test_duplicate_order_changing_during_analysis_is_not_recorded(self):
        self.entries = [self.entry(["-DONE"]), self.entry(["-DTWO"])]
        self.write_db()
        replacement = self.root / "replacement_db.json"
        replacement.write_text(json.dumps(list(reversed(self.entries))), encoding="utf-8")
        self.env["FAKE_TIDY_DB_FROM"] = str(replacement)
        self.env["FAKE_TIDY_DB"] = str(self.build / "compile_commands.json")
        self.assertRan(self.run_cache(), 1)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_corrupt_or_foreign_records_are_misses(self):
        self.assertRan(self.run_cache(), 1)
        records = list(self.cache.rglob("*.json"))
        self.assertEqual(len(records), 1)
        records[0].write_text("{not json", encoding="utf-8")
        self.assertRan(self.run_cache(), 2)
        record = list(self.cache.rglob("*.json"))[0]
        data = json.loads(record.read_text(encoding="utf-8"))
        data["tidy_exit"] = 1
        record.write_text(json.dumps(data), encoding="utf-8")
        self.assertRan(self.run_cache(), 3)
        data = json.loads(record.read_text(encoding="utf-8"))
        self.assertEqual(data["tidy_exit"], 0)
        data["key"] = "0" * 64
        record.write_text(json.dumps(data), encoding="utf-8")
        self.assertRan(self.run_cache(), 4)

    def test_dependency_scan_failure_runs_tidy_and_records_nothing(self):
        self.entries = [self.entry(["-DBREAK_DEPS"])]
        self.write_db()
        self.assertRan(self.run_cache(), 1)
        self.assertRan(self.run_cache(), 2)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_missing_compile_database_entry_runs_tidy_and_records_nothing(self):
        other = self.src / "b.cpp"
        other.write_text("int b();\n", encoding="utf-8")
        result = self.run_cache(files=[other])
        self.assertRan(result, 1)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_analyzer_failure_records_nothing(self):
        self.main.write_text("int a(); // TIDY_FAIL\n", encoding="utf-8")
        result = self.run_cache()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("fake diagnostic", result.stdout)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_inputs_changing_during_analysis_are_not_recorded(self):
        self.env["FAKE_TIDY_MUTATE"] = str(self.src / "util.h")
        self.assertRan(self.run_cache(), 1)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_writeonly_mode_never_reads_but_writes(self):
        self.assertRan(self.run_cache(mode="writeonly"), 1)
        self.assertEqual(len(list(self.cache.rglob("*.json"))), 1)
        self.assertRan(self.run_cache(mode="writeonly"), 2)
        self.assertRan(self.run_cache(mode="readwrite"), 2)

    def test_header_only_visible_to_the_analyzer_is_part_of_the_key(self):
        guarded = self.src / "analyzer_only.h"
        guarded.write_text("int analyzer_only();\n", encoding="utf-8")
        self.main.write_text(
            self.main.read_text(encoding="utf-8")
            + '#ifdef __clang_analyzer__\n#include "analyzer_only.h"\n#endif\n',
            encoding="utf-8",
        )
        self.assertRan(self.run_cache(), 1)
        guarded.write_text("int analyzer_only(int);\n", encoding="utf-8")
        self.assertRan(self.run_cache(), 2)

    def test_response_files_are_never_cached(self):
        rsp = self.root / "flags.rsp"
        rsp.write_text("-DONE\n", encoding="utf-8")
        entry = self.entry([f"@{rsp}"])
        self.entries = [entry]
        self.write_db()
        self.assertRan(self.run_cache(), 1)
        self.assertRan(self.run_cache(), 2)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_same_version_banner_but_different_tool_binary_misses(self):
        self.assertRan(self.run_cache(), 1)
        with open(self.tidy, "a", encoding="utf-8") as handle:
            handle.write("# a different build with the same --version banner\n")
        self.assertRan(self.run_cache(), 2)
        with open(self.cc, "a", encoding="utf-8") as handle:
            handle.write("# a different compiler build, same banner\n")
        self.assertRan(self.run_cache(), 3)

    def test_compile_database_changing_during_analysis_is_not_recorded(self):
        replacement = self.root / "replacement_db.json"
        replacement.write_text(json.dumps([self.entry(["-DOTHER"])]), encoding="utf-8")
        self.env["FAKE_TIDY_DB_FROM"] = str(replacement)
        self.env["FAKE_TIDY_DB"] = str(self.build / "compile_commands.json")
        self.assertRan(self.run_cache(), 1)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_boolean_or_float_success_status_is_not_a_hit(self):
        self.assertRan(self.run_cache(), 1)
        record = list(self.cache.rglob("*.json"))[0]
        original = json.loads(record.read_text(encoding="utf-8"))
        runs = 1
        for bad in (False, 0.0):
            tampered = dict(original, tidy_exit=bad)
            record.write_text(json.dumps(tampered), encoding="utf-8")
            runs += 1
            self.assertRan(self.run_cache(), runs)

    def test_unsupported_options_are_never_cached(self):
        for option in ("-U__clang_analyzer__", "-ivfsoverlay", "-fmodules", "-Xclang", "--sysroot=/x"):
            with self.subTest(option=option):
                self.entries = [self.entry([option])]
                self.write_db()
                before = self.tidy_runs()
                self.assertRan(self.run_cache(), before + 1)
                self.assertRan(self.run_cache(), before + 2)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_clock_dependent_macros_are_never_cached(self):
        self.main.write_text(self.main.read_text(encoding="utf-8") + "const char* t = __TIMESTAMP__;\n", encoding="utf-8")
        self.assertRan(self.run_cache(), 1)
        self.assertRan(self.run_cache(), 2)
        self.assertEqual(list(self.cache.rglob("*.json")), [])

    def test_include_path_environment_is_part_of_the_key(self):
        self.assertRan(self.run_cache(), 1)
        self.env["CPATH"] = str(self.system)
        self.assertRan(self.run_cache(), 2)
        self.assertRan(self.run_cache(), 2)
        self.env["CPATH"] = str(self.build)
        self.assertRan(self.run_cache(), 3)

    def test_prune_removes_records_not_used_since_the_cutoff(self):
        self.assertRan(self.run_cache(), 1)
        record = list(self.cache.rglob("*.json"))[0]
        stale = self.cache / "ff"
        # The real record shards by its key's first two hex digits, so it lands
        # in "ff" one run in 256; the bucket may already exist.
        stale.mkdir(exist_ok=True)
        (stale / ("f" * 64 + ".json")).write_text("{}", encoding="utf-8")
        cutoff = time.time() + 5
        os.utime(record, (cutoff + 10, cutoff + 10))
        os.utime(stale / ("f" * 64 + ".json"), (cutoff - 100, cutoff - 100))
        result = subprocess.run(
            [sys.executable, "-I", "-B", str(SCRIPT), "prune", "--cache-dir", str(self.cache), "--before", str(cutoff)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(record.exists())
        self.assertFalse((stale / ("f" * 64 + ".json")).exists())


class CompileCommandParserTests(unittest.TestCase):
    def test_windows_compiler_paths_choose_windows_quoting(self):
        for compiler in ("g++.exe", "clang++.exe", r"C:\msys64\ucrt64\bin\g++.exe",
                         r"C:\Program Files\LLVM\bin\clang++.exe", r"C:\tools\g++"):
            arguments = [compiler, "-c", r"C:\source dir\unit.cpp", '-DVALUE=a"b',
                         r'-DPATH=C:\some\path\\', ""]
            with self.subTest(compiler=compiler):
                self.assertEqual(MODULE.entry_arguments({"directory": r"C:\build",
                                                        "command": subprocess.list2cmdline(arguments)}),
                                 arguments)

    def test_windows_embedded_quotes_and_backslash_runs(self):
        cases = [
            (r'"a b" c', ["a b", "c"]),
            (r'a\\\b d"e f"g h', [r'a\\\b', "de fg", "h"]),
            (r'a\\\"b c d', [r'a\"b', "c", "d"]),
            (r'a\\\\"b c" d e', [r'a\\b c', "d", "e"]),
            ('"" "unterminated space', ["", "unterminated space"]),
            ('"a"""b', ['a"b']),
        ]
        for tail, expected in cases:
            with self.subTest(tail=tail):
                self.assertEqual(MODULE.entry_arguments({"command_platform": "windows",
                                                        "command": "g++.exe " + tail}),
                                 ["g++.exe", *expected])

    def test_posix_commands_keep_shell_quoting_and_escapes(self):
        for compiler in ("/usr/bin/g++", "g++.exe", "clang++.exe", "/ucrt64/bin/g++.exe"):
            with self.subTest(compiler=compiler):
                self.assertEqual(MODULE.entry_arguments({"directory": "/build",
                    "command": compiler + r''' '-DVALUE=a"b' -Iwith\ space -c 'unit file.cpp' '''}),
                    [compiler, '-DVALUE=a"b', "-Iwith space", "-c", "unit file.cpp"])

    def test_quoting_context_overrides_host_and_compiler_suffix(self):
        with patch.object(MODULE.os, "name", "nt"):
            self.assertEqual(MODULE.entry_arguments({"command": "g++.exe -c 'unit file.cpp'"}),
                             ["g++.exe", "-c", "unit file.cpp"])
            self.assertEqual(MODULE.entry_arguments({"directory": "/msys/build",
                "command": "g++.exe -c 'unit file.cpp'"}), ["g++.exe", "-c", "unit file.cpp"])
        for directory in (r"C:\build", r"\\server\share\build"):
            self.assertEqual(MODULE.entry_arguments({"directory": directory,
                "command": 'g++.exe -c "unit file.cpp"'}), ["g++.exe", "-c", "unit file.cpp"])
        arguments = [r"C:\bin\g++.exe", "-c", "unit file.cpp"]
        self.assertEqual(MODULE.entry_arguments({"directory": "/msys/build",
            "command": shlex.join(arguments)}), arguments)
        self.assertEqual(MODULE.entry_arguments({"directory": r"C:\build", "command_platform": "posix",
            "command": "g++.exe -c 'unit file.cpp'"}), ["g++.exe", "-c", "unit file.cpp"])
        self.assertEqual(MODULE.entry_arguments({"directory": "/build", "command_platform": "windows",
            "command": subprocess.list2cmdline(arguments)}), arguments)

    def test_native_driver_paths_without_directory_choose_windows_quoting(self):
        for prefix in ([], [r"C:\tools\ccache.exe"], ["sccache.exe"]):
            arguments = [*prefix, r"C:\bin\g++.exe", "-c", "unit file.cpp"]
            self.assertEqual(MODULE.entry_arguments({"command": subprocess.list2cmdline(arguments)}),
                             arguments)

    def test_native_working_directory_preserves_posix_producer_quoting(self):
        for directory in (r"C:\build with spaces", r"\\server\share\build"):
            for compiler in ("/usr/bin/g++", "g++.exe", "g++-14", "clang++-18",
                             r"C:\msys64\ucrt64\bin\g++.exe"):
                arguments = [compiler, "-I", r"C:\headers with spaces", "-c",
                             r"C:\changed_tus\unit file.cpp", '-DVALUE=a"b', "-DNAME=O'Brien"]
                with self.subTest(directory=directory, compiler=compiler):
                    self.assertEqual(MODULE.entry_arguments({"directory": directory,
                        "command": shlex.join(arguments)}), arguments)

    def test_native_quotes_can_contain_literal_apostrophes(self):
        arguments = [r"C:\O'Brien\g++.exe", "-c", r"C:\unit file.cpp",
                     "-DNAME=one 'two' three"]
        for directory in (r"C:\build", r"\\server\share\build"):
            with self.subTest(directory=directory):
                self.assertEqual(MODULE.entry_arguments({"directory": directory,
                    "command": subprocess.list2cmdline(arguments)}), arguments)

    def test_arguments_array_is_never_reparsed(self):
        arguments = [r"C:\bin\g++.exe", '"quoted value"', r"back\slash", ""]
        self.assertEqual(MODULE.entry_arguments({"arguments": arguments, "command": "ignored"}),
                         arguments)


class ToolIdentityTests(unittest.TestCase):
    def test_dependency_hashing_rejects_reparse_paths_and_hashes_safety_code(self):
        with patch.object(MODULE._PATH_SAFETY, "WINDOWS", True), \
                patch.object(MODULE._PATH_SAFETY, "is_reparse_point", return_value=True):
            with self.assertRaisesRegex(MODULE.CacheMiss, "reparse point"):
                MODULE.KeyBuilder._hash_dependency(str(self.tool))
            with self.assertRaisesRegex(OSError, "reparse point"):
                MODULE.sha256_file(str(self.tool))
        safety_content = "before"

        def content(path):
            return safety_content if path == MODULE._PATH_SPEC.origin else "unchanged"

        with patch.object(MODULE, "sha256_file", side_effect=content):
            before = MODULE.combined_tool_hash([])
            safety_content = "after"
            self.assertNotEqual(MODULE.combined_tool_hash([]), before)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tool-identity-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tool = self.root / "compiler"
        self.tick = 10_000_000_000
        self.write_tool(b"old tool")
        self.tools = MODULE.ToolIdentity()

    def write_tool(self, content):
        self.tool.write_bytes(content)
        os.utime(self.tool, ns=(self.tick, self.tick))

    def test_same_tick_rewrite_is_detected_on_delayed_reuse(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        with patch.object(MODULE, "run_process", return_value=result) as version, \
                patch.object(MODULE.time, "time_ns", return_value=self.tick + 500_000_000) as clock:
            before = self.tools.describe(str(self.tool), str(self.root))
            self.write_tool(b"new tool")
            clock.return_value = self.tick + 2_000_000_001
            after = self.tools.describe(str(self.tool), str(self.root))
            self.assertNotEqual(before, after)
            self.assertIn(MODULE.sha256_bytes(b"new tool"), after)
            self.assertEqual(version.call_count, 2)
            self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), after)
            self.assertEqual(version.call_count, 2)

    def test_identity_is_revalidated_until_tick_closes(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        with patch.object(MODULE, "run_process", return_value=result) as version, \
                patch.object(MODULE, "sha256_file", wraps=MODULE.sha256_file) as content, \
                patch.object(MODULE.time, "time_ns", return_value=self.tick + 500_000_000) as clock:
            before = self.tools.describe(str(self.tool), str(self.root))
            self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), before)
            self.assertEqual(content.call_count, 2)
            self.assertEqual(version.call_count, 1)
            clock.return_value = self.tick + 2_000_000_000
            self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), before)
            self.assertEqual(content.call_count, 3)
            self.assertEqual(version.call_count, 1)
            self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), before)
            self.assertEqual(content.call_count, 3)
            self.assertEqual(version.call_count, 1)

    def test_identical_content_with_new_fingerprint_reuses_version(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\r\n", b"")
        with patch.object(MODULE, "run_process", return_value=result) as version, \
                patch.object(MODULE, "sha256_file", wraps=MODULE.sha256_file) as content:
            before = self.tools.describe(str(self.tool), str(self.root))
            # Windows file times have 100ns resolution: a 1ns change rounds
            # back to the old fingerprint and correctly takes the cache hit.
            os.utime(self.tool, ns=(self.tick + 1_000_000_000, self.tick + 1_000_000_000))
            self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), before)
            self.assertEqual(content.call_count, 2)
            self.assertEqual(version.call_count, 1)

    def test_windows_tool_spellings_share_identity_in_command_directory(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        info = SimpleNamespace(st_mode=stat.S_IFREG, st_dev=1, st_ino=1,
                               st_size=8, st_mtime_ns=self.tick)
        with patch.object(MODULE.os, "path", ntpath), \
                patch.object(MODULE.os, "sep", "\\"), \
                patch.object(ntpath, "isfile", return_value=True), \
                patch.object(ntpath, "realpath", side_effect=ntpath.normpath), \
                patch.object(MODULE.os, "stat", return_value=info), \
                patch.object(MODULE, "sha256_file", return_value="content") as content, \
                patch.object(MODULE, "run_process", return_value=result) as version, \
                patch.object(MODULE.shutil, "which") as which, \
                patch.object(MODULE.time, "time_ns", return_value=self.tick + 2_000_000_000):
            before = self.tools.describe("../bin/CLANG++.EXE", r"C:\toolchain\build")
            for executable in (r"..\bin\clang++.exe", "C:/toolchain/bin/clang++.exe"):
                self.assertEqual(self.tools.describe(executable, r"C:\toolchain\build"), before)
            which.assert_not_called()
            self.assertEqual(content.call_count, 1)
            self.assertEqual(version.call_count, 1)
            self.assertEqual(version.call_args.args[0], [r"c:\toolchain\bin\clang++.exe", "--version"])

    def concurrent_descriptions(self, failure=None):
        started, release, joined = threading.Event(), threading.Event(), threading.Event()
        result_lock = threading.Lock()
        waiters, outcomes = [], []
        original_hash = MODULE.sha256_file

        class ObservedFuture(concurrent.futures.Future):
            def result(self, timeout=None):
                with result_lock:
                    waiters.append(self)
                    if len(waiters) == 3:
                        joined.set()
                return super().result(timeout)

        def hash_tool(path):
            started.set()
            if not release.wait(5):
                raise OSError("identity check did not finish")
            if failure is not None:
                raise failure
            return original_hash(path)

        def describe():
            try:
                outcome = self.tools.describe(str(self.tool), str(self.root))
            except MODULE.CacheMiss as error:
                outcome = error
            with result_lock:
                outcomes.append(outcome)

        threads = [threading.Thread(target=describe) for _ in range(4)]
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        with patch.object(MODULE.concurrent.futures, "Future", ObservedFuture), \
                patch.object(MODULE, "sha256_file", side_effect=hash_tool) as content, \
                patch.object(MODULE, "run_process", return_value=result) as version, \
                patch.object(MODULE.time, "time_ns", return_value=self.tick + 500_000_000):
            threads[0].start()
            try:
                self.assertTrue(started.wait(5))
                for thread in threads[1:]:
                    thread.start()
                self.assertTrue(joined.wait(5))
            finally:
                release.set()
                for thread in threads:
                    if thread.ident is not None:
                        thread.join(5)
                        self.assertFalse(thread.is_alive())
            self.assertEqual(content.call_count, 1)
            self.assertEqual(version.call_count, 0 if failure is not None else 1)
        self.assertEqual(len(outcomes), 4)
        self.assertEqual(self.tools._pending, {})
        return outcomes

    def test_concurrent_identity_checks_share_hash_and_version(self):
        outcomes = self.concurrent_descriptions()
        self.assertTrue(all(isinstance(outcome, str) for outcome in outcomes))
        self.assertEqual(len(set(outcomes)), 1)

    def test_failed_concurrent_identity_check_refuses_all_waiters_and_retries(self):
        outcomes = self.concurrent_descriptions(PermissionError("unreadable executable"))
        self.assertTrue(all(isinstance(outcome, MODULE.CacheMiss) for outcome in outcomes))
        self.assertTrue(all("unreadable executable" in str(outcome) for outcome in outcomes))
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        with patch.object(MODULE, "run_process", return_value=result) as version:
            self.assertIn(MODULE.sha256_bytes(b"old tool"),
                          self.tools.describe(str(self.tool), str(self.root)))
            self.assertEqual(version.call_count, 1)

    def test_revalidation_failures_cannot_reuse_stale_identity(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        for failure in (PermissionError("unreadable"), OSError("partial read")):
            with self.subTest(failure=failure), \
                    patch.object(MODULE, "run_process", return_value=result) as version, \
                    patch.object(MODULE.time, "time_ns", return_value=self.tick + 500_000_000):
                self.tools = MODULE.ToolIdentity()
                before = self.tools.describe(str(self.tool), str(self.root))
                with patch.object(MODULE, "sha256_file", side_effect=failure), \
                        self.assertRaises(MODULE.CacheMiss):
                    self.tools.describe(str(self.tool), str(self.root))
                self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), before)
                self.assertEqual(version.call_count, 1)

    def test_version_failures_are_not_cached_and_can_retry(self):
        success = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        failures = (subprocess.TimeoutExpired("tool", 60), OSError("cannot launch"),
                    subprocess.CompletedProcess([], 1, b"partial version", b"error"),
                    subprocess.CompletedProcess([], 0, b"", b""))
        for failure in failures:
            with self.subTest(failure=failure), \
                    patch.object(MODULE.time, "time_ns", return_value=self.tick + 500_000_000):
                self.tools = MODULE.ToolIdentity()
                self.write_tool(b"old tool")
                with patch.object(MODULE, "run_process", return_value=success):
                    before = self.tools.describe(str(self.tool), str(self.root))
                self.write_tool(b"new tool")
                with patch.object(MODULE, "run_process", side_effect=[failure, success]) as version:
                    with self.assertRaises(MODULE.CacheMiss):
                        self.tools.describe(str(self.tool), str(self.root))
                    after = self.tools.describe(str(self.tool), str(self.root))
                    self.assertNotEqual(after, before)
                    self.assertIn(MODULE.sha256_bytes(b"new tool"), after)
                    self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), after)
                    self.assertEqual(version.call_count, 2)

    def test_identity_observed_after_tick_closes_uses_fingerprint(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        with patch.object(MODULE, "run_process", return_value=result) as version, \
                patch.object(MODULE.time, "time_ns", return_value=self.tick + 2_000_000_000):
            before = self.tools.describe(str(self.tool), str(self.root))
            self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), before)
            self.assertEqual(version.call_count, 1)

    def test_device_and_inode_changes_invalidate_trusted_identity(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        for field in ("st_dev", "st_ino"):
            with self.subTest(field=field):
                tools = MODULE.ToolIdentity()
                self.write_tool(b"old tool")
                info = SimpleNamespace(st_mode=stat.S_IFREG, st_dev=1, st_ino=1,
                                       st_size=8, st_mtime_ns=self.tick)
                with patch.object(MODULE.os, "stat", return_value=info), \
                        patch.object(MODULE, "run_process", return_value=result) as version, \
                        patch.object(MODULE.time, "time_ns", return_value=self.tick + 2_000_000_000):
                    before = tools.describe(str(self.tool), str(self.root))
                    self.write_tool(b"new tool")
                    setattr(info, field, 2)
                    after = tools.describe(str(self.tool), str(self.root))
                    self.assertNotEqual(before, after)
                    self.assertIn(MODULE.sha256_bytes(b"new tool"), after)
                    self.assertEqual(version.call_count, 2)

    def test_kind_change_after_path_check_cannot_reuse_trusted_identity(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        info = SimpleNamespace(st_mode=stat.S_IFREG, st_dev=1, st_ino=1,
                               st_size=8, st_mtime_ns=self.tick)
        with patch.object(MODULE.os.path, "isfile", return_value=True), \
                patch.object(MODULE.os, "stat", return_value=info), \
                patch.object(MODULE, "run_process", return_value=result) as version, \
                patch.object(MODULE.time, "time_ns", return_value=self.tick + 2_000_000_000):
            self.tools.describe(str(self.tool), str(self.root))
            # Model a replacement between the path check and the stat call.
            info.st_mode = stat.S_IFDIR
            with self.assertRaisesRegex(MODULE.CacheMiss, "not a regular file"):
                self.tools.describe(str(self.tool), str(self.root))
            self.assertEqual(version.call_count, 1)


class TidyCachePathTests(unittest.TestCase):
    def test_fixture_paths_match_resolved_tool_paths(self):
        parent = tempfile.TemporaryDirectory(prefix="tidy-cache-parent-")
        self.addCleanup(parent.cleanup)
        root = Path(parent.name) / "fixture"
        root.mkdir()
        # A second spelling exercises normalization without Windows-only APIs.
        alias = root / ".." / root.name
        fixture = TidyCacheTests("test_second_identical_run_is_a_hit")
        self.addCleanup(fixture.doCleanups)
        with patch.object(tempfile, "mkdtemp", return_value=str(alias)):
            fixture.setUp()
        fixture.test_second_identical_run_is_a_hit()


class MakeDependencyParserTests(unittest.TestCase):
    def test_parses_continuations_escaped_spaces_and_dollars(self):
        text = "main.o: /a/b.cpp \\\n /a/with\\ space.h /a/dollar$$.h\n"
        self.assertEqual(
            MODULE.parse_make_deps(text),
            ["/a/b.cpp", "/a/with space.h", "/a/dollar$.h"],
        )

    def test_rejects_garbage_and_multiple_rules(self):
        for text in ("", "nothing here", "main.o:\n", "a.o: x.c\nb.o: y.c\n"):
            with self.assertRaises(MODULE.CacheMiss, msg=text):
                MODULE.parse_make_deps(text)

    def test_rejects_tab_in_file_name(self):
        with self.assertRaises(MODULE.CacheMiss):
            MODULE.parse_make_deps("main.o: /a/with\ttab.h /a/b.cpp\n")

    def test_dependency_command_strips_outputs_and_adds_m(self):
        command = MODULE.dependency_command(
            ["cc", "-DX", "-MD", "-MT", "t", "-MF", "t.d", "-o", "a.o", "-c", "a.cpp"]
        )
        self.assertEqual(command, ["cc", "-D__clang_analyzer__", "-DX", "a.cpp", "-M"])


if __name__ == "__main__":
    unittest.main()

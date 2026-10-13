#!/usr/bin/env python3
"""Contract tests for repeat_qualification.py using a small fake child executable.

Run with `python3 -I test_repeat_qualification.py`. The harness is loaded from its file path,
so the test does not depend on the current directory or on sys.path.
"""

from __future__ import annotations

import errno
import hashlib
import importlib.util
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
from types import ModuleType
from unittest import mock

SCRIPT = Path(__file__).resolve().with_name("repeat_qualification.py")
FIXTURE_DIAGNOSTIC_ENV = "LUMINUMBRA_RQ_CONTRACT_DIAGNOSTIC_DIR"
FIXTURE_MAX_WAIT_SECONDS = 120.0
FIXTURE_STARTUP_ALLOWANCE_SECONDS = 15.0

REGISTRY_ROOT = r"HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps"


def load_harness() -> ModuleType:
    spec = importlib.util.spec_from_file_location("repeat_qualification", SCRIPT)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {SCRIPT}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rq = load_harness()

FAKE_CHILD_SOURCE = r'''
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

mode = "pass"
json_path = None
for argument in sys.argv[1:]:
    if argument.startswith("--fake="):
        mode = argument.split("=", 1)[1]
    elif argument.startswith("--gtest_output=json:"):
        json_path = argument.split(":", 1)[1]

crash_dir = Path(os.environ["LUMINUMBRA_CRASH_DIR"])
iteration = int(crash_dir.parent.name.split("-", 1)[1])
print(f"fake child iteration={iteration} mode {mode}", flush=True)

kind, _, target = mode.partition("@")
target_iteration = int(target) if target else None
if target_iteration is not None and iteration != target_iteration:
    kind = "pass"


def write_gtest_json(tests, failures, result):
    if json_path is None:
        return
    cases = [{"name": "Case", "status": "RUN", "result": result} for _ in range(tests)]
    suites = [{"name": "Suite", "tests": tests, "failures": failures, "testsuite": cases}] if cases else []
    report = {"tests": tests, "failures": failures, "disabled": 0, "errors": 0,
              "name": "AllTests", "testsuites": suites}
    Path(json_path).parent.mkdir(parents=True, exist_ok=True)
    Path(json_path).write_text(json.dumps(report), encoding="utf-8")


if kind == "exit3":
    sys.exit(3)
if kind == "kill":
    os.kill(os.getpid(), signal.SIGKILL)
if kind == "abort":
    os.kill(os.getpid(), signal.SIGABRT)
if kind == "segv":
    os.kill(os.getpid(), signal.SIGSEGV)
if kind == "segv_core":
    # Stands in for the kernel: a core named core.<pid> appears in the child's working directory.
    Path(f"core.{os.getpid()}").write_bytes(b"core")
    os.kill(os.getpid(), signal.SIGSEGV)
if kind == "hang":
    grandchild = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(600)"])
    (crash_dir / "grandchild.pid").write_text(str(grandchild.pid))
    if "LUMINUMBRA_FIXTURE_READY_FILE" in os.environ:
        Path(os.environ["LUMINUMBRA_FIXTURE_READY_FILE"]).write_text("ready", encoding="utf-8")
    time.sleep(600)
if kind == "probe":
    receipt = json.loads(Path(os.environ["LUMINUMBRA_TEST_RECEIPT"]).read_text())
    print(f"receipt_iterations={len(receipt['iterations'])}", flush=True)
    write_gtest_json(1, 0, "COMPLETED")
    sys.exit(0)
if kind == "nojson":
    sys.exit(0)
if kind == "unmatched":
    write_gtest_json(0, 0, "COMPLETED")
    sys.exit(0)
if kind == "skipped":
    write_gtest_json(1, 0, "SKIPPED")
    sys.exit(0)
if kind == "failures":
    write_gtest_json(1, 1, "COMPLETED")
    sys.exit(0)
if kind == "envcheck":
    leaked = [name for name in ("GTEST_SHARD_INDEX", "GTEST_TOTAL_SHARDS", "GTEST_REPEAT",
                                "GTEST_FILTER", "GTEST_ALSO_RUN_DISABLED_TESTS") if name in os.environ]
    if leaked:
        print("leaked " + " ".join(leaked), flush=True)
        sys.exit(6)
    write_gtest_json(1, 0, "COMPLETED")
    sys.exit(0)
write_gtest_json(1, 0, "COMPLETED")
sys.exit(0)
'''

POSIX_LAUNCHER = "#!{python}\n"
WINDOWS_LAUNCHER = '@"{python}" -I "{child}" %*\r\n'


def posix_crash_capture_ready(cwd: Path) -> bool:
    """True when this host's core pattern and core limit let the harness collect cores."""
    if os.name == "nt":
        return False
    pattern = rq.read_core_pattern()
    current = rq.resource.getrlimit(rq.resource.RLIMIT_CORE)
    child_soft = rq.core_limit_target(current)[0]
    return not rq.posix_preflight_problems(
        {
            "core_pattern": pattern,
            "core_pattern_piped": bool(pattern and pattern.startswith("|")),
            "core_directory": rq.core_directory_for_pattern(pattern, cwd),
        },
        child_soft,
    )


def pid_alive(pid: int, timeout: float = 30) -> bool:
    if os.name == "nt":
        listing = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        ).stdout
        return f'"{pid}"' in listing
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    stat = Path(f"/proc/{pid}/stat")
    if stat.is_file():
        # A zombie still answers kill(pid, 0) but is already dead.
        fields = stat.read_text().rsplit(")", 1)[-1].split()
        if fields and fields[0] == "Z":
            return False
    return True


def terminate_fixture_tree(process) -> list[str]:
    """The outer fixture owns iterations that start separate POSIX sessions."""
    errors = []
    if os.name == "posix":
        try:
            # Freeze the owning harness before inspecting descendants: otherwise
            # a killed iteration can wake it and launch a new separate session.
            if process.poll() is None:
                os.kill(process.pid, signal.SIGSTOP)
                stopped_deadline = time.monotonic() + rq.CLEANUP_TIMEOUT_SECONDS
                while True:
                    waited_pid, status = os.waitpid(process.pid, os.WNOHANG | os.WUNTRACED)
                    if waited_pid:
                        if os.WIFSTOPPED(status):
                            break
                        if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                            process.returncode = os.waitstatus_to_exitcode(status)
                            break
                    if time.monotonic() >= stopped_deadline:
                        raise OSError("owned fixture did not stop within cleanup deadline")
                    time.sleep(0.01)
            listing = subprocess.run(
                ["ps", "-e", "-o", "pid=,ppid=,pgid="], capture_output=True,
                encoding="utf-8", check=True, timeout=rq.CLEANUP_TIMEOUT_SECONDS,
            ).stdout
            rows = [tuple(map(int, line.split())) for line in listing.splitlines() if line.strip()]
            descendants = {process.pid}
            while True:
                found = {pid for pid, parent, _group in rows if parent in descendants}
                if found <= descendants:
                    break
                descendants.update(found)
            groups = {group for pid, _parent, group in rows if pid in descendants
                      and group in descendants and group != os.getpgrp()}
            # Iteration session leaders and their descendants are discovered
            # while the owning harness is still alive; never scan command text.
            for group in groups - {process.pid}:
                try:
                    os.killpg(group, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                except OSError as exc:
                    errors.append(f"fixture descendant group {group}: {exc}")
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            errors.append(f"fixture descendant discovery: {type(exc).__name__}: {exc}")
    errors.extend(rq.terminate_process_tree(process))
    return errors


class HarnessTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="repeat-qualification-test-")
        self.root = Path(self._tmp.name)
        self.work = self.root / "work"
        self.work.mkdir()
        # The receipt directory contains a space so the output path is exercised with spaces.
        self.receipt = self.root / "out dir" / "receipt.json"
        self.exe = self._write_fake_child()
        self._fixture_sequence = 0

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_fake_child(self) -> Path:
        # The directory name contains a space so the launcher path is exercised with spaces.
        bin_dir = self.root / "bin dir"
        bin_dir.mkdir()
        child_py = bin_dir / "fake_child.py"
        with child_py.open("w", encoding="utf-8", newline="\n") as handle:
            handle.write(FAKE_CHILD_SOURCE)
        if os.name == "nt":
            launcher = bin_dir / "fake_child.cmd"
            with launcher.open("w", encoding="utf-8", newline="") as handle:
                handle.write(WINDOWS_LAUNCHER.format(python=sys.executable, child=child_py))
        else:
            launcher = bin_dir / "fake_child"
            with launcher.open("w", encoding="utf-8", newline="\n") as handle:
                handle.write(POSIX_LAUNCHER.format(python=sys.executable) + FAKE_CHILD_SOURCE)
            launcher.chmod(0o755)
        return launcher

    def base_args(self) -> list[str]:
        args = [
            "--exe",
            str(self.exe),
            "--gtest-filter",
            "Fake.Filter",
            "--cwd",
            str(self.work),
            "--receipt",
            str(self.receipt),
        ]
        if os.name == "nt":
            # Dump preflight is covered by the unit tests below; the run itself needs no dumps.
            args.append("--allow-no-dumps")
        elif not posix_crash_capture_ready(self.work):
            # Hosts whose core_pattern is piped or whose core limit is zero cannot collect cores.
            # The fail-closed preflight is covered by the unit tests; these runs record the override.
            args.append("--allow-no-dumps")
        return args

    def preserve_fixture_failure(self, diagnostic_dir: Path | None = None) -> tuple[Path | None, list[str]]:
        """Keep synthetic fixture evidence outside TemporaryDirectory cleanup."""
        errors = []
        base = diagnostic_dir or os.environ.get(FIXTURE_DIAGNOSTIC_ENV)
        try:
            parent = Path(base).resolve() if base else Path(tempfile.gettempdir()).resolve()
            # A descendant would recursively copy the copy itself.
            if parent == self.root.resolve() or self.root.resolve() in parent.parents:
                raise ValueError("fixture diagnostic directory must be outside its fixture")
            parent.mkdir(parents=True, exist_ok=True)
            retained = Path(tempfile.mkdtemp(prefix="fixture-watchdog-", dir=parent))
            shutil.copytree(self.root, retained / "fixture")
            return retained, errors
        except (OSError, ValueError) as exc:
            errors.append(f"fixture preservation: {type(exc).__name__}: {exc}")
            return None, errors

    def run_raw(self, args: list[str], extra_env: dict[str, str] | None = None,
                timeout: float = 120, ready_file: Path | None = None,
                diagnostic_dir: Path | None = None):
        env = dict(os.environ)
        env.update(extra_env or {})
        command = [sys.executable, "-I", "-X", "utf8", str(SCRIPT), *args]
        self._fixture_sequence += 1
        logs = self.root / "outer-runs" / f"run-{self._fixture_sequence}"
        logs.mkdir(parents=True)
        stdout_path, stderr_path = logs / "stdout.log", logs / "stderr.log"
        options = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt"
                   else {"start_new_session": True})
        expired = False
        startup_failure = None
        cleanup_errors = []
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            # File-backed streams cannot retain communicate() pipe readers after
            # a fixture descendant outlives the direct harness process.
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=stdout,
                                       stderr=stderr, env=env, **options)
            try:
                if ready_file is not None:
                    startup_deadline = time.monotonic() + 30
                    while not ready_file.is_file():
                        if process.poll() is not None:
                            startup_failure = "fixture exited before readiness"
                            break
                        if time.monotonic() >= startup_deadline:
                            startup_failure = "fixture startup watchdog expired after 30s"
                            break
                        time.sleep(0.05)
                    if startup_failure is not None:
                        expired = True
                        cleanup_errors = terminate_fixture_tree(process)
                if not expired:
                    process.wait(timeout=min(timeout, FIXTURE_MAX_WAIT_SECONDS))
            except subprocess.TimeoutExpired:
                expired = True
                # Kill separate iteration groups and the outer tree while owned.
                cleanup_errors = terminate_fixture_tree(process)
        output = stdout_path.read_text(encoding="utf-8", errors="replace")
        error = stderr_path.read_text(encoding="utf-8", errors="replace")
        if expired:
            retained, preservation_errors = self.preserve_fixture_failure(diagnostic_dir)
            label = startup_failure or f"fixture watchdog expired after {min(timeout, FIXTURE_MAX_WAIT_SECONDS)}s"
            self.fail(
                f"{label}; "
                f"harness PID={process.pid}; cleanup={cleanup_errors}; "
                f"retained={retained}; preservation={preservation_errors}\n"
                f"stdout:\n{output}\nstderr:\n{error}"
            )
        return subprocess.CompletedProcess(command, process.returncode, output, error)

    def run_script(
        self,
        *,
        count: int,
        mode: str,
        timeout: float = 30,
        extra_env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        args = [
            *self.base_args(),
            "--count",
            str(count),
            "--timeout-seconds",
            str(timeout),
            "--",
            f"--fake={mode}",
        ]
        # Budget the declared iteration count plus bounded tree cleanup and
        # startup, with a hard fixture ceiling below CTest's suite deadline.
        outer_timeout = min(FIXTURE_MAX_WAIT_SECONDS,
                            count * timeout + 3 * rq.CLEANUP_TIMEOUT_SECONDS +
                            FIXTURE_STARTUP_ALLOWANCE_SECONDS)
        return self.run_raw(args, extra_env=extra_env, timeout=outer_timeout)

    def load_receipt(self) -> dict:
        return json.loads(self.receipt.read_text(encoding="utf-8"))

    def run_dir(self) -> Path:
        runs = [p for p in self.receipt.parent.resolve().iterdir() if p.is_dir() and p.name.startswith("run-")]
        self.assertEqual(len(runs), 1, runs)
        return runs[0]

class RepeatQualificationTest(HarnessTestCase):
    def test_all_iterations_pass(self) -> None:
        result = self.run_script(count=3, mode="pass")
        self.assertEqual(result.returncode, 0, result.stderr)

        receipt = self.load_receipt()
        self.assertEqual(receipt["schema"], "luminumbra.repeat_qualification.v1")
        self.assertEqual(receipt["label"], "process repetitions")
        self.assertEqual(receipt["status"], "passed")
        self.assertEqual(len(receipt["iterations"]), 3)
        self.assertEqual(receipt["exit_code_histogram"], {"exit:0": 3})
        self.assertIsNone(receipt["first_failure"])
        self.assertEqual(receipt["failure_count"], 0)
        self.assertIsNotNone(receipt["end_time"])
        self.assertEqual(
            receipt["exe"]["sha256"], hashlib.sha256(self.exe.read_bytes()).hexdigest()
        )
        self.assertIn("--gtest_catch_exceptions=0", receipt["command"])
        self.assertIn("scrubbed_gtest_env", receipt["environment"])
        run_dir = self.run_dir()
        self.assertEqual(receipt["run_dir"], str(run_dir))
        for index in range(1, 4):
            log = run_dir / f"iter-{index}.log"
            self.assertTrue(log.is_file(), log)
            self.assertIn(f"fake child iteration={index}", log.read_text(encoding="utf-8"))
            gtest_json = Path(receipt["iterations"][index - 1]["gtest_json"])
            self.assertEqual(json.loads(gtest_json.read_text())["tests"], 1)

    def test_nonzero_exit_is_a_failure(self) -> None:
        result = self.run_script(count=3, mode="exit3@2")
        self.assertEqual(result.returncode, 1)

        receipt = self.load_receipt()
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(receipt["exit_code_histogram"], {"exit:0": 2, "exit:3": 1})
        self.assertEqual(receipt["failure_count"], 1)
        self.assertEqual(receipt["iterations"][1]["exit_code"], 3)
        self.assertTrue(receipt["iterations"][1]["failed"])
        first = receipt["first_failure"]
        self.assertEqual(first["index"], 2)
        self.assertEqual(first["log_path"], str(self.run_dir() / "iter-2.log"))

    def test_timeout_kills_the_whole_process_tree(self) -> None:
        result = self.run_script(count=1, mode="hang@1", timeout=5)
        self.assertEqual(result.returncode, 1)

        receipt = self.load_receipt()
        iteration = receipt["iterations"][0]
        self.assertTrue(iteration["timed_out"])
        self.assertEqual(iteration["cleanup_errors"], [])
        self.assertEqual(receipt["exit_code_histogram"], {"timeout": 1})
        self.assertEqual(receipt["first_failure"]["index"], 1)

        grandchild_file = Path(iteration["crash_dir"]) / "grandchild.pid"
        self.assertTrue(grandchild_file.is_file())
        grandchild_pid = int(grandchild_file.read_text())
        deadline = time.monotonic() + 5
        while pid_alive(grandchild_pid) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(pid_alive(grandchild_pid), "grandchild survived the timeout kill")

    def test_receipt_is_rewritten_after_each_iteration(self) -> None:
        result = self.run_script(
            count=3, mode="probe", extra_env={"LUMINUMBRA_TEST_RECEIPT": str(self.receipt)}
        )
        self.assertEqual(result.returncode, 0, result.stderr)

        # At the start of iteration n the receipt must already hold the n-1 finished iterations.
        for index in range(1, 4):
            log = (self.run_dir() / f"iter-{index}.log").read_text(encoding="utf-8")
            self.assertIn(f"receipt_iterations={index - 1}", log)
        self.assertEqual(len(self.load_receipt()["iterations"]), 3)

    def test_missing_executable_is_rejected(self) -> None:
        self.exe.unlink()
        result = self.run_script(count=1, mode="pass")
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.receipt.exists())

    def test_unmatched_filter_is_a_failure(self) -> None:
        result = self.run_script(count=1, mode="unmatched")
        self.assertEqual(result.returncode, 1)
        receipt = self.load_receipt()
        self.assertEqual(receipt["status"], "failed")
        self.assertEqual(receipt["exit_code_histogram"], {"gtest_invalid": 1})
        self.assertIn("expected 1 executed test", receipt["iterations"][0]["failure_reason"])

    def test_skipped_test_is_a_failure(self) -> None:
        result = self.run_script(count=1, mode="skipped")
        self.assertEqual(result.returncode, 1)
        receipt = self.load_receipt()
        self.assertEqual(receipt["exit_code_histogram"], {"gtest_invalid": 1})
        self.assertIn("skipped", receipt["iterations"][0]["failure_reason"])

    def test_failed_gtest_report_is_a_failure(self) -> None:
        result = self.run_script(count=1, mode="failures")
        self.assertEqual(result.returncode, 1)
        self.assertIn("failures=1", self.load_receipt()["iterations"][0]["failure_reason"])

    def test_missing_gtest_json_is_a_failure(self) -> None:
        result = self.run_script(count=1, mode="nojson")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not written", self.load_receipt()["iterations"][0]["failure_reason"])

    def test_conflicting_gtest_environment_is_scrubbed(self) -> None:
        polluted = {
            "GTEST_REPEAT": "0",
            "GTEST_FILTER": "Other.*",
            "GTEST_SHARD_INDEX": "0",
            "GTEST_TOTAL_SHARDS": "2",
            "GTEST_ALSO_RUN_DISABLED_TESTS": "1",
        }
        result = self.run_script(count=1, mode="envcheck", extra_env=polluted)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        receipt = self.load_receipt()
        self.assertEqual(receipt["status"], "passed")
        self.assertEqual(
            sorted(receipt["environment"]["scrubbed_gtest_env"]),
            sorted(polluted),
        )

    def test_launch_error_is_recorded_and_receipt_finalized(self) -> None:
        # Keep the valid launcher, but prevent its working directory from existing.
        # A file ancestor also prevents POSIX core preflight from creating it.
        blocker = self.root / "cwd-blocker"
        blocker.write_bytes(b"not a directory")
        self.work = blocker / "missing-cwd"
        args = [*self.base_args(), "--count", "2", "--timeout-seconds", "30"]
        if "--allow-no-dumps" not in args:
            args.append("--allow-no-dumps")
        result = self.run_raw([*args, "--", "--fake=pass"], timeout=90)
        self.assertEqual(result.returncode, 1, result.stderr)

        receipt = self.load_receipt()
        self.assertEqual(receipt["status"], "failed")
        self.assertIsNotNone(receipt["end_time"])
        self.assertEqual(len(receipt["iterations"]), 2)
        self.assertTrue(all(entry["status"] == "launch_error" for entry in receipt["iterations"]))
        self.assertTrue(receipt["first_failure"]["failure_reason"].startswith("launch_error"))

    def test_invalid_count_and_timeout_are_rejected(self) -> None:
        cases = [
            ["--count", "0", "--timeout-seconds", "5"],
            ["--count", "1", "--timeout-seconds", "nan"],
            ["--count", "1", "--timeout-seconds", "inf"],
            ["--count", "1", "--timeout-seconds", "-1"],
            ["--count", "1", "--timeout-seconds", "0"],
        ]
        for extra in cases:
            with self.subTest(args=extra):
                result = self.run_raw([*self.base_args(), *extra, "--", "--fake=pass"])
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertFalse(self.receipt.exists())

    def test_reserved_gtest_flags_are_rejected(self) -> None:
        args = [*self.base_args(), "--count", "1", "--", "--gtest_repeat=5"]
        result = self.run_raw(args)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertFalse(self.receipt.exists())


class FixtureWatchdogTest(HarnessTestCase):
    def test_outer_watchdog_fails_preserves_output_and_reaps_descendant(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rq-watchdog-retained-") as destination:
            args = [*self.base_args(), "--count", "2", "--timeout-seconds", "600",
                    "--", "--fake=hang"]
            with self.assertRaisesRegex(AssertionError, "fixture watchdog expired"):
                self.run_raw(args, timeout=1, ready_file=self.root / "ready",
                             extra_env={"LUMINUMBRA_FIXTURE_READY_FILE": str(self.root / "ready")},
                             diagnostic_dir=Path(destination))
            retained = list(Path(destination).glob("fixture-watchdog-*/fixture"))
            self.assertEqual(len(retained), 1)
            receipt = json.loads((retained[0] / "out dir" / "receipt.json").read_text())
            # POSIX iteration groups are stopped before their outer owner; it
            # may truthfully record their signal failure before outer cleanup.
            self.assertIn(receipt["status"], ("running", "failed", "aborted"))
            self.assertTrue(all(entry["failed"] for entry in receipt["iterations"]))
            self.assertLessEqual(len(receipt["iterations"]), 1)
            pid_files = list(retained[0].rglob("grandchild.pid"))
            self.assertEqual(len(pid_files), 1)
            grandchild_pid = int(pid_files[0].read_text())
            deadline = time.monotonic() + 5
            while pid_alive(grandchild_pid) and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertFalse(pid_alive(grandchild_pid), "fixture descendant survived outer watchdog")
            logs = list(retained[0].rglob("iter-1.log"))
            self.assertEqual(len(logs), 1)
            self.assertIn("fake child iteration=1 mode hang", logs[0].read_text())

    def test_fixture_runner_keeps_separate_output_streams(self) -> None:
        result = self.run_script(count=1, mode="pass")
        self.assertEqual(result.returncode, 0, result.stderr)
        logs = self.root / "outer-runs" / "run-1"
        self.assertEqual(result.stdout, (logs / "stdout.log").read_text(encoding="utf-8"))
        self.assertEqual(result.stderr, (logs / "stderr.log").read_text(encoding="utf-8"))
        self.assertTrue((logs / "stderr.log").is_file())


class InterruptCleanupTest(HarnessTestCase):
    def test_interrupted_wait_terminates_owned_child_and_grandchild(self) -> None:
        crash = self.root / "iter-1" / "crash"
        crash.mkdir(parents=True)
        ready = self.root / "interrupt-ready"
        log = self.root / "interrupted.log"
        env = rq.build_child_environment(dict(os.environ), crash)
        env["LUMINUMBRA_FIXTURE_READY_FILE"] = str(ready)
        original_popen = subprocess.Popen
        owned = []
        grandchild_pid = None
        injected = False

        def factory(*args, **kwargs):
            process = original_popen(*args, **kwargs)
            if not owned:
                original_wait = process.wait
                owned.append((process, original_wait))

                def wait_once(timeout=None):
                    nonlocal grandchild_pid, injected
                    if injected:
                        return original_wait(timeout=timeout)
                    deadline = time.monotonic() + 30
                    while not ready.exists():
                        if process.poll() is not None:
                            self.fail("interrupt fixture exited before readiness")
                        if time.monotonic() >= deadline:
                            self.fail("interrupt fixture did not become ready")
                        time.sleep(0.01)
                    grandchild_pid = int((crash / "grandchild.pid").read_text())
                    self.assertGreater(grandchild_pid, 0)
                    self.assertIsNone(process.poll())
                    injected = True
                    raise KeyboardInterrupt("fixture interrupt")

                process.wait = wait_once
            return process

        try:
            with mock.patch.object(rq.subprocess, "Popen", side_effect=factory):
                with self.assertRaises(KeyboardInterrupt):
                    rq.run_iteration([str(self.exe), "--fake=hang"], self.work, env, log, 60)
            self.assertTrue(injected)
            process = owned[0][0]
            self.assertIsNotNone(process.poll(), "direct child was not reaped")
            deadline = time.monotonic() + 5
            while pid_alive(grandchild_pid, timeout=max(0.01, deadline - time.monotonic())):
                if time.monotonic() >= deadline:
                    self.fail("fixture grandchild survived interruption cleanup")
                time.sleep(0.01)
            self.assertIn("fake child iteration=1 mode hang", log.read_text(encoding="utf-8"))
        except BaseException:
            self.preserve_fixture_failure()
            raise
        finally:
            # The factory patch has ended; fallback cleanup cannot intercept
            # Windows taskkill/tasklist or recursively create another fixture.
            for process, original_wait in owned:
                process.wait = original_wait
                rq.terminate_process_tree(process)
            if grandchild_pid is not None and pid_alive(grandchild_pid, timeout=5):
                if os.name == "nt":
                    subprocess.run(["taskkill", "/T", "/F", "/PID", str(grandchild_pid)],
                                   capture_output=True, check=False, timeout=rq.CLEANUP_TIMEOUT_SECONDS)
                else:
                    os.kill(grandchild_pid, signal.SIGKILL)

    def test_cleanup_failure_preserves_interrupt_and_reports_diagnostics(self) -> None:
        for failure in (["fixture cleanup refused"], OSError("fixture cleanup raised")):
            with self.subTest(failure=str(failure)):
                interrupt = KeyboardInterrupt("fixture interrupt")
                process = mock.Mock()
                process.wait.side_effect = interrupt
                cleanup_options = {"side_effect": failure} if isinstance(failure, Exception) else {"return_value": failure}
                with mock.patch.object(rq.subprocess, "Popen", return_value=process), \
                        mock.patch.object(rq, "terminate_process_tree", **cleanup_options):
                    with self.assertRaises(KeyboardInterrupt) as caught:
                        rq.run_iteration([str(self.exe)], self.work, {}, self.root / "cleanup-error.log", 60)
                self.assertIs(caught.exception, interrupt)
                self.assertIn("process-tree cleanup errors", str(caught.exception))
                self.assertIn("fixture cleanup", str(caught.exception))

    def test_interruption_finalizes_aborted_receipt_without_inventing_entry(self) -> None:
        args_list = [*self.base_args(), "--count", "2"]
        if "--allow-no-dumps" not in args_list:
            args_list.append("--allow-no-dumps")
        args = rq.parse_args(args_list)
        completed = {"index": 1, "failed": False, "status": "exit:0", "failure_reason": None,
                     "log_path": "completed.log", "crash_artifacts": [], "collection_errors": []}
        interrupt = KeyboardInterrupt("process-tree cleanup errors: fixture diagnostic")
        with mock.patch.object(rq, "run_indexed_iteration", side_effect=[completed, interrupt]):
            with self.assertRaises(KeyboardInterrupt):
                rq.run_qualification(args, ["--fake=pass"], core_limits=lambda: (0, 0),
                                     read_pattern=lambda: "|fixture")
        receipt = self.load_receipt()
        self.assertEqual(receipt["status"], "aborted")
        self.assertIsNotNone(receipt["end_time"])
        self.assertEqual(receipt["iterations"], [completed])
        self.assertIn("process-tree cleanup errors: fixture diagnostic", receipt["abort_reason"])


if os.name == "posix":

    class PosixSignalTest(HarnessTestCase):
        def test_child_killed_by_signal_is_a_failure(self) -> None:
            result = self.run_script(count=2, mode="kill@1")
            self.assertEqual(result.returncode, 1)

            receipt = self.load_receipt()
            first = receipt["iterations"][0]
            self.assertEqual(first["signal"], "SIGKILL")
            self.assertIsNone(first["exit_code"])
            self.assertEqual(receipt["exit_code_histogram"], {"exit:0": 1, "signal:SIGKILL": 1})
            self.assertEqual(receipt["first_failure"]["index"], 1)

        def test_child_aborted_by_signal_is_a_failure(self) -> None:
            result = self.run_script(count=1, mode="abort@1")
            self.assertEqual(result.returncode, 1)
            self.assertEqual(self.load_receipt()["iterations"][0]["signal"], "SIGABRT")

        def test_crash_artifacts_live_inside_the_run_directory(self) -> None:
            # Whether a core file is written depends on the host; any artifact listed must be
            # a file inside this run's directory.
            result = self.run_script(count=1, mode="abort@1")
            self.assertEqual(result.returncode, 1)
            receipt = self.load_receipt()
            environment = receipt["environment"]
            # The child's soft limit is raised to the parent's hard limit and nothing else changes.
            self.assertEqual(environment["rlimit_core_child"]["soft"], environment["rlimit_core_parent"]["hard"])
            self.assertEqual(environment["rlimit_core_child"]["hard"], environment["rlimit_core_parent"]["hard"])
            self.assertIn("core_pattern", environment)
            for artifact in receipt["iterations"][0]["crash_artifacts"]:
                self.assertTrue(Path(artifact).is_file(), artifact)
                self.assertTrue(artifact.startswith(str(self.run_dir())), artifact)

        def test_missing_core_directory_under_cwd_is_created_and_run_passes(self) -> None:
            args = self.in_process_args(1)
            code = rq.run_qualification(
                args,
                ["--fake=pass"],
                core_limits=lambda: (4096, 4096),
                read_pattern=lambda: "cores/core.%p",
            )
            self.assertEqual(code, 0)
            self.assertTrue((self.work / "cores").is_dir())
            receipt = self.load_receipt()
            self.assertEqual(receipt["environment"]["core_problems"], [])
            self.assertTrue(receipt["environment"]["core_directory_preflight"]["created"])

        def test_missing_core_directory_outside_cwd_is_a_preflight_failure(self) -> None:
            outside = self.root / "missing-outside" / "core.%p"
            args = self.in_process_args(1)
            code = rq.run_qualification(
                args,
                ["--fake=pass"],
                core_limits=lambda: (4096, 4096),
                read_pattern=lambda: str(outside),
            )
            self.assertEqual(code, rq.PREFLIGHT_EXIT_CODE)
            receipt = self.load_receipt()
            self.assertEqual(receipt["status"], "PREFLIGHT_FAILED")
            self.assertIn("outside the child working directory", receipt["failure_reason"])
            self.assertFalse((self.root / "missing-outside").exists())

        def test_core_file_copy_failure_keeps_the_classified_iteration(self) -> None:
            args = self.in_process_args(1)
            with mock.patch.object(rq.shutil, "copy2", side_effect=OSError("disk full")):
                code = rq.run_qualification(
                    args,
                    ["--fake=segv_core"],
                    core_limits=lambda: (4096, 4096),
                    read_pattern=lambda: "core.%p",
                )
            self.assertEqual(code, 1)
            receipt = self.load_receipt()
            self.assertEqual(receipt["status"], "failed")
            self.assertNotIn("abort_reason", receipt)
            self.assertEqual(len(receipt["iterations"]), 1)
            iteration = receipt["iterations"][0]
            self.assertEqual(iteration["signal"], "SIGSEGV")
            self.assertEqual(iteration["status"], "signal:SIGSEGV")
            self.assertTrue(iteration["failed"])
            self.assertTrue(any("disk full" in error for error in iteration["collection_errors"]))
            self.assertEqual(receipt["collection_error_count"], len(iteration["collection_errors"]))
            self.assertEqual(receipt["first_failure"]["index"], 1)

        def test_post_crash_listing_error_keeps_the_classified_iteration(self) -> None:
            args = self.in_process_args(1)
            real_snapshot = rq.snapshot_files
            recursive_calls: list[Path] = []

            def listing_fails_after_crash(root, recursive, skip=None, errors=None):  # type: ignore[no-untyped-def]
                # The first recursive snapshot is taken before the child runs; the next is post-crash.
                if recursive and root == self.work:
                    recursive_calls.append(root)
                    if len(recursive_calls) > 1:
                        raise PermissionError("listing denied")
                return real_snapshot(root, recursive, skip, errors)

            with mock.patch.object(rq, "snapshot_files", side_effect=listing_fails_after_crash):
                code = rq.run_qualification(
                    args,
                    ["--fake=segv_core"],
                    core_limits=lambda: (4096, 4096),
                    read_pattern=lambda: "core.%p",
                )
            self.assertEqual(code, 1)
            receipt = self.load_receipt()
            self.assertEqual(receipt["status"], "failed")
            self.assertEqual(len(receipt["iterations"]), 1)
            iteration = receipt["iterations"][0]
            self.assertEqual(iteration["status"], "signal:SIGSEGV")
            self.assertEqual(iteration["signal"], "SIGSEGV")
            self.assertTrue(iteration["failed"])
            self.assertTrue(any("listing denied" in error for error in iteration["collection_errors"]))
            self.assertEqual(receipt["first_failure"]["index"], 1)

        def in_process_args(self, count: int, *extra: str) -> object:
            return rq.parse_args(
                [
                    "--exe",
                    str(self.exe),
                    "--cwd",
                    str(self.work),
                    "--receipt",
                    str(self.receipt),
                    "--count",
                    str(count),
                    *extra,
                ]
            )

        def test_zero_hard_core_limit_fails_preflight_without_allow(self) -> None:
            args = self.in_process_args(1)
            code = rq.run_qualification(args, ["--fake=pass"], core_limits=lambda: (0, 0))
            self.assertEqual(code, rq.PREFLIGHT_EXIT_CODE)
            receipt = self.load_receipt()
            self.assertEqual(receipt["status"], "PREFLIGHT_FAILED")
            self.assertEqual(receipt["iterations"], [])
            self.assertIn("soft limit is 0", receipt["failure_reason"])
            self.assertIn("--allow-no-dumps", receipt["failure_reason"])
            self.assertIn("kernel.core_pattern", receipt["failure_reason"])

        def test_zero_hard_core_limit_runs_with_allow_and_records_child_limit(self) -> None:
            args = self.in_process_args(1, "--allow-no-dumps")
            code = rq.run_qualification(args, ["--fake=pass"], core_limits=lambda: (0, 0))
            self.assertEqual(code, 0)
            receipt = self.load_receipt()
            self.assertEqual(receipt["status"], "passed")
            self.assertEqual(receipt["environment"]["rlimit_core_child"], {"soft": 0, "hard": 0})
            self.assertTrue(any("soft limit is 0" in p for p in receipt["environment"]["core_problems"]))
            self.assertEqual(receipt["iterations"][0]["status"], "exit:0")

        def test_spawn_time_setup_error_is_a_launch_error_iteration(self) -> None:
            args = self.in_process_args(2, "--allow-no-dumps")
            with mock.patch.object(rq, "apply_core_limit", side_effect=OSError("setrlimit denied")):
                code = rq.run_qualification(args, ["--fake=pass"], core_limits=lambda: (1024, 4096))
            self.assertEqual(code, 1)
            receipt = self.load_receipt()
            self.assertEqual(receipt["status"], "failed")
            self.assertNotIn("abort_reason", receipt)
            self.assertEqual(len(receipt["iterations"]), 2)
            self.assertTrue(receipt["iterations"][0]["status"].startswith("launch_error"))
            # CPython reports any exception raised in preexec_fn to the parent as SubprocessError.
            self.assertIn("preexec_fn", receipt["iterations"][0]["launch_error"] or "")
            self.assertTrue(receipt["first_failure"]["failure_reason"].startswith("launch_error"))


class RunDirectoryAndSnapshotTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="repeat-qualification-unit-")
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_run_directory_is_never_reused(self) -> None:
        first = rq.make_run_directory(self.root, "20260101T000000000000Z", 42)
        self.assertTrue(first.is_dir())
        with self.assertRaises(FileExistsError):
            rq.make_run_directory(self.root, "20260101T000000000000Z", 42)

    def test_rewritten_file_with_same_name_and_size_is_detected(self) -> None:
        folder = self.root / "dumps"
        folder.mkdir()
        core = folder / "core"
        core.write_bytes(b"first")
        before = rq.snapshot_files(folder, recursive=False)
        self.assertEqual(rq.changed_files(before, before), [])

        core.write_bytes(b"other")  # same size, new contents
        later = core.stat().st_mtime_ns + 2_000_000_000
        os.utime(core, ns=(later, later))
        after = rq.snapshot_files(folder, recursive=False)
        self.assertEqual(rq.changed_files(before, after), [str(core)])

    def test_copy_changed_files_copies_only_new_or_rewritten_files(self) -> None:
        folder = self.root / "dumps"
        folder.mkdir()
        (folder / "old.dmp").write_bytes(b"old")
        before = rq.snapshot_files(folder, recursive=False)
        (folder / "new.dmp").write_bytes(b"new")
        copies = rq.copy_changed_files(folder, before, self.root / "iter-1" / "wer")
        self.assertEqual([Path(c).name for c in copies], ["new.dmp"])
        self.assertEqual(Path(copies[0]).read_bytes(), b"new")


    def test_snapshot_stat_failure_propagates_without_error_list(self) -> None:
        folder = self.root / "dumps"
        folder.mkdir()
        denied = folder / "denied.dmp"
        denied.write_bytes(b"dump")
        real_stat = Path.stat

        def selective_stat(path, *args, **kwargs):
            if path == denied:
                raise PermissionError("inspection denied")
            return real_stat(path, *args, **kwargs)

        with mock.patch.object(Path, "stat", selective_stat):
            with self.assertRaisesRegex(PermissionError, "inspection denied"):
                rq.snapshot_files(folder, recursive=False)

    def test_snapshot_traversal_errors_preserve_healthy_entries(self) -> None:
        folder = self.root / "dumps"
        denied = folder / "denied"
        denied.mkdir(parents=True)
        healthy = folder / "healthy.dmp"
        healthy.write_bytes(b"healthy")
        real_scandir = os.scandir

        def selective_scandir(path):
            if Path(path) == denied:
                raise PermissionError(13, "traversal denied", str(denied))
            return real_scandir(path)

        errors: list[str] = []
        with mock.patch.object(rq.os, "scandir", side_effect=selective_scandir):
            snapshot = rq.snapshot_files(folder, recursive=True, errors=errors)
            with self.assertRaisesRegex(PermissionError, "traversal denied"):
                rq.snapshot_files(folder, recursive=True)
        self.assertEqual(list(snapshot), [str(healthy)])
        self.assertEqual(len(errors), 1)
        self.assertIn(str(denied), errors[0])
        self.assertIn("PermissionError", errors[0])
        self.assertIn("traversal denied", errors[0])

    def test_snapshot_root_inspection_failure_is_reported_or_propagated(self) -> None:
        missing = self.root / "missing dumps"
        errors: list[str] = []
        self.assertEqual(rq.snapshot_files(missing, False, errors=errors), {})
        self.assertEqual(len(errors), 1)
        self.assertIn(str(missing), errors[0])
        self.assertIn("FileNotFoundError", errors[0])
        with self.assertRaises(FileNotFoundError):
            rq.snapshot_files(missing, False)

    def test_dump_settle_snapshot_reports_stat_errors(self) -> None:
        folder = self.root / "dumps"
        folder.mkdir()
        denied = folder / "denied.dmp"
        denied.write_bytes(b"dump")
        (folder / "healthy.dmp").write_bytes(b"healthy")
        real_stat = Path.stat

        def selective_stat(path, *args, **kwargs):
            if path == denied:
                raise PermissionError("settle inspection denied")
            return real_stat(path, *args, **kwargs)

        errors: list[str] = []
        # Fixed time and a mocked pause make the two stable snapshots deterministic.
        with mock.patch.object(Path, "stat", selective_stat), \
                mock.patch.object(rq.time, "monotonic", return_value=0.0), \
                mock.patch.object(rq.time, "sleep"):
            rq.wait_for_dump_settle(folder, {}, 30.0, errors)
        self.assertEqual(len(errors), 2)
        self.assertTrue(all(str(denied) in error and "PermissionError: settle inspection denied" in error
                            for error in errors))

    def test_receipt_ownership_uses_canonical_parent(self) -> None:
        parent = self.root / "receipts"
        parent.mkdir()
        (parent / "nested").mkdir()
        receipt = parent / "receipt.json"
        alias = parent / "nested" / ".." / receipt.name
        self.assertEqual(rq.receipt_ownership_path(receipt), rq.receipt_ownership_path(alias))

    def test_receipt_ownership_file_is_excluded_from_artifact_scans(self) -> None:
        receipt = self.root / "receipt.json"
        ownership = rq.receipt_ownership_path(receipt)
        ownership.write_bytes(b"owner")
        healthy = self.root / "healthy.dmp"
        healthy.write_bytes(b"dump")
        skip = rq.harness_output_skip(self.root / "run", receipt)
        self.assertEqual(list(rq.snapshot_files(self.root, False, skip)), [str(healthy)])


class WindowsDumpLockTest(unittest.TestCase):
    """Simulate mandatory Windows byte locks, using real unbuffered file handles."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="rq-windows-lock-test-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.guard = threading.Lock()
        self.owner = None
        self.second_lock_attempted = threading.Event()
        self.initializer_fd = None
        self.initializing = threading.Event()
        self.finish_initialization = threading.Event()
        self.pause_initialization = False
        self.deny_initialization_once = False
        self.initialization_writes = 0
        self.verified_reads = 0
        self.unlocks = 0
        self.windows = ModuleType("msvcrt")
        self.windows.LK_NBLCK = 1
        self.windows.LK_UNLCK = 0
        self.windows.locking = self.locking
        self.host_os = mock.Mock(wraps=os)
        self.host_os.name = "nt"
        self.host_os.O_RDWR = os.O_RDWR
        self.host_os.O_CREAT = os.O_CREAT
        self.host_os.fdopen.side_effect = self.fdopen

    def locking(self, fd, operation, size):
        self.assertEqual(size, 1)
        with self.guard:
            if operation == self.windows.LK_UNLCK:
                self.assertEqual(self.owner, fd)
                self.owner = None
                self.unlocks += 1
            else:
                if self.owner is None:
                    self.owner = fd
                else:
                    self.second_lock_attempted.set()
                    raise PermissionError(errno.EACCES, "byte zero is held by another contender")
                if self.initializer_fd is not None and fd != self.initializer_fd:
                    self.second_lock_attempted.set()

    def fdopen(self, fd, *args, **kwargs):
        raw = os.fdopen(fd, *args, **kwargs)
        test = self

        class TrackedFile:
            def __enter__(self):
                return self

            def __exit__(self, *exception):
                with test.guard:
                    if test.owner == fd:
                        test.owner = None
                raw.close()

            def fileno(self):
                return raw.fileno()

            def seek(self, offset):
                return raw.seek(offset)

            def write(self, content):
                # Model Windows faithfully: unlocked writes are permitted, but a
                # write overlapping another contender's locked byte is denied.
                with test.guard:
                    if test.owner is not None and test.owner != fd:
                        raise PermissionError(errno.EACCES, "initialization overlaps a locked byte")
                    test.initialization_writes += 1
                    if test.initializer_fd is None:
                        test.initializer_fd = fd
                    pause = test.pause_initialization and test.initializer_fd == fd
                    deny = test.deny_initialization_once
                    test.deny_initialization_once = False
                if deny:
                    raise PermissionError(errno.EACCES, "transient initialization denial")
                if pause:
                    test.initializing.set()
                    if not test.finish_initialization.wait(5):
                        raise RuntimeError("initialization hook was not released")
                with test.guard:
                    if test.owner is not None and test.owner != fd:
                        raise PermissionError(errno.EACCES, "initialization overlaps a locked byte")
                    return raw.write(content)

            def read(self, size):
                with test.guard:
                    test.assertEqual(test.owner, fd)
                    test.verified_reads += 1
                return raw.read(size)

        return TrackedFile()

    def patches(self):
        @contextmanager
        def installed():
            with mock.patch.object(rq, "os", self.host_os), \
                    mock.patch.dict(sys.modules, {"msvcrt": self.windows}):
                yield
        return installed()

    def test_two_contenders_interleave_before_initialization_finishes(self) -> None:
        self.pause_initialization = True
        first_in_body = threading.Event()
        second_in_body = threading.Event()
        release_first = threading.Event()
        release_second = threading.Event()

        def contender(first):
            with rq.shared_dump_directory_locks(self.folder):
                if first:
                    first_in_body.set()
                    if not release_first.wait(5):
                        raise RuntimeError("first lock owner was not released")
                else:
                    second_in_body.set()
                    if not release_second.wait(5):
                        raise RuntimeError("second lock owner was not released")

        with self.patches(), ThreadPoolExecutor(max_workers=2) as workers:
            first = workers.submit(contender, True)
            try:
                self.assertTrue(self.initializing.wait(5), "first initializer never reached its hook")
                second = workers.submit(contender, False)
                self.assertTrue(self.second_lock_attempted.wait(5), "second contender never tried byte zero")
                self.assertEqual((self.folder / rq.DUMP_LOCK_NAME).read_bytes(), b"")
                self.assertFalse(second_in_body.is_set())
                self.finish_initialization.set()
                self.assertTrue(first_in_body.wait(5))
                self.assertFalse(second_in_body.is_set())
            finally:
                self.finish_initialization.set()
                release_first.set()
                release_second.set()
            first.result(timeout=5)
            second.result(timeout=5)
        self.assertTrue(second_in_body.is_set())
        self.assertEqual(self.initialization_writes, 1)
        self.assertEqual(self.verified_reads, 2)
        self.assertEqual((self.folder / rq.DUMP_LOCK_NAME).read_bytes(), b"\0")
        self.assertIsNone(self.owner)

    def test_initialization_permission_error_releases_and_retries(self) -> None:
        self.deny_initialization_once = True
        with self.patches(), mock.patch.object(rq.time, "sleep"):
            with rq.shared_dump_directory_locks(self.folder):
                self.assertEqual((self.folder / rq.DUMP_LOCK_NAME).read_bytes(), b"\0")
        self.assertEqual(self.initialization_writes, 2)
        self.assertEqual(self.verified_reads, 1)
        self.assertEqual(self.unlocks, 1)
        self.assertIsNone(self.owner)

    def test_contention_times_out_within_bounded_loop(self) -> None:
        self.owner = -1
        with self.patches(), mock.patch.object(rq.time, "monotonic", side_effect=[0.0, 301.0]):
            with self.assertRaises(TimeoutError):
                with rq.shared_dump_directory_locks(self.folder):
                    self.fail("contended lock must not be acquired")
        self.assertEqual(self.initialization_writes, 0)
        self.assertEqual(self.owner, -1)

    def test_existing_lock_byte_is_verified_without_rewriting(self) -> None:
        (self.folder / rq.DUMP_LOCK_NAME).write_bytes(b"\0")
        with self.patches():
            with rq.shared_dump_directory_locks(self.folder):
                self.assertIsNotNone(self.owner)
        self.assertEqual(self.initialization_writes, 0)
        self.assertEqual(self.verified_reads, 1)
        self.assertIsNone(self.owner)

    def test_invalid_lock_byte_releases_ownership_and_fails_verification(self) -> None:
        (self.folder / rq.DUMP_LOCK_NAME).write_bytes(b"x")
        with self.patches():
            with self.assertRaisesRegex(OSError, "initialization verification failed"):
                with rq.shared_dump_directory_locks(self.folder):
                    self.fail("invalid lock byte must not be accepted")
        self.assertEqual(self.unlocks, 1)
        self.assertIsNone(self.owner)


class QualificationSafetyTest(HarnessTestCase):
    """Deterministic receipt tests; no native child, registry, or core access."""

    def safety_args(self, count: int = 3, allow: bool = True, folder: Path | None = None):
        arguments = ["--exe", str(self.exe), "--cwd", str(self.work),
                     "--receipt", str(self.receipt), "--count", str(count)]
        if allow:
            arguments.append("--allow-no-dumps")
        if folder is not None:
            arguments.extend(["--wer-dump-folder", str(folder)])
        return rq.parse_args(arguments)

    def clean_child(self, command, cwd, env, log_path, timeout, core_target):
        self.assertEqual(command[0], str(self.exe))
        self.assertEqual(cwd, self.work)
        report = Path(next(arg.split("json:", 1)[1] for arg in command
                           if arg.startswith("--gtest_output=json:")))
        report.write_text(json.dumps({
            "tests": 1, "failures": 0, "errors": 0,
            "testsuites": [{"testsuite": [{"name": "Case", "status": "RUN", "result": "COMPLETED"}]}],
        }), encoding="utf-8")
        log_path.write_text("simulated clean child", encoding="utf-8")
        return {"exit_code": 0, "signal": None, "timed_out": False,
                "duration_seconds": 0.0, "status": "exit:0", "launch_error": None,
                "cleanup_errors": []}

    def qualify(self, args=None, child=None, query=None):
        with mock.patch.object(rq, "IS_WINDOWS", True), \
                mock.patch.object(rq, "run_iteration", side_effect=child or self.clean_child) as launch:
            code = rq.run_qualification(args or self.safety_args(), [], query=query or (lambda key: (1, "")))
        return code, launch

    def assert_finalized(self, status: str) -> dict:
        receipt = self.load_receipt()
        self.assertEqual(receipt["status"], status)
        self.assertIsNotNone(receipt["end_time"])
        self.assertFalse(rq.receipt_ownership_path(self.receipt).exists())
        return receipt

    def test_unchanged_executable_is_checked_before_and_after_every_child(self) -> None:
        with mock.patch.object(rq, "sha256_file", wraps=rq.sha256_file) as hash_file:
            code, launch = self.qualify()
        self.assertEqual(code, 0)
        self.assertEqual(launch.call_count, 3)
        self.assertEqual(hash_file.call_count, 7)
        receipt = self.assert_finalized("passed")
        self.assertTrue(all(entry["executable_identity_error"] is None for entry in receipt["iterations"]))

    def test_replacement_between_iterations_refuses_next_launch(self) -> None:
        real_write = rq.write_receipt

        def replace_after_first_receipt(path, receipt):
            real_write(path, receipt)
            if receipt["status"] == "running" and len(receipt["iterations"]) == 1:
                self.exe.write_bytes(b"replacement between iterations")

        with mock.patch.object(rq, "write_receipt", side_effect=replace_after_first_receipt):
            code, launch = self.qualify()
        self.assertEqual(code, 1)
        self.assertEqual(launch.call_count, 1)
        receipt = self.assert_finalized("failed")
        self.assertEqual(len(receipt["iterations"]), 2)
        self.assertEqual(receipt["failure_count"], 1)
        self.assertEqual(receipt["first_failure"]["index"], 2)
        refused = receipt["iterations"][1]
        self.assertIsNone(refused["exit_code"])
        self.assertIsNone(refused["signal"])
        self.assertFalse(refused["timed_out"])
        self.assertEqual(refused["status"], "executable_identity_error")
        self.assertIn("before launch", receipt["first_failure"]["failure_reason"])
        self.assertIn("SHA-256 mismatch", receipt["failure_reason"])

    def test_replacement_during_child_fails_even_after_clean_exit(self) -> None:
        def replacing_child(*arguments):
            result = self.clean_child(*arguments)
            self.exe.write_bytes(b"replacement during child")
            return result

        code, launch = self.qualify(child=replacing_child)
        self.assertEqual(code, 1)
        self.assertEqual(launch.call_count, 1)
        receipt = self.assert_finalized("failed")
        self.assertEqual(len(receipt["iterations"]), 1)
        self.assertEqual(receipt["failure_count"], 1)
        self.assertEqual(receipt["first_failure"]["index"], 1)
        entry = receipt["iterations"][0]
        self.assertEqual(entry["exit_code"], 0)
        self.assertEqual(entry["status"], "exit:0")
        self.assertIsNone(entry["signal"])
        self.assertFalse(entry["timed_out"])
        self.assertTrue(entry["failed"])
        self.assertIn("after child", entry["failure_reason"])
        self.assertIn("SHA-256 mismatch", receipt["first_failure"]["failure_reason"])
        self.assertEqual(receipt["exit_code_histogram"], {"exit:0": 1})

    def test_unreadable_executable_before_launch_finalizes_without_child_exit(self) -> None:
        real_hash = rq.sha256_file
        with mock.patch.object(rq, "sha256_file", side_effect=[real_hash(self.exe), PermissionError("hash denied")]):
            code, launch = self.qualify()
        self.assertEqual(code, 1)
        launch.assert_not_called()
        receipt = self.assert_finalized("failed")
        self.assertEqual(len(receipt["iterations"]), 1)
        self.assertEqual(receipt["first_failure"]["index"], 1)
        self.assertIsNone(receipt["iterations"][0]["exit_code"])
        self.assertIn("PermissionError: hash denied", receipt["failure_reason"])

    def test_unreadable_initial_executable_hash_finalizes_preflight(self) -> None:
        with mock.patch.object(rq, "sha256_file", side_effect=PermissionError("initial hash denied")):
            code, launch = self.qualify()
        self.assertEqual(code, rq.PREFLIGHT_EXIT_CODE)
        launch.assert_not_called()
        receipt = self.assert_finalized("PREFLIGHT_FAILED")
        self.assertEqual(receipt["iterations"], [])
        self.assertIsNone(receipt["exe"]["sha256"])
        self.assertIn("executable identity initial hash", receipt["failure_reason"])
        self.assertIn("PermissionError: initial hash denied", receipt["failure_reason"])

    def test_unreadable_executable_after_child_retains_child_exit(self) -> None:
        digest = rq.sha256_file(self.exe)
        with mock.patch.object(rq, "sha256_file", side_effect=[digest, digest, PermissionError("hash denied")]):
            code, launch = self.qualify()
        self.assertEqual(code, 1)
        self.assertEqual(launch.call_count, 1)
        receipt = self.assert_finalized("failed")
        self.assertEqual(len(receipt["iterations"]), 1)
        self.assertEqual(receipt["first_failure"]["index"], 1)
        self.assertEqual(receipt["iterations"][0]["exit_code"], 0)
        self.assertEqual(receipt["iterations"][0]["status"], "exit:0")
        self.assertIn("after child", receipt["failure_reason"])
        self.assertIn("PermissionError: hash denied", receipt["failure_reason"])

    def dump_query(self, folder: Path):
        return lambda key: (0, WerPreflightTest.reg_output(str(folder)))

    def check_concurrent_dump_collection(self, windows: bool) -> None:
        folder = self.work / "shared-dumps"
        folder.mkdir()
        first_in_child = threading.Event()
        second_at_lock = threading.Event()
        second_in_child = threading.Event()
        release_first = threading.Event()
        attempt_mutex = threading.Lock()
        attempts = 0
        real_locks = rq.shared_dump_directory_locks

        @contextmanager
        def tracked_locks(*folders):
            nonlocal attempts
            with attempt_mutex:
                attempts += 1
                if attempts == 2:
                    second_at_lock.set()
            with real_locks(*folders):
                yield

        names = ({"first": "world.exe.101.dmp", "second": "world.exe.202.dmp"}
                 if windows else {"first": "core.101", "second": "core.202"})

        def child(command, *arguments):
            owner = command[-1].split("=", 1)[1]
            (folder / names[owner]).write_bytes(owner.encode("ascii"))
            if owner == "first":
                first_in_child.set()
                if not release_first.wait(10):
                    raise RuntimeError("first synthetic child was not released")
            else:
                second_in_child.set()
            return self.clean_child(command, *arguments)

        def qualify(owner):
            args = self.safety_args(count=1, folder=folder if windows else None)
            args.receipt = self.receipt.with_name(f"{owner}.json")
            return rq.run_qualification(
                args, [f"--owner={owner}"], query=self.dump_query(folder),
                core_limits=lambda: (4096, 4096),
                # Simulate the kernel's '/' separators even on a Windows host;
                # backslashes would be literal characters in a POSIX core name.
                read_pattern=lambda: (folder / "core.%p").as_posix(),
            )

        with mock.patch.object(rq, "IS_WINDOWS", windows), \
                mock.patch.object(rq, "run_iteration", side_effect=child), \
                mock.patch.object(rq, "shared_dump_directory_locks", side_effect=tracked_locks), \
                ThreadPoolExecutor(max_workers=2) as workers:
            first = workers.submit(qualify, "first")
            try:
                self.assertTrue(first_in_child.wait(10), "first invocation never launched")
                second = workers.submit(qualify, "second")
                self.assertTrue(second_at_lock.wait(10), "second invocation never attempted its lock")
                self.assertFalse(second_in_child.wait(0.2), "shared dump writers overlapped")
            finally:
                release_first.set()
            self.assertEqual(first.result(timeout=10), 0)
            self.assertEqual(second.result(timeout=10), 0)

        self.assertTrue(second_in_child.is_set())
        for owner, name in names.items():
            receipt_path = self.receipt.with_name(f"{owner}.json")
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            self.assertEqual(receipt["status"], "passed")
            entry = receipt["iterations"][0]
            self.assertEqual(entry["collection_errors"], [])
            self.assertEqual([Path(path).name for path in entry["crash_artifacts"]], [name])
            self.assertEqual(Path(entry["crash_artifacts"][0]).read_bytes(), owner.encode("ascii"))
            self.assertFalse(rq.receipt_ownership_path(receipt_path).exists())
        self.assertTrue((folder / rq.DUMP_LOCK_NAME).is_file())
        self.assertNotIn(str(folder / rq.DUMP_LOCK_NAME), rq.snapshot_files(folder, True))

    def test_concurrent_distinct_receipts_serialize_wer_collection(self) -> None:
        self.check_concurrent_dump_collection(windows=True)

    def test_concurrent_distinct_receipts_serialize_core_collection(self) -> None:
        self.check_concurrent_dump_collection(windows=False)

    def test_dump_lock_failure_finalizes_without_launching_child(self) -> None:
        folder = self.root / "dumps"
        with mock.patch.object(rq, "shared_dump_directory_locks",
                               side_effect=PermissionError("lock denied")):
            code, launch = self.qualify(self.safety_args(count=1, folder=folder),
                                       query=self.dump_query(folder))
        self.assertEqual(code, 1)
        launch.assert_not_called()
        entry = self.assert_finalized("failed")["iterations"][0]
        self.assertEqual(entry["status"], "harness_error")
        self.assertIsNone(entry["exit_code"])
        self.assertIn("shared dump directory lock: PermissionError: lock denied", entry["failure_reason"])

    def test_snapshot_stat_errors_in_baseline_and_post_run_keep_healthy_artifacts(self) -> None:
        folder = self.root / "dumps"
        folder.mkdir()
        denied = folder / "denied.dmp"
        denied.write_bytes(b"unreadable")
        healthy = folder / "healthy.dmp"
        real_stat = Path.stat

        def selective_stat(path, *args, **kwargs):
            if path == denied:
                raise PermissionError("inspection denied")
            return real_stat(path, *args, **kwargs)

        def child_with_dump(*arguments):
            healthy.write_bytes(b"healthy dump")
            return self.clean_child(*arguments)

        with mock.patch.object(Path, "stat", selective_stat):
            code, launch = self.qualify(self.safety_args(count=1, folder=folder), child_with_dump,
                                       self.dump_query(folder))
        self.assertEqual(code, 0)
        self.assertEqual(launch.call_count, 1)
        receipt = self.assert_finalized("passed")
        entry = receipt["iterations"][0]
        self.assertEqual(entry["status"], "exit:0")
        self.assertFalse(entry["failed"])
        self.assertEqual(receipt["collection_error_count"], 2)
        self.assertEqual(len(entry["collection_errors"]), 2)
        self.assertTrue(all(str(denied) in error and "PermissionError: inspection denied" in error
                            for error in entry["collection_errors"]))
        self.assertEqual([Path(path).name for path in entry["crash_artifacts"]], ["healthy.dmp"])
        self.assertEqual(Path(entry["crash_artifacts"][0]).read_bytes(), b"healthy dump")

    def check_dump_folder_failure(self, allow: bool) -> None:
        folder = self.root / "denied dumps"
        real_mkdir = Path.mkdir

        def selective_mkdir(path, *args, **kwargs):
            if path == folder:
                raise PermissionError("dump mkdir denied")
            return real_mkdir(path, *args, **kwargs)

        with mock.patch.object(Path, "mkdir", selective_mkdir):
            code, launch = self.qualify(self.safety_args(count=1, allow=allow, folder=folder),
                                       query=self.dump_query(folder))
        receipt = self.assert_finalized("passed" if allow else "PREFLIGHT_FAILED")
        self.assertEqual(code, 0 if allow else rq.PREFLIGHT_EXIT_CODE)
        self.assertEqual(launch.call_count, 1 if allow else 0)
        self.assertEqual(len(receipt["iterations"]), 1 if allow else 0)
        wer = receipt["environment"]["wer"]
        self.assertFalse(wer["configured"])
        self.assertIn(str(folder), wer["dump_folder_error"])
        self.assertIn("PermissionError: dump mkdir denied", wer["reason"])
        self.assertEqual(receipt["environment"]["allow_no_dumps"], allow)
        if not allow:
            self.assertIn("dump mkdir denied", receipt["failure_reason"])

    def test_dump_folder_creation_failure_finalizes_preflight_without_launch(self) -> None:
        self.check_dump_folder_failure(False)

    def test_dump_folder_creation_failure_with_opt_out_preserves_diagnostic(self) -> None:
        self.check_dump_folder_failure(True)

    def test_competing_receipt_owner_is_refused_before_run_directory_or_child(self) -> None:
        self.receipt.parent.mkdir()
        ownership = rq.receipt_ownership_path(self.receipt)
        fd = os.open(ownership, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with mock.patch.object(rq, "make_run_directory") as create_run:
                code, launch = self.qualify()
            self.assertEqual(code, 2)
            create_run.assert_not_called()
            launch.assert_not_called()
            self.assertTrue(ownership.is_file())
            self.assertFalse(self.receipt.exists())
            self.assertEqual(list(self.receipt.parent.resolve().iterdir()), [ownership])
        finally:
            os.close(fd)
            ownership.unlink()

    def test_existing_receipt_is_preserved_byte_for_byte(self) -> None:
        self.receipt.parent.mkdir()
        original = b"existing receipt\r\n\x00"
        self.receipt.write_bytes(original)
        with mock.patch.object(rq, "make_run_directory") as create_run:
            code, launch = self.qualify()
        self.assertEqual(code, 2)
        create_run.assert_not_called()
        launch.assert_not_called()
        self.assertEqual(self.receipt.read_bytes(), original)
        self.assertFalse(rq.receipt_ownership_path(self.receipt).exists())

    def test_existing_receipt_symlink_is_refused_including_dangling_link(self) -> None:
        self.receipt.parent.mkdir()
        target = self.root / "missing receipt"
        try:
            self.receipt.symlink_to(target)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"symlink creation unavailable: {exc}")
        with mock.patch.object(rq, "make_run_directory") as create_run:
            code, launch = self.qualify()
        self.assertEqual(code, 2)
        create_run.assert_not_called()
        launch.assert_not_called()
        self.assertTrue(self.receipt.is_symlink())
        self.assertFalse(target.exists())
        self.assertFalse(rq.receipt_ownership_path(self.receipt).exists())

    def test_distinct_receipts_work_while_another_receipt_is_owned(self) -> None:
        self.receipt.parent.mkdir()
        ownership = rq.receipt_ownership_path(self.receipt)
        fd = os.open(ownership, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            self.receipt = self.receipt.with_name("independent.json")
            code, launch = self.qualify(self.safety_args(count=1))
            self.assertEqual(code, 0)
            self.assertEqual(launch.call_count, 1)
            self.assert_finalized("passed")
            self.assertTrue(ownership.is_file())
        finally:
            os.close(fd)
            ownership.unlink()

    def test_receipt_ownership_releases_after_preflight_failure(self) -> None:
        code, launch = self.qualify(self.safety_args(allow=False))
        self.assertEqual(code, rq.PREFLIGHT_EXIT_CODE)
        launch.assert_not_called()
        self.assert_finalized("PREFLIGHT_FAILED")

    def test_receipt_ownership_releases_after_abort(self) -> None:
        with self.assertRaisesRegex(KeyboardInterrupt, "controlled abort"):
            self.qualify(child=lambda *args: (_ for _ in ()).throw(KeyboardInterrupt("controlled abort")))
        receipt = self.assert_finalized("aborted")
        self.assertEqual(receipt["iterations"], [])
        self.assertIn("controlled abort", receipt["abort_reason"])


class WerPreflightTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="repeat-qualification-wer-")
        self.folder = Path(self._tmp.name) / "dumps"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    @staticmethod
    def reg_output(folder: str, dump_type: str = "2") -> str:
        return (
            "\r\n"
            f"{REGISTRY_ROOT}\\world.exe\r\n"
            f"    DumpFolder    REG_EXPAND_SZ    {folder}\r\n"
            f"    DumpType    REG_DWORD    0x{dump_type}\r\n"
        )

    def test_matching_exe_key_is_configured(self) -> None:
        def query(key: str) -> tuple[int, str]:
            if key.endswith("\\world.exe"):
                return 0, self.reg_output(str(self.folder))
            return 1, ""

        result = rq.evaluate_wer_preflight("world.exe", self.folder, query)
        self.assertTrue(result["configured"], result)
        self.assertEqual(result["dump_type"], "0x2")
        self.assertIsNone(result["reason"])

    def test_global_key_is_used_when_exe_key_is_missing(self) -> None:
        def query(key: str) -> tuple[int, str]:
            if key == REGISTRY_ROOT:
                return 0, self.reg_output(str(self.folder))
            return 1, ""

        result = rq.evaluate_wer_preflight("world.exe", self.folder, query)
        self.assertTrue(result["configured"], result)
        self.assertEqual(result["key"], REGISTRY_ROOT)

    def test_mismatched_folder_is_not_configured(self) -> None:
        def query(key: str) -> tuple[int, str]:
            return 0, self.reg_output(str(self.folder.parent / "elsewhere"))

        result = rq.evaluate_wer_preflight("world.exe", self.folder, query)
        self.assertFalse(result["configured"])
        self.assertIn("does not match", result["reason"])

    def test_missing_keys_are_not_configured(self) -> None:
        result = rq.evaluate_wer_preflight("world.exe", self.folder, lambda key: (1, ""))
        self.assertFalse(result["configured"])
        self.assertIsNone(result["key"])

    def test_windows_preflight_without_folder_is_not_configured(self) -> None:
        result = rq.windows_dump_preflight(Path("world.exe"), None, lambda key: (0, ""))
        self.assertFalse(result["configured"])
        self.assertIn("not given", result["reason"])

    def test_unconfigured_windows_run_fails_closed(self) -> None:
        args = rq.parse_args(
            ["--exe", str(self.exe_path()), "--count", "1", "--receipt", str(self.folder / "r.json")]
        )
        with mock.patch.object(rq, "IS_WINDOWS", True):
            code = rq.run_qualification(args, [], query=lambda key: (1, ""))
        self.assertEqual(code, rq.PREFLIGHT_EXIT_CODE)
        receipt = json.loads((self.folder / "r.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt["status"], "PREFLIGHT_FAILED")
        self.assertEqual(receipt["iterations"], [])
        self.assertFalse(receipt["environment"]["wer"]["configured"])
        self.assertFalse(receipt["environment"]["allow_no_dumps"])

    def exe_path(self) -> Path:
        path = Path(self._tmp.name) / "world.exe"
        path.write_bytes(b"placeholder")
        return path

    def test_application_key_with_only_dump_type_keeps_global_folder(self) -> None:
        app_key = f"{REGISTRY_ROOT}\\world.exe"
        outputs = {
            app_key: "\r\n    DumpType    REG_DWORD    0x1\r\n",
            REGISTRY_ROOT: self.reg_output(str(self.folder), dump_type="2"),
        }

        def query(key: str) -> tuple[int, str]:
            return (0, outputs[key]) if key in outputs else (1, "")

        result = rq.evaluate_wer_preflight("world.exe", self.folder, query)
        self.assertTrue(result["configured"], result)
        self.assertEqual(result["dump_type"], "0x1")
        self.assertEqual(result["dump_type_value"], 1)
        self.assertEqual(result["sources"], {"DumpFolder": REGISTRY_ROOT, "DumpType": app_key})
        self.assertEqual(result["key"], REGISTRY_ROOT)

    def test_application_value_overrides_global_value_individually(self) -> None:
        app_key = f"{REGISTRY_ROOT}\\world.exe"
        other = self.folder.parent / "other"
        outputs = {
            app_key: "\r\n    DumpFolder    REG_EXPAND_SZ    " + str(other) + "\r\n",
            REGISTRY_ROOT: self.reg_output(str(self.folder), dump_type="2"),
        }

        def query(key: str) -> tuple[int, str]:
            return (0, outputs[key]) if key in outputs else (1, "")

        result = rq.evaluate_wer_preflight("world.exe", self.folder, query)
        self.assertFalse(result["configured"])
        self.assertIn("does not match", result["reason"])
        self.assertEqual(result["sources"]["DumpFolder"], app_key)
        self.assertEqual(result["sources"]["DumpType"], REGISTRY_ROOT)
        self.assertEqual(result["dump_type"], "0x2")

    def test_invalid_application_dump_type_fails_validation(self) -> None:
        app_key = f"{REGISTRY_ROOT}\\world.exe"
        outputs = {app_key: self.reg_output(str(self.folder), dump_type="99")}

        def query(key: str) -> tuple[int, str]:
            return (0, outputs[key]) if key in outputs else (1, "")

        result = rq.evaluate_wer_preflight("world.exe", self.folder, query)
        self.assertFalse(result["configured"])
        self.assertIn("not one of 0, 1, 2", result["reason"])
        self.assertEqual(result["dump_type_value"], 0x99)

    def test_invalid_global_dump_type_is_used_when_application_key_is_silent(self) -> None:
        app_key = f"{REGISTRY_ROOT}\\world.exe"
        outputs = {
            app_key: "\r\n    DumpFolder    REG_EXPAND_SZ    " + str(self.folder) + "\r\n",
            REGISTRY_ROOT: self.reg_output(str(self.folder), dump_type="99"),
        }

        def query(key: str) -> tuple[int, str]:
            return (0, outputs[key]) if key in outputs else (1, "")

        result = rq.evaluate_wer_preflight("world.exe", self.folder, query)
        self.assertFalse(result["configured"])
        self.assertIn("not one of 0, 1, 2", result["reason"])
        self.assertEqual(result["sources"]["DumpType"], REGISTRY_ROOT)

    def test_empty_dump_folder_fails_validation(self) -> None:
        app_key = f"{REGISTRY_ROOT}\\world.exe"
        outputs = {app_key: "\r\n    DumpFolder    REG_SZ    \r\n"}

        def query(key: str) -> tuple[int, str]:
            return (0, outputs[key]) if key in outputs else (1, "")

        result = rq.evaluate_wer_preflight("world.exe", self.folder, query)
        self.assertFalse(result["configured"])
        self.assertEqual(result["reason"], "effective DumpFolder is empty")


class CorePatternTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="repeat-qualification-core-")
        self.root = Path(self._tmp.name)
        self.work = self.root / "work"
        self.work.mkdir()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_relative_parent_resolves_against_child_cwd(self) -> None:
        self.assertEqual(
            rq.core_directory_for_pattern("cores/core.%p", self.work),
            os.path.normpath(str(self.work / "cores")),
        )

    def test_bare_relative_pattern_uses_child_cwd(self) -> None:
        self.assertEqual(rq.core_directory_for_pattern("core.%p", self.work), os.path.normpath(str(self.work)))

    def test_core_name_regex_matches_only_names_the_pattern_can_produce(self) -> None:
        names = rq.core_name_regex("cores/core.%p")
        self.assertIsNotNone(names.fullmatch("core.123"))
        self.assertIsNone(names.fullmatch("chunk.bin"))
        self.assertIsNone(names.fullmatch("core.123.bak"))
        self.assertIsNone(names.fullmatch("core"))

        default = rq.core_name_regex("core")
        self.assertIsNotNone(default.fullmatch("core"))
        self.assertIsNotNone(default.fullmatch("core.42"))
        self.assertIsNone(default.fullmatch("core.x"))
        self.assertIsNone(default.fullmatch("chunk.bin"))

        executable = rq.core_name_regex("/var/crash/%e.core.%p")
        self.assertIsNotNone(executable.fullmatch("world.exe.core.9"))
        self.assertIsNone(executable.fullmatch("world.exe.dump"))

        literal = rq.core_name_regex("100%%.core")
        self.assertIsNotNone(literal.fullmatch("100%.core"))

    def test_pid_suffix_is_accepted_for_patterns_without_pid(self) -> None:
        # The kernel appends .PID when core_uses_pid is set and the pattern has no %p.
        executable = rq.core_name_regex("%e.core")
        self.assertIsNotNone(executable.fullmatch("world_test.core"))
        self.assertIsNotNone(executable.fullmatch("world_test.core.123"))
        self.assertIsNone(executable.fullmatch("world_test.core.123.4"))
        self.assertIsNone(executable.fullmatch("world_test.core.x"))

        literal = rq.core_name_regex("100%%.core")
        self.assertIsNotNone(literal.fullmatch("100%.core.7"))

    def test_pattern_with_pid_takes_no_extra_suffix(self) -> None:
        names = rq.core_name_regex("core.%p")
        self.assertIsNotNone(names.fullmatch("core.123"))
        self.assertIsNone(names.fullmatch("core.123.4"))
        self.assertIsNone(names.fullmatch("core.123.bak"))

        # %%p is a literal "%p", not a pid specifier, so the pid suffix still applies.
        literal_percent_p = rq.core_name_regex("core.%%p")
        self.assertIsNotNone(literal_percent_p.fullmatch("core.%p"))
        self.assertIsNotNone(literal_percent_p.fullmatch("core.%p.9"))
        self.assertIsNone(literal_percent_p.fullmatch("core.%p.9.9"))

    def test_core_collection_copies_only_core_named_files(self) -> None:
        skip = rq.core_collection_skip(self.root / "run", self.root / "out" / "receipt.json", "core.%p")
        before = rq.snapshot_files(self.work, recursive=True, skip=skip)
        (self.work / "core.123").write_bytes(b"core")
        (self.work / "world").mkdir()
        (self.work / "world" / "chunk.bin").write_bytes(b"chunk")
        copies = rq.copy_changed_files(self.work, before, self.root / "iter-1" / "core", recursive=True, skip=skip)
        self.assertEqual([Path(copy).name for copy in copies], ["core.123"])

    def test_copy_failure_is_reported_per_file_when_errors_are_collected(self) -> None:
        (self.work / "core.1").write_bytes(b"first")
        before = rq.snapshot_files(self.work, recursive=True)
        (self.work / "core.2").write_bytes(b"second")
        (self.work / "core.3").write_bytes(b"third")
        errors: list[str] = []
        with mock.patch.object(rq.shutil, "copy2", side_effect=OSError("disk full")):
            copies = rq.copy_changed_files(
                self.work, before, self.root / "iter-1" / "core", recursive=True, errors=errors
            )
        self.assertEqual(copies, [])
        self.assertEqual(len(errors), 2)
        self.assertTrue(all("disk full" in error for error in errors))

    def test_copy_failure_propagates_without_an_error_list(self) -> None:
        before = rq.snapshot_files(self.work, recursive=True)
        (self.work / "core.2").write_bytes(b"second")
        with mock.patch.object(rq.shutil, "copy2", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                rq.copy_changed_files(self.work, before, self.root / "iter-1" / "core", recursive=True)

    def test_core_directory_outside_cwd_that_is_missing_fails(self) -> None:
        missing = self.root / "nowhere" / "cores"
        check = rq.core_directory_preflight(missing, self.work)
        self.assertIn("outside the child working directory", check["problem"])
        self.assertFalse(check["created"])
        self.assertFalse(missing.exists())

    def test_missing_relative_core_directory_under_cwd_is_created(self) -> None:
        directory = Path(rq.core_directory_for_pattern("cores/core.%p", self.work))
        check = rq.core_directory_preflight(directory, self.work)
        self.assertIsNone(check["problem"])
        self.assertTrue(check["created"])
        self.assertTrue(check["writable"])
        self.assertTrue(directory.is_dir())

    def test_unwritable_core_directory_fails(self) -> None:
        cores = self.work / "cores"
        cores.mkdir()
        check = rq.core_directory_preflight(cores, self.work, access=lambda path, mode: False)
        self.assertIn("not writable", check["problem"])
        self.assertFalse(check["writable"])

    def test_core_directory_that_cannot_be_created_fails(self) -> None:
        (self.work / "blocker").write_text("a file, not a directory")
        check = rq.core_directory_preflight(self.work / "blocker" / "cores", self.work)
        self.assertIn("could not be created", check["problem"])

    def test_absolute_parent_is_used_as_is(self) -> None:
        absolute = self.root / "abs" / "core.%p"
        self.assertEqual(
            rq.core_directory_for_pattern(str(absolute), self.work),
            os.path.normpath(str(self.root / "abs")),
        )

    def test_piped_and_unresolvable_patterns_have_no_directory(self) -> None:
        self.assertIsNone(rq.core_directory_for_pattern("|/wsl-capture-crash %p", self.work))
        self.assertIsNone(rq.core_directory_for_pattern(str(self.root / "%e" / "core"), self.work))
        self.assertIsNone(rq.core_directory_for_pattern(None, self.work))

    def test_preflight_problems_fail_closed_for_each_gap(self) -> None:
        good = {
            "core_pattern": "core.%p",
            "core_pattern_piped": False,
            "core_directory": str(self.work),
        }
        self.assertEqual(rq.posix_preflight_problems(good, 4096), [])

        piped = dict(good, core_pattern="|/wsl-capture-crash %p", core_pattern_piped=True, core_directory=None)
        self.assertIn("pipes cores", rq.posix_preflight_problems(piped, 4096)[0])

        unknown = dict(good, core_directory=None)
        self.assertIn("cannot be determined", rq.posix_preflight_problems(unknown, 4096)[0])

        zero = rq.posix_preflight_problems(good, 0)
        self.assertIn("soft limit is 0", zero[0])

    def test_core_limit_target_raises_soft_to_hard_only(self) -> None:
        self.assertEqual(rq.core_limit_target((100, 4096)), (4096, 4096))
        self.assertEqual(rq.core_limit_target((0, 0)), (0, 0))

    def test_relative_core_directory_collects_nested_cores(self) -> None:
        cores = self.work / "cores"
        nested = cores / "nested"
        nested.mkdir(parents=True)
        before = rq.snapshot_files(cores, recursive=True)
        (nested / "core.1234").write_bytes(b"core")
        copies = rq.copy_changed_files(cores, before, self.root / "iter-1" / "core", recursive=True)
        self.assertEqual(len(copies), 1, copies)
        self.assertEqual(Path(copies[0]).relative_to(self.root / "iter-1" / "core"), Path("nested") / "core.1234")
        self.assertEqual(Path(copies[0]).read_bytes(), b"core")

    def test_harness_output_inside_core_directory_is_not_collected(self) -> None:
        cores = self.work / "cores"
        receipt = cores / "out" / "receipt.json"
        run_dir = cores / "out" / "run-1"
        run_dir.mkdir(parents=True)
        skip = rq.harness_output_skip(run_dir, receipt)
        before = rq.snapshot_files(cores, recursive=True, skip=skip)
        (run_dir / "iter-1.log").write_text("log")
        (cores / "out" / receipt.name).write_text("{}")
        (cores / "out" / f".{receipt.name}.abc.tmp").write_text("{}")
        self.assertEqual(rq.copy_changed_files(cores, before, self.root / "dest", True, skip), [])


class WindowsCommandTest(unittest.TestCase):
    def test_batch_launcher_with_spaced_launcher_and_output_directories(self) -> None:
        exe = Path(r"C:\Test Bin\fake child.cmd")
        json_path = Path(r"C:\Out Dir\run 1\iter-1\gtest.json")
        with mock.patch.object(rq, "IS_WINDOWS", True):
            target = rq.spawn_target(rq.build_command(exe, "Fake.Filter", [], json_path))
        self.assertEqual(
            target,
            'cmd.exe /d /s /c ""C:\\Test Bin\\fake child.cmd" "--gtest_filter=Fake.Filter" '
            '"--gtest_output=json:C:\\Out Dir\\run 1\\iter-1\\gtest.json" "--gtest_catch_exceptions=0""',
        )

    def test_batch_launcher_quotes_metacharacter_paths(self) -> None:
        exe = Path(r"C:\A&B\child.cmd")
        json_path = Path(r"C:\R&D\gtest.json")
        with mock.patch.object(rq, "IS_WINDOWS", True):
            target = rq.spawn_target(rq.build_command(exe, "Fake.Filter", [], json_path))
        self.assertEqual(
            target,
            'cmd.exe /d /s /c ""C:\\A&B\\child.cmd" "--gtest_filter=Fake.Filter" '
            '"--gtest_output=json:C:\\R&D\\gtest.json" "--gtest_catch_exceptions=0""',
        )

    def test_batch_launcher_rejects_percent_in_an_argument(self) -> None:
        with mock.patch.object(rq, "IS_WINDOWS", True):
            with self.assertRaises(ValueError):
                rq.spawn_target(rq.build_command(Path(r"C:\100%\child.cmd"), "Fake.Filter", [], Path("g.json")))

    def test_quote_cmd_argument(self) -> None:
        self.assertEqual(rq.quote_cmd_argument(r"C:\plain\x.exe"), r"C:\plain\x.exe")
        self.assertEqual(rq.quote_cmd_argument(r"C:\A&B\child.cmd"), r'"C:\A&B\child.cmd"')
        self.assertEqual(rq.quote_cmd_argument(r"C:\R&D\gtest.json"), r'"C:\R&D\gtest.json"')
        self.assertEqual(rq.quote_cmd_argument(r"C:\Test Bin\x.exe"), r'"C:\Test Bin\x.exe"')
        self.assertEqual(rq.quote_cmd_argument("a|b<c>d^e(f)g!h,i;j=k"), '"a|b<c>d^e(f)g!h,i;j=k"')
        with self.assertRaises(ValueError):
            rq.quote_cmd_argument(r"C:\100%\x.exe")
        with self.assertRaises(ValueError):
            rq.quote_cmd_argument('say "hi"')
        with self.assertRaises(ValueError):
            rq.quote_cmd_argument("C:\\dir with space\\")

    def test_executable_launcher_is_passed_as_an_argument_list(self) -> None:
        with mock.patch.object(rq, "IS_WINDOWS", True):
            command = rq.build_command(Path(r"C:\Test Bin\world.exe"), "Fake.Filter", [], Path("g.json"))
            target = rq.spawn_target(command)
        self.assertEqual(target, command)


if __name__ == "__main__":
    # Every passing case still runs. Stop on the first real failure so bounded
    # fixture failures cannot accumulate until CTest erases their diagnostics.
    unittest.main(failfast=True)

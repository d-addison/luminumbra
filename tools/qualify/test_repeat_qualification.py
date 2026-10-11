#!/usr/bin/env python3
"""Contract tests for repeat_qualification.py using a small fake child executable.

Run with `python3 -I test_repeat_qualification.py`. The harness is loaded from its file path,
so the test does not depend on the current directory or on sys.path.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

SCRIPT = Path(__file__).resolve().with_name("repeat_qualification.py")
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
print(f"fake child iteration {iteration} mode {mode}", flush=True)

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
if kind == "hang":
    grandchild = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(600)"])
    (crash_dir / "grandchild.pid").write_text(str(grandchild.pid))
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


def pid_alive(pid: int) -> bool:
    if os.name == "nt":
        listing = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
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


class HarnessTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="repeat-qualification-test-")
        self.root = Path(self._tmp.name)
        self.work = self.root / "work"
        self.work.mkdir()
        self.receipt = self.root / "out" / "receipt.json"
        self.exe = self._write_fake_child()

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
        return args

    def run_raw(self, args: list[str], extra_env: dict[str, str] | None = None, timeout: float = 120):
        env = dict(os.environ)
        env.update(extra_env or {})
        return subprocess.run(
            [sys.executable, "-I", str(SCRIPT), *args],
            capture_output=True,
            text=True,
            env=env,
            timeout=timeout,
            check=False,
        )

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
        return self.run_raw(args, extra_env=extra_env, timeout=timeout * 10 + 60)

    def load_receipt(self) -> dict:
        return json.loads(self.receipt.read_text(encoding="utf-8"))

    def run_dir(self) -> Path:
        runs = [p for p in self.receipt.parent.iterdir() if p.is_dir() and p.name.startswith("run-")]
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
            self.assertIn(f"fake child iteration {index}", log.read_text(encoding="utf-8"))
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
        # A file without execute permission (POSIX) or a non-executable image (Windows)
        # makes the launch itself fail.
        broken = self.root / "bin dir" / "not_executable.exe"
        broken.write_bytes(b"not an executable image")
        if os.name != "nt":
            broken.chmod(0o644)
        self.exe = broken
        result = self.run_script(count=2, mode="pass")
        self.assertEqual(result.returncode, 1, result.stderr)

        receipt = self.load_receipt()
        self.assertEqual(receipt["status"], "failed")
        self.assertIsNotNone(receipt["end_time"])
        self.assertEqual(len(receipt["iterations"]), 2)
        self.assertTrue(receipt["iterations"][0]["status"].startswith("launch_error"))
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
            self.assertEqual(environment["rlimit_core_child"], "unlimited")
            self.assertIn("core_pattern", environment)
            for artifact in receipt["iterations"][0]["crash_artifacts"]:
                self.assertTrue(Path(artifact).is_file(), artifact)
                self.assertTrue(artifact.startswith(str(self.run_dir())), artifact)


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


if __name__ == "__main__":
    unittest.main()

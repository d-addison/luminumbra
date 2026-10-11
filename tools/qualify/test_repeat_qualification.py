#!/usr/bin/env python3
"""Contract tests for repeat_qualification.py using a small fake child executable."""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().with_name("repeat_qualification.py")

FAKE_CHILD_SOURCE = r'''
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

mode = "pass"
for argument in sys.argv[1:]:
    if argument.startswith("--fake="):
        mode = argument.split("=", 1)[1]

crash_dir = Path(os.environ["LUMINUMBRA_CRASH_DIR"])
iteration = int(crash_dir.name.split("-", 1)[1])
print(f"fake child iteration {iteration} mode {mode}", flush=True)

kind, _, target = mode.partition("@")
target_iteration = int(target) if target else None

if kind == "pass" or (target_iteration is not None and iteration != target_iteration):
    sys.exit(0)
if kind == "exit3":
    sys.exit(3)
if kind == "kill":
    os.kill(os.getpid(), signal.SIGKILL)
if kind == "hang":
    grandchild = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(600)"])
    (crash_dir / "grandchild.pid").write_text(str(grandchild.pid))
    time.sleep(600)
if kind == "probe":
    receipt = json.loads(Path(os.environ["LUMINUMBRA_TEST_RECEIPT"]).read_text())
    print(f"receipt_iterations={len(receipt['iterations'])}", flush=True)
    sys.exit(0)
sys.exit(4)
'''

POSIX_LAUNCHER = "#!{python}\n"
WINDOWS_LAUNCHER = '@"{python}" -I "{child}" %*\r\n'


def pid_alive(pid: int) -> bool:
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


class RepeatQualificationTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="repeat-qualification-test-")
        self.root = Path(self._tmp.name)
        self.crash_dir = self.root / "crash"
        self.receipt = self.root / "out" / "receipt.json"
        self.exe = self._write_fake_child()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_fake_child(self) -> Path:
        child_py = self.root / "fake_child.py"
        child_py.write_text(FAKE_CHILD_SOURCE, encoding="utf-8")
        if os.name == "nt":
            launcher = self.root / "fake_child.cmd"
            launcher.write_text(
                WINDOWS_LAUNCHER.format(python=sys.executable, child=child_py), encoding="utf-8"
            )
        else:
            launcher = self.root / "fake_child"
            launcher.write_text(
                POSIX_LAUNCHER.format(python=sys.executable) + FAKE_CHILD_SOURCE, encoding="utf-8"
            )
            launcher.chmod(0o755)
        return launcher

    def run_script(
        self,
        *,
        count: int,
        mode: str,
        timeout: float = 30,
        extra_env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            "-I",
            str(SCRIPT),
            "--exe",
            str(self.exe),
            "--gtest-filter",
            "Fake.Filter",
            "--count",
            str(count),
            "--timeout-seconds",
            str(timeout),
            "--crash-dir",
            str(self.crash_dir),
            "--receipt",
            str(self.receipt),
            "--",
            f"--fake={mode}",
        ]
        env = dict(os.environ)
        env.update(extra_env or {})
        return subprocess.run(
            command, capture_output=True, text=True, env=env, timeout=timeout * 10 + 60, check=False
        )

    def load_receipt(self) -> dict:
        return json.loads(self.receipt.read_text(encoding="utf-8"))

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
        for index in range(1, 4):
            log = self.receipt.parent / f"iter-{index}.log"
            self.assertTrue(log.is_file(), log)
            self.assertIn(f"fake child iteration {index}", log.read_text(encoding="utf-8"))

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
        self.assertEqual(first["log_path"], str(self.receipt.parent / "iter-2.log"))

    @unittest.skipIf(os.name == "nt", "signal death is POSIX-only")
    def test_child_killed_by_signal_is_a_failure(self) -> None:
        result = self.run_script(count=2, mode="kill@1")
        self.assertEqual(result.returncode, 1)

        receipt = self.load_receipt()
        first = receipt["iterations"][0]
        self.assertEqual(first["signal"], "SIGKILL")
        self.assertIsNone(first["exit_code"])
        self.assertEqual(receipt["exit_code_histogram"], {"exit:0": 1, "signal:SIGKILL": 1})
        self.assertEqual(receipt["first_failure"]["index"], 1)

    def test_timeout_kills_the_whole_process_tree(self) -> None:
        result = self.run_script(count=1, mode="hang@1", timeout=5)
        self.assertEqual(result.returncode, 1)

        receipt = self.load_receipt()
        iteration = receipt["iterations"][0]
        self.assertTrue(iteration["timed_out"])
        self.assertEqual(receipt["exit_code_histogram"], {"timeout": 1})
        self.assertEqual(receipt["first_failure"]["index"], 1)

        grandchild_file = self.crash_dir / "iter-1" / "grandchild.pid"
        self.assertTrue(grandchild_file.is_file())
        if os.name != "nt":
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
            log = (self.receipt.parent / f"iter-{index}.log").read_text(encoding="utf-8")
            self.assertIn(f"receipt_iterations={index - 1}", log)
        self.assertEqual(len(self.load_receipt()["iterations"]), 3)

    def test_missing_executable_is_rejected(self) -> None:
        self.exe.unlink()
        result = self.run_script(count=1, mode="pass")
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.receipt.exists())


if __name__ == "__main__":
    unittest.main()

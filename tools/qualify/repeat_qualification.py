#!/usr/bin/env python3
"""Run a gtest executable repeatedly and write a qualification receipt.

Each iteration runs in its own process group (session on POSIX) with output captured
to a per-iteration log. An iteration that exceeds the timeout is killed together with
its whole process tree. Crash artifacts written under a per-iteration directory are
recorded. The receipt is rewritten atomically after every iteration.

Any non-zero exit, signal death, or timeout counts as a failure, and the script exits
non-zero when any iteration failed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import signal
import subprocess
import sys
import tempfile
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCHEMA = "luminumbra.repeat_qualification.v1"
LABEL = "process repetitions"
DEFAULT_GTEST_FILTER = (
    "WorldOpenRefusal.CurrentServerBootKeepsLoadedAuthorityAndSkipsWaterSettle"
)
DEFAULT_TIMEOUT_SECONDS = 120.0
CRASH_DIR_ENV = "LUMINUMBRA_CRASH_DIR"
IS_WINDOWS = os.name == "nt"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Repeat a gtest executable and write a qualification receipt. "
        "Arguments after '--' are passed to the executable."
    )
    parser.add_argument("--exe", required=True, type=Path, help="executable to run")
    parser.add_argument(
        "--gtest-filter",
        default=DEFAULT_GTEST_FILTER,
        help="value for --gtest_filter (default: %(default)s)",
    )
    parser.add_argument("--cwd", type=Path, default=None, help="working directory for each run")
    parser.add_argument("--count", type=int, required=True, help="number of iterations")
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help="per-iteration timeout (default: %(default)s)",
    )
    parser.add_argument(
        "--crash-dir",
        required=True,
        type=Path,
        help="directory for crash artifacts; each iteration uses <crash-dir>/iter-<n>",
    )
    parser.add_argument("--receipt", required=True, type=Path, help="receipt JSON path")
    return parser.parse_args(argv)


def split_extra_args(argv: list[str]) -> tuple[list[str], list[str]]:
    if "--" in argv:
        index = argv.index("--")
        return argv[:index], argv[index + 1 :]
    return argv, []


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def snapshot_files(root: Path) -> set[str]:
    if not root.is_dir():
        return set()
    return {str(path) for path in root.rglob("*") if path.is_file()}


def signal_name(signum: int) -> str:
    try:
        return signal.Signals(signum).name
    except ValueError:
        return f"SIG{signum}"


def kill_process_tree(process: subprocess.Popen[bytes]) -> None:
    if IS_WINDOWS:
        subprocess.run(
            ["taskkill", "/T", "/F", "/PID", str(process.pid)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def run_iteration(
    command: list[str],
    cwd: Path | None,
    env: dict[str, str],
    log_path: Path,
    timeout: float,
) -> dict[str, Any]:
    popen_options: dict[str, Any] = {}
    if IS_WINDOWS:
        popen_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        popen_options["start_new_session"] = True

    started = time.monotonic()
    timed_out = False
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            command,
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
            kill_process_tree(process)
            returncode = process.wait()
    duration = time.monotonic() - started

    if timed_out:
        return {
            "exit_code": None,
            "signal": None,
            "timed_out": True,
            "duration_seconds": round(duration, 3),
            "status": "timeout",
        }
    if not IS_WINDOWS and returncode < 0:
        name = signal_name(-returncode)
        return {
            "exit_code": None,
            "signal": name,
            "timed_out": False,
            "duration_seconds": round(duration, 3),
            "status": f"signal:{name}",
        }
    return {
        "exit_code": returncode,
        "signal": None,
        "timed_out": False,
        "duration_seconds": round(duration, 3),
        "status": f"exit:{returncode}",
    }


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


def run_qualification(args: argparse.Namespace, extra: list[str]) -> int:
    exe = Path(os.path.abspath(args.exe))
    if not exe.is_file():
        print(f"error: executable not found: {exe}", file=sys.stderr)
        return 2
    if args.count < 1:
        print("error: --count must be at least 1", file=sys.stderr)
        return 2

    receipt_path = Path(os.path.abspath(args.receipt))
    log_dir = receipt_path.parent
    log_dir.mkdir(parents=True, exist_ok=True)
    crash_dir = Path(os.path.abspath(args.crash_dir))
    crash_dir.mkdir(parents=True, exist_ok=True)
    cwd = Path(os.path.abspath(args.cwd)) if args.cwd is not None else None

    command = [str(exe), f"--gtest_filter={args.gtest_filter}", *extra]
    receipt: dict[str, Any] = {
        "schema": SCHEMA,
        "label": LABEL,
        "command": command,
        "exe": {"path": str(exe), "sha256": sha256_file(exe)},
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "python": platform.python_version(),
        },
        "cwd": str(cwd) if cwd is not None else os.getcwd(),
        "crash_dir": str(crash_dir),
        "count": args.count,
        "timeout_seconds": args.timeout_seconds,
        "status": "running",
        "start_time": utc_now(),
        "end_time": None,
        "iterations": [],
        "exit_code_histogram": {},
        "failure_count": 0,
        "first_failure": None,
    }
    write_receipt(receipt_path, receipt)

    histogram: Counter[str] = Counter()
    failures = 0
    for index in range(1, args.count + 1):
        iteration_crash_dir = crash_dir / f"iter-{index}"
        iteration_crash_dir.mkdir(parents=True, exist_ok=True)
        log_path = log_dir / f"iter-{index}.log"

        before = snapshot_files(crash_dir)
        env = dict(os.environ)
        env[CRASH_DIR_ENV] = str(iteration_crash_dir)
        result = run_iteration(command, cwd, env, log_path, args.timeout_seconds)
        crash_artifacts = sorted(snapshot_files(crash_dir) - before)

        failed = result["timed_out"] or result["exit_code"] != 0 or result["signal"] is not None
        entry = {
            "index": index,
            "log_path": str(log_path),
            "crash_artifacts": crash_artifacts,
            "failed": failed,
            **result,
        }
        receipt["iterations"].append(entry)
        histogram[result["status"]] += 1
        receipt["exit_code_histogram"] = dict(sorted(histogram.items()))

        if failed:
            failures += 1
            if receipt["first_failure"] is None:
                receipt["first_failure"] = {
                    "index": index,
                    "log_path": str(log_path),
                    "crash_artifacts": crash_artifacts,
                }
        receipt["failure_count"] = failures
        write_receipt(receipt_path, receipt)

    receipt["status"] = "failed" if failures else "passed"
    receipt["end_time"] = utc_now()
    write_receipt(receipt_path, receipt)

    summary = "failed" if failures else "passed"
    print(f"{LABEL}: {summary} ({args.count - failures}/{args.count} iterations clean), receipt {receipt_path}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    raw = sys.argv[1:] if argv is None else argv
    own_args, extra = split_extra_args(raw)
    args = parse_args(own_args)
    return run_qualification(args, extra)


if __name__ == "__main__":
    sys.exit(main())

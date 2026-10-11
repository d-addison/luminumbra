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
directory. On POSIX the child runs with RLIMIT_CORE set to unlimited and the new core files
in its working directory are copied into the iteration. On Windows the executable must have
a WER LocalDumps DumpFolder that matches --wer-dump-folder, and new dumps are copied into the
iteration. Without that setup the run fails closed with status PREFLIGHT_FAILED.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

try:
    import resource
except ImportError:  # Windows
    resource = None

SCHEMA = "luminumbra.repeat_qualification.v1"
LABEL = "process repetitions"
DEFAULT_GTEST_FILTER = (
    "WorldOpenRefusal.CurrentServerBootKeepsLoadedAuthorityAndSkipsWaterSettle"
)
DEFAULT_TIMEOUT_SECONDS = 120.0
CLEANUP_TIMEOUT_SECONDS = 5.0
DUMP_SETTLE_SECONDS = 30.0
REGISTRY_QUERY_TIMEOUT_SECONDS = 30.0
PREFLIGHT_EXIT_CODE = 3
CRASH_DIR_ENV = "LUMINUMBRA_CRASH_DIR"
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


def make_run_directory(parent: Path, stamp: str, pid: int) -> Path:
    """Create run-<stamp>-<pid> under parent; refuses (FileExistsError) if it already exists."""
    run_dir = parent / f"run-{stamp}-{pid}"
    run_dir.mkdir(parents=True, exist_ok=False)
    return run_dir


def snapshot_files(root: Path | None, recursive: bool) -> dict[str, tuple[int, int]]:
    """Map each file path to (size, mtime_ns). Rewritten files change their entry."""
    if root is None or not root.is_dir():
        return {}
    paths = root.rglob("*") if recursive else root.iterdir()
    result: dict[str, tuple[int, int]] = {}
    for path in paths:
        try:
            if not path.is_file():
                continue
            stat = path.stat()
        except OSError:
            continue
        result[str(path)] = (stat.st_size, stat.st_mtime_ns)
    return result


def changed_files(before: dict[str, tuple[int, int]], after: dict[str, tuple[int, int]]) -> list[str]:
    return sorted(path for path, stamp in after.items() if before.get(path) != stamp)


def copy_changed_files(
    root: Path | None,
    before: dict[str, tuple[int, int]],
    destination: Path,
    recursive: bool = False,
) -> list[str]:
    copies: list[str] = []
    for source in changed_files(before, snapshot_files(root, recursive)):
        destination.mkdir(parents=True, exist_ok=True)
        target = destination / Path(source).name
        shutil.copy2(source, target)
        copies.append(str(target))
    return copies


def signal_name(signum: int) -> str:
    try:
        return signal.Signals(signum).name
    except ValueError:
        return f"SIG{signum}"


def allow_core_dumps() -> None:
    if resource is None:
        return
    resource.setrlimit(resource.RLIMIT_CORE, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))


def limit_text(value: int) -> int | str:
    if resource is not None and value == resource.RLIM_INFINITY:
        return "unlimited"
    return value


def posix_crash_environment(cwd: Path) -> dict[str, Any]:
    pattern_file = Path("/proc/sys/kernel/core_pattern")
    pattern = pattern_file.read_text(encoding="utf-8").strip() if pattern_file.is_file() else None
    soft, hard = resource.getrlimit(resource.RLIMIT_CORE) if resource is not None else (None, None)
    core_directory: str | None = None
    if pattern and not pattern.startswith("|"):
        candidate = Path(pattern).parent if os.path.isabs(pattern) else cwd
        if "%" not in str(candidate):
            core_directory = str(candidate)
    return {
        "core_pattern": pattern,
        "core_directory": core_directory,
        "rlimit_core_parent": {
            "soft": None if soft is None else limit_text(soft),
            "hard": None if hard is None else limit_text(hard),
        },
        "rlimit_core_child": "unlimited",
    }


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


def evaluate_wer_preflight(
    exe_name: str,
    expected_folder: Path,
    query: Callable[[str], tuple[int, str]] = query_registry_key,
) -> dict[str, Any]:
    """Check the LocalDumps configuration for exe_name, then the global LocalDumps key."""
    for key in (f"{WER_KEY_ROOT}\\{exe_name}", WER_KEY_ROOT):
        returncode, output = query(key)
        if returncode != 0:
            continue
        values = parse_reg_values(output)
        folder = values.get("DumpFolder")
        if folder is None:
            continue
        actual = os.path.expandvars(folder)
        configured = same_directory(actual, str(expected_folder))
        reason = None
        if not configured:
            reason = f"DumpFolder {actual!r} does not match --wer-dump-folder {str(expected_folder)!r}"
        return {
            "configured": configured,
            "key": key,
            "dump_folder": actual,
            "dump_type": values.get("DumpType"),
            "reason": reason,
        }
    return {
        "configured": False,
        "key": None,
        "dump_folder": None,
        "dump_type": None,
        "reason": f"no LocalDumps DumpFolder under {WER_KEY_ROOT} for {exe_name} or globally",
    }


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
    prefix = [str(exe)]
    if IS_WINDOWS and exe.suffix.lower() in (".cmd", ".bat"):
        prefix = ["cmd.exe", "/d", "/c", str(exe)]
    return [
        *prefix,
        f"--gtest_filter={gtest_filter}",
        f"--gtest_output=json:{json_path}",
        "--gtest_catch_exceptions=0",
        *extra,
    ]


def build_child_environment(base: dict[str, str], crash_dir: Path) -> dict[str, str]:
    env = {name: value for name, value in base.items() if name not in SCRUBBED_GTEST_ENV}
    env[CRASH_DIR_ENV] = str(crash_dir)
    return env


def terminate_process_tree(process: subprocess.Popen[bytes]) -> list[str]:
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
) -> dict[str, Any]:
    popen_options: dict[str, Any] = {}
    if IS_WINDOWS:
        popen_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        popen_options["start_new_session"] = True
        popen_options["preexec_fn"] = allow_core_dumps

    started = time.monotonic()
    timed_out = False
    cleanup_errors: list[str] = []
    try:
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
                cleanup_errors = terminate_process_tree(process)
                returncode = process.returncode
    except (OSError, ValueError) as exc:
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
    gtest_filter: str,
    extra: list[str],
    cwd: Path | None,
    timeout: float,
    expected_tests: int,
    dump_folder: Path | None,
    core_directory: Path | None,
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

    dump_before = snapshot_files(dump_folder, recursive=False)
    core_before = snapshot_files(core_directory, recursive=False)
    command = build_command(exe, gtest_filter, extra, gtest_json)
    env = build_child_environment(dict(os.environ), crash_dir)
    result = run_iteration(command, cwd, env, log_path, timeout)

    problem: str | None = None
    if result["status"] == "exit:0":
        problem = gtest_report_problem(gtest_json, expected_tests)
        if problem is not None:
            result["status"] = "gtest_invalid"

    artifacts = sorted(str(path) for path in crash_dir.rglob("*") if path.is_file())
    if IS_WINDOWS and dump_folder is not None and result["status"] not in ("exit:0", "timeout"):
        wait_for_dump_settle(dump_folder, dump_before, DUMP_SETTLE_SECONDS)
    artifacts += copy_changed_files(dump_folder, dump_before, iter_dir / "wer")
    artifacts += copy_changed_files(core_directory, core_before, iter_dir / "core")
    return build_entry(index, log_path, gtest_json, crash_dir, sorted(artifacts), result, problem)


def wait_for_dump_settle(folder: Path, before: dict[str, tuple[int, int]], timeout: float) -> None:
    """Wait until new dump files stop changing, so WER output is not copied half-written."""
    deadline = time.monotonic() + timeout
    previous: dict[str, tuple[int, int]] | None = None
    while time.monotonic() < deadline:
        current = {path: stamp for path, stamp in snapshot_files(folder, False).items() if before.get(path) != stamp}
        if current and current == previous:
            return
        previous = current
        time.sleep(1.0)


def build_entry(
    index: int,
    log_path: Path,
    gtest_json: Path,
    crash_dir: Path,
    artifacts: list[str],
    result: dict[str, Any],
    problem: str | None,
) -> dict[str, Any]:
    reason = failure_reason(result, problem)
    return {
        "index": index,
        "log_path": str(log_path),
        "gtest_json": str(gtest_json),
        "crash_dir": str(crash_dir),
        "crash_artifacts": artifacts,
        "failed": reason is not None,
        "failure_reason": reason,
        **result,
    }


def run_qualification(
    args: argparse.Namespace,
    extra: list[str],
    query: Callable[[str], tuple[int, str]] = query_registry_key,
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
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    try:
        run_dir = make_run_directory(receipt_path.parent, stamp, os.getpid())
    except FileExistsError:
        print(f"error: run directory already exists; refusing to reuse it: {receipt_path.parent}", file=sys.stderr)
        return 2
    cwd = Path(os.path.abspath(args.cwd)) if args.cwd is not None else None
    base_cwd = cwd if cwd is not None else Path.cwd()

    environment: dict[str, Any] = {
        "scrubbed_gtest_env": [name for name in SCRUBBED_GTEST_ENV if name in os.environ],
        "allow_no_dumps": bool(args.allow_no_dumps),
    }
    dump_folder: Path | None = None
    core_directory: Path | None = None
    preflight_failed = False
    if IS_WINDOWS:
        preflight = windows_dump_preflight(exe, args.wer_dump_folder, query)
        environment["wer"] = preflight
        if preflight["configured"]:
            dump_folder = Path(os.path.abspath(args.wer_dump_folder))
            dump_folder.mkdir(parents=True, exist_ok=True)
        elif not args.allow_no_dumps:
            preflight_failed = True
    else:
        posix = posix_crash_environment(base_cwd)
        environment.update(posix)
        if posix["core_directory"] is not None:
            core_directory = Path(posix["core_directory"])

    receipt: dict[str, Any] = {
        "schema": SCHEMA,
        "label": LABEL,
        "command": build_command(exe, args.gtest_filter, extra, Path("<iter-N>") / "gtest.json"),
        "exe": {"path": str(exe), "sha256": sha256_file(exe)},
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
        "first_failure": None,
    }
    if preflight_failed:
        receipt["status"] = "PREFLIGHT_FAILED"
        receipt["failure_reason"] = environment["wer"]["reason"]
        receipt["end_time"] = utc_now()
        write_receipt(receipt_path, receipt)
        print(f"error: crash dumps are not configured: {receipt['failure_reason']}", file=sys.stderr)
        print(f"receipt {receipt_path}", file=sys.stderr)
        return PREFLIGHT_EXIT_CODE

    write_receipt(receipt_path, receipt)
    histogram: Counter[str] = Counter()
    failures = 0
    try:
        for index in range(1, args.count + 1):
            entry = run_indexed_iteration(
                index,
                run_dir,
                exe,
                args.gtest_filter,
                extra,
                cwd,
                args.timeout_seconds,
                args.expected_tests,
                dump_folder,
                core_directory,
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
            write_receipt(receipt_path, receipt)
        receipt["status"] = "failed" if failures else "passed"
    except BaseException as exc:
        receipt["status"] = "aborted"
        receipt["abort_reason"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        receipt["end_time"] = utc_now()
        write_receipt(receipt_path, receipt)

    summary = "failed" if failures else "passed"
    print(
        f"{LABEL}: {summary} ({args.count - failures}/{args.count} iterations clean), receipt {receipt_path}"
    )
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    raw = sys.argv[1:] if argv is None else argv
    own_args, extra = split_extra_args(raw)
    args = parse_args(own_args)
    return run_qualification(args, extra)


if __name__ == "__main__":
    sys.exit(main())

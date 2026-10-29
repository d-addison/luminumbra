#!/usr/bin/env python3
"""Offline policy tests for draft guards and existing workflow routing.

Read the workflows' indented job/step blocks using only the standard library.
This is deliberately scoped to their current YAML layout, not a YAML parser.

The 72-case baseline launched 772 subprocesses on POSIX; aggregate cancellation,
ASan admission and Bash syntax cases accounted for 340, 210 and 79 launches.
Windows qualification exhausted the suite's 30-second CTest deadline without
per-case timings. Thirteen disjoint registrations isolate those workloads and keep
each 30-second deadline. Each selected case reports elapsed time so a slow case
remains visible. Native per-case qualification is still required; local timings
do not establish that every Windows case fits its deadline. Direct invocation
and unittest discovery retain the complete suite.
"""

import argparse
from collections import deque
from concurrent.futures import Future, ThreadPoolExecutor, wait
from contextlib import closing
from itertools import islice, product
import fnmatch
import hashlib
import importlib.util
import io
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path
from typing import NamedTuple
from unittest.mock import MagicMock, patch


sys.path.insert(0, str(Path(__file__).resolve().parent))
import ci_shell as shell
import ctest_shards as shards


BASH = shell.bash_executable()
ROOT = Path(__file__).resolve().parents[2]


def write_bash_script(path, script):
    path.write_bytes(script.replace("\r\n", "\n").encode("utf-8"))
    return path.as_posix()


def run_bash_script(script, *, flags=("-e", "-o", "pipefail"), executable=BASH, arguments=(), **kwargs):
    # Close the LF-only file before Bash opens it; keep scripts out of Windows argv.
    with tempfile.TemporaryDirectory(prefix="workflow shell ") as directory:
        path = write_bash_script(Path(directory) / "script.sh", script)
        return subprocess.run([executable, "--noprofile", "--norc", *flags, path, *arguments], **kwargs)


GUARDED = {
    "ci.yml": ("linux-result", "Linux / GCC"),
    "asan.yml": ("asan", "Linux / GCC ASan"),
    "performance.yml": ("preview-frame", "Preview frame / software OpenGL"),
}
READY = "github.event_name != 'pull_request' || github.event.pull_request.draft == false"
CI_ACTIVE_EVENT = (
    "github.event_name != 'pull_request' || github.event.action != 'edited' || github.event.changes.base"
)
CI_METADATA_SUFFIX = (
    "${{ github.event_name == 'pull_request' && github.event.action == 'edited' && "
    "!github.event.changes.base && ' (metadata edit)' || '' }}"
)


def ci_event_condition(condition=None):
    return "${{ " + (f"({CI_ACTIVE_EVENT}) && ({condition})" if condition else CI_ACTIVE_EVENT) + " }}"


def render_edit_context(expression, action="synchronize", changes=None):
    # Actions treats objects (including {}) as truthy; missing properties are ''.
    base = changes.get("base", "") if isinstance(changes, dict) else ""
    return expression.replace("github.event.action", repr(action)).replace(
        "github.event.changes.base", repr(True if isinstance(base, dict) else base))


ASAN_MINIMUM_TESTS = 2320
ASAN_REQUIRED_TESTS = (
    "ConsoleFixture.CompleteInventoryDefaultsBindingsAndUnavailableRows",
    "UiFixture.DefaultPassiveHudRendersWithoutShellOrFocusAndCanBeDisabled",
    "DeveloperPersistence.DirectConstructionMatchesFactoryDefaultsAndInitializesRecords",
)
ASAN_MERGE_COMMAND = ('python3 tools/ci/ctest_shards.py merge --profile asan --directory build/asan-shards '
                      '--output build/debug-asan/ctest.junit.xml --needs-heavy "$CI_NEEDS_HEAVY" '
                      '--reason "$CI_HEAVY_REASON"')
UCRT_CLASSIFICATION = (
    "needs.changes.outputs.needs_cpp != 'false' || "
    "(github.event_name == 'pull_request' && github.base_ref == 'main')"
)
UCRT_ROUTE = (
    "(github.event_name == 'push' && (github.ref == 'refs/heads/devel' || "
    "github.ref == 'refs/heads/main')) || github.event_name == 'workflow_dispatch' || "
    "(github.event_name == 'pull_request' && "
    "(github.base_ref == 'main' || "
    "contains(github.event.pull_request.labels.*.name, 'windows-proof') || "
    "needs.changes.outputs.needs_ucrt64 == 'true'))"
)
SERVER_ROUTE = (
    "((github.event_name == 'pull_request' && github.base_ref == 'main') || "
    "(github.event_name == 'push' && "
    "(github.ref == 'refs/heads/main' || github.ref == 'refs/heads/devel') && "
    "github.event.before != '0000000000000000000000000000000000000000'))"
)


def bash_executable() -> str:
    # Prefer Git Bash over the Windows WSL launcher. CI's shell: bash uses
    # this same Git installation; native paths work with its MSYS tools.
    git = shutil.which("git")
    if git:
        bash = Path(git).resolve().parent.parent / "bin/bash.exe"
        if bash.is_file():
            return str(bash)
    return "bash"


class WorkflowShellTests(unittest.TestCase):
    def test_windows_candidates_prefer_git_over_wsl_for_all_git_layouts(self):
        root = r"C:\Program Files\Git"
        wsl = r"C:\Windows\System32\bash.exe"
        expected = [root + r"\bin\bash.exe", root + r"\usr\bin\bash.exe", wsl]
        for location in (r"cmd\git.exe", r"bin\git.exe", r"mingw64\bin\git.exe",
                         r"mingw32\bin\git.exe", r"usr\bin\git.exe"):
            with self.subTest(location=location):
                self.assertEqual(shell._windows_candidates(root + "\\" + location, wsl, ""), expected)
        self.assertEqual(shell._windows_candidates(None, wsl, ""), [wsl])
        self.assertEqual(shell._windows_candidates(r"relative\git.exe", None, ""), [])
        self.assertEqual(shell._windows_candidates(None, None, ""), [])

    def test_active_msys_keeps_its_path_shell_and_deduplicates_git_siblings(self):
        msys = r"D:\MSYS installation\usr\bin\bash.exe"
        git = r"D:\MSYS installation\usr\bin\git.exe"
        self.assertEqual(shell._windows_candidates(git, msys, "UCRT64"),
                         [msys, r"D:\MSYS installation\bin\bash.exe"])
        self.assertEqual(shell._windows_candidates(None, msys, "UCRT64"), [msys])

    def test_lookup_excludes_empty_and_relative_path_entries_and_cwd_results(self):
        entries = ["", ".", "relative/bin", str(ROOT / "directory with spaces")]
        with patch.object(shell.os, "get_exec_path", return_value=entries), patch.object(
                shell.shutil, "which", return_value=str(ROOT / "directory with spaces/bash")) as which:
            self.assertEqual(shell._absolute_which("bash"), str(ROOT / "directory with spaces/bash"))
            which.assert_called_once_with("bash", path=entries[-1])
            which.return_value = "bash.exe"
            self.assertIsNone(shell._absolute_which("bash"))
        with patch.object(shell.os, "get_exec_path", return_value=["", "."]), patch.object(
                shell.shutil, "which") as which:
            self.assertIsNone(shell._absolute_which("bash"))
            which.assert_not_called()

    def windows_resolution(self, msystem="", responses=None):
        git = r"C:\Program Files\Git\cmd\git.exe"
        wsl = r"C:\Windows\System32\bash.exe"

        def path(candidate):
            result = MagicMock()
            result.is_absolute.return_value = True
            result.is_file.return_value = True
            result.resolve.return_value = result
            result.__str__.return_value = candidate
            return result

        with patch.object(shell.sys, "platform", "win32"), patch.dict(
                os.environ, MSYSTEM=msystem), patch.object(
                shell, "_absolute_which", side_effect=lambda name: git if name == "git" else wsl), patch.object(
                shell, "Path", side_effect=path), patch.object(shell.os, "access", return_value=True), patch.object(
                shell.subprocess, "run", side_effect=responses) as run:
            return shell.bash_executable(), run.call_args_list

    def test_native_windows_resolution_uses_absolute_git_path_as_one_argument(self):
        success = subprocess.CompletedProcess([], 0, b"ci-bash-ready\n", b"")
        executable, calls = self.windows_resolution(responses=[success])
        self.assertEqual(executable, r"C:\Program Files\Git\bin\bash.exe")
        self.assertEqual(len(calls), 1)
        args, kwargs = calls[0]
        self.assertEqual(args[0][:-1], [executable, "--noprofile", "--norc", "-e", "-c"])
        self.assertIn('[[ "$OSTYPE" == msys* || "$OSTYPE" == cygwin ]] || { ci_bash_observe ostype; exit 12; };', args[0][-1])
        self.assertIn('[[ -n "$BASH_VERSION" ]] || { ci_bash_observe bash-version; exit 11; };', args[0][-1])
        self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
        self.assertTrue(kwargs["capture_output"])
        self.assertEqual(kwargs["timeout"], 2)
        self.assertNotIn("shell", kwargs)

    def test_windows_rejects_wsl_collision_and_tries_git_siblings(self):
        wsl = subprocess.CompletedProcess([], 1, "Windows Subsystem for Linux has no installed distributions".encode("utf-16"), b"")
        success = subprocess.CompletedProcess([], 0, b"ci-bash-ready\n", b"")
        executable, calls = self.windows_resolution("UCRT64", [wsl, OSError("unusable"), success])
        self.assertEqual(executable, r"C:\Program Files\Git\usr\bin\bash.exe")
        self.assertEqual([call.args[0][0] for call in calls], [
            r"C:\Windows\System32\bash.exe", r"C:\Program Files\Git\bin\bash.exe", executable])
        with self.assertRaisesRegex(RuntimeError, "usable native Bash"):
            self.windows_resolution(responses=[wsl, wsl, wsl])

    def test_posix_lookup_preserves_path_selection_and_does_not_query_git(self):
        with patch.object(shell.sys, "platform", "linux"), patch.object(
                shell, "_absolute_which", return_value=BASH) as which:
            self.assertEqual(shell.bash_executable(), BASH)
            which.assert_called_once_with("bash")
        with patch.object(shell.sys, "platform", "linux"), patch.object(
                shell, "_absolute_which", return_value=None), self.assertRaises(RuntimeError):
            shell.bash_executable()

    def test_overrides_require_absolute_regular_executable_files_without_fallback(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve(strict=True)
            for candidate in ("", "relative/bash", str(root / "missing"), str(root)):
                with self.subTest(candidate=candidate), patch.object(shell, "_absolute_which") as which, patch.object(
                        shell.subprocess, "run") as run, self.assertRaises(RuntimeError):
                    shell.bash_executable(candidate)
                which.assert_not_called()
                run.assert_not_called()
        for failure in (False, PermissionError("denied"), RuntimeError("link loop")):
            with self.subTest(failure=failure), patch.object(shell.os, "access", **(
                    {"return_value": failure} if failure is False else {"side_effect": failure})), patch.object(
                    shell.subprocess, "run") as run, self.assertRaises(RuntimeError):
                shell.bash_executable(BASH)
            run.assert_not_called()

    def test_probe_refuses_partial_output_wsl_errors_deadline_and_unusable_binaries(self):
        failures = [
            subprocess.CompletedProcess([], 1, b"ci-bash-ready\n", b""),
            subprocess.CompletedProcess([], 0, b"ci-bash", b""),
            subprocess.CompletedProcess([], 0, b"ci-bash-ready\n", b"warning"),
            subprocess.CompletedProcess([], 0, "Windows Subsystem for Linux has no installed distributions".encode("utf-16-le"), b""),
            subprocess.TimeoutExpired([BASH], 2), PermissionError("denied"), OSError("invalid executable"),
        ]
        for failure in failures:
            with self.subTest(failure=failure), patch.object(shell.subprocess, "run", **(
                    {"side_effect": failure} if isinstance(failure, Exception) else {"return_value": failure})), self.assertRaises(RuntimeError):
                shell.bash_executable(BASH)

    def test_probe_does_not_run_inherited_shell_startup_hooks(self):
        untrusted = dict(BASH_ENV="untrusted", ENV="untrusted", SHELLOPTS="untrusted", BASHOPTS="untrusted",
                         OSTYPE="msys", BASH_VERSION="untrusted")
        untrusted["BASH_FUNC_exit%%"] = "() { return 0; }"
        with patch.dict(os.environ, untrusted), patch.object(
                shell.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"ci-bash-ready\n", b"")) as run:
            self.assertEqual(shell.bash_executable(BASH), BASH)
        self.assertFalse(untrusted.keys() & run.call_args.kwargs["env"].keys())

    def test_resolved_bash_executes_literal_arguments_cwd_crlf_and_exit_status(self):
        with tempfile.TemporaryDirectory(prefix="shell contract ") as directory:
            root = Path(directory).resolve(strict=True)
            (root / "input with spaces.txt").write_bytes(b"first\r\nsecond\r\n")
            script = ('[[ "$1" == \'C:/directory with spaces/file\' && "$2" == \'a "quote"\' && -z "$3" ]]\n'
                      '[[ "$CONTRACT_VALUE" == kept ]]\n'
                      'IFS= read -r line < "input with spaces.txt"\n'
                      '[[ "$line" == $\'first\\r\' ]]\n'
                      'printf \'contract passed\\n\'\nexit 7')
            result = run_bash_script(script, executable=shell.bash_executable(),
                                    arguments=("C:/directory with spaces/file", 'a "quote"', ""),
                                    cwd=root, env=dict(os.environ, CONTRACT_VALUE="kept"),
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 7, result.stdout + result.stderr)
            self.assertEqual(result.stdout, "contract passed\n")
            self.assertEqual(result.stderr, "")

    def test_script_files_preserve_bytes_and_cleanup_after_process_failures(self):
        script = 'while read -r value; do\r\n  printf \'%s\\n\' \'a "quote" [path]\\tail\'\r\ndone\r\n'
        expected = script.replace("\r\n", "\n").encode("utf-8")
        for response in (subprocess.CompletedProcess([], 0, b"", b""),
                         subprocess.CompletedProcess([], 7, b"partial", b"\xfferror"),
                         PermissionError("denied"),
                         subprocess.TimeoutExpired([BASH], 10, output=b"partial", stderr=b"\xfferror")):
            paths = []
            def launch(command, **kwargs):
                self.assertEqual(command[:6], [BASH, "--noprofile", "--norc", "-e", "-o", "pipefail"])
                self.assertEqual(len(command), 7)
                self.assertNotIn("\\", command[-1])
                path = Path(command[-1])
                paths.append(path)
                self.assertEqual(path.read_bytes(), expected)
                # Windows refuses to rename a file that the parent left open.
                moved = path.with_suffix(".moved")
                path.rename(moved)
                moved.rename(path)
                self.assertEqual(kwargs, dict(capture_output=True, timeout=10))
                if isinstance(response, Exception):
                    raise response
                return response
            with self.subTest(response=response), patch.object(subprocess, "run", side_effect=launch):
                if isinstance(response, Exception):
                    with self.assertRaises(type(response)):
                        run_bash_script(script, capture_output=True, timeout=10)
                else:
                    self.assertIs(run_bash_script(script, capture_output=True, timeout=10), response)
            self.assertEqual(len(paths), 1)
            self.assertFalse(paths[0].parent.exists())


class WorkflowShellDiagnosticTests(unittest.TestCase):
    @staticmethod
    def candidate_path(candidate):
        path = MagicMock()
        path.is_absolute.return_value = True
        path.is_file.return_value = True
        path.resolve.return_value = path
        path.__str__.return_value = candidate
        return path

    def failure(self, override=BASH, platform="linux", discoveries=None, path=None,
                access=True, response=None):
        if response is None:
            response = subprocess.CompletedProcess([], 1, b"", b"")
        run_options = ({"side_effect": response} if isinstance(response, (Exception, list))
                       else {"return_value": response})
        access_options = {"side_effect": access} if isinstance(access, Exception) else {"return_value": access}
        with patch.object(shell.sys, "platform", platform), patch.object(
                shell, "_absolute_which", side_effect=lambda name: (discoveries or {}).get(name)) as which, patch.object(
                shell, "Path", side_effect=path or self.candidate_path) as construct, patch.object(
                shell.os, "access", **access_options) as executable, patch.object(
                shell.subprocess, "run", **run_options) as run, self.assertRaises(RuntimeError) as caught:
            shell.bash_executable(override)
        message = str(caught.exception)
        self.assertTrue(message.startswith("A usable native Bash is required"))
        self.assertLess(len(message), 24 * 1024)
        return message, which, construct, executable, run

    def test_failure_reports_actual_absolute_discovery_and_allowlisted_platform_context(self):
        environment = {
            "PATH": os.pathsep.join(("", ".", "relative/bin", str(ROOT / "absolute bin"))),
            "PATHEXT": ".EXE;.CMD", "MSYSTEM": "UCRT64", "MSYS2_ARG_CONV_EXCL": "*",
            "MSYS_NO_PATHCONV": "1", "MSYS2_ENV_CONV_EXCL": "PATH", "MSYS2_PATH_TYPE": "minimal",
            "ACCESS_TOKEN": "private-value", "BASH_ENV": "private-hook", "BASH_FUNC_exit%%": "private-function",
        }
        entries = ["", ".", "relative/bin", str(ROOT / "absolute bin")]
        for found in (None, "bash.exe"):
            with self.subTest(found=found), patch.dict(os.environ, environment, clear=True), patch.object(
                    shell.sys, "platform", "linux"), patch.object(shell.sys, "executable", "C:/runtime/python.exe"), patch.object(
                    shell.os, "get_exec_path", return_value=entries) as get_path, patch.object(
                    shell.shutil, "which", return_value=found) as which, patch.object(
                    shell.subprocess, "run") as run, self.assertRaises(RuntimeError) as caught:
                shell.bash_executable()
            message = str(caught.exception)
            for fragment in ("interpreter='C:/runtime/python.exe'", "python=", "sys.platform='linux'",
                             f"path_module={os.path.__name__!r}", f"pathsep={os.pathsep!r}", "os.name=",
                             f"discovery.bash.absolute_PATH={entries[-1]!r}", f"discovery.bash.which={found!r}",
                             "discovery.bash=None", "candidates=0", "absolute PATH entries only"):
                self.assertIn(fragment, message)
            for name, value in environment.items():
                if name not in ("ACCESS_TOKEN", "BASH_ENV", "BASH_FUNC_exit%%"):
                    self.assertIn(f"{name}={value!r}", message)
            for private in ("ACCESS_TOKEN", "private-value", "private-hook", "private-function", "BASH_ENV", "BASH_FUNC_"):
                self.assertNotIn(private, message)
            get_path.assert_called_once_with()
            which.assert_called_once_with("bash", path=entries[-1])
            run.assert_not_called()

    def test_nested_discovery_keeps_failure_observations_isolated(self):
        outer_path = str(ROOT / "outer bin")
        inner_path = str(ROOT / "inner bin")
        inner_messages = []

        def lookup(name, *, path):
            self.assertEqual(name, "bash")
            if path == outer_path:
                with self.assertRaises(RuntimeError) as inner:
                    shell.bash_executable()
                inner_messages.append(str(inner.exception))
            return None

        with patch.object(shell.sys, "platform", "linux"), patch.object(
                shell.os, "get_exec_path", side_effect=[[outer_path], [inner_path]]) as get_path, patch.object(
                shell.shutil, "which", side_effect=lookup) as which, patch.object(
                shell.subprocess, "run") as run, self.assertRaises(RuntimeError) as outer:
            shell.bash_executable()
        self.assertEqual(get_path.call_count, 2)
        self.assertEqual(which.call_count, 2)
        run.assert_not_called()
        self.assertEqual(len(inner_messages), 1)
        for message, included, excluded in ((str(outer.exception), outer_path, inner_path),
                                            (inner_messages[0], inner_path, outer_path)):
            self.assertIn(f"discovery.bash.absolute_PATH={included!r}", message)
            self.assertNotIn(f"discovery.bash.absolute_PATH={excluded!r}", message)

    def test_windows_failure_retains_discovery_order_drive_paths_and_probe_budget(self):
        git = r"C:\PROGRA~1\Git\cmd\git.exe"
        bash = r"C:\Windows\System32\bash.exe"
        for msystem, expected in (("", [r"C:\PROGRA~1\Git\bin\bash.exe", r"C:\PROGRA~1\Git\usr\bin\bash.exe", bash]),
                                  ("UCRT64", [bash, r"C:\PROGRA~1\Git\bin\bash.exe", r"C:\PROGRA~1\Git\usr\bin\bash.exe"])):
            with self.subTest(msystem=msystem), patch.dict(os.environ, MSYSTEM=msystem):
                message, which, construct, executable, run = self.failure(
                    override=None, platform="win32", discoveries={"bash": bash, "git": git})
            self.assertIn(f"discovery.git={git!r}", message)
            self.assertIn(f"discovery.bash={bash!r}", message)
            self.assertIn("candidates=3", message)
            self.assertEqual([call.args for call in which.call_args_list], [("bash",), ("git",)])
            self.assertEqual([call.args[0] for call in construct.call_args_list], expected)
            self.assertEqual(executable.call_count, 3)
            self.assertEqual(run.call_count, 3)
            for index, (candidate, call) in enumerate(zip(expected, run.call_args_list)):
                self.assertIn(f"candidate[{index}]={candidate!r}", message)
                self.assertIn(f"candidate[{index}].probe.returncode=1", message)
                self.assertEqual(call.args[0][0], candidate)
                self.assertEqual(call.kwargs["timeout"], 2)
                self.assertIn('[[ "$OSTYPE" == msys* || "$OSTYPE" == cygwin ]] || { ci_bash_observe ostype; exit 12; };', call.args[0][-1])

    def test_validation_failures_name_the_last_stage_without_extra_checks_or_fallback(self):
        for stage in ("is_absolute", "is_file", "access"):
            path = self.candidate_path(BASH)
            if stage != "access":
                getattr(path, stage).return_value = False
            with self.subTest(stage=stage):
                message, which, construct, executable, run = self.failure(
                    path=lambda candidate: path, access=stage != "access")
                field = "executable" if stage == "access" else stage
                self.assertIn(f"candidate[0].{field}=False", message)
                self.assertIn("selection=override", message)
                self.assertEqual(construct.call_count, 1)
                self.assertEqual(path.is_absolute.call_count, 1)
                self.assertEqual(path.is_file.call_count, int(stage != "is_absolute"))
                self.assertEqual(executable.call_count, int(stage == "access"))
                path.resolve.assert_not_called()
                which.assert_not_called()
                run.assert_not_called()

    def test_validation_exceptions_report_stage_and_codes_without_exception_payload(self):
        for stage in ("path", "is_absolute", "is_file", "access", "resolve"):
            for error in (PermissionError(13, "private-exception-payload"), RuntimeError("private-exception-payload")):
                path = self.candidate_path(BASH)
                if stage not in ("path", "access"):
                    getattr(path, stage).side_effect = error

                def construct(candidate):
                    if stage == "path":
                        raise error
                    return path

                with self.subTest(stage=stage, error=type(error).__name__):
                    message, which, _, _, run = self.failure(
                        path=construct, access=error if stage == "access" else True)
                    self.assertIn(f"candidate[0].{stage}.exception={type(error).__name__}", message)
                    self.assertNotIn("private-exception-payload", message)
                    if isinstance(error, OSError):
                        self.assertIn("candidate[0].errno=13", message)
                        self.assertIn("candidate[0].winerror=None", message)
                    which.assert_not_called()
                    run.assert_not_called()

    def test_probe_refusals_preserve_raw_bytes_returncode_and_resolved_path(self):
        failures = (
            (7, b"ci-bash-ready\n", b""), (0, b"", b""), (0, b"ci-bash", b""),
            (0, b"ci-bash-ready\r\n", b""), (0, b"ci-bash-ready\n", b"warning\r\n"),
            (1, "WSL unavailable".encode("utf-16-le"), b"\xff\x00\x1b"),
        )
        resolved = self.candidate_path("C:/long directory/bash.exe")
        for returncode, stdout, stderr in failures:
            path = self.candidate_path(BASH)
            path.resolve.return_value = resolved
            with self.subTest(returncode=returncode, stdout=stdout, stderr=stderr):
                message, _, _, _, run = self.failure(path=lambda candidate: path,
                    response=subprocess.CompletedProcess([], returncode, stdout, stderr))
                self.assertIn(f"candidate[0].probe.returncode={returncode}", message)
                self.assertIn(f"candidate[0].probe.stdout={stdout!r}", message)
                self.assertIn(f"candidate[0].probe.stderr={stderr!r}", message)
                self.assertIn("candidate[0].resolved='C:/long directory/bash.exe'", message)
                path.resolve.assert_called_once_with(strict=True)
                run.assert_called_once()
                self.assertEqual(run.call_args.args[0][0], str(resolved))

    def test_launch_and_deadline_failures_report_codes_and_available_partial_bytes(self):
        denied = OSError(5, "private-launch-payload")
        denied.winerror = 193
        for error in (denied, subprocess.TimeoutExpired(["private-command"], 2),
                      subprocess.TimeoutExpired(["private-command"], 2, output=b"partial\r\n", stderr=b"\xffwarning")):
            with self.subTest(error=type(error).__name__, output=getattr(error, "output", None)):
                message, _, _, _, run = self.failure(response=error)
                self.assertIn(f"candidate[0].probe.exception={type(error).__name__}", message)
                self.assertNotIn("private-launch-payload", message)
                self.assertNotIn("private-command", message)
                if isinstance(error, subprocess.TimeoutExpired):
                    self.assertIn(f"candidate[0].probe.stdout={error.output!r}", message)
                    self.assertIn(f"candidate[0].probe.stderr={error.stderr!r}", message)
                else:
                    self.assertIn("candidate[0].errno=5", message)
                    self.assertIn("candidate[0].winerror=193", message)
                run.assert_called_once()
                self.assertEqual(run.call_args.kwargs["timeout"], 2)

    def test_failure_bounds_apply_before_formatting_all_context_candidates_and_output(self):
        huge = "\U0001f600\n\x1b" * 100000
        huge_bytes = b"\x00\xff\r\n" * 100000
        absolute = "//server/share/" + huge
        environment = dict.fromkeys(("PATH", "PATHEXT", "MSYSTEM", "MSYS2_ARG_CONV_EXCL", "MSYS_NO_PATHCONV",
                                     "MSYS2_ENV_CONV_EXCL", "MSYS2_PATH_TYPE"), huge)
        with patch.object(shell.os, "environ", environment), patch.object(shell.sys, "executable", huge), patch.object(
                shell.sys, "version", huge), patch.object(shell.sys, "platform", "win32"), patch.object(
                shell.os, "get_exec_path", return_value=[absolute]) as get_path, patch.object(
                shell.shutil, "which", side_effect=lambda name, **kwargs: absolute + (
                    "/cmd/git.exe" if name == "git" else "/bash.exe")) as which, patch.object(
                shell, "Path", side_effect=self.candidate_path) as construct, patch.object(
                shell.os, "access", return_value=True), patch.object(shell.subprocess, "run",
                    return_value=subprocess.CompletedProcess([], 1, huge_bytes, huge_bytes)) as run, self.assertRaises(RuntimeError) as caught:
            shell.bash_executable()
        message = str(caught.exception)
        self.assertLess(len(message), 24 * 1024)
        self.assertEqual(get_path.call_count, 2)
        self.assertEqual(which.call_count, 2)
        for name in ("bash", "git"):
            self.assertIn(f"discovery.{name}.absolute_PATH=", message)
            self.assertIn(f"discovery.{name}.which=", message)
        self.assertIn("candidates=3", message)
        self.assertEqual(construct.call_count, 3)
        self.assertEqual(run.call_count, 3)
        self.assertIn(f"length={len(huge_bytes)}, truncated=True", message)
        self.assertIn(f"length={len(huge)}, truncated=True", message)
        self.assertIn("candidate[2].probe.stderr=", message)
        self.assertTrue(message.isascii())
        self.assertNotIn("\x1b", message)
        self.assertNotIn("\r", message)
        self.assertLess(len(message.splitlines()), 50)

    def test_success_stays_silent_after_failed_probe_and_keeps_exact_success_contract(self):
        failure = subprocess.CompletedProcess([], 0, b"ci-bash-ready\r\n", b"")
        success = subprocess.CompletedProcess([], 0, b"ci-bash-ready\n", b"")
        with patch.object(shell.sys, "stdout") as stdout, patch.object(shell.sys, "stderr") as stderr:
            executable, calls = WorkflowShellTests().windows_resolution("UCRT64", [failure, success])
        self.assertEqual(executable, r"C:\Program Files\Git\bin\bash.exe")
        self.assertEqual(len(calls), 2)
        stdout.write.assert_not_called()
        stderr.write.assert_not_called()

    def run_probe_body(self, *, platform="win32", prefix="", replace=None):
        # Capture the actual production command, then execute it outside mocks.
        # Prefixes/rewrites force guard inputs; they do not emulate MSYS decoding.
        _, _, _, _, run = self.failure(platform=platform)
        command = list(run.call_args.args[0])
        script = prefix + command[-1]
        if replace is not None:
            before, after = replace
            self.assertIn(before, script)
            script = script.replace(before, after)
        command[-1] = script
        result = subprocess.run(command, **run.call_args.kwargs)
        return result, script

    def assert_refusal_observations(self, result, script, status, stage):
        self.assertEqual(result.returncode, status, result.stderr)
        self.assertEqual(result.stdout, b"BASH_EXECUTION_STRING=" + script[:512].encode("utf-8"))
        self.assertTrue(result.stderr.startswith(f"stage={stage} BASH_VERSION=".encode("ascii")))
        self.assertIn(b" OSTYPE=", result.stderr)
        message, _, _, _, run = self.failure(response=result)
        self.assertIn(f"candidate[0].probe.returncode={status}", message)
        self.assertIn(shell._observation("candidate[0].probe.stdout", result.stdout), message)
        self.assertIn(shell._observation("candidate[0].probe.stderr", result.stderr), message)
        run.assert_called_once()

    def test_actual_probe_distinguishes_empty_bash_version_on_both_platform_branches(self):
        for platform in ("linux", "win32"):
            with self.subTest(platform=platform):
                result, script = self.run_probe_body(platform=platform, prefix="BASH_VERSION=; ")
                self.assert_refusal_observations(result, script, 11, "bash-version")
                self.assertIn(b"BASH_VERSION='' OSTYPE=", result.stderr)

    def test_actual_windows_guard_reports_empty_and_non_msys_intrinsics(self):
        for prefix, ostype in (("OSTYPE=; ", b"''"), ("OSTYPE=linux-gnu; ", b"linux-gnu")):
            with self.subTest(prefix=prefix):
                result, script = self.run_probe_body(prefix=prefix)
                self.assert_refusal_observations(result, script, 12, "ostype")
                self.assertRegex(result.stderr, rb"BASH_VERSION=[^\n ]+ OSTYPE=" + re.escape(ostype) + rb"\n$")
                self.assertIn(ascii(result.stdout), shell._observation("stdout", result.stdout))
                self.assertIn("truncated=False", shell._observation("stdout", result.stdout))

    def test_actual_refusal_observes_changed_decoded_quotes_without_evaluating_command(self):
        result, script = self.run_probe_body(prefix="OSTYPE=msys; ",
                                            replace=('"$OSTYPE"', r'\"$OSTYPE\"'))
        self.assert_refusal_observations(result, script, 12, "ostype")
        self.assertIn(rb'\"$OSTYPE\"', result.stdout)
        self.assertTrue(result.stderr.endswith(b" OSTYPE=msys\n"))

    def test_actual_probe_success_retains_only_exact_magic_on_both_platform_branches(self):
        for platform, prefix in (("linux", ""), ("win32", "OSTYPE=msys; ")):
            with self.subTest(platform=platform):
                result, _ = self.run_probe_body(platform=platform, prefix=prefix)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"ci-bash-ready\n", b""))
                with patch.object(shell.subprocess, "run", return_value=result) as run, patch.object(
                        shell.sys, "stdout") as stdout, patch.object(shell.sys, "stderr") as stderr:
                    self.assertEqual(shell.bash_executable(BASH), BASH)
                run.assert_called_once()
                stdout.write.assert_not_called()
                stderr.write.assert_not_called()

    def test_actual_windows_probe_accepts_legacy_msys_and_documented_cygwin(self):
        for msystem in ("", "UCRT64", "untrusted"):
            for ostype, version in (("msys", "4.4.23(1)-release"),
                                    ("msys2", "5.3.20(2)-release"),
                                    ("cygwin", "5.3.20(2)-release")):
                with self.subTest(msystem=msystem, ostype=ostype), patch.dict(os.environ, MSYSTEM=msystem):
                    result, _ = self.run_probe_body(prefix=f"BASH_VERSION='{version}'; OSTYPE={ostype}; ")
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"ci-bash-ready\n", b""))
                # Feed the executed probe's raw result through the Windows
                # override path, including spaces and an 8.3 installation alias.
                for candidate in (r"D:\MSYS installation\usr\bin\bash.exe",
                                  r"C:\PROGRA~1\Git\usr\bin\bash.exe"):
                    with self.subTest(candidate=candidate), patch.object(shell.sys, "platform", "win32"), patch.object(
                            shell, "Path", side_effect=self.candidate_path), patch.object(
                            shell.os, "access", return_value=True), patch.object(shell, "_absolute_which") as which, patch.object(
                            shell.subprocess, "run", return_value=result) as run, patch.object(
                            shell.sys, "stdout") as stdout, patch.object(shell.sys, "stderr") as stderr:
                        self.assertEqual(shell.bash_executable(candidate), candidate)
                    which.assert_not_called()
                    run.assert_called_once()
                    self.assertEqual(run.call_args.args[0][0], candidate)
                    self.assertEqual(run.call_args.kwargs["timeout"], 2)
                    stdout.write.assert_not_called()
                    stderr.write.assert_not_called()

    def test_actual_windows_probe_refuses_other_identities_despite_msystem(self):
        for msystem in ("", "UCRT64", "MINGW64"):
            for ostype in ("", "linux-gnu", "linux-musl", "unknown", "cygwin-extra", "CYGWIN", "darwin"):
                # WSL and caller-supplied Windows identities cannot override the
                # actual shell intrinsics. Hooks must also remain filtered.
                untrusted = dict(MSYSTEM=msystem, WSL_DISTRO_NAME="Ubuntu", WSL_INTEROP="untrusted",
                                 OSTYPE="cygwin", BASH_VERSION="5.3.20(2)-release",
                                 BASH_ENV="untrusted", ENV="untrusted", SHELLOPTS="untrusted", BASHOPTS="untrusted")
                untrusted["BASH_FUNC_exit%%"] = "() { return 0; }"
                with self.subTest(msystem=msystem, ostype=ostype), patch.dict(os.environ, untrusted):
                    result, script = self.run_probe_body(prefix=f"BASH_VERSION='5.3.20(2)-release'; OSTYPE='{ostype}'; ")
                self.assert_refusal_observations(result, script, 12, "ostype")
                self.assertIn(b"BASH_VERSION=5.3.20\\(2\\)-release OSTYPE=" + (ostype.encode("ascii") or b"''"),
                              result.stderr)

    def test_actual_documented_cygwin_identity_still_refuses_empty_bash_version(self):
        with patch.dict(os.environ, MSYSTEM="UCRT64"):
            result, script = self.run_probe_body(prefix="BASH_VERSION=; OSTYPE=cygwin; ")
        self.assert_refusal_observations(result, script, 11, "bash-version")
        self.assertEqual(result.stderr, b"stage=bash-version BASH_VERSION='' OSTYPE=cygwin\n")

    def test_actual_abort_without_guard_observations_leaves_body_stage_unknown(self):
        result, _ = self.run_probe_body(prefix="exit 1; ")
        self.assertEqual((result.returncode, result.stdout, result.stderr), (1, b"", b""))
        message, _, _, _, run = self.failure(response=result)
        self.assertIn("candidate[0].probe.returncode=1", message)
        self.assertIn("candidate[0].probe.stdout=b''", message)
        self.assertIn("candidate[0].probe.stderr=b''", message)
        self.assertNotIn("stage=", message)
        run.assert_called_once()

    def test_actual_guard_observation_write_failures_never_reach_success_magic(self):
        for descriptor in (1, 2):
            with self.subTest(descriptor=descriptor):
                result, _ = self.run_probe_body(prefix=f"exec {descriptor}>&-; OSTYPE=refused; ")
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(b"ci-bash-ready\n", result.stdout)
                _, _, _, _, run = self.failure(response=result)
                run.assert_called_once()

    def test_actual_guard_observations_filter_hooks_and_do_not_dump_environment(self):
        private = "private-observation-payload"
        untrusted = dict(ACCESS_TOKEN=private, BASH_EXECUTION_STRING=private, BASH_ENV=private,
                         ENV=private, BASH_VERSION=private, OSTYPE="msys", SHELLOPTS=private, BASHOPTS=private)
        untrusted["BASH_FUNC_ci_bash_observe%%"] = "() { printf " + private + "; }"
        with patch.dict(os.environ, untrusted):
            result, script = self.run_probe_body(prefix="OSTYPE=refused; ")
        self.assert_refusal_observations(result, script, 12, "ostype")
        for forbidden in (private.encode("ascii"), b"ACCESS_TOKEN", b"BASH_ENV", b"BASH_FUNC_"):
            self.assertNotIn(forbidden, result.stdout + result.stderr)

    def test_actual_guard_observations_bound_intrinsics_and_decoded_command_and_escape_controls(self):
        prefix = "BASH_VERSION=" + "v" * 1000 + "; OSTYPE=" + "o" * 1000 + "; "
        result, script = self.run_probe_body(prefix=prefix)
        self.assert_refusal_observations(result, script, 12, "ostype")
        self.assertEqual(result.stderr, b"stage=ostype BASH_VERSION=" + b"v" * 32 + b" OSTYPE=" + b"o" * 32 + b"\n")
        self.assertEqual(len(result.stdout), len(b"BASH_EXECUTION_STRING=") + 512)
        result, script = self.run_probe_body(prefix=r"BASH_VERSION=$'ver\n\033'; OSTYPE=$'other\r\033'; ")
        self.assert_refusal_observations(result, script, 12, "ostype")
        self.assertIn(rb"ver\n\E", result.stderr)
        self.assertIn(rb"other\r\E", result.stderr)
        self.assertNotIn(b"\x1b", result.stderr)
        self.assertEqual(result.stderr.count(b"\n"), 1)
        observation = shell._observation("stderr", result.stderr)
        self.assertTrue(observation.isascii())
        self.assertNotIn("\x1b", observation)
        self.assertNotIn("\r", observation)

    def test_byte_observation_keeps_display_bound_before_formatting_and_retains_command_tail(self):
        class BoundedBytes(bytes):
            def __repr__(self):
                raise AssertionError("unsliced output was formatted")

            def __getitem__(self, key):
                self_test.assertIsInstance(key, slice)
                self_test.assertLessEqual(key.stop, 640)
                return super().__getitem__(key)

        self_test = self
        for payload in (b"x" * 500 + b"command-tail", b"\xff\x00\r\n" * 100000):
            with self.subTest(length=len(payload)):
                observation = shell._observation("stdout", BoundedBytes(payload))
                self.assertTrue(observation.isascii())
                preview = observation.split("=", 1)[1].rsplit(" (length=", 1)[0]
                self.assertLessEqual(len(preview), 640)
                if len(payload) < 640:
                    self.assertIn("command-tail", observation)
                    self.assertIn("truncated=False", observation)
                else:
                    self.assertIn("truncated=True", observation)
        partial = b"x" * 200 + b"decoded-command-tail"
        message, _, _, _, run = self.failure(response=subprocess.TimeoutExpired(
            ["private-command"], 2, output=partial, stderr=b"stage=ostype"))
        self.assertIn("decoded-command-tail", message)
        self.assertNotIn("private-command", message)
        run.assert_called_once()

    def test_refusal_observations_cannot_make_unknown_output_or_guard_status_successful(self):
        for status in (0, 11, 12):
            failures = ((b"BASH_EXECUTION_STRING=command", b"stage=ostype"),
                        (b"ci-bash-ready\n", b"stage=ostype"),
                        (b"ci-bash-ready\nextra", b""))
            if status:
                failures += ((b"ci-bash-ready\n", b""),)
            for stdout, stderr in failures:
                with self.subTest(status=status, stdout=stdout, stderr=stderr):
                    _, _, _, _, run = self.failure(response=subprocess.CompletedProcess([], status, stdout, stderr))
                    run.assert_called_once()


def jobs(workflow):
    body = workflow.split("\njobs:\n", 1)[1]
    headers = list(re.finditer(r"^  ([\w-]+):\n", body, re.MULTILINE))
    return {
        header[1]: body[header.end():headers[i + 1].start() if i + 1 < len(headers) else len(body)]
        for i, header in enumerate(headers)
    }


def steps(job):
    return re.split(r"(?=^      - name: )", job, flags=re.MULTILINE)[1:]


def value(block, key, indent):
    match = re.search(rf"^{' ' * indent}{re.escape(key)}: *(.*)$", block, re.MULTILINE)
    if match is None:
        return None
    result = match[1]
    if result not in ("|", ">-"):
        return result
    lines = []
    for line in block[match.end():].splitlines()[1:]:
        if line.strip() and len(line) - len(line.lstrip()) <= indent:
            break
        lines.append(line)
    body = textwrap.dedent("\n".join(lines)).strip()
    return " ".join(body.split()) if result == ">-" else body


def aggregate_invalid_expression(producer):
    expression = ("needs.changes.result != 'success' || "
                  f"(needs.{producer}.result != 'success' && needs.{producer}.result != 'skipped')")
    if producer == "asan-shard":
        expression += " || (needs.fast-lane.result != 'success' && needs.fast-lane.result != 'skipped')"
    return expression


def evaluate_condition(expression, dependencies, *, cancelled=False, failed=None):
    """Evaluate this workflow's status/result conditions in their Actions context.

    At job admission/re-evaluation cancelled is workflow cancellation. At step
    evaluation it is the current job's cancellation, which can still be false
    when a job condition kept it running during workflow cancellation.
    """
    expression = render_edit_context(expression or "success()").removeprefix("${{ ").removesuffix(" }}")
    if failed is None:
        failed = "failure" in dependencies.values()
    status = {"always": True, "cancelled": cancelled, "failure": failed,
              "success": not cancelled and not failed and
              all(result == "success" for result in dependencies.values())}
    expression, count = re.subn(r"\b(always|cancelled|success|failure)\(\)",
                               lambda match: repr(status[match[1]]), expression)
    expression = re.sub(r"needs\.([\w-]+)\.result",
                        lambda match: repr(dependencies[match[1]]), expression)
    expression = expression.replace("github.event_name", repr("pull_request"))
    expression = re.sub(r"!(?!=)", "not ", expression)
    expression = expression.replace("== false", "== False").replace("&&", " and ").replace("||", " or ")
    return bool((count or status["success"]) and
                eval(expression, {"__builtins__": {}, "contains": lambda items, item:
                                  item.lower() in [entry.lower() for entry in items]}, {}))


def guarded_jobs(workflows):
    for filename, (job_id, check_name) in GUARDED.items():
        yield filename, job_id, check_name, jobs(workflows[filename])[job_id]
    yield ("asan.yml", "asan-shard", "Linux / GCC ASan shard ${{ matrix.shard }}/3",
           jobs(workflows["asan.yml"])["asan-shard"])
    ci = jobs(workflows["ci.yml"])
    for job_id, name in (("linux", value(ci["linux"], "name", 4)),
                         ("windows-msvc-result", "Windows / MSVC"),
                         ("windows-ucrt64-result", "Windows / MSYS2 UCRT64")):
        yield "ci.yml", job_id, name, ci[job_id]


def status_function_violations(workflow):
    """Find status expressions outside if values in our indented YAML layout."""
    if_values = []
    scalar_key, scalar_indent, scalar_start = None, -1, 0
    offset = 0
    for line in workflow.splitlines(keepends=True):
        indent = len(line) - len(line.lstrip(" "))
        if line.strip() and not line.lstrip().startswith("#"):
            if scalar_key is not None and indent <= scalar_indent:
                if scalar_key == "if":
                    if_values.append((scalar_start, offset))
                scalar_key = None
            if scalar_key is None:
                header = re.match(r" *(?:- +)?(?P<key>[\w-]+|'[^']+'|\"[^\"]+\"): *(?P<value>.*)", line)
                if header and (header["key"].strip("'\"") == "if" or
                               (header["value"].strip() and not header["value"].startswith("#"))):
                    scalar_key = header["key"].strip("'\"")
                    scalar_indent = header.start("key")
                    scalar_start = offset + header.start("value")
        offset += len(line)
    if scalar_key == "if":
        if_values.append((scalar_start, offset))

    violations = []
    for expression in re.finditer(r"\$\{\{(?:'(?:[^']|'')*'|(?!\}\}).)*\}\}", workflow, re.DOTALL):
        if re.search(r"\b(?:success|failure|cancelled|always)\s*\(", expression[0], re.IGNORECASE):
            if not any(start <= expression.start() and expression.end() <= end
                       for start, end in if_values):
                violations.append(workflow.count("\n", 0, expression.start()) + 1)
    return violations


class WorkflowExpressionPolicyTests(unittest.TestCase):
    def test_status_functions_are_only_used_in_if_values(self):
        paths = sorted(path for path in (ROOT / ".github/workflows").rglob("*")
                       if path.suffix in (".yml", ".yaml"))
        self.assertTrue(paths)
        for path in paths:
            with self.subTest(workflow=path.name):
                self.assertEqual(status_function_violations(path.read_text(encoding="utf-8")), [],
                                 "Status functions are only valid in if values")

    def test_status_expression_contexts(self):
        for function in ("success", "failure", "cancelled", "always"):
            expression = "${{ " + function + "() }}"
            for key in ("name", "env", "with", "run", "outputs"):
                for scalar in (expression, "|\n  " + expression,
                               ">-\n  ${{\n    " + function + "()\n  }}"):
                    with self.subTest(function=function, key=key, scalar=scalar):
                        self.assertTrue(status_function_violations(key + ": " + scalar + "\n"))
            for condition in ("if: " + expression, "- if: " + expression,
                              "'if': " + expression, '"if": ' + expression,
                              "if:\n  " + expression,
                              "if: >-\n  " + expression,
                              "if: ${{\n  " + function + "()\n  }}"):
                with self.subTest(condition=condition):
                    self.assertEqual(status_function_violations(condition + "\n"), [])
        # Script text that looks like an if key must not grant an exemption.
        self.assertEqual(status_function_violations("run: |\n  if: ${{ always() }}\n"), [2])
        self.assertEqual(status_function_violations(
            "if: ${{ always() }}\nenv:\n  BAD: ${{ cancelled() }}\n"), [3])
        self.assertEqual(status_function_violations("name: ${{ ALWAYS () }}\n"), [1])
        self.assertEqual(status_function_violations("name: ${{ '}}' || cancelled() }}\n"), [1])
        self.assertEqual(status_function_violations("if: ${{ '}}' || cancelled() }}\n"), [])
        self.assertEqual(status_function_violations("name: ${{ github.workflow }}\n"), [])
        self.assertEqual(status_function_violations(""), [])


class WorkflowPolicyFixture:
    @classmethod
    def setUpClass(cls):
        cls.workflows = {
            filename: (ROOT / ".github/workflows" / filename).read_text(encoding="utf-8")
            for filename in GUARDED
        }


class WorkflowDraftPolicyTests(WorkflowPolicyFixture, unittest.TestCase):
    def test_profile_source_checker_requires_complete_ordered_configuration(self):
        spec = importlib.util.spec_from_file_location(
            "profile_contract", ROOT / "test/tools/check_profiler_contract.py")
        contract = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(contract)
        contract.check_enabled_compile()
        original_read = contract.read
        workflow = self.workflows["ci.yml"]
        init = "          python3 -I -B tools/ci/rmlui_source.py init --root build/gns-probe\n"
        binding = "\"-DPython3_EXECUTABLE=$(python3 -I -B -c 'import sys; print(sys.executable)')\""
        flags = "-DBUILD_TESTING=ON -DLUMINUMBRA_ENABLE_GNS=ON"
        mutations = [
            workflow.replace(init, ""),
            workflow.replace(init, "").replace(flags + "\n", flags + "\n" + init),
            workflow.replace(init, init.replace("gns-probe", "wrong-root")),
            workflow.replace(binding, ""),
            workflow.replace(binding, binding.replace("python3 -I -B -c", "python3 -c")),
            workflow.replace("-DBUILD_TESTING=ON", "-DBUILD_TESTING=OFF"),
            workflow.replace("-DBUILD_TESTING=ON", ""),
            workflow.replace("-DLUMINUMBRA_ENABLE_GNS=ON", ""),
        ]
        for index, mutated in enumerate(mutations):
            with self.subTest(mutation=index):
                self.assertNotEqual(mutated, workflow)
                with patch.object(contract, "read", side_effect=lambda path:
                                  mutated if path == ".github/workflows/ci.yml" else original_read(path)):
                    with self.assertRaisesRegex(AssertionError, "CI must configure the enabled profile preset"):
                        contract.check_enabled_compile()

    def test_performance_apt_install_steps_have_bounded_updates_and_retries(self):
        install_jobs = []
        for job_id, job in jobs(self.workflows["performance.yml"]).items():
            for step in steps(job):
                script = value(step, "run", 8) or ""
                if "apt-get install" not in script:
                    continue
                install_jobs.append(job_id)
                with self.subTest(job=job_id):
                    self.assertEqual(value(step, "timeout-minutes", 8), "25")
                    self.assertIsNone(value(step, "continue-on-error", 8))
                    self.assertEqual(script.count("apt-get"), 2)
                    self.assertEqual(script.splitlines()[:2], [
                        "sudo timeout --kill-after=30s 5m apt-get -o Acquire::Retries=3 "
                        "-o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 update",
                        "sudo apt-get install -y --no-install-recommends \\",
                    ])
        self.assertCountEqual(install_jobs, [
            "preview-frame", "server-smoke-comparison", "qualification-capture",
        ])

    def test_fast_lane_runs_drafts_on_a_standard_runner_with_its_own_bound(self):
        fast = jobs(self.workflows["ci.yml"])["fast-compile"]
        self.assertEqual(value(fast, "name", 4), "Fast compile / GCC + clang-tidy" + CI_METADATA_SUFFIX)
        self.assertEqual(value(fast, "runs-on", 4), "ubuntu-24.04")
        self.assertEqual(value(fast, "timeout-minutes", 4), "20")
        self.assertEqual(value(fast, "if", 4), ci_event_condition())
        self.assertIsNone(value(fast, "needs", 4))
        self.assertNotIn("draft", fast)
        self.assertNotIn("continue-on-error", fast)
        self.assertIn("fetch-depth: 0", fast)

    def test_fast_lane_explicit_route_dual_configure_glad_and_bounded_execution(self):
        fast = jobs(self.workflows["ci.yml"])["fast-compile"]
        route = steps(fast)[1]
        self.assertIn("changed_tus.py route", route)
        self.assertIn("github.event.pull_request.base.sha || github.event.before", route)
        self.assertIn('--output "$GITHUB_OUTPUT" --summary-file "$GITHUB_STEP_SUMMARY"', route)
        self.assertIn("CI_FULL_SELECTION: ${{ steps.route.outputs.full }}", fast)
        self.assertIn('if [ "$CI_FULL_SELECTION" != false ]', fast)
        for step in steps(fast)[3:6]:
            self.assertEqual(value(step, "if", 8), "steps.route.outputs.native != 'false'")
        evidence = steps(fast)[6]
        self.assertEqual(value(evidence, "if", 8), "always() && steps.route.outputs.native != 'false'")
        self.assertIn("uses: actions/upload-artifact@", evidence)
        self.assertIn("path: build/fast-checks-evidence.json", evidence)
        self.assertIn("if-no-files-found: error", evidence)
        self.assertIn("--evidence-file build/fast-checks-evidence.json", steps(fast)[5])
        for compiler in ("gcc", "clang"):
            self.assertIn(f"cmake -S . -B build/fast-{compiler}", fast)
            self.assertIn(f"cmake --build build/fast-{compiler} --target glad --parallel 2", fast)
        self.assertEqual(fast.count("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"), 2)
        for flag in ("-DLUMINUMBRA_WARNINGS_AS_ERRORS=ON", "-DBUILD_TESTING=ON",
                     "-DCMAKE_BUILD_TYPE=Release", "-DLUMINUMBRA_ENABLE_DILIGENT=OFF"):
            self.assertEqual(fast.count(flag), 2)
        for flag in ("-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++",
                     "-DCMAKE_C_COMPILER=clang-18", "-DCMAKE_CXX_COMPILER=clang++-18"):
            self.assertIn(flag, fast)
        self.assertIn("--build-dir build/fast-gcc --clang-build-dir build/fast-clang", fast)
        self.assertIn('--jobs 2 --summary-file "$GITHUB_STEP_SUMMARY"', fast)
        self.assertNotIn("ctest", fast.lower())
        self.assertNotIn("--preset", fast)
        self.assertNotIn("-Wno", fast)

    def test_fast_lane_has_only_bounded_local_cache_and_no_required_consumers(self):
        blocks = jobs(self.workflows["ci.yml"])
        fast = blocks["fast-compile"]
        self.assertIn("CCACHE_DIR: ${{ github.workspace }}/scratch/fast-ccache", fast)
        self.assertIn("CCACHE_MAXSIZE: 250M", fast)
        self.assertIn("CCACHE_COMPILERCHECK: content", fast)
        self.assertIn('CCACHE_REMOTE_STORAGE: ""', fast)
        self.assertIn("ccache --max-size 250M", fast)
        self.assertNotIn("actions/cache", fast)
        self.assertNotIn("save", fast.lower())
        self.assertIn("Advisory during warning and duration calibration; never a required gate.", fast)
        for name, job in blocks.items():
            if name != "fast-compile":
                self.assertNotIn("fast-compile", value(job, "needs", 4) or "")
        for path in sorted((ROOT / ".github").rglob("*")):
            if path.is_file() and any(word in path.name for word in ("ruleset", "required")):
                self.assertNotIn("Fast compile / GCC + clang-tidy", path.read_text())
        inventory = blocks["public-tree"]
        contracts = steps(fast)[2]
        self.assertIn("python3 -I -B tools/ci/test_changed_tus.py", contracts)
        self.assertIsNone(value(contracts, "if", 8))
        for suite in ("test_tidy_shard.py", "test_workflow_policy.py"):
            self.assertIn("tools/ci/" + suite, inventory)

    def test_msvc_configure_keeps_dependency_compiles_cacheable(self):
        # ccache cannot cache precompiled-header or /Zi compiles; the lane already
        # embeds symbols through its own global flags.
        ci = jobs(self.workflows["ci.yml"])
        msvc, = [step for step in steps(ci["windows-msvc"]) if step.startswith("      - name: Configure, build, and test\n")]
        run = value(msvc, "run", 8)
        self.assertIn("-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DGENERATE_DEBUG_SYMBOLS=OFF `\n", run)
        self.assertIn("-DCMAKE_C_FLAGS=/Z7 '-DCMAKE_CXX_FLAGS=/Z7 /EHsc' `\n", run)
        self.assertIn("'-DCMAKE_EXE_LINKER_FLAGS=/DEBUG /OPT:REF /OPT:ICF'", run)
        for name in ("linux", "tidy-shard", "fast-compile"):
            self.assertNotIn("DISABLE_PRECOMPILE_HEADERS", ci[name], name)

    def test_every_preexisting_ci_job_definition_is_unchanged(self):
        # Pin base-edit admission and check names along with existing limits and gates.
        expected = {
            "changes": "4ad09fa893c78aef7e7aeb3c44fd0f95f0628d176f81db59190168a059f67070",
            # Require all three offline-dependency partitions in local-check evidence.
            "linux": "306cf8a35ba0bf836205559a40ea0d9d1da3bdd08b28f2b0291787b698223494",
            "gns-compile-probe": "7add7cbd1528a989a68922e779bf47e1bd867d8812e911ea6c0261dcf6762057",
            "diligent-rhi": "dcd14e3c82e44abaa18c301cb39cad12a342fd23f13dbe2fef729ce1c46d73a8",
            # Pin the selected Python, partitioned local checks, render failure symbols,
            # shard-1-only Python contracts and verbose compiler cache statistics.
            "windows-msvc": "417e0b31c5bede0076e48e3b6e9da528e30f529c3fd92017be9ba3f7519bcee9",
            # Pin UCRT64 opt-in routing, native evidence and minimal debug information.
            "windows-ucrt64": "e062bc566e6220e653ee96f6d85cd1494206802d3971d58f95f62a20b31a59c4",
            "linux-result": "71c786e40840265f992d2bb874c7edfee620914601469eb4f5a723b15e76f28b",
            "windows-msvc-result": "8ed308a876a2cabec06a4083fcf87981cdb572e2cf32cbab65aefd327d4e5073",
            "windows-ucrt64-result": "34a33c1e0bcb8212b01f3803ccf6798f6c37c8d1d66c575df6b9148e1cb2dc80",
            "lint": "b32b7f25c8c59c2cdc664382725f86838f8aef9328f8cee52fd0741ba41b86e0",
            "tidy-shard": "53d7432d5fec3526d1f32296223f8a28541b584c3b3d5039ac8a2f9476a4a104",
            "tidy": "771a907ee610dfff3fb01bf35c7a34fe5fdf8b5300ab92dbcbbe813ea6ebc949",
            "public-tree": "fac1e5b341a1fd73c12fffb8c215e3560f904a9302af168f82e70c5b35ab7b36",
        }
        actual = jobs(self.workflows["ci.yml"])
        self.assertEqual(set(actual), set(expected) | {"fast-compile"})
        for name, digest in expected.items():
            self.assertEqual(hashlib.sha256(actual[name].encode()).hexdigest(), digest, name)

    def test_bash_uses_git_installation_and_falls_back_when_unavailable(self):
        with patch.object(shutil, "which", return_value=None):
            self.assertEqual(bash_executable(), "bash")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve(strict=True)
            with patch.object(shutil, "which", return_value=str(root / "cmd/git.exe")):
                self.assertEqual(bash_executable(), "bash")
                bash = root / "bin/bash.exe"
                bash.parent.mkdir()
                bash.touch()
                self.assertEqual(bash_executable(), str(bash.resolve()))

    def test_msvc_python_consumers_share_the_selected_runtime(self):
        def check(producer):
            blocks = steps(producer)
            selectors = [step for step in blocks if "uses: actions/setup-python@" in step]
            self.assertEqual(len(selectors), 1)
            selector = selectors[0]
            self.assertEqual(value(selector, "uses", 8),
                             "actions/setup-python@ece7cb06caefa5fff74198d8649806c4678c61a1 # v6")
            self.assertIsNone(value(selector, "if", 8))
            self.assertEqual(value(selector, "python-version", 10), "'3.14.8'")
            self.assertEqual(value(selector, "architecture", 10), "x64")
            self.assertEqual(value(selector, "check-latest", 10), "false")
            self.assertIsNone(value(selector, "cache", 10))
            runtime_id = value(selector, "id", 8)
            self.assertTrue(runtime_id)
            output = "${{ steps." + runtime_id + ".outputs.python-path }}"
            consumers = 0
            configures = 0
            for step in blocks:
                script = value(step, "run", 8) or ""
                configure = re.findall(r"(?m)^cmake (?:-S |--preset )[^\n]*", script)
                commands = [line.strip() for line in script.splitlines()
                            if re.match(r'^(?:python(?:3)? |& "\$env:CI_PYTHON_EXECUTABLE" |"\$CI_PYTHON_EXECUTABLE" )',
                                        line.strip())]
                if not configure and not commands:
                    continue
                consumers += 1
                configures += len(configure)
                self.assertLess(blocks.index(selector), blocks.index(step))
                self.assertEqual(value(step, "CI_PYTHON_EXECUTABLE", 10), output)
                if value(step, "shell", 8) == "bash":
                    executable = "$CI_PYTHON_EXECUTABLE"
                    prefix = '"$CI_PYTHON_EXECUTABLE" '
                elif "Enter-VsDevShell " in script:
                    executable = "$env:CI_PYTHON_EXECUTABLE"
                    self.assertEqual(script.count("$pythonExecutable ="), 1)
                    self.assertTrue(script.startswith("$pythonExecutable = $env:CI_PYTHON_EXECUTABLE\n"))
                    if commands:
                        alias = "Set-Alias -Name python -Value $pythonExecutable"
                        self.assertEqual(script.count(alias), 1)
                        self.assertLess(script.rindex("Enter-VsDevShell "), script.index(alias))
                        self.assertLess(script.index(alias), script.index(commands[0]))
                    prefix = "python "
                else:
                    executable = "$env:CI_PYTHON_EXECUTABLE"
                    prefix = '& "$env:CI_PYTHON_EXECUTABLE" '
                for command in commands:
                    self.assertTrue(command.startswith(prefix), command)
                for command in configure:
                    self.assertIn('"-DPython3_EXECUTABLE=' + executable + '"', command)
                    self.assertEqual(command.count("-DPython3_EXECUTABLE="), 1)
            self.assertEqual(consumers, 8)
            self.assertEqual(configures, 4)

        producer = jobs(self.workflows["ci.yml"])["windows-msvc"]
        check(producer)
        # Exercise producer/configure disagreements, including the PATH reset
        # between dependency installation and native inventory generation.
        for old, new in (
                ("python-version: '3.14.8'", "python-version: '3.14'"),
                ("architecture: x64", "architecture: x86"),
                ("check-latest: false", "check-latest: true"),
                ("id: python", "if: false\n        id: python"),
                (".outputs.python-path", ".outputs.python-version"),
                (' "-DPython3_EXECUTABLE=$CI_PYTHON_EXECUTABLE"', ""),
                (' "-DPython3_EXECUTABLE=$env:CI_PYTHON_EXECUTABLE"', ""),
                ("Set-Alias -Name python -Value $pythonExecutable", ""),
                ('& "$env:CI_PYTHON_EXECUTABLE" -m pip', "python -m pip")):
            with self.subTest(mutation=old):
                self.assertIn(old, producer)
                with self.assertRaises(AssertionError):
                    check(producer.replace(old, new, 1))

    def test_ready_and_draft_transitions_trigger_existing_pr_routes(self):
        for filename, workflow in self.workflows.items():
            with self.subTest(workflow=filename):
                pr = re.split(r"\n  (?=\S)", workflow.split("  pull_request:\n", 1)[1], maxsplit=1)[0]
                self.assertEqual(
                    value(pr, "types", 4),
                    ("[opened, synchronize, reopened, ready_for_review, converted_to_draft, labeled, unlabeled, edited]"
                     if filename == "ci.yml" else
                     "[opened, synchronize, reopened, ready_for_review, converted_to_draft]"),
                )
                self.assertEqual(value(pr, "branches", 4), "[main, devel]")

    def test_guard_runs_first_under_existing_check_name_without_draft_job_skip(self):
        for filename, job_id, check_name, job in guarded_jobs(self.workflows):
            with self.subTest(workflow=filename, job=job_id):
                self.assertEqual(value(job, "name", 4), check_name + (CI_METADATA_SUFFIX if filename == "ci.yml" and job_id != "linux" else ""))
                self.assertEqual(value(job, "runs-on", 4), "ubuntu-24.04")
                self.assertEqual(value(job, "needs", 4),
                                 "[changes, fast-lane, asan-shard]" if job_id == "asan" else
                                 "[changes, fast-lane]" if job_id == "asan-shard" else
                                 f"[changes, {job_id.removesuffix('-result')}]" if job_id.endswith("-result") else "changes")
                condition = ("!cancelled() || " + aggregate_invalid_expression(
                    "asan-shard" if job_id == "asan" else job_id.removesuffix("-result"))
                    if job_id == "asan" or job_id.endswith("-result") else "!cancelled()")
                if filename == "performance.yml" or job_id == "asan-shard":
                    condition += (
                        " && ((github.event_name == 'pull_request' && "
                        "toJSON(github.event.pull_request.draft) != 'false') || "
                        "needs.changes.outputs.needs_cpp != 'false')"
                    )
                if job_id == "asan-shard":
                    condition += (
                        " && (github.event_name != 'pull_request' || "
                        "toJSON(github.event.pull_request.draft) != 'false' || "
                        "needs.fast-lane.result == 'success')"
                    )
                self.assertEqual(value(job, "if", 4), ci_event_condition(condition) if filename == "ci.yml" else "${{ " + condition + " }}")
                self.assertIsNone(value(job, "continue-on-error", 4))
                guard = steps(job)[0]
                self.assertTrue(guard.startswith("      - name: Reject draft pull requests\n"))
                self.assertEqual(value(guard, "shell", 8), "bash")
                self.assertIsNone(value(guard, "if", 8))
                self.assertIsNone(value(guard, "continue-on-error", 8))
                self.assertEqual(value(guard, "CI_EVENT_NAME", 10), "${{ github.event_name }}")
                self.assertEqual(value(guard, "CI_PR_DRAFT", 10), "${{ github.event.pull_request.draft }}")
                self.assertIn("uses: actions/checkout@", steps(job)[1])

    def test_actual_guard_script_rejects_drafts_but_allows_ready_and_non_pr_events(self):
        cases = [
            ("pull_request", "true", 1),
            ("pull_request", "", 1),  # Fail closed if PR metadata is absent.
            ("pull_request", "false", 0),
            ("push", "", 0),
            ("schedule", "", 0),
            ("workflow_dispatch", "", 0),
        ]
        with tempfile.TemporaryDirectory() as directory:
            for filename, job_id, _, job in guarded_jobs(self.workflows):
                script = value(steps(job)[0], "run", 8)
                self.assertIsNotNone(script)
                for event, draft, expected in cases:
                    with self.subTest(workflow=filename, job=job_id, event=event, draft=draft):
                        result = run_bash_script(
                            script, flags=("-e",),
                            cwd=directory,
                            env=dict(os.environ, CI_EVENT_NAME=event, CI_PR_DRAFT=draft),
                            capture_output=True,
                            text=True,
                            timeout=10,
                        )
                        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                        if expected:
                            self.assertIn("::error::Draft pull requests cannot merge", result.stdout)

    def test_failed_guard_cannot_be_bypassed_by_build_or_evidence_steps(self):
        for filename, job_id, _, job in guarded_jobs(self.workflows):
            for step in steps(job)[1:]:
                with self.subTest(workflow=filename, job=job_id, step=step.splitlines()[0]):
                    condition = value(step, "if", 8) or ""
                    if (job_id == "asan" or job_id.endswith("-result")) and step.startswith("      - name: Reject invalid dependencies\n"):
                        self.assertEqual(condition, "${{ always() && (" + aggregate_invalid_expression(
                            "asan-shard" if job_id == "asan" else job_id.removesuffix("-result")) + ") }}")
                        self.assertIsNone(value(step, "continue-on-error", 8))
                        self.assertEqual(value(step, "run", 8).splitlines()[-1], "exit 1")
                        self.assertEqual(len(value(step, "run", 8).splitlines()), 2)
                        continue
                    # Ordinary steps implicitly require success(). Evidence steps
                    # using always() must explicitly exclude drafts or be manual-only.
                    if "always()" in condition:
                        self.assertTrue(
                            READY in condition or "github.event_name == 'workflow_dispatch'" in condition,
                            condition,
                        )
                    self.assertNotIn("failure()", condition)
                    self.assertNotIn("cancelled()", condition)
                    self.assertNotRegex(condition, r"!(?!=)")

    def test_unconditional_always_on_shard_build_fails_draft_policy(self):
        workflow = self.workflows["asan.yml"]
        original = jobs(workflow)["asan-shard"]
        mutated = original.replace("      - name: Build\n",
                                   "      - name: Build\n        if: always()\n", 1)
        self.assertNotEqual(mutated, original)
        case = WorkflowDraftPolicyTests("test_failed_guard_cannot_be_bypassed_by_build_or_evidence_steps")
        case.workflows = {**self.workflows, "asan.yml": workflow.replace(original, mutated, 1)}
        result = unittest.TestResult()
        case.run(result)
        self.assertEqual(len(result.errors), 0, result.errors)
        self.assertEqual(len(result.failures), 1, result.failures)
        self.assertIn("asan-shard", str(result.failures[0][0]))
        self.assertIn("Build", str(result.failures[0][0]))

    def test_other_heavy_jobs_still_skip_drafts_and_keep_routed_skips(self):
        ci = jobs(self.workflows["ci.yml"])
        for job_id in ("gns-compile-probe", "diligent-rhi", "tidy-shard"):
            with self.subTest(job=job_id):
                self.assertEqual(value(ci[job_id], "needs", 4), "changes")
                self.assertEqual(value(ci[job_id], "if", 4),
                                 ci_event_condition("!cancelled() && needs.changes.outputs.needs_cpp != 'false' && (" + READY + ")"))
        self.assertEqual(value(ci["windows-msvc"], "needs", 4), "changes")
        self.assertEqual(value(ci["windows-msvc"], "if", 4), ci_event_condition("!cancelled() && (needs.changes.outputs.needs_cpp != 'false' || needs.changes.outputs.needs_python != 'false') && (" + READY + ")"))
        self.assertEqual(value(ci["tidy"], "needs", 4), "[changes, tidy-shard]")
        self.assertEqual(value(ci["tidy"], "if", 4), ci_event_condition("(!cancelled() || " + aggregate_invalid_expression("tidy-shard") + ") && (" + READY + ")"))
        self.assertEqual(value(ci["windows-ucrt64"], "if", 4), ci_event_condition(f"!cancelled() && ({UCRT_CLASSIFICATION}) && ({READY}) && ({UCRT_ROUTE})"))
        perf = jobs(self.workflows["performance.yml"])
        self.assertEqual(value(perf["server-smoke-comparison"], "if", 4), "!cancelled() && needs.changes.outputs.needs_cpp != 'false' && " + f"({READY}) && {SERVER_ROUTE}")
        self.assertEqual(
            value(perf["qualification-capture"], "if", 4),
            "${{ !cancelled() && needs.changes.outputs.needs_cpp != 'false' && "
            "github.event_name == 'workflow_dispatch' && inputs.qualification_capture }}",
        )
        for job in (ci["lint"], ci["public-tree"]):
            self.assertEqual(value(job, "if", 4), ci_event_condition())

    def test_asan_fast_lane_runs_exact_registered_stdlib_contracts(self):
        fast = jobs(self.workflows["asan.yml"])["fast-lane"]
        self.assertEqual(value(fast, "needs", 4), "changes")
        self.assertEqual(value(fast, "runs-on", 4), "ubuntu-24.04")
        self.assertEqual(value(fast, "timeout-minutes", 4), "5")
        self.assertIsNone(value(fast, "continue-on-error", 4))
        registered = (ROOT / "tools/ci/local_check_contracts.cmake").read_text(encoding="utf-8")
        # The offline-dependency suite is registered once per partition; the fast lane runs the whole script once.
        scripts = list(dict.fromkeys(re.findall(
            r'COMMAND "\$\{Python3_EXECUTABLE\}" -I -B "\$\{_local_check_source_dir\}/([^"\n]+)"', registered)))
        self.assertEqual(scripts, ["test_changed_tus.py", "test_offline_deps.py"])
        commands = [value(step, "run", 8) for step in steps(fast)[1:]]
        self.assertEqual(commands, [f"python3 -I -B tools/ci/{script}" for script in scripts])
        checkout = steps(jobs(self.workflows["ci.yml"])["linux"])[1]
        self.assertEqual(value(steps(fast)[0], "uses", 8), value(checkout, "uses", 8))
        for step in steps(fast):
            self.assertIsNone(value(step, "if", 8))
            self.assertIsNone(value(step, "continue-on-error", 8))
        contracts = (ROOT / "tools/ci/contracts/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("project(LocalCheckContracts LANGUAGES NONE)", contracts)
        self.assertIn('include("${CMAKE_CURRENT_LIST_DIR}/../local_check_contracts.cmake")', contracts)

    @staticmethod
    def asan_route_condition(condition, event, draft, cpp, dependencies, cancelled=False):
        for key, item in (("toJSON(github.event.pull_request.draft)", draft),
                          ("github.event_name", event),
                          ("needs.changes.outputs.needs_cpp", cpp)):
            condition = condition.replace(key, repr(item))
        return evaluate_condition(condition, dependencies, cancelled=cancelled)

    def test_asan_fast_lane_gates_only_ready_native_pull_requests(self):
        blocks = jobs(self.workflows["asan.yml"])
        self.assertEqual(value(blocks["asan-shard"], "needs", 4), "[changes, fast-lane]")
        # All combinations are finite, in-memory evaluations of the actual YAML.
        for event in ("pull_request", "push", "workflow_dispatch"):
            for draft in ("false", "true", "null", "", "unknown"):
                for cpp in ("true", "false", "", "unknown"):
                    for result in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                        for cancelled in (False, True):
                            with self.subTest(event=event, draft=draft, cpp=cpp,
                                              result=result, cancelled=cancelled):
                                dependencies = {"changes": "success", "fast-lane": result}
                                ready_pr = event == "pull_request" and draft == "false"
                                routed = (event == "pull_request" and draft != "false") or cpp != "false"
                                fast_runs = self.asan_route_condition(
                                    value(blocks["fast-lane"], "if", 4), event, draft, cpp,
                                    {"changes": "success"}, cancelled)
                                self.assertEqual(fast_runs, not cancelled and ready_pr and cpp != "false")
                                shard_runs = self.asan_route_condition(
                                    value(blocks["asan-shard"], "if", 4), event, draft, cpp,
                                    dependencies, cancelled)
                                self.assertEqual(shard_runs, not cancelled and routed and
                                                 (not ready_pr or result == "success"))
        push = self.workflows["asan.yml"].split("  push:\n", 1)[1].split("  pull_request:", 1)[0]
        self.assertEqual(value(push, "branches", 4), "[main, devel]")
        self.assertIn("  workflow_dispatch:\n    inputs:\n      qualification_capture:", self.workflows["asan.yml"])

    def test_asan_aggregate_refuses_failed_fast_lane_even_with_skipped_shards(self):
        aggregate = jobs(self.workflows["asan.yml"])["asan"]
        self.assertEqual(value(aggregate, "needs", 4), "[changes, fast-lane, asan-shard]")
        refusal = next(step for step in steps(aggregate) if "name: Reject invalid dependencies\n" in step)
        route = next(step for step in steps(aggregate) if "name: Require all ASan shards to pass\n" in step)
        results = ("success", "failure", "cancelled", "skipped", "", "unknown")
        for fast in results:
            for classification in results:
                for shard in results:
                    for cancelled in (False, True):
                        with self.subTest(fast=fast, classification=classification,
                                          shard=shard, cancelled=cancelled):
                            dependencies = {"changes": classification, "fast-lane": fast, "asan-shard": shard}
                            invalid = classification != "success" or any(
                                result not in ("success", "skipped") for result in (fast, shard))
                            self.assertEqual(evaluate_condition(value(aggregate, "if", 4), dependencies,
                                                                cancelled=cancelled), not cancelled or invalid)
                            # always() must refuse even after checkout or the draft guard fails.
                            self.assertEqual(evaluate_condition(value(refusal, "if", 8), dependencies,
                                                                cancelled=cancelled, failed=True), invalid)
        with tempfile.TemporaryDirectory() as directory:
            for fast in results:
                for cpp in ("true", "false", ""):
                    with self.subTest(fast=fast, cpp=cpp):
                        dependencies = {"changes": "success", "fast-lane": fast, "asan-shard": "skipped"}
                        refuses = evaluate_condition(value(refusal, "if", 8), dependencies)
                        script = (value(refusal, "run", 8) + "\n" if refuses else "") + value(route, "run", 8)
                        process = run_bash_script(
                            script,
                            env=dict(os.environ, NEEDS_CPP=cpp, ASAN_SHARD_RESULT="skipped",
                                     CI_PATHS_RESULT="success", GITHUB_STEP_SUMMARY=(Path(directory) / "summary").as_posix()),
                            capture_output=True, text=True, timeout=10)
                        self.assertEqual(process.returncode == 0,
                                         cpp == "false" and fast in ("success", "skipped"), process.stderr)

    def test_required_check_names_stay_unchanged(self):
        expected = {
            "ci.yml": {"linux-result": "Linux / GCC", "windows-msvc-result": "Windows / MSVC",
                       "public-tree": "Public tree / inventory honesty", "lint": "Format changed files",
                       "tidy": "Static analysis / clang-tidy", "gns-compile-probe": "GNS transport / compile probe",
                       "diligent-rhi": "Diligent RHI / software GL"},
            "asan.yml": {"asan": "Linux / GCC ASan"},
            "docs.yml": {"doxygen": "Build Doxygen reference"},
            "performance.yml": {"runner-contract": "Runner contract / ${{ matrix.os }}",
                                "preview-frame": "Preview frame / software OpenGL"},
        }
        for filename, names in expected.items():
            workflow = (ROOT / ".github/workflows" / filename).read_text(encoding="utf-8")
            for job_id, name in names.items():
                with self.subTest(workflow=filename, job=job_id):
                    self.assertEqual(value(jobs(workflow)[job_id], "name", 4),
                                     name + (CI_METADATA_SUFFIX if filename == "ci.yml" else ""))

    def test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence(self):
        workflow = self.workflows["asan.yml"]
        blocks = jobs(workflow)
        self.assertEqual(set(blocks), {"changes", "fast-lane", "asan-shard", "asan"})
        shard, aggregate = blocks["asan-shard"], blocks["asan"]
        self.assertEqual(value(shard, "name", 4), "Linux / GCC ASan shard ${{ matrix.shard }}/3")
        self.assertEqual(value(shard, "needs", 4), "[changes, fast-lane]")
        self.assertEqual(value(shard, "if", 4), "${{ !cancelled() && ((github.event_name == 'pull_request' && toJSON(github.event.pull_request.draft) != 'false') || needs.changes.outputs.needs_cpp != 'false') && (github.event_name != 'pull_request' || toJSON(github.event.pull_request.draft) != 'false' || needs.fast-lane.result == 'success') }}")
        self.assertEqual(value(shard, "runs-on", 4), "ubuntu-24.04")
        self.assertEqual(value(shard, "timeout-minutes", 4), "75")
        self.assertEqual(value(shard, "fail-fast", 6), "false")
        self.assertEqual(value(shard, "shard", 8), "[1, 2, 3]")
        self.assertIsNone(value(shard, "continue-on-error", 4))
        self.assertEqual(steps(shard)[0], steps(aggregate)[0])
        self.assertEqual(value(aggregate, "timeout-minutes", 4), "5")
        shard_steps = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(shard)}
        aggregate_steps = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(aggregate)}
        self.assertEqual(value(shard_steps["Test"], "id", 8), "tests")
        self.assertEqual(value(shard_steps["Test"], "run", 8),
                         'ctest --preset debug-asan -LE manual --show-only=json-v1 > build/debug-asan/ctest.inventory.json\n'
                         'ctest --preset debug-asan --show-only=json-v1 > build/debug-asan/ctest.all.json\n'
                         '# Balance fixture and required-suite groups using recorded costs.\n'
                         'python3 tools/ci/ctest_shards.py plan --profile asan --shard ${{ matrix.shard }} --directory build/debug-asan --require-suite SimBudgetWorldParity. --require-suite AmbientDeterminismReceipt. --needs-heavy "$CI_NEEDS_HEAVY"\n'
                         '# Recheck the ordered roster in the same context immediately before CTest.\n'
                         'env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11 LIBGL_ALWAYS_SOFTWARE=1 \\\n'
                         '  xvfb-run -a python3 tools/ci/ctest_shards.py run --profile asan --directory build/debug-asan')
        self.assertNotIn("check_ctest_junit.py", shard)
        self.assertNotIn("compare_ambient_receipts.py", shard)
        check = aggregate_steps["Merge shard JUnit and check inventory"]
        self.assertEqual(value(check, "id", 8), "tests")
        commands = value(check, "run", 8).splitlines()
        merge_command = ASAN_MERGE_COMMAND
        inventory_prefix = "python3 tools/ci/check_ctest_junit.py build/debug-asan/ctest.junit.xml --minimum-tests "
        inventory_commands = [command for command in commands if command.startswith(inventory_prefix)]
        self.assertIn(merge_command, commands)
        self.assertEqual(len(inventory_commands), 1, inventory_commands)
        inventory_command = inventory_commands[0]
        self.assertRegex(inventory_command,
                         rf'^{re.escape(inventory_prefix)}"\$minimum_tests"(?: --require-test [\w./-]+)*$')
        floor_commands = [command for command in commands if re.fullmatch(r"minimum_tests=[0-9]+", command)]
        self.assertEqual(len(floor_commands), 1, floor_commands)
        floor_command = floor_commands[0]
        self.assertGreaterEqual(int(floor_command.split("=", 1)[1]), ASAN_MINIMUM_TESTS)
        floor_index = commands.index(floor_command)
        self.assertEqual(commands[floor_index + 1:floor_index + 4], [
            'if [ "$CI_NEEDS_HEAVY" = false ]; then',
            '  minimum_tests=$((minimum_tests - 2))',
            'fi',
        ])
        self.assertLess(commands.index(merge_command), floor_index)
        self.assertLess(floor_index + 3, commands.index(inventory_command))
        self.assertEqual(sum("minimum_tests=" in command for command in commands), 2)
        required_tests = set(re.findall(r" --require-test ([\w./-]+)", inventory_command))
        self.assertLessEqual(set(ASAN_REQUIRED_TESTS), required_tests)
        self.assertLess(commands.index(merge_command), commands.index(inventory_command))
        coverage_command = "python3 tools/ci/compare_ambient_receipts.py asan=build/debug-asan/ctest.junit.xml \\"
        self.assertIn(coverage_command, commands)
        self.assertLess(commands.index(inventory_command), commands.index(coverage_command))
        coverage_index = commands.index(coverage_command)
        self.assertEqual(commands[coverage_index + 1:coverage_index + 2],
                         ["  --expect-labels asan --require-probe asan"])
        for name in ("Download shard JUnit", "Merge shard JUnit and check inventory"):
            self.assertEqual(value(aggregate_steps[name], "if", 8), "needs.asan-shard.result == 'success'")
            self.assertIsNone(value(aggregate_steps[name], "continue-on-error", 8))
        self.assertIn("actions/download-artifact@9000827ccba6bdab643e8b6fd33ac0654aef8333", aggregate)
        self.assertNotIn("merge-multiple: true", aggregate)
        upload = shard_steps["Upload shard JUnit"]
        self.assertIn("name: asan-junit-${{ github.sha }}-${{ github.run_id }}-shard-${{ matrix.shard }}", upload)
        self.assertEqual(value(upload, "overwrite", 10), "true")
        self.assertIn("build/debug-asan/ctest.inventory.json", upload)
        self.assertIn("build/debug-asan/ctest.junit.xml", upload)
        self.assertIn("build/debug-asan/qualification-shard.json", upload)
        self.assertIn("if-no-files-found: error", upload)
        diagnostics = shard_steps["Upload sanitizer evidence"]
        self.assertEqual(value(diagnostics, "name", 10),
                         "asan-test-evidence-${{ github.sha }}-${{ github.run_id }}-shard-${{ matrix.shard }}-attempt-${{ github.run_attempt }}")
        self.assertIsNone(value(diagnostics, "overwrite", 10))
        self.assertEqual(value(shard_steps["Bind manual qualification shard evidence"], "QUALIFICATION_PRODUCERS", 10),
                         "'{\"identity\":\"${{ steps.identity.outcome }}\",\"configure\":\"${{ steps.configure.outcome }}\",\"build\":\"${{ steps.build.outcome }}\",\"tests\":\"${{ steps.tests.outcome }}\"}'")
        for name in ("identity", "configure", "build", "tests"):
            self.assertIn(f"id: {name}\n", shard)
        self.assertIn('asan-shard --lane asan --expected-sha "$QUALIFICATION_EXPECTED_SHA" --shard ${{ matrix.shard }}', shard)
        self.assertIn('--asan-shards build/asan-shards', aggregate_steps["Bind manual qualification evidence"])
        self.assertEqual(value(aggregate_steps["Bind manual qualification evidence"], "QUALIFICATION_PRODUCERS", 10),
                         "'{\"tests\":\"${{ steps.tests.outcome }}\"}'")
        self.assertIn("name: qualification-asan-${{ github.sha }}-${{ github.run_id }}-${{ github.run_attempt }}", aggregate)
        self.assertIn("python3 tools/ci/test_asan_shards.py", jobs(self.workflows["ci.yml"])["public-tree"])

    def test_asan_matrix_name_and_planner_count_must_agree(self):
        spec = importlib.util.spec_from_file_location("ctest_shards", ROOT / "tools/ci/ctest_shards.py")
        planner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(planner)
        block = jobs(self.workflows["asan.yml"])["asan-shard"]
        self.assertEqual(value(block, "shard", 8), str(list(planner.shard_ids("asan"))))
        for old, new in (("shard: [1, 2, 3]", "shard: [1, 2]"),
                         ("matrix.shard }}/3", "matrix.shard }}/2")):
            case = type(self)("test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence")
            case.workflows = {**self.workflows, "asan.yml": self.workflows["asan.yml"].replace(old, new)}
            result = unittest.TestResult()
            case.run(result)
            self.assertEqual(len(result.failures), 1)
            self.assertFalse(result.errors)

    def test_native_matrices_aggregates_limits_and_full_qualification_route(self):
        ci = jobs(self.workflows["ci.yml"])
        matrix = "${{ github.event_name == 'workflow_dispatch' && inputs.qualification_capture && fromJSON('[0]') || fromJSON('[1,2]') }}"
        for job_id, profile, title, runner, limit, floor in (
                ("linux", "gcc", "Linux / GCC", "ubuntu-24.04", "60", "2340"),
                ("windows-msvc", "msvc", "Windows / MSVC", "windows-2025", "90", "2320"),
                ("windows-ucrt64", "ucrt64", "Windows / MSYS2 UCRT64", "windows-2025", "85", "2320")):
            with self.subTest(job=job_id):
                producer, aggregate = ci[job_id], ci[job_id + "-result"]
                self.assertEqual(value(producer, "shard", 8), matrix)
                self.assertEqual(value(producer, "fail-fast", 6), "false")
                self.assertEqual(value(producer, "timeout-minutes", 4), limit)
                self.assertEqual(value(producer, "runs-on", 4), runner)
                self.assertIn(title + " full qualification", value(producer, "name", 4))
                self.assertIn(title + " shard {0}/2", value(producer, "name", 4))
                self.assertEqual(value(aggregate, "name", 4), title + CI_METADATA_SUFFIX)
                self.assertEqual(value(aggregate, "if", 4), ci_event_condition("!cancelled() || " + aggregate_invalid_expression(job_id)))
                self.assertEqual(value(aggregate, "needs", 4), f"[changes, {job_id}]")
                self.assertEqual(value(aggregate, "timeout-minutes", 4), "5")
                self.assertEqual(self.workflows["ci.yml"].count("    name: " + title + CI_METADATA_SUFFIX + "\n"), 1)
                self.assertIsNone(value(producer, "continue-on-error", 4))
                self.assertIsNone(value(aggregate, "continue-on-error", 4))
                for command in ("plan", "run"):
                    self.assertIn(f"ctest_shards.py {command} --profile {profile}", producer)
                self.assertIn("ctest --preset release -LE manual --show-only=json-v1", producer)
                self.assertIn("ctest --preset release --show-only=json-v1", producer)
                self.assertIn("--require-suite SimBudgetWorldParity. --require-suite AmbientDeterminismReceipt.", producer)
                gate = next(step for step in steps(aggregate) if "name: Require expected producers and route\n" in step)
                self.assertEqual(value(gate, "SHARD_RESULT", 10), "${{ needs." + job_id + ".result }}")
                for key, expression in (("NEEDS_CPP", "needs.changes.outputs.needs_cpp"),
                                        ("CI_EVENT", "github.event_name"),
                                        ("CI_DRAFT", "github.event.pull_request.draft")):
                    self.assertEqual(value(gate, key, 10),
                                     '"true"' if key == "NEEDS_CPP" and profile == "ucrt64"
                                     else "${{ " + expression + " }}")
                self.assertEqual(value(gate, "CI_PATHS_RESULT", 10), "${{ needs.changes.result }}")
                self.assertIn('--classification "$CI_PATHS_RESULT"', gate)
                self.assertEqual(value(gate, "CI_CANCELLED", 10), '"false"')
                self.assertIn('ctest_shards.py gate --result "$SHARD_RESULT" --cpp "$NEEDS_CPP"', gate)
                self.assertIn('--cancelled "$CI_CANCELLED" --event "$CI_EVENT" --draft "$CI_DRAFT"', gate)
                merge = next(step for step in steps(aggregate) if "name: Merge shard JUnit and check inventory\n" in step)
                self.assertIn(f"ctest_shards.py merge --profile {profile}", merge)
                self.assertIn('--needs-heavy "$CI_NEEDS_HEAVY" --reason "$CI_HEAVY_REASON"', merge)
                check = ("tools/ci/check_ctest_junit.py build/release/ctest.junit.xml"
                         " --minimum-tests " + floor)
                for name in ASAN_REQUIRED_TESTS:
                    check += " --require-test " + name
                commands = value(merge, "run", 8).splitlines()
                floor_index = commands.index("minimum_tests=" + floor)
                self.assertEqual(commands[floor_index + 1:floor_index + 4], [
                    'if [ "$CI_NEEDS_HEAVY" = false ]; then',
                    '  minimum_tests=$((minimum_tests - 2))',
                    'fi',
                ])
                self.assertEqual(sum("minimum_tests=" in command for command in commands), 2)
                selected_check = "python3 " + check.replace(floor, '"$minimum_tests"', 1)
                self.assertIn(selected_check, commands)
                merge_index = next(index for index, command in enumerate(commands)
                                   if "ctest_shards.py merge --profile " + profile in command)
                self.assertLess(merge_index, floor_index)
                self.assertLess(floor_index + 3, commands.index(selected_check))
                test_name = "Configure, build, and test" if profile == "msvc" else "Test"
                test = next(step for step in steps(producer) if "name: " + test_name + "\n" in step)
                full_check = ("if (${{ matrix.shard }} -eq 0) {\n  python " + check + "\n}"
                              if profile == "msvc" else
                              "if [ '${{ matrix.shard }}' = 0 ]; then\n  " +
                              ("python3 " if profile == "gcc" else "python ") + check + "\nfi")
                self.assertIn(full_check, value(test, "run", 8))
                self.assertEqual(value(test, "run", 8).count("check_ctest_junit.py"), 1)
                self.assertEqual(value(merge, "run", 8).count("check_ctest_junit.py"), 1)
                self.assertIn('if [ "$CI_QUALIFICATION" = true ]; then full=(--full); fi', merge)
                self.assertIn('"${full[@]}"', merge)
                self.assertNotIn("merge-multiple: true", aggregate)
                self.assertNotIn("actions/cache", aggregate)


    def test_heavy_policy_inputs_reach_every_producer_and_required_aggregate(self):
        for filename, producers, aggregates in (
                ("ci.yml", ("linux", "windows-msvc", "windows-ucrt64"),
                 ("linux-result", "windows-msvc-result", "windows-ucrt64-result")),
                ("asan.yml", ("asan-shard",), ("asan",))):
            blocks = jobs(self.workflows[filename])
            classifier = blocks["changes"]
            for key in ("needs_heavy", "heavy_reason"):
                self.assertIn(key + ": ${{ steps.paths.outputs." + key + " }}", classifier)
            for job_id in (*producers, *aggregates):
                with self.subTest(workflow=filename, job=job_id):
                    job = blocks[job_id]
                    self.assertEqual(value(job.split("    steps:\n", 1)[0], "CI_NEEDS_HEAVY", 6), "${{ needs.changes.outputs.needs_heavy }}")
                    self.assertEqual(value(job.split("    steps:\n", 1)[0], "CI_HEAVY_REASON", 6), "${{ needs.changes.outputs.heavy_reason }}")
                    variable = "$env:CI_NEEDS_HEAVY" if job_id == "windows-msvc" else "$CI_NEEDS_HEAVY"
                    self.assertIn('--needs-heavy "' + variable + '"', job)
                    self.assertNotIn("--selection light", job)
                    if job_id in aggregates:
                        self.assertIn("ctest_shards.py merge", job)
                        merge = next(step for step in steps(job) if "name: Merge shard JUnit and check inventory" in step)
                        self.assertIsNone(value(merge, "continue-on-error", 8))
                        upload = next(step for step in steps(job) if "name: Upload merged JUnit" in step)
                        self.assertIn("ctest.junit.selection.json", upload)
                        self.assertEqual(value(upload, "retention-days", 10), "30")

    def test_bootstrap_pins_full_events_main_prs_and_missing_or_invalid_outputs(self):
        fallback = {"needs_cpp": "true", "needs_python": "true", "needs_heavy": "true",
                    "heavy_reason": "classification_uncertain"}
        light = {"needs_cpp": "true", "needs_python": "false", "needs_heavy": "false",
                 "heavy_reason": "reviewed_presentation_or_docs"}
        # Even an exempt diff or a base classifier asking for light cannot
        # downgrade a merged tip, manual run, or a main-bound PR.
        cases = [
            ("push", "main", light, True), ("push", "devel", light, True),
            ("workflow_dispatch", "devel", light, True),
            ("pull_request", "main", light, True), ("pull_request", "", light, True),
            ("pull_request", "devel", light, False),
            ("pull_request", "devel", {"needs_cpp": "false", "needs_python": "false"}, True),
            ("pull_request", "devel", {**light, "needs_heavy": ""}, True),
            ("pull_request", "devel", {**light, "needs_heavy": "0"}, True),
            ("pull_request", "devel", {**light, "heavy_reason": "unreviewed"}, True),
            ("pull_request", "devel", {**light, "needs_cpp": "false", "needs_heavy": "true"}, True),
        ]
        cases = [(*case, "owner/repo", "owner/repo") for case in cases]
        cases.extend(("pull_request", "devel", light, True, repo, head_repo)
                     for repo, head_repo in (("", ""), ("owner/repo", ""),
                                             ("", "owner/repo"), ("owner/repo", "fork/repo")))
        for filename in ("ci.yml", "asan.yml", "performance.yml", "ucrt-attribution.yml"):
            classifier = jobs((ROOT / ".github/workflows" / filename).read_text(encoding="utf-8"))["changes"]
            step = next(step for step in steps(classifier) if "name: Classify merge-base diff" in step)
            self.assertEqual(value(step, "CI_BASE_REF", 10), "${{ github.base_ref }}")
            script = value(step, "run", 8)
            self.assertIn('git show "$CI_BASE_SHA:tools/ci/ci_paths.py"', script)
            self.assertNotIn("git fetch", script)
            for event, base_ref, outputs, full, repo, head_repo in cases:
                with self.subTest(workflow=filename, event=event, base_ref=base_ref, outputs=outputs), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve(strict=True)
                    fixtures = (
                        'git() { case "$1" in\n'
                        'diff) return 0;;\nshow) cat "$FAKE_BASE_SCRIPT";;\n'
                        '*) return 1;;\nesac; }\n'
                        'python3() { "$FAKE_PYTHON" -I -B "$@"; }\n'
                    )
                    base = root / "base.py"
                    content = "".join(key + "=" + val + "\n" for key, val in outputs.items())
                    base.write_text("import os\nfrom pathlib import Path\n"
                                    "Path(os.environ['GITHUB_OUTPUT']).write_text(" + repr(content) + ")\n")
                    output = root / "output"
                    # Stale temporary evidence must not survive a base script
                    # which emits no value on this run.
                    (root / "classified-paths.txt").write_text(content)
                    env = dict(os.environ,
                               CI_EVENT=event, CI_BASE_REF=base_ref, CI_BASE_SHA="base", CI_HEAD_SHA="head",
                               CI_REPOSITORY=repo, CI_HEAD_REPOSITORY=head_repo,
                               RUNNER_TEMP=root.as_posix(), GITHUB_WORKSPACE=root.as_posix(),
                               GITHUB_OUTPUT=output.as_posix(), FAKE_BASE_SCRIPT=base.as_posix(),
                               FAKE_PYTHON=Path(sys.executable).as_posix())
                    result = run_bash_script(fixtures + script,
                                            env=env, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    actual = dict(line.split("=", 1) for line in output.read_text().splitlines())
                    self.assertEqual(actual, fallback if full else light)

    def test_ucrt64_producer_and_aggregate_enable_all_push_and_manual_routes(self):
        blocks = jobs(self.workflows["ci.yml"])
        producer = value(blocks["windows-ucrt64"], "if", 4)
        self.assertEqual(producer, ci_event_condition(f"!cancelled() && ({UCRT_CLASSIFICATION}) && ({READY}) && ({UCRT_ROUTE})"))
        aggregate = next(step for step in steps(blocks["windows-ucrt64-result"])
                         if "name: Require expected producers and route" in step)
        self.assertEqual(value(aggregate, "CI_ENABLED", 10), "${{ (" + UCRT_CLASSIFICATION + ") && (" + UCRT_ROUTE + ") }}")
        # Opted-in native routes require native evidence, including failed-open classification.
        self.assertEqual(value(aggregate, "NEEDS_CPP", 10), '"true"')
        upload = next(step for step in steps(blocks["windows-ucrt64"])
                      if "name: Upload shard JUnit" in step)
        self.assertEqual(value(upload, "if", 8), "always() && (" + READY + ")")
        for name in ("Download shard JUnit", "Merge shard JUnit and check inventory", "Upload merged JUnit"):
            step = next(step for step in steps(blocks["windows-ucrt64-result"])
                        if step.startswith("      - name: " + name + "\n"))
            self.assertEqual(value(step, "if", 8), "needs.windows-ucrt64.result == 'success'")
        self.assertEqual(value(blocks["windows-ucrt64"], "timeout-minutes", 4), "85")

    def test_windows_python_selection_is_exact_and_precedes_every_consumer(self):
        ci = jobs(self.workflows["ci.yml"])
        for job_id in ("windows-msvc", "windows-ucrt64"):
            with self.subTest(job=job_id):
                producer = ci[job_id]
                selected = [step for step in steps(producer) if "uses: actions/setup-python@" in step]
                self.assertEqual(len(selected), 1)
                setup = selected[0]
                self.assertEqual(value(setup, "uses", 8),
                                 "actions/setup-python@ece7cb06caefa5fff74198d8649806c4678c61a1 # v6")
                self.assertEqual(value(setup, "id", 8), "python")
                self.assertEqual(dict(re.findall(r"^          ([\w-]+): (.*)$", setup, re.MULTILINE)), {
                    "python-version": "'3.14.8'", "architecture": "x64", "check-latest": "false",
                    "allow-prereleases": "false", "freethreaded": "false", "update-environment": "true",
                })
                # Default success propagation must cover both shards and shard 0;
                # an unavailable runtime cannot fall through to CMake discovery.
                self.assertIsNone(value(setup, "if", 8))
                self.assertIsNone(value(setup, "continue-on-error", 8))
                self.assertIsNone(value(setup, "env", 8))
                for step in steps(producer):
                    script = value(step, "run", 8) or ""
                    if re.search(r"\b(?:python|python3|cmake)\b", script):
                        self.assertLess(producer.index(setup), producer.index(step))
                self.assertEqual(value(producer, "shard", 8),
                                 "${{ github.event_name == 'workflow_dispatch' && inputs.qualification_capture "
                                 "&& fromJSON('[0]') || fromJSON('[1,2]') }}")

    def test_windows_cmake_discovery_binds_the_selected_python_on_every_route(self):
        ci = jobs(self.workflows["ci.yml"])
        expected = {
            ("windows-msvc", "Run registered local-check deadlines"): (1, "bash", None),
            ("windows-msvc", "Configure, build, and test"): (
                1, "pwsh", "${{ needs.changes.outputs.needs_cpp != 'false' }}"),
            ("windows-msvc", "Validate dormant and enabled Windows packaging"): (
                2, "pwsh", "${{ needs.changes.outputs.needs_cpp != 'false' && "
                "(github.ref_name == 'main' || github.base_ref == 'main') }}"),
            ("windows-ucrt64", "Configure"): (1, "msys2 {0}", None),
        }
        actual = {}
        for job_id in ("windows-msvc", "windows-ucrt64"):
            for step in steps(ci[job_id]):
                commands = re.findall(r"^cmake (?:--preset|-S) .*$", value(step, "run", 8) or "", re.MULTILINE)
                if not commands:
                    continue
                name = step.splitlines()[0].removeprefix("      - name: ")
                with self.subTest(job=job_id, step=name):
                    actual[job_id, name] = (len(commands), value(step, "shell", 8), value(step, "if", 8))
                    self.assertEqual(value(step, "CI_PYTHON_EXECUTABLE", 10),
                                     "${{ steps.python.outputs.python-path }}")
                    self.assertEqual(step.count("        env:\n"), 1)
                    self.assertIsNone(value(step, "continue-on-error", 8))
                    variable = ("$env:CI_PYTHON_EXECUTABLE" if value(step, "shell", 8) == "pwsh"
                                else "$CI_PYTHON_EXECUTABLE")
                    for command in commands:
                        self.assertEqual(command.count("-DPython3_EXECUTABLE="), 1)
                        self.assertIn('"-DPython3_EXECUTABLE=' + variable + '"', command)
        self.assertEqual(actual, expected)

    def test_windows_bash_configure_preserves_python_path_as_one_literal_argument(self):
        ci = jobs(self.workflows["ci.yml"])
        for job_id, name in (("windows-msvc", "Run registered local-check deadlines"),
                             ("windows-ucrt64", "Configure")):
            step = next(step for step in steps(ci[job_id]) if step.startswith("      - name: " + name + "\n"))
            command, = re.findall(r"^cmake (?:--preset|-S) .*$", value(step, "run", 8), re.MULTILINE)
            self.assertIn("-DPython3_EXECUTABLE=", command)
            # Execute only argument transport into a shell function. Neither
            # CMake nor a Windows interpreter is executed by this lexical check.
            script = 'cmake() { printf "%s\\0" "$@"; };\n' + command
            for path in ("C:/hostedtoolcache/windows/Python/3.14.8/x64/python.exe",
                         r"D:\Python runtime\3.14.8\python.exe",
                         "C:/PROGRA~1/Python/python.exe", "C:/Python $literal; value/python.exe"):
                with self.subTest(job=job_id, path=path), tempfile.TemporaryDirectory() as directory:
                    result = run_bash_script(script, flags=("-e",),
                                            cwd=directory, env=dict(os.environ, CI_PYTHON_EXECUTABLE=path),
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stderr, "")
                    arguments = result.stdout.split("\0")[:-1]
                    self.assertEqual([arg for arg in arguments if arg.startswith("-DPython3_EXECUTABLE=")],
                                     ["-DPython3_EXECUTABLE=" + path])

    def test_native_cache_writers_and_attempt_specific_retained_evidence(self):
        ci = jobs(self.workflows["ci.yml"])
        for job_id, profile in (("linux", "gcc"), ("windows-msvc", "msvc"), ("windows-ucrt64", "ucrt64")):
            producer, aggregate = ci[job_id], ci[job_id + "-result"]
            producer_steps = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(producer)}
            for step in ("Clean compiler cache before saving", "Save compiler cache"):
                self.assertTrue(value(producer_steps[step], "if", 8).startswith("${{ matrix.shard == 1 && "))
            self.assertNotIn("matrix.shard", producer_steps["Restore compiler cache"])
            upload = producer_steps["Upload shard JUnit"]
            self.assertEqual(value(upload, "retention-days", 10), "30")
            self.assertEqual(value(upload, "if-no-files-found", 10), "error")
            self.assertEqual(value(upload, "path", 10).splitlines(), [
                "build/release/ctest.junit.xml", "build/release/ctest.inventory.json", "build/release/ctest.partition.json",
                *(["build/release/ctest-discovery.json", "build/release/netsoak.junit.xml"] if profile == "gcc" else [])])
            self.assertEqual(value(upload, "name", 10), profile + "-junit-${{ github.sha }}-${{ github.run_id }}-shard-${{ matrix.shard }}-attempt-${{ github.run_attempt }}")
            self.assertIn("pattern: " + profile + "-junit-${{ github.sha }}-${{ github.run_id }}-shard-*-attempt-*", aggregate)
            self.assertIn("--attempt ${{ github.run_attempt }}", aggregate)
        self.assertIn("cache: ${{ matrix.shard == 1 && github.event_name == 'push'", ci["windows-ucrt64"])

    def test_gcc_preserves_prefab_netsoak_packaging_and_diagnostics(self):
        linux = jobs(self.workflows["ci.yml"])["linux"]
        self.assertIn("-DLUMINUMBRA_ENABLE_PREFAB_RUNTIME=ON", linux)
        self.assertIn("--require-suite PrefabRuntimeTest. --require-suite PrefabDigest.", linux)
        prefab = next(step for step in steps(linux) if "name: Qualify installed prefab runtime\n" in step)
        self.assertIn("steps.tests.outputs.prefab == 'true'", value(prefab, "if", 8))
        self.assertIn("--ctest-report build/release/ctest.junit.xml", prefab)
        netsoak = next(step for step in steps(linux) if "name: Test over-the-wire networking\n" in step)
        self.assertIn("matrix.shard != 2", value(netsoak, "if", 8))
        self.assertIn("-R '^NetSoak$'", netsoak)
        self.assertIn("--parallel 1 --output-junit netsoak.junit.xml", netsoak)
        for text in ("build/package-off", "build/package-linux", "build/release/Testing/Temporary",
                     "build/release/test-artifacts", "include-hidden-files: true"):
            self.assertIn(text, linux)
        self.assertIn("python3 -I -B tools/ci/test_ctest_shards.py", jobs(self.workflows["ci.yml"])["public-tree"])

    def test_acceptance_execution_uses_verified_union_and_retains_networking_evidence(self):
        # Hosted shards discover these unchanged labels from the engine CTest
        # inventory. Recovery belongs to the same disjoint partition roster.
        cmake = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        registrations = re.findall(
            r'add_test\(\s*NAME (AcceptanceMatrixContract\d*)\s+COMMAND(.*?)\)', cmake, re.S)
        self.assertEqual(len(registrations), 225)
        self.assertEqual({int(re.search(r'--contract-partition ([0-9]+)', command)[1])
                          for _, command in registrations}, set(range(225)))
        self.assertNotIn("NAME AcceptanceRecoveryContract", cmake)
        for name, command in registrations:
            self.assertIn('ci/test_check_acceptance_matrix.py', command)
            self.assertRegex(cmake, rf'set_tests_properties\({name}\s+PROPERTIES LABELS "hygiene;tools" TIMEOUT 30\)')
        ci = jobs(self.workflows["ci.yml"])
        producer, aggregate = ci["linux"], ci["linux-result"]
        self.assertNotIn("check_acceptance_matrix.py", producer)
        self.assertIn("ctest --preset release --show-only=json-v1 > build/release/ctest-discovery.json", producer)
        named = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(aggregate)}
        check = named["Check acceptance matrix CTest execution"]
        self.assertLess(aggregate.index("name: Merge shard JUnit and check inventory\n"),
                        aggregate.index("name: Check acceptance matrix CTest execution\n"))
        self.assertEqual(value(check, "if", 8),
                         "needs.linux.result == 'success' && needs.changes.outputs.needs_cpp != 'false'")
        self.assertIsNone(value(check, "continue-on-error", 8))
        self.assertEqual(value(check, "CI_QUALIFICATION", 10),
                         "${{ github.event_name == 'workflow_dispatch' && inputs.qualification_capture }}")
        script = value(check, "run", 8)
        prepare, command = script.split("python3 tools/ci/check_acceptance_matrix.py", 1)
        self.assertEqual(" ".join(command.replace("\\\n", "").split()),
                         "--junit build/release/ctest.junit.xml --junit build/release/netsoak.junit.xml "
                         "--discovery build/release/ctest-discovery.json --ci-context "
                         "--execution-receipt build/release/test-artifacts/acceptance-execution.json "
                         '--preparation-receipt-file "$RUNNER_TEMP/acceptance-preparation-custody.json"')
        upload = named["Upload merged JUnit"]
        self.assertEqual(value(upload, "if", 8),
                         "always() && needs.linux.result == 'success' && "
                         "needs.changes.outputs.needs_cpp != 'false' && (" + READY + ")")
        self.assertEqual(value(upload, "path", 10).splitlines(), [
            "build/release/ctest.junit.xml", "build/release/ctest.junit.selection.json",
            "build/release/ctest-discovery.json",
            "build/release/netsoak.junit.xml", "build/release/test-artifacts/acceptance-execution.json"])
        self.assertEqual(value(upload, "retention-days", 10), "30")
        public = ci["public-tree"]
        self.assertIn("python3 tools/ci/test_check_acceptance_matrix.py", public)
        self.assertIn("python3 tools/ci/check_acceptance_matrix.py --check-doc docs/acceptance-matrix.md", public)
        # Run the actual evidence-selection script for both execution routes.
        for full, owner in (("false", 1), ("true", 0)):
            for missing in (None, "ctest-discovery.json", "netsoak.junit.xml"):
                with self.subTest(full=full, missing=missing), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve(strict=True)
                    (root / "build/release").mkdir(parents=True)
                    source = root / f"build/gcc-shards/shard-{owner}"
                    source.mkdir(parents=True)
                    for filename in ("ctest-discovery.json", "netsoak.junit.xml"):
                        if filename != missing:
                            (source / filename).write_text(f"owner-{owner}-{filename}", encoding="utf-8")
                    result = run_bash_script(
                        prepare,
                        cwd=root, env=dict(os.environ, CI_QUALIFICATION=full),
                        capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode == 0, missing is None, result.stderr)
                    if missing is None:
                        for filename in ("ctest-discovery.json", "netsoak.junit.xml"):
                            self.assertEqual((root / "build/release" / filename).read_bytes(),
                                             (source / filename).read_bytes())

    def test_ci_and_asan_bash_scripts_parse_without_executing_native_commands(self):
        for filename in ("ci.yml", "asan.yml"):
            for job_id, job in jobs(self.workflows[filename]).items():
                windows = value(job, "runs-on", 4).startswith("windows-")
                for step in steps(job):
                    script = value(step, "run", 8)
                    shell = value(step, "shell", 8) or ("pwsh" if windows else "bash")
                    if script is None or not shell.startswith(("bash", "msys2")):
                        continue
                    script = re.sub(r"\$\{\{.*?\}\}", "1", script)
                    with self.subTest(workflow=filename, job=job_id, step=step.splitlines()[0]):
                        result = run_bash_script(script, flags=("-n",),
                                                capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stderr)

    def test_inventory_floors_only_defer_the_reviewed_two_cases_in_every_profile(self):
        for filename, job, directory, floor in (
                ("asan.yml", "asan", "build/debug-asan", 2320),
                ("ci.yml", "linux-result", "build/release", 2340),
                ("ci.yml", "windows-msvc-result", "build/release", 2320),
                ("ci.yml", "windows-ucrt64-result", "build/release", 2320)):
            aggregate = jobs(self.workflows[filename])[job]
            step = next(step for step in steps(aggregate) if "name: Merge shard JUnit and check inventory\n" in step)
            commands = value(step, "run", 8).splitlines()
            start = next(index for index, command in enumerate(commands) if command.startswith("minimum_tests="))
            end = next(index for index, command in enumerate(commands) if command.startswith("python3 tools/ci/check_ctest_junit.py"))
            script = "\n".join(commands[start:end + 1])
            for needs_heavy in ("false", "true", "", "False", "0", "unknown"):
                with self.subTest(job=job, needs_heavy=needs_heavy):
                    result = run_bash_script(
                        'python3() { printf "%s\\n" "$@"; }\n' + script,
                        env=dict(os.environ, CI_NEEDS_HEAVY=needs_heavy), capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    arguments = result.stdout.splitlines()
                    expected = str(floor - 2 if needs_heavy == "false" else floor)
                    self.assertEqual(arguments[:4], ["tools/ci/check_ctest_junit.py", directory + "/ctest.junit.xml",
                                                     "--minimum-tests", expected])
                    for name in ASAN_REQUIRED_TESTS:
                        self.assertIn(name, arguments)

    def test_asan_aggregate_rejects_changed_selection_or_floor_setup(self):
        workflow = self.workflows["asan.yml"]
        setup = "          minimum_tests=2320\n"
        for original, replacement in (
            (setup, ""),
            (setup, setup.replace("minimum_tests=", "# minimum_tests=")),
            (setup, setup.replace("2320", "unknown")),
            ('if [ "$CI_NEEDS_HEAVY" = false ]; then', 'if [ "$CI_NEEDS_HEAVY" != true ]; then'),
            ("minimum_tests - 2", "minimum_tests - 3"),
            ("minimum_tests - 2", "minimum_tests - 1"),
            (' --needs-heavy "$CI_NEEDS_HEAVY" --reason "$CI_HEAVY_REASON"', ""),
        ):
            with self.subTest(original=original, replacement=replacement):
                mutated = workflow.replace(original, replacement, 1)
                self.assertNotEqual(mutated, workflow)
                case = WorkflowDraftPolicyTests("test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence")
                case.workflows = {**self.workflows, "asan.yml": mutated}
                result = unittest.TestResult()
                case.run(result)
                self.assertEqual(len(result.errors), 0, result.errors)
                self.assertEqual(len(result.failures), 1, result.failures)

    def test_asan_aggregate_requires_inventory_floor_after_merge(self):
        workflow = self.workflows["asan.yml"]
        merge = "          " + ASAN_MERGE_COMMAND + "\n"
        inventory = next(line for line in workflow.splitlines(keepends=True)
                         if line.startswith("          python3 tools/ci/check_ctest_junit.py "
                                            "build/debug-asan/ctest.junit.xml --minimum-tests "))
        for mutation in ("removed", "lowered", "commented", "before-merge"):
            with self.subTest(mutation=mutation):
                replacement = {
                    "removed": "",
                    "lowered": inventory,
                    "commented": inventory.replace("python3", "# python3", 1),
                    "before-merge": "",
                }[mutation]
                mutated = workflow.replace(inventory, replacement, 1)
                if mutation == "lowered":
                    mutated = re.sub(r"(?m)^          minimum_tests=[0-9]+$",
                                     f"          minimum_tests={ASAN_MINIMUM_TESTS - 1}", mutated, count=1)
                if mutation == "before-merge":
                    mutated = mutated.replace(merge, inventory + merge, 1)
                self.assertNotEqual(mutated, workflow)
                case = WorkflowDraftPolicyTests(
                    "test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence")
                case.workflows = {**self.workflows, "asan.yml": mutated}
                result = unittest.TestResult()
                case.run(result)
                self.assertEqual(len(result.errors), 0, result.errors)
                self.assertEqual(len(result.failures), 1, result.failures)

    def test_asan_aggregate_accepts_stronger_inventory_checks_after_merge(self):
        workflow = self.workflows["asan.yml"]
        for floor in (ASAN_MINIMUM_TESTS, ASAN_MINIMUM_TESTS + 1):
            with self.subTest(floor=floor):
                strengthened, count = re.subn(
                    r"(?m)^          minimum_tests=[0-9]+$",
                    f"          minimum_tests={floor}", workflow,
                )
                self.assertEqual(count, 1)
                strengthened, count = re.subn(
                    r'(?m)^(          python3 tools/ci/check_ctest_junit.py '
                    r'build/debug-asan/ctest.junit.xml --minimum-tests "\$minimum_tests"[^\n]*)$',
                    r"\g<1> --require-test AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes"
                    " --require-test AmbientInputBounds.UnpaddedTwoElementInputFaultsAtActiveIsa",
                    strengthened,
                )
                self.assertEqual(count, 1)
                for method in (
                    "test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence",
                    "test_asan_aggregate_requires_inventory_floor_after_merge",
                ):
                    case = WorkflowDraftPolicyTests(method)
                    case.workflows = {**self.workflows, "asan.yml": strengthened}
                    result = unittest.TestResult()
                    case.run(result)
                    self.assertTrue(result.wasSuccessful(), result.errors + result.failures)

    def test_asan_aggregate_rejects_removed_or_replaced_required_tests(self):
        workflow = self.workflows["asan.yml"]
        mutations = []
        for index, name in enumerate(ASAN_REQUIRED_TESTS):
            argument = f" --require-test {name}"
            self.assertIn(argument, workflow)
            mutations.extend((
                (f"removed-{name}", workflow.replace(argument, "", 1)),
                (f"replaced-{name}", workflow.replace(
                    argument, " --require-test AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes", 1)),
                (f"duplicated-instead-of-{name}", workflow.replace(
                    argument, f" --require-test {ASAN_REQUIRED_TESTS[(index + 1) % len(ASAN_REQUIRED_TESTS)]}", 1)),
            ))
        mutations.append(("removed-all", re.sub(r" --require-test [\w./-]+", "", workflow)))
        for mutation, mutated in mutations:
            with self.subTest(mutation=mutation):
                self.assertNotEqual(mutated, workflow)
                case = WorkflowDraftPolicyTests(
                    "test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence")
                case.workflows = {**self.workflows, "asan.yml": mutated}
                result = unittest.TestResult()
                case.run(result)
                self.assertEqual(len(result.errors), 0, result.errors)
                self.assertEqual(len(result.failures), 1, result.failures)
                self.assertIn("not less than or equal", result.failures[0][1])

    def test_asan_aggregate_rejects_invalid_inventory_and_coverage_commands(self):
        workflow = self.workflows["asan.yml"]
        inventory = next(line for line in workflow.splitlines(keepends=True)
                         if line.startswith("          python3 tools/ci/check_ctest_junit.py "
                                            "build/debug-asan/ctest.junit.xml --minimum-tests "))
        coverage = ('          python3 tools/ci/compare_ambient_receipts.py '
                    'asan=build/debug-asan/ctest.junit.xml \\\n'
                    '            --expect-labels asan --require-probe asan\n')
        for mutation, original, replacement in (
            ("missing-floor", inventory, inventory.replace(' --minimum-tests "$minimum_tests"', "")),
            ("invalid-floor", inventory, inventory.replace('--minimum-tests "$minimum_tests"', "--minimum-tests unknown")),
            ("ignored-failure", inventory, inventory.rstrip() + " || true\n"),
            ("partial-report", inventory, inventory.replace("build/debug-asan/", "build/asan-shards/shard-1/")),
            ("empty-required-test", inventory, inventory.rstrip() + " --require-test\n"),
            ("missing-coverage", coverage, ""),
            ("commented-coverage", coverage, coverage.replace("python3", "# python3", 1)),
            ("missing-probe", coverage, coverage.replace(" --require-probe asan", "")),
            ("missing-coverage-options", coverage,
             coverage.replace("            --expect-labels asan --require-probe asan\n", "")),
            ("coverage-before-floor", coverage, ""),
        ):
            with self.subTest(mutation=mutation):
                mutated = workflow.replace(original, replacement, 1)
                if mutation == "coverage-before-floor":
                    mutated = mutated.replace(inventory, coverage + inventory, 1)
                self.assertNotEqual(mutated, workflow)
                case = WorkflowDraftPolicyTests(
                    "test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence")
                case.workflows = {**self.workflows, "asan.yml": mutated}
                result = unittest.TestResult()
                case.run(result)
                self.assertEqual(len(result.errors), 0, result.errors)
                self.assertEqual(len(result.failures), 1, result.failures)

    def assert_asan_rerun_artifacts(self, rerun_shards):
        blocks = jobs(self.workflows["asan.yml"])
        upload = next(step for step in steps(blocks["asan-shard"])
                      if step.startswith("      - name: Upload shard JUnit\n"))
        download = next(step for step in steps(blocks["asan"])
                        if step.startswith("      - name: Download shard JUnit\n"))
        merge = next(step for step in steps(blocks["asan"])
                     if step.startswith("      - name: Merge shard JUnit and check inventory\n"))

        def render(text, attempt, shard=""):
            for key, item in (("github.sha", "a" * 40), ("github.run_id", "10"),
                              ("github.run_attempt", str(attempt)), ("matrix.shard", str(shard))):
                text = text.replace("${{ " + key + " }}", item)
            self.assertNotIn("${{", text)
            return text

        # Model the artifact store across attempts using the actual workflow names.
        artifacts = {}
        for attempt, producers in ((1, (1, 2, 3)), (2, rerun_shards)):
            for shard in producers:
                name = render(value(upload, "name", 10), attempt, shard)
                if name in artifacts:
                    self.assertEqual(value(upload, "overwrite", 10), "true")
                artifacts[name] = (attempt, shard)
        artifacts["asan-junit-foreign-10-shard-1"] = (1, 1)
        artifacts["asan-junit-" + "a" * 40 + "-9-shard-2"] = (1, 2)
        selected = {name: producer for name, producer in artifacts.items()
                    if fnmatch.fnmatchcase(name, render(value(download, "pattern", 10), 2))}
        self.assertEqual(len(selected), 3, selected)
        self.assertEqual(sorted(selected.values(), key=lambda item: item[1]),
                         [(2 if shard in rerun_shards else 1, shard) for shard in (1, 2, 3)])
        script = render(value(merge, "run", 8), 2)
        for name, (_, shard) in selected.items():
            self.assertIn(f'mv "{name}" "build/asan-shards/shard-{shard}"',
                          script.replace("$shard", str(shard)).replace("$source", name))
        prepare = script.split(ASAN_MERGE_COMMAND, 1)[0]
        self.assertNotIn("ctest_shards.py", prepare)
        for omitted in (None, 3):
            with self.subTest(omitted=omitted), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve(strict=True)
                for name, (attempt, shard) in selected.items():
                    if shard != omitted:
                        source = root / "build/asan-shards" / name
                        source.mkdir(parents=True)
                        (source / "producer").write_text(str(attempt), encoding="utf-8")
                result = run_bash_script(
                    prepare,
                    cwd=root, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                for _, (attempt, shard) in selected.items():
                    producer = root / f"build/asan-shards/shard-{shard}/producer"
                    self.assertEqual(producer.exists(), shard != omitted)
                    if shard != omitted:
                        self.assertEqual(producer.read_text(encoding="utf-8"), str(attempt))
        self.assertEqual(value(download, "if", 8), "needs.asan-shard.result == 'success'")

    def test_asan_aggregate_only_rerun_downloads_all_original_shards(self):
        self.assert_asan_rerun_artifacts(())

    def test_asan_single_shard_rerun_downloads_latest_and_unchanged_shards(self):
        self.assert_asan_rerun_artifacts((3,))

    def test_asan_rerun_evidence_retained_for_full_30_day_window(self):
        shard = jobs(self.workflows["asan.yml"])["asan-shard"]
        upload = next(step for step in steps(shard)
                      if step.startswith("      - name: Upload shard JUnit\n"))
        self.assertEqual(value(upload, "retention-days", 10), "30")
        # Keep the recovery artifact limited to reports/receipts, excluding
        # binaries, compiler caches and the separate bulky diagnostic bundle.
        self.assertEqual(value(upload, "path", 10).splitlines(), [
            "build/debug-asan/ctest.junit.xml",
            "build/debug-asan/ctest.inventory.json",
            "build/debug-asan/ctest.partition.json",
            "build/debug-asan/qualification-shard.json",
        ])

    def test_aggregate_status_conditions_use_valid_contexts(self):
        # jobs.<id>.if and jobs.<id>.steps.if permit status functions and needs;
        # env/with/run do not permit status functions (checked separately).
        for filename, workflow in self.workflows.items():
            for job_id, job in jobs(workflow).items():
                if "name: Reject invalid dependencies" not in job:
                    continue
                declared = set(value(job, "needs", 4).strip("[]").split(", "))
                for block, indent in [(job, 4)] + [(step, 8) for step in steps(job)]:
                    condition = value(block, "if", indent)
                    if condition is None:
                        continue
                    with self.subTest(workflow=filename, job=job_id, condition=condition):
                        self.assertLessEqual(set(re.findall(r"needs\.([\w-]+)\.", condition)), declared)
                        functions = set(re.findall(r"\b(\w+)\(", condition))
                        self.assertLessEqual(functions, {"always", "cancelled", "failure", "success"})
                        self.assertNotIn("steps.", value(job, "if", 4))

    def test_aggregates_cancel_at_each_phase_and_fail_invalid_dependencies(self):
        aggregates = [("asan-shard", jobs(self.workflows["asan.yml"])["asan"])]
        ci = jobs(self.workflows["ci.yml"])
        aggregates += [(producer, ci[producer + "-result"])
                       for producer in ("linux", "windows-msvc", "windows-ucrt64")]
        aggregates.append(("tidy-shard", ci["tidy"]))
        for producer, aggregate in aggregates:
            condition = value(aggregate, "if", 4).replace(READY, "True")
            refusal = next(step for step in steps(aggregate)
                           if step.startswith("      - name: Reject invalid dependencies\n"))
            self.assertIsNone(value(refusal, "continue-on-error", 8))
            for classification in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                for result in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                    dependencies = {"changes": classification, producer: result}
                    if producer == "asan-shard":
                        dependencies["fast-lane"] = "success"
                    invalid = classification != "success" or result not in ("success", "skipped")
                    for cancelled in (False, True):
                        with self.subTest(producer=producer, classification=classification,
                                          result=result, cancelled=cancelled):
                            admitted = evaluate_condition(condition, dependencies, cancelled=cancelled)
                            self.assertEqual(admitted, not cancelled or invalid)
                            # Kept-running jobs have not received job cancellation:
                            # cancelled() at step level is false, despite workflow cancellation.
                            for earlier_step_failed in (False, True):
                                refuses = evaluate_condition(value(refusal, "if", 8), dependencies,
                                                             failed=earlier_step_failed)
                                self.assertEqual(refuses, invalid)
                            if admitted and invalid:
                                process = run_bash_script(
                                    value(refusal, "run", 8), flags=("-e",),
                                    capture_output=True, text=True, timeout=10)
                                self.assertNotEqual(process.returncode, 0)
            # Re-evaluate admission after each actual step, including download,
            # validation and uploads. A completed early guard offers no protection.
            for result in ("success", "skipped"):
                dependencies = {"changes": "success", producer: result}
                if producer == "asan-shard":
                    dependencies["fast-lane"] = "success"
                self.assertTrue(evaluate_condition(condition, dependencies))
                for phase in steps(aggregate):
                    with self.subTest(producer=producer, phase=phase.splitlines()[0], result=result):
                        self.assertFalse(evaluate_condition(condition, dependencies, cancelled=True))

    def test_native_aggregates_preserve_route_checks(self):
        cases = [
            (result, cpp, "pull_request", "false", "false", "true", result == "success")
            for result in ("success", "failure", "cancelled", "skipped", "", "unknown")
            for cpp in ("true", "false", "")
        ]
        cases.extend([
            ("success", "true", "pull_request", "true", "false", "true", False),
            ("success", "true", "pull_request", "", "false", "true", False),
            ("success", "true", "pull_request", "unknown", "false", "true", False),
            ("success", "true", "push", "", "false", "true", True),
            ("success", "true", "workflow_dispatch", "", "true", "true", True),
            ("success", "false", "workflow_dispatch", "", "true", "true", False),
            ("skipped", "false", "push", "", "false", "false", True),
            ("success", "true", "push", "", "false", "false", False),
            ("", "false", "push", "", "false", "false", False),
            ("unknown", "false", "push", "", "false", "false", False),
        ])
        ci = jobs(self.workflows["ci.yml"])
        with tempfile.TemporaryDirectory() as directory:
            for job in ("linux-result", "windows-msvc-result", "windows-ucrt64-result"):
                aggregate_steps = steps(ci[job])
                refusal, gate = aggregate_steps[2:4]
                self.assertTrue(refusal.startswith("      - name: Reject invalid dependencies\n"))
                self.assertEqual(value(refusal, "if", 8), "${{ always() && (" + aggregate_invalid_expression(job.removesuffix("-result")) + ") }}")
                self.assertIsNone(value(refusal, "continue-on-error", 8))
                self.assertTrue(gate.startswith("      - name: Require expected producers and route\n"))
                self.assertIsNone(value(gate, "if", 8))
                self.assertIsNone(value(gate, "continue-on-error", 8))
                refusal_script, gate_script = value(refusal, "run", 8), value(gate, "run", 8)
                self.assertIsNotNone(refusal_script)
                self.assertIsNotNone(gate_script)
                # Only this grid may overlap, using case-local launch inputs.
                environment = tuple(os.environ.items())

                def route_cases():
                    index = 0
                    for classification in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                        for result, cpp, event, draft, qualification, enabled, accepted in cases:
                            script = 'python3() { "$POLICY_PYTHON" -I -B "$@"; }\n'
                            script += (refusal_script + "\n" if classification != "success" or result not in ("success", "skipped") else "") + gate_script
                            path = write_bash_script(Path(directory) / f"route-{job}-{index}.sh", script)
                            yield PolicyRouteCase(
                                (classification, result, cpp, event, draft, qualification, enabled), accepted,
                                (BASH, "--noprofile", "--norc", "-e", "-o", "pipefail", path),
                                str(ROOT),
                                tuple(dict(environment, POLICY_PYTHON=Path(sys.executable).as_posix(),
                                           CI_PATHS_RESULT=classification, CI_CANCELLED="false", SHARD_RESULT=result, NEEDS_CPP=cpp, CI_EVENT=event, CI_DRAFT=draft,
                                           CI_QUALIFICATION=qualification, CI_ENABLED=enabled,
                                           GITHUB_STEP_SUMMARY=(Path(directory) / f"summary-{job}-{index}").as_posix()).items()),
                            )
                            index += 1

                with closing(policy_route_results(route_cases(), getattr(self, "route_case_workers", 1))) as results:
                    for case, future in results:
                        classification, result, cpp, event, draft, qualification, enabled = case.parameters
                        accepted = case.accepted
                        with self.subTest(job=job, classification=classification, result=result, cpp=cpp,
                                          event=event, draft=draft, qualification=qualification, enabled=enabled):
                            process = future.result()
                            self.assertEqual(process.returncode == 0, accepted and classification == "success",
                                             process.stdout + process.stderr)
                            if classification != "success" or result not in ("success", "skipped"):
                                self.assertIn("Aggregate dependencies did not pass", process.stderr)

    def test_asan_aggregate_accepts_only_success_or_the_original_path_skip(self):
        aggregate = jobs(self.workflows["asan.yml"])["asan"]
        step = next(step for step in steps(aggregate) if step.splitlines()[0].removeprefix("      - name: ") == "Require all ASan shards to pass")
        self.assertEqual(value(step, "ASAN_SHARD_RESULT", 10), "${{ needs.asan-shard.result }}")
        self.assertEqual(value(step, "CI_PATHS_RESULT", 10), "${{ needs.changes.result }}")
        self.assertEqual(value(step, "NEEDS_CPP", 10), "${{ needs.changes.outputs.needs_cpp }}")
        self.assertIsNone(value(step, "ASAN_CANCELLED", 10))
        self.assertEqual(value(step, "if", 8), "always() && (" + READY + ")")
        self.assertIsNone(value(step, "continue-on-error", 8))
        self.check_asan_aggregate_results(value(step, "run", 8))

    def check_asan_aggregate_results(self, script):
        aggregate = jobs(self.workflows["asan.yml"])["asan"]
        refusal = next(step for step in steps(aggregate)
                       if step.startswith("      - name: Reject invalid dependencies\n"))
        self.assertEqual(value(refusal, "if", 8), "${{ always() && (" + aggregate_invalid_expression("asan-shard") + ") }}")
        self.assertIsNone(value(refusal, "continue-on-error", 8))
        refusal_script = value(refusal, "run", 8)
        self.assertIsNotNone(refusal_script)
        with tempfile.TemporaryDirectory() as directory:
            for classification in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                for cancelled in (False, True):
                    for cpp in ("true", "false", ""):
                        for result in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                            with self.subTest(cancelled=cancelled, classification=classification,
                                              cpp=cpp, result=result):
                                dependencies = {"changes": classification, "asan-shard": result,
                                                "fast-lane": "success"}
                                admitted = evaluate_condition(value(aggregate, "if", 4), dependencies,
                                                              cancelled=cancelled)
                                if not admitted:
                                    self.assertTrue(cancelled)
                                    self.assertEqual(classification, "success")
                                    self.assertIn(result, ("success", "skipped"))
                                    continue
                                refuses = evaluate_condition(value(refusal, "if", 8), dependencies, failed=False)
                                process = run_bash_script(
                                    (refusal_script + "\n" if refuses else "") + script,
                                    env=dict(os.environ, NEEDS_CPP=cpp, ASAN_SHARD_RESULT=result,
                                             CI_PATHS_RESULT=classification,
                                             GITHUB_STEP_SUMMARY=(Path(directory) / "summary").as_posix()),
                                    capture_output=True, text=True, timeout=10,
                                )
                                accepted = not cancelled and classification == "success" and (
                                    result == "success" or (classification == "success" and
                                                            cpp == "false" and result == "skipped"))
                                self.assertEqual(process.returncode == 0, accepted, process.stdout + process.stderr)
                                if cancelled:
                                    self.assertIn("Aggregate dependencies did not pass", process.stderr)

    def test_native_aggregates_refuse_invalid_dependencies_before_the_route_gate(self):
        ci = jobs(self.workflows["ci.yml"])
        for job in ("linux-result", "windows-msvc-result", "windows-ucrt64-result"):
            with self.subTest(job=job):
                aggregate = ci[job]
                refusal = next(step for step in steps(aggregate)
                               if step.startswith("      - name: Reject invalid dependencies\n"))
                gate = next(step for step in steps(aggregate)
                            if step.startswith("      - name: Require expected producers and route\n"))
                self.assertEqual(value(refusal, "if", 8), "${{ always() && (" + aggregate_invalid_expression(job.removesuffix("-result")) + ") }}")
                self.assertIsNone(value(refusal, "continue-on-error", 8))
                self.assertIsNone(value(gate, "if", 8))
                self.assertIsNone(value(gate, "continue-on-error", 8))
                self.assertLess(aggregate.index(refusal), aggregate.index(gate))
                result = run_bash_script(
                    value(refusal, "run", 8), capture_output=True, text=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Aggregate dependencies did not pass", result.stderr)

    def test_ucrt64_configure_changes_only_debug_information_level(self):
        job = jobs(self.workflows["ci.yml"])["windows-ucrt64"]
        configure, = [step for step in steps(job) if step.startswith("      - name: Configure\n")]
        self.assertEqual(value(configure, "run", 8),
                         'cmake --preset release -DCMAKE_C_COMPILER_LAUNCHER=ccache '
                         '"-DPython3_EXECUTABLE=$CI_PYTHON_EXECUTABLE" '
                         '-DCMAKE_CXX_COMPILER_LAUNCHER=ccache '
                         '-DCMAKE_C_FLAGS=-g1 -DCMAKE_CXX_FLAGS=-g1')

    def test_ucrt_cache_guards_keep_saves_on_trusted_pushes_and_bypass_captures(self):
        job = jobs(self.workflows["ci.yml"])["windows-ucrt64"]
        named = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(job)}
        trusted_push = (
            "github.event_name == 'push' && "
            "(github.ref == 'refs/heads/main' || github.ref == 'refs/heads/devel')"
        )
        capture = "github.event_name == 'workflow_dispatch' && inputs.qualification_capture"
        self.assertEqual(value(named["Set up MSYS2 UCRT64"], "cache", 10),
                         "${{ matrix.shard == 1 && " + trusted_push + " }}")
        disable = named["Disable compiler cache for qualification captures"]
        self.assertEqual(value(disable, "if", 8), capture)
        self.assertIn('CCACHE_DISABLE=1', value(disable, "run", 8))
        self.assertEqual(value(named["Restore compiler cache"], "if", 8),
                         "${{ !(" + capture + ") }}")
        for name in ("Clean compiler cache before saving", "Save compiler cache"):
            with self.subTest(step=name):
                self.assertEqual(
                    value(named[name], "if", 8),
                    "${{ matrix.shard == 1 && always() && " + trusted_push + " && !(" + capture + ") && "
                    "steps.compiler-cache.outcome == 'success' && "
                    "steps.compiler-cache.outputs.cache-hit != 'true' }}",
                )

    def test_concurrency_keeps_pr_cancellation_and_batches_only_devel_pushes(self):
        group = (
            "${{ github.event_name == 'workflow_dispatch' && "
            "format('{0}-manual-{1}', github.workflow, github.run_id) || "
            "github.event_name == 'push' && github.ref == 'refs/heads/main' && "
            "format('{0}-{1}-{2}-{3}', github.workflow, github.ref, github.sha, github.run_id) || "
            "github.event_name == 'push' && github.ref == 'refs/heads/devel' && "
            "format('{0}-{1}-push', github.workflow, github.ref) || "
            "format('{0}-{1}', github.workflow, github.ref) }}"
        )
        cancellation = "${{ github.event_name != 'push' && github.event_name != 'workflow_dispatch' }}"
        workflows = dict(self.workflows)
        workflows["ucrt-attribution.yml"] = (
            ROOT / ".github/workflows/ucrt-attribution.yml"
        ).read_text(encoding="utf-8")
        for filename, workflow in workflows.items():
            with self.subTest(workflow=filename):
                expected_group = group
                expected_cancellation = cancellation
                if filename == "ci.yml":
                    expected_group = group.replace("${{ ", "${{ (github.event_name == 'pull_request' && "
                                                   "github.event.action == 'edited' && !github.event.changes.base) && "
                                                   "format('{0}-metadata-{1}', github.workflow, github.run_id) || ", 1)
                    expected_cancellation = cancellation.replace(
                        "${{ ", "${{ !(github.event_name == 'pull_request' && github.event.action == 'edited' && "
                        "!github.event.changes.base) && ", 1)
                self.assertEqual(value(workflow, "group", 2), expected_group)
                self.assertEqual(value(workflow, "cancel-in-progress", 2), expected_cancellation)

    def test_policy_suite_is_part_of_cheap_inventory_check(self):
        inventory = jobs(self.workflows["ci.yml"])["public-tree"]
        self.assertIn("python3 tools/ci/test_workflow_policy.py", inventory)

    def test_freeze_audit_comment_is_identical_in_every_workflow(self):
        expected = {
            "ci.yml", "asan.yml", "performance.yml", "docs.yml",
            "game-assets.yml", "blender-authoring.yml", "ucrt-attribution.yml",
        }
        comments = {}
        for path in (ROOT / ".github/workflows").glob("*.yml"):
            workflow = path.read_text(encoding="utf-8")
            matches = re.findall(
                r"^  # The campaign freeze audit[^\n]*\n(?:  #[^\n]*\n)*?"
                r"(?=  # Main pushes)", workflow, re.MULTILINE,
            )
            with self.subTest(workflow=path.name):
                if path.name in expected:
                    self.assertEqual(len(matches), 1)
                if matches:
                    self.assertEqual(len(matches), 1)
                    comments[path.name] = matches[0]
        self.assertEqual(set(comments), expected)
        self.assertEqual(len(set(comments.values())), 1, comments)


class WorkflowRequiredAggregatePolicyTests(WorkflowPolicyFixture, unittest.TestCase):
    def test_required_aggregates_cancel_running_jobs_but_fail_cancelled_dependencies(self):
        # At job level cancelled() reads workflow cancellation. At step level
        # it reads the current job status, which can still be successful while
        # the server re-evaluates the job's condition during artifact download.
        # Read both conditions from the workflow and never treat a job skip as
        # an acceptable substitute for an explicit dependency refusal.
        aggregates = (("asan.yml", "asan", "asan-shard"),
                      ("ci.yml", "tidy", "tidy-shard"),
                      ("ci.yml", "linux-result", "linux"),
                      ("ci.yml", "windows-msvc-result", "windows-msvc"),
                      ("ci.yml", "windows-ucrt64-result", "windows-ucrt64"))
        self.check_required_aggregates(aggregates)

    def check_required_aggregates(self, aggregates):
        results = ("success", "failure", "cancelled", "skipped", "", "unknown")
        for filename, job_id, shard_id in aggregates:
            aggregate = jobs(self.workflows[filename])[job_id]
            self.assertEqual(value(aggregate, "needs", 4),
                             "[changes, fast-lane, asan-shard]" if shard_id == "asan-shard"
                             else f"[changes, {shard_id}]")
            self.assertIsNone(value(aggregate, "continue-on-error", 4))
            refusal = next(step for step in steps(aggregate)
                           if step.startswith("      - name: Reject invalid dependencies\n"))
            self.assertIsNone(value(refusal, "continue-on-error", 8))
            # This must be an unconditional failure whenever the refusal runs.
            process = run_bash_script(
                value(refusal, "run", 8), capture_output=True, text=True, timeout=10,
            )
            self.assertNotEqual(process.returncode, 0, process.stdout + process.stderr)
            for cancelled in (False, True):
                for changes in results:
                    for shard in results:
                        with self.subTest(job=job_id, cancelled=cancelled, changes=changes, shard=shard):
                            dependency_invalid = changes != "success" or shard not in ("success", "skipped")
                            # Ready PRs and pushes retain their existing routing.
                            for event in ("pull_request", "push", "workflow_dispatch"):
                                self.assertEqual(
                                    self.aggregate_condition(value(aggregate, "if", 4), shard_id,
                                                             cancelled, changes, shard, event),
                                    not cancelled or dependency_invalid,
                                )
                            # Even if the current job is still successful, any
                            # invalid dependency must run the failure step.
                            for job_cancelled in (False, True):
                                self.assertEqual(
                                    self.aggregate_condition(value(refusal, "if", 8), shard_id,
                                                             job_cancelled, changes, shard, step=True),
                                    dependency_invalid,
                                )

    @staticmethod
    def aggregate_condition(expression, shard_id, cancelled, changes, shard, event="pull_request", step=False):
        expression = render_edit_context(expression).removeprefix("${{ ").removesuffix(" }}")
        status = {"always": True, "cancelled": cancelled,
                  "success": not cancelled and (step or changes == shard == "success"),
                  "failure": not step and "failure" in (changes, shard)}
        rendered, count = re.subn(r"\b(always|cancelled|success|failure)\(\)",
                                 lambda match: str(status[match[1]]), expression)
        for key, item in (("needs.changes.result", changes), (f"needs.{shard_id}.result", shard),
                          ("needs.fast-lane.result", "success"),
                          ("github.event_name", event), ("github.event.pull_request.draft", False)):
            rendered = rendered.replace(key, repr(item))
        rendered = re.sub(r"!(?!=)", "not ", rendered.replace("false", "False"))
        rendered = rendered.replace("&&", " and ").replace("||", " or ")
        return bool((count > 0 or status["success"]) and eval(rendered, {"__builtins__": {}}, {}))

    def test_required_aggregate_graph_rejects_cancellation_regressions(self):
        method = "test_required_aggregates_cancel_running_jobs_but_fail_cancelled_dependencies"
        for filename, job_id, shard_id in (("asan.yml", "asan", "asan-shard"),
                                           ("ci.yml", "tidy", "tidy-shard"),
                                           ("ci.yml", "linux-result", "linux"),
                                           ("ci.yml", "windows-msvc-result", "windows-msvc"),
                                           ("ci.yml", "windows-ucrt64-result", "windows-ucrt64")):
            workflow = self.workflows[filename]
            aggregate = jobs(workflow)[job_id]
            job_if = value(aggregate, "if", 4)
            refusal = next(step for step in steps(aggregate)
                           if step.startswith("      - name: Reject invalid dependencies\n"))
            for mutation, original, replacement in (
                ("uncancellable-job", job_if, "${{ always() }}"),
                ("skipped-cancelled-dependencies", job_if, "${{ !cancelled() }}"),
                ("implicit-success", job_if, "${{ github.event_name == 'pull_request' }}"),
                ("missed-classifier-cancellation", job_if,
                 "${{ !cancelled() || needs." + shard_id + ".result == 'cancelled' }}"),
                ("missed-shard-cancellation", job_if,
                 "${{ !cancelled() || needs.changes.result == 'cancelled' }}"),
                ("job-status-refusal", value(refusal, "if", 8), "${{ cancelled() }}"),
                ("implicit-success-refusal", value(refusal, "if", 8),
                 "${{ needs.changes.result == 'cancelled' || needs." + shard_id + ".result == 'cancelled' }}"),
                ("successful-refusal", "          exit 1", "          exit 0"),
            ):
                with self.subTest(job=job_id, mutation=mutation):
                    # Apply only inside this job, not the classifier or shards.
                    mutated = aggregate.replace(original, replacement)
                    self.assertNotEqual(mutated, aggregate)
                    case = type(self)(method)
                    case.workflows = {**self.workflows, filename: workflow.replace(aggregate, mutated, 1)}
                    result = unittest.TestResult()
                    unittest.FunctionTestCase(
                        lambda: case.check_required_aggregates(((filename, job_id, shard_id),))
                    ).run(result)
                    self.assertEqual(result.errors, [], result.errors)
                    self.assertTrue(result.failures)

    def test_required_aggregate_verdicts_run_after_failure_and_cancellation(self):
        results = ("success", "failure", "cancelled", "skipped", "", "unknown")
        aggregates = []
        # Validate both jobs before running either grid. Mutation checks must
        # reject an invalid tidy condition without replaying the valid ASan grid.
        for filename, job_id, shard_id, label in (
            ("asan.yml", "asan", "asan-shard", "ASan"),
            ("ci.yml", "tidy", "tidy-shard", "clang-tidy"),
        ):
            aggregate = jobs(self.workflows[filename])[job_id]
            ordered_steps = steps(aggregate)
            self.assertIn("      - name: Reject invalid dependencies\n", aggregate)
            refusal = next(step for step in ordered_steps
                           if step.startswith("      - name: Reject invalid dependencies\n"))
            verdict = next(step for step in ordered_steps
                           if step.startswith(f"      - name: Require all {label} shards to pass\n"))
            # Explicit always() also runs after an earlier step fails or is
            # cancelled. The job condition stops workflow cancellation when
            # no dependency needs the explicit refusal.
            expected = "always() && (" + READY + ")" if job_id == "asan" else "always()"
            self.assertEqual(value(verdict, "if", 8), expected)
            self.assertIsNone(value(verdict, "continue-on-error", 8))
            self.assertLess(ordered_steps.index(refusal), ordered_steps.index(verdict))
            result_key = "ASAN_SHARD_RESULT" if job_id == "asan" else "TIDY_SHARD_RESULT"
            for key, expression in (
                (result_key, "${{ needs." + shard_id + ".result }}"),
                ("CI_PATHS_RESULT", "${{ needs.changes.result }}"),
                ("NEEDS_CPP", "${{ needs.changes.outputs.needs_cpp }}"),
            ):
                self.assertEqual(value(verdict, key, 10), expression)
            aggregates.append((job_id, shard_id, aggregate, result_key, (refusal, verdict)))

        with tempfile.TemporaryDirectory() as directory:
            for job_id, shard_id, aggregate, result_key, checked_steps in aggregates:
                condition = value(aggregate, "if", 4)
                conditions = [value(step, "if", 8) for step in checked_steps]
                scripts = [value(step, "run", 8) for step in checked_steps]
                # At most 144 input triples per job, each with two steps. Keep
                # exact scripts (including exit/errexit) in isolated subshells,
                # but pay for Bash interpreter startup only once per job.
                driver = ["set +e"]
                observations = {}
                cases = []
                for classification in results:
                    for shard in results:
                        for cpp in ("true", "false", "", "unknown"):
                            for cancelled in (False, True):
                                with self.subTest(job=job_id, classification=classification,
                                                  shard=shard, cpp=cpp, cancelled=cancelled):
                                    scheduled = self.aggregate_condition(condition, shard_id, cancelled,
                                                                         classification, shard)
                                    if not scheduled:
                                        self.assertTrue(cancelled)
                                        self.assertNotIn("cancelled", (classification, shard))
                                        continue
                                    selected = tuple(index for index, step_condition in enumerate(conditions)
                                                     if self.aggregate_condition(step_condition, shard_id, cancelled,
                                                                                 classification, shard, step=True))
                                    # Cancellation changes scheduling, not shell
                                    # inputs. Reuse only an identical scheduled
                                    # step sequence with identical environment.
                                    key = (classification, shard, cpp, selected)
                                    if key not in observations:
                                        summary = Path(directory) / f"summary-{job_id}-{len(observations)}"
                                        observations[key] = summary
                                        environment = {result_key: shard, "CI_PATHS_RESULT": classification,
                                                       "NEEDS_CPP": cpp, "GITHUB_STEP_SUMMARY": summary.as_posix()}
                                        driver.append("export " + " ".join(f"{name}={shlex.quote(item)}"
                                                                          for name, item in environment.items()))
                                        for index in selected:
                                            driver.append("(\nset -e -o pipefail\n" + scripts[index]
                                                          + "\n) >/dev/null 2>&1\nprintf '%s\\n' \"$?\"")
                                    cases.append((classification, shard, cpp, cancelled, key))
                # Use a file, rather than -c, to stay below Windows argv limits.
                path = Path(directory) / f"{job_id}.sh"
                write_bash_script(path, "\n".join(driver) + "\n")
                process = subprocess.run([BASH, "--noprofile", "--norc", path.as_posix()],
                                         cwd=directory, capture_output=True, text=True, timeout=10)
                self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                observed = process.stdout.splitlines()
                self.assertEqual(len(observed), sum(len(key[-1]) for key in observations))
                outcomes_by_key = {}
                offset = 0
                for key in observations:
                    count = len(key[-1])
                    outcomes_by_key[key] = [int(code) for code in observed[offset:offset + count]]
                    offset += count
                for classification, shard, cpp, cancelled, key in cases:
                    with self.subTest(job=job_id, classification=classification,
                                      shard=shard, cpp=cpp, cancelled=cancelled):
                        summary = observations[key]
                        outcomes = outcomes_by_key[key]
                        self.assertTrue(outcomes, "An aggregate cannot pass without validation")
                        routed = classification == "success" and cpp == "false" and shard == "skipped"
                        accepted = not cancelled and classification == "success" and (
                            shard == "success" or routed)
                        self.assertEqual(all(code == 0 for code in outcomes), accepted)
                        self.assertEqual(summary.exists(), routed)
                        if routed:
                            self.assertIn("route: skipped", summary.read_text(encoding="utf-8"))

    def test_required_aggregate_verdict_policy_rejects_weakened_conditions(self):
        method = "test_required_aggregate_verdicts_run_after_failure_and_cancellation"
        for filename, job_id in (("asan.yml", "asan"), ("ci.yml", "tidy")):
            condition = "        if: " + ("always() && (" + READY + ")" if job_id == "asan" else "always()") + "\n"
            workflow = self.workflows[filename]
            aggregate = jobs(workflow)[job_id]
            verdict = next(step for step in steps(aggregate)
                           if step.startswith("      - name: Require all "))
            for mutation, original, replacement in (
                ("implicit-success", condition, ""),
                ("success-only", condition, "        if: success()\n"),
                ("cancellation-skip", condition, "        if: ${{ !cancelled() }}\n"),
                ("always-false", condition, "        if: always() && false\n"),
                ("ignored-verdict", "        env:\n", "        continue-on-error: true\n        env:\n"),
            ):
                with self.subTest(job=job_id, mutation=mutation):
                    mutated = verdict.replace(original, replacement, 1)
                    self.assertNotEqual(mutated, verdict)
                    case = type(self)(method)
                    case.workflows = {
                        **self.workflows,
                        filename: workflow.replace(aggregate, aggregate.replace(verdict, mutated, 1), 1),
                    }
                    result = unittest.TestResult()
                    with patch.object(subprocess, "run", wraps=subprocess.run) as run:
                        case.run(result)
                    self.assertEqual(run.call_count, 0)
                    self.assertEqual(result.errors, [], result.errors)
                    self.assertTrue(result.failures)

    def test_required_aggregate_verdict_grid_keeps_bounded_launches_and_step_isolation(self):
        method = "test_required_aggregate_verdicts_run_after_failure_and_cancellation"
        case = type(self)(method)
        case.workflows = dict(self.workflows)
        for filename, job_id in (("asan.yml", "asan"), ("ci.yml", "tidy")):
            workflow = case.workflows[filename]
            aggregate = jobs(workflow)[job_id]
            mutated = aggregate
            for step in steps(aggregate):
                if not step.startswith(("      - name: Reject invalid dependencies\n",
                                        "      - name: Require all ")):
                    continue
                script = value(step, "run", 8)
                # Each real script must get a fresh shell scope, and exit must
                # stop that step without preventing the next step from running.
                isolated = ('if [[ -n "${POLICY_STEP_SCOPE-}" ]]; then exit 99; fi\n'
                            'export POLICY_STEP_SCOPE=occupied\n' + script + '\nexit 0')
                if step.startswith("      - name: Reject invalid dependencies\n"):
                    # Errexit still aborts, and return outside a function must
                    # remain an error just as in a standalone workflow script.
                    failure = "false" if job_id == "asan" else "return 0"
                    isolated = isolated.replace("exit 1", failure + "\nexit 0")
                mutated = mutated.replace(step, step.replace(
                    "          " + script.replace("\n", "\n          "),
                    "          " + isolated.replace("\n", "\n          "), 1), 1)
            self.assertNotEqual(mutated, aggregate)
            case.workflows[filename] = workflow.replace(aggregate, mutated, 1)
        result = unittest.TestResult()
        with patch.dict(os.environ), patch.object(subprocess, "run", wraps=subprocess.run) as run:
            os.environ.pop("POLICY_STEP_SCOPE", None)
            case.run(result)
        self.assertEqual(result.errors, [], result.errors)
        self.assertEqual(result.failures, [], result.failures)
        self.assertEqual(run.call_count, 2)

        for response in (
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 0, "0\n", ""),
            subprocess.CompletedProcess([], 1, "", "driver failed"),
            subprocess.TimeoutExpired([BASH], 10, output=b"0\n"),
        ):
            with self.subTest(response=response):
                case = type(self)(method)
                case.workflows = self.workflows
                result = unittest.TestResult()
                options = ({"side_effect": response} if isinstance(response, Exception)
                           else {"return_value": response})
                with patch.object(subprocess, "run", **options):
                    case.run(result)
                self.assertTrue(result.failures or result.errors)


class WorkflowUcrtClassifierPolicyTests(WorkflowPolicyFixture, unittest.TestCase):
    def test_ucrt_route_seeds_devel_and_preserves_other_event_and_path_guards(self):
        ci = jobs(self.workflows["ci.yml"])
        job = ci["windows-ucrt64"]
        self.assertEqual(value(job, "needs", 4), "changes")
        gate = next(step for step in steps(ci["windows-ucrt64-result"])
                    if step.startswith("      - name: Require expected producers and route\n"))
        self.assertEqual(value(gate, "CI_ENABLED", 10),
                         "${{ (" + UCRT_CLASSIFICATION + ") && (" + UCRT_ROUTE + ") }}")
        expression = value(job, "if", 4).removeprefix("${{ ").removesuffix(" }}")
        enabled = value(gate, "CI_ENABLED", 10).removeprefix("${{ ").removesuffix(" }}")
        # Fixed grid: 11,520 inputs; expressions and the gate run without children.
        for event, branch, draft, cpp, cancelled, capture, label, paths in product(
                ("pull_request", "push", "workflow_dispatch", "schedule"),
                ("main", "devel", "feature"), (True, False, None),
                ("true", "false", "", "unknown"), (False, True), (False, True),
                ("", "windows-proof", "WINDOWS-PROOF", "windows-proof-extra"),
                ("true", "false", "", "unknown", "TRUE")):
            eligible = (
                (event == "push" and branch in ("main", "devel"))
                or event == "workflow_dispatch"
                or (event == "pull_request" and
                    (branch == "main" or label.lower() == "windows-proof" or paths.lower() == "true"))
            )
            native = cpp != "false" or (event == "pull_request" and branch == "main")
            expected = not cancelled and native and (event != "pull_request" or draft is False) and eligible
            def evaluate(condition):
                actual = render_edit_context(condition).replace("!cancelled()", repr(not cancelled)).replace(
                    "contains(github.event.pull_request.labels.*.name, 'windows-proof')",
                    repr(label.lower() == "windows-proof"))
                for key, replacement in (
                        ("github.event_name", event), ("github.ref_name", branch),
                        ("github.base_ref", branch),
                        ("github.ref", f"refs/heads/{branch}"),
                        ("github.event.pull_request.draft", draft),
                        ("needs.changes.outputs.needs_cpp", cpp),
                        ("needs.changes.outputs.needs_ucrt64", paths.lower()),
                        ("inputs.qualification_capture", capture)):
                    actual = actual.replace(key, repr(replacement))
                return eval(actual.replace("== false", "== False").replace("&&", "and").replace("||", "or"),
                            {"__builtins__": {}})
            with self.subTest(event=event, branch=branch, draft=draft, cpp=cpp,
                              cancelled=cancelled, capture=capture, label=label, paths=paths):
                self.assertEqual(evaluate(expression), expected)
                enabled_value = str(evaluate(enabled)).lower()
                self.assertEqual(enabled_value, str(native and eligible).lower())
                for classification, result in product(("success", "failure", "", "unknown"),
                                                      ("success", "skipped", "failure", "cancelled", "", "unknown")):
                    accepted = (classification == "success" and not cancelled
                                and (event != "pull_request" or draft is False)
                                and result == ("success" if native and eligible else "skipped"))
                    arguments = (result, "true", str(cancelled).lower(), event, str(draft).lower(),
                                 str(event == "workflow_dispatch" and capture).lower(), enabled_value,
                                 classification)
                    if accepted:
                        self.assertEqual(shards.gate(*arguments),
                                         "skipped" if enabled_value == "false" else
                                         "full" if event == "workflow_dispatch" and capture else "sharded")
                    else:
                        with self.assertRaises(ValueError, msg=repr(arguments)):
                            shards.gate(*arguments)

    def test_windows_docs_skip_precedes_checkout_and_keeps_unknown_classification_native(self):
        ci = jobs(self.workflows["ci.yml"])
        for job_id in ("windows-msvc", "windows-ucrt64"):
            job = ci[job_id]
            expression = value(job, "if", 4)
            self.assertIn("needs.changes.outputs.needs_cpp != 'false'", expression)
            self.assertLess(job.index("    if:"), job.index("actions/checkout@"))
            # MSVC also carries the standalone Python contracts, so a Python-only change keeps it.
            python_runs = job_id == "windows-msvc"
            for event, action, base_changed in product(
                    ("pull_request", "push", "workflow_dispatch"),
                    ("synchronize", "edited"), (False, True)):
                for cpp in ("false", "true", "", "unknown"):
                    for python in ("false", "true", ""):
                        for draft in (False, True, None):
                            for cancelled in (False, True):
                                condition = render_edit_context(
                                    expression, action, {"base": {}} if base_changed else {}).replace(
                                    "github.base_ref", repr("main")).replace(
                                    "github.ref", repr("refs/heads/main")).replace(
                                    "contains(github.event.pull_request.labels.*.name, 'windows-proof')", "False").replace(
                                    "needs.changes.outputs.needs_ucrt64", repr("false")).replace(
                                    "github.event_name", repr(event)).replace(
                                    "github.event.pull_request.draft", repr(draft)).replace(
                                    "needs.changes.outputs.needs_cpp", repr(cpp)).replace(
                                    "needs.changes.outputs.needs_python", repr(python)).replace("== false", "== False")
                                native = (cpp != "false" or (python_runs and python != "false")
                                          or (job_id == "windows-ucrt64" and event == "pull_request"))
                                expected = (not cancelled and native and (event != "pull_request" or draft is False)
                                            and (event != "pull_request" or action != "edited" or base_changed))
                                with self.subTest(job=job_id, event=event, cpp=cpp, python=python, draft=draft,
                                                  cancelled=cancelled, action=action, base_changed=base_changed):
                                    self.assertEqual(evaluate_condition(condition, {"changes": "success"}, cancelled=cancelled), expected)
            # Consumers require classification directly or the successful classified producer.
            aggregate = ci[job_id + "-result"]
            for step in steps(aggregate):
                if any(marker in step for marker in ("actions/download-artifact@", "ctest_shards.py merge", "actions/upload-artifact@")):
                    if job_id == "windows-ucrt64":
                        self.assertEqual(value(step, "if", 8), "needs.windows-ucrt64.result == 'success'")
                    else:
                        self.assertIn("needs.changes.outputs.needs_cpp != 'false'", value(step, "if", 8))

    def test_windows_intentional_skip_gate_uses_real_workflow_inputs_and_refuses_uncertainty(self):
        ci = jobs(self.workflows["ci.yml"])
        with tempfile.TemporaryDirectory() as directory:
            for job_id in ("windows-msvc", "windows-ucrt64"):
                gate = next(step for step in steps(ci[job_id + "-result"])
                            if "name: Require expected producers and route" in step)
                for index, (base, (cpp, python, classification, result)) in enumerate(product(("main", "devel"), (
                        ("false", "false", "success", "skipped"),
                        ("false", "false", "failure", "skipped"),
                        ("false", "false", "", "skipped"),
                        ("", "false", "success", "skipped"),
                        ("unknown", "false", "success", "skipped"),
                        ("true", "false", "success", "skipped"),
                        ("false", "false", "success", "failure"),
                        # MSVC keeps running the Python contracts on a Python-only change.
                        ("false", "true", "success", "success"),
                        ("false", "true", "success", "skipped")))):
                    native = (cpp != "false" or (python != "false" if job_id == "windows-msvc" else base == "main"))
                    accepted = classification == "success" and result == ("success" if native else "skipped")
                    enabled = value(gate, "CI_ENABLED", 10).replace(
                        "github.base_ref", repr(base)).replace(
                        "github.ref", repr("refs/heads/main")).replace(
                        "contains(github.event.pull_request.labels.*.name, 'windows-proof')", "True").replace(
                        "needs.changes.outputs.needs_ucrt64", repr("false")).replace(
                        "needs.changes.outputs.needs_cpp", repr(cpp)).replace(
                        "needs.changes.outputs.needs_python", repr(python)).replace("github.event_name", repr("pull_request"))
                    enabled = str(evaluate_condition(enabled, {})).lower()
                    needs_cpp = value(gate, "NEEDS_CPP", 10).replace(
                        "${{ needs.changes.outputs.needs_cpp }}", cpp).strip('"')
                    script = 'python3() { "$POLICY_PYTHON" -I -B "$@"; }\n' + value(gate, "run", 8)
                    # Bytes on stdin avoid Windows quote and CRLF conversion.
                    process = subprocess.run([BASH, "--noprofile", "--norc", "-e"], input=script.encode("utf-8"),
                                             cwd=ROOT, env=dict(os.environ,
                                                 POLICY_PYTHON=Path(sys.executable).as_posix(),
                                                 CI_PATHS_RESULT=classification, SHARD_RESULT=result, NEEDS_CPP=needs_cpp,
                                                 CI_CANCELLED="false", CI_EVENT="pull_request", CI_DRAFT="false",
                                                 CI_QUALIFICATION="false", CI_ENABLED=enabled,
                                                 GITHUB_STEP_SUMMARY=(Path(directory) / f"summary-{job_id}-{index}").as_posix()),
                                             capture_output=True, timeout=10)
                    with self.subTest(job=job_id, base=base, cpp=cpp, python=python, classification=classification, result=result):
                        self.assertEqual(process.returncode == 0, accepted, process.stdout + process.stderr)
                        if accepted and result == "skipped":
                            self.assertIn(b"skipped", process.stdout)

    def test_windows_standalone_contract_step_runs_once_on_shard_one_and_full_capture(self):
        ci = jobs(self.workflows["ci.yml"])
        step = next(step for step in steps(ci["windows-msvc"])
                    if "name: Run standalone Python contracts" in step)
        self.assertEqual(value(step, "run", 8),
                         '& "$env:CI_PYTHON_EXECUTABLE" -B tools/ci/ci_paths.py python-tests')
        for shard in (0, 1, 2):
            for cpp, python in (("true", "true"), ("true", "false"), ("", "")):
                condition = value(step, "if", 8).replace("matrix.shard", repr(shard)).replace(
                    "needs.changes.outputs.needs_cpp", repr(cpp)).replace("needs.changes.outputs.needs_python", repr(python))
                with self.subTest(shard=shard, cpp=cpp, python=python):
                    self.assertEqual(evaluate_condition(condition, {}), python != "false" and shard != 2)
        linux_step = next(step for step in steps(ci["linux"]) if "name: Run standalone Python contracts" in step)
        self.assertEqual(value(linux_step, "run", 8), "python3 -B tools/ci/ci_paths.py python-tests")

    def test_ucrt64_path_classifier_uses_merge_base_and_fails_open(self):
        changes = jobs(self.workflows["ci.yml"])["changes"]
        self.assertEqual(value(changes, "needs_ucrt64", 6), "${{ steps.ucrt64.outputs.needs_ucrt64 }}")
        step, = [step for step in steps(changes) if "name: Classify UCRT64 paths\n" in step]
        self.assertEqual(value(step, "id", 8), "ucrt64")
        for key, expression in (("CI_EVENT", "github.event_name"),
                                ("CI_BASE_SHA", "github.event.pull_request.base.sha"),
                                ("CI_HEAD_SHA", "github.event.pull_request.head.sha")):
            self.assertEqual(value(step, key, 10), "${{ " + expression + " }}")
        script = value(step, "run", 8)
        paths = (
            (".github/workflows/ci.yml", b"changed\n", True),
            ("cmake/dependencies.cmake", b"changed\n", True),
            ("CMakeLists.txt", b"changed\n", True), ("CMakePresets.json", b"changed\n", True),
            ("vendor/CMakeLists.txt", b"changed\n", True),
            ("vendor/fastnoise/README.md", b"changed\n", True),
            ("src/luminumbra_common/CMakeLists.txt", b"changed\n", True),
            ("src/luminumbra_common/sources.cmake", b"changed\n", True),
            ("tools/ci/ucrt_attribution.sh", b"changed\n", True),
            ("scripts/lint.sh", b"changed\n", True),
            ("tools/static_preview/main.cpp", b"changed\n", True),
            ("src/luminumbra_common/core/FilesystemPath.cpp", b"#ifdef _WIN32\n#endif\n", True),
            ("src/luminumbra_common/core/CrashReport.cpp", b"#ifdef _MSC_VER\n#endif\n", True),
            ("src/luminumbra_common/persistence/WorldSaveService.cpp", b"#include <windows.h>\n", True),
            ("docs/architecture.md", b"shared design\n", False),
            ("src/luminumbra_common/core/FilesystemPath.cpp", b"shared code\n", False),
            ("data/common/biomes.json", b"{}\n", False),
            ("src/platform/posix.cpp", b"shared code\n", False),
            ("src/platformish/window.cpp", b"shared code\n", False),
            ("cmakeish/file.txt", b"shared code\n", False),
            ("docs/a name [with] spaces.md", b"shared design\n", False),
            ("docs/a name w spaces.md", b"_WIN32\n", True),
            ("docs/a name [quoted].md", b"_WIN32\n", True),
            ("docs/empty.md", b"", False),
            ("data/binary.dat", b"\x00shared\xff\n", False),
            ("data/binary.dat", b"\x00_WIN32\xff\n", True),
        )
        markers = ("_WIN32", "WIN32", "_WIN64", "WIN64", "__MINGW32__", "__MINGW64__", "_MSC_VER",
                   "_M_X64", "_M_AMD64", "MSYS",
                   "MinGW", "Mingw", "mingw", "minGW", "Windows", "windows")
        paths += tuple(("src/luminumbra_common/core/FilesystemPath.cpp", marker.encode(), True)
                       for marker in markers)
        paths += tuple((f"docs/{marker}.md", b"shared design\n", True) for marker in markers)
        # Primary path fixtures must stay aligned with the repository layout.
        for path, _, _ in paths[:17]:
            self.assertTrue((ROOT / path).is_file(), path)
        with tempfile.TemporaryDirectory(prefix="ucrt64 classifier ") as directory:
            root = Path(directory).resolve(strict=True)
            # One import builds a base and independent children with byte-exact blobs.
            # No checkout, configure, native executable or network is needed.
            result = subprocess.run(["git", "init", "--quiet", str(root)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0,
                             f"git init: returncode={result.returncode}; stderr={result.stderr.decode(errors='replace')}")
            stream = bytearray(b"commit refs/heads/base\nmark :1\ncommitter Fixture <fixture@example.invalid> 1 +0000\ndata 4\nbase\n\n")
            def commit(name, parent, operations, mark=""):
                stream.extend((f"commit refs/heads/{name}\n" + mark +
                               "committer Fixture <fixture@example.invalid> 2 +0000\ndata 4\nhead\n"
                               f"from {parent}\n").encode())
                for operation, content in operations:
                    stream.extend((operation + "\n").encode())
                    if content is not None:
                        stream.extend(f"data {len(content)}\n".encode() + content + b"\n")
                stream.extend(b"\n")
            def quote_path(path):
                return '"' + path.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
            for index, (path, content, _) in enumerate(paths, 2):
                commit(f"case-{index}", ":1", [(f"M 100644 inline {quote_path(path)}", content)],
                       f"mark :{index}\n")
            platform_path = "src/luminumbra_common/core/FilesystemPath.cpp"
            platform_parent = ":" + str(next(index for index, (path, _, expected) in enumerate(paths, 2)
                                              if path == platform_path and expected))
            platform_ref = "case-" + platform_parent.removeprefix(":")
            for name, parent, operations in (
                ("deleted", ":2", [("D .github/workflows/ci.yml", None)]),
                ("renamed", ":2", [("R .github/workflows/ci.yml docs/moved.yml", None)]),
                ("platform-deleted", platform_parent, [(f"D {platform_path}", None)]),
                ("platform-renamed", platform_parent, [(f"R {platform_path} docs/moved.cpp", None)]),
                ("platform-removed", platform_parent, [(f"M 100644 inline {platform_path}", b"shared code\n")]),
                ("mixed", ":1", [("M 100644 inline docs/design.md", b"shared design\n"),
                                  (f"M 100644 inline {platform_path}", b"_WIN32\n")]),
                ("literal-path", ":1", [("M 100644 inline \"docs/a name [with] spaces.md\"", b"shared design\n"),
                                         ("M 100644 inline \"docs/a name w spaces.md\"", b"_WIN32\n")]),
                ("literal-change", "refs/heads/literal-path",
                 [("M 100644 inline \"docs/a name [with] spaces.md\"", b"updated shared design\n")]),
            ):
                commit(name, parent, operations)
            # Fast-import lengths count bytes; Windows text pipes expand LF.
            result = subprocess.run(["git", "fast-import", "--quiet"], cwd=root, input=bytes(stream),
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0,
                             f"git fast-import: returncode={result.returncode}; stderr={result.stderr.decode(errors='replace')}")
            cases = [("pull_request", "base", f"case-{index}", expected)
                     for index, (_, _, expected) in enumerate(paths, 2)]
            cases.extend([
                ("pull_request", "base", "base", False),  # Empty diff.
                ("pull_request", "case-2", "deleted", True),
                ("pull_request", "case-2", "renamed", True),
                ("pull_request", platform_ref, "platform-deleted", True),
                ("pull_request", platform_ref, "platform-renamed", True),
                ("pull_request", platform_ref, "platform-removed", True),
                ("pull_request", "base", "mixed", True),
                ("pull_request", "literal-path", "literal-change", False),
                # The base advanced with Windows changes absent from the PR.
                ("pull_request", "case-2", "case-17", False),
                ("pull_request", "", "case-2", True),
                ("pull_request", "base", "", True),
                ("pull_request", "missing", "case-2", True),
                ("pull_request", "base", "missing", True),
                ("pull_request", "base", "0" * 40, True),
                ("push", "", "", True), ("workflow_dispatch", "", "", True),
            ])
            output = root / "output"
            # Small batches keep each subprocess below its existing deadline.
            for offset in range(0, len(cases), 8):
                batch = cases[offset:offset + 8]
                commands = []
                for event, base, head, _ in batch:
                    commands.append(f"CI_EVENT={shlex.quote(event)}; CI_BASE_SHA={shlex.quote(base)}; "
                                    f"CI_HEAD_SHA={shlex.quote(head)}\n" + script)
                with self.subTest(cases=batch):
                    result = run_bash_script("\n".join(commands),
                                            cwd=root, env=dict(os.environ, GITHUB_OUTPUT=output.as_posix(),
                                                               TMPDIR=root.as_posix()),
                                            capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, 0,
                                     f"bash: returncode={result.returncode}; stderr={result.stderr.decode(errors='replace')}")
                    self.assertEqual(list(root.glob("tmp.*")), [])
            self.assertEqual(output.read_text().splitlines(),
                             ["needs_ucrt64=" + str(expected).lower() for *_, expected in cases])
            # An unwritable output must fail classification, never publish a skip.
            result = run_bash_script(script, cwd=root,
                                    env=dict(os.environ, CI_EVENT="push", GITHUB_OUTPUT=root.as_posix(),
                                             TMPDIR=root.as_posix()),
                                    capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0,
                                f"bash: returncode={result.returncode}; stderr={result.stderr.decode(errors='replace')}")

    def test_ucrt64_content_classifier_fails_open_on_git_errors(self):
        changes = jobs(self.workflows["ci.yml"])["changes"]
        step, = [step for step in steps(changes) if "name: Classify UCRT64 paths\n" in step]
        script = value(step, "run", 8)
        # Shell functions reach the actual subprocess and model partial Git output.
        wrapper = r'''
        git() {
          case "$1" in
            merge-base) printf 'base\n'; return "$MERGE_STATUS" ;;
            diff) printf 'src/shared.cpp\0'; return "$DIFF_STATUS" ;;
            grep) return "$GREP_STATUS" ;;
            *) return 99 ;;
          esac
        }
        '''
        cases = ((0, 0, 1, False), (0, 0, 2, True), (0, 0, 128, True),
                 (0, 1, 1, True), (0, 128, 1, True), (128, 0, 1, True))
        with tempfile.TemporaryDirectory(prefix="ucrt64 classifier ") as directory:
            root = Path(directory).resolve(strict=True)
            output = root / "output"
            commands = []
            for merge, diff, grep, _ in cases:
                commands.append(f"MERGE_STATUS={merge}; DIFF_STATUS={diff}; GREP_STATUS={grep}\n" + script)
            env = dict(os.environ, CI_EVENT="pull_request", CI_BASE_SHA="base", CI_HEAD_SHA="head",
                       GITHUB_OUTPUT=output.as_posix(), TMPDIR=root.as_posix())
            result = run_bash_script(wrapper + "\n".join(commands),
                                    cwd=root, env=env, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0,
                             f"bash: returncode={result.returncode}; stderr={result.stderr.decode(errors='replace')}")
            self.assertEqual(list(root.glob("tmp.*")), [])
            self.assertEqual(output.read_text().splitlines(),
                             ["needs_ucrt64=" + str(expected).lower() for *_, expected in cases])
            output.unlink()
            result = run_bash_script("mktemp() { return 1; }\n" + script,
                                    cwd=root, env=env, capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0,
                                f"bash: returncode={result.returncode}; stderr={result.stderr.decode(errors='replace')}")
            self.assertFalse(output.exists())

    def test_ucrt64_classifier_fixtures_use_portable_subprocesses(self):
        run = subprocess.run
        def checked_run(command, **kwargs):
            self.assertFalse({"text", "encoding", "universal_newlines"} & kwargs.keys(),
                             "classifier subprocess pipes must remain binary")
            if command[0] == BASH:
                self.assertFalse("-c" in command, "Bash scripts must be passed by file")
                self.assertEqual(command[1:6], ["--noprofile", "--norc", "-e", "-o", "pipefail"])
                self.assertEqual(len(command), 7)
                self.assertNotIn("\\", command[-1])
                self.assertNotIn(b"\r", Path(command[-1]).read_bytes())
                for name in ("TMPDIR", "GITHUB_OUTPUT"):
                    self.assertNotIn("\\", kwargs["env"][name])
            elif command[:2] == ["git", "fast-import"]:
                self.assertIsInstance(kwargs["input"], bytes)
                # The import's quoted M paths must also be legal Windows names.
                for line in kwargs["input"].decode("utf-8", errors="replace").splitlines():
                    if line.startswith("M 100644 inline "):
                        path, = shlex.split(line.removeprefix("M 100644 inline "))
                        self.assertNotRegex(path, r'[<>:"\\|?*\x00-\x1f]')
                        for part in path.split("/"):
                            self.assertFalse(part.endswith((".", " ")), path)
            return run(command, **kwargs)
        for method in ("test_ucrt64_path_classifier_uses_merge_base_and_fails_open",
                       "test_ucrt64_content_classifier_fails_open_on_git_errors"):
            with self.subTest(method=method), patch.object(subprocess, "run", side_effect=checked_run):
                case = type(self)(method)
                case.workflows = self.workflows
                result = unittest.TestResult()
                case.run(result)
                self.assertEqual(result.errors, [], result.errors)
                self.assertEqual(result.failures, [], result.failures)


class WorkflowBaseEditPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        cls.blocks = jobs(cls.workflow)

    @staticmethod
    def context(*, event="pull_request", action="edited", changes=None, base="main",
                draft=False, labels=(), paths="false", cpp="true", result="success", run_id=1):
        # Fixed workflow contexts only; no filesystem, clock or external service.
        return {
            "github.event_name": event, "github.event.action": action,
            "github.event.changes.base": (True if isinstance(changes, dict)
                                          and isinstance(changes.get("base"), dict)
                                          else (changes or {}).get("base", "")),
            "github.base_ref": base, "github.event.pull_request.draft": draft,
            "github.event.pull_request.labels.*.name": labels,
            "github.ref": "refs/pull/example/merge" if event == "pull_request" else f"refs/heads/{base}",
            "github.workflow": "CI", "github.run_id": run_id, "github.sha": "same-sha",
            "matrix.shard": 1, "inputs.qualification_capture": False,
            "needs.changes.outputs.needs_ucrt64": paths,
            "needs.changes.outputs.needs_cpp": cpp,
            "needs.changes.outputs.needs_python": "false",
            **{f"needs.{job}.result": result for job in (
                "changes", "linux", "windows-msvc", "windows-ucrt64", "tidy-shard")},
        }

    @staticmethod
    def render(expression, context):
        # Match whole property paths, so github.ref cannot consume github.ref_name.
        return re.sub(r"\b(?:github|needs|matrix|inputs)\.[\w.*-]+",
                      lambda match: repr(context[match[0]]), expression)

    @classmethod
    def expression(cls, expression, context):
        expression = cls.render(expression.removeprefix("${{ ").removesuffix(" }}"), context)
        expression = re.sub(r"!(?!=)", "not ", expression)
        expression = expression.replace("== false", "== False")
        expression = expression.replace("&&", " and ").replace("||", " or ")
        return eval(expression, {"__builtins__": {}, "format": str.format,
                                 "contains": lambda items, item: item.lower() in
                                 [entry.lower() for entry in items]})

    @classmethod
    def name(cls, job, context):
        return re.sub(r"\$\{\{ .*? \}\}",
                      lambda match: str(cls.expression(match[0], context)), value(job, "name", 4))

    @classmethod
    def admitted(cls, job, context, cancelled=False):
        rendered = cls.render(value(job, "if", 4) or "success()", context)
        return evaluate_condition(rendered, {}, cancelled=cancelled)

    def test_edited_without_base_change_skips_every_job_and_uses_distinct_names(self):
        ignored = (None, {}, {"title": {"from": "old"}}, {"body": {"from": ""}},
                   {"unknown": {}}, {"base": None}, {"base": ""}, {"base": False})
        # At most 14 jobs x 8 payloads x 6 dependency results x 2 cancellations.
        # Even failed/unknown dependencies must not revive an aggregate on edits.
        normal = self.context(action="synchronize")
        for changes, result, cancelled in product(
                ignored, ("success", "failure", "cancelled", "skipped", "", "unknown"), (False, True)):
            context = self.context(changes=changes, result=result)
            for job_id, job in self.blocks.items():
                with self.subTest(job=job_id, changes=changes, result=result, cancelled=cancelled):
                    self.assertFalse(self.admitted(job, context, cancelled))
                    self.assertNotEqual(self.name(job, context), self.name(job, normal))
                    self.assertTrue(self.name(job, context).endswith(" (metadata edit)"))

    def test_edited_base_change_preserves_normal_ready_draft_and_path_routing(self):
        pr = self.workflow.split("  pull_request:\n", 1)[1].split("  workflow_dispatch:", 1)[0]
        self.assertIn("edited", value(pr, "types", 4).strip("[]").split(", "))
        # 14 jobs x 2 base payloads x 2 bases x 3 drafts x 3 path outputs x 2 cancellations.
        for changes, base, draft, paths, cancelled in product(
                ({"base": {}}, {"base": {"ref": {"from": "devel"}}}),
                ("main", "devel"), (False, True, None), ("true", "false", ""), (False, True)):
            context = self.context(changes=changes, base=base, draft=draft, paths=paths)
            normal = {**context, "github.event.action": "synchronize"}
            for job_id, job in self.blocks.items():
                with self.subTest(job=job_id, changes=changes, base=base, draft=draft,
                                  paths=paths, cancelled=cancelled):
                    self.assertEqual(self.admitted(job, context, cancelled), self.admitted(job, normal, cancelled))
                    self.assertEqual(self.name(job, context), self.name(job, normal))
        context = self.context(changes={"base": {}}, paths="true")
        self.assertEqual([job_id for job_id, job in self.blocks.items() if self.admitted(job, context)],
                         list(self.blocks))

    def test_metadata_edit_concurrency_is_unique_and_cannot_cancel_validation(self):
        group = value(self.workflow, "group", 2)
        cancellation = value(self.workflow, "cancel-in-progress", 2)
        normal = self.context(action="synchronize")
        normal_group = self.expression(group, normal)
        self.assertEqual(normal_group, "CI-refs/pull/example/merge")
        self.assertIs(self.expression(cancellation, normal), True)
        for changes in (None, {}, {"title": {}}, {"body": {}}, {"base": None}, {"base": ""}):
            groups = []
            for run_id in (1, 2):
                context = self.context(changes=changes, run_id=run_id)
                groups.append(self.expression(group, context))
                self.assertNotEqual(groups[-1], normal_group)
                self.assertIs(self.expression(cancellation, context), False)
            self.assertNotEqual(*groups)
        for changes in ({"base": {}}, {"base": {"ref": {"from": "devel"}}}):
            context = self.context(changes=changes)
            self.assertEqual(self.expression(group, context), normal_group)
            self.assertIs(self.expression(cancellation, context), True)
        for event, base, expected in (("push", "devel", "CI-refs/heads/devel-push"),
                                      ("push", "main", "CI-refs/heads/main-same-sha-1"),
                                      ("workflow_dispatch", "main", "CI-manual-1")):
            context = self.context(event=event, base=base)
            self.assertEqual(self.expression(group, context), expected)
            self.assertIs(self.expression(cancellation, context), False)

    def test_retarget_to_main_runs_ucrt64_without_label_or_sensitive_paths(self):
        producer, aggregate = self.blocks["windows-ucrt64"], self.blocks["windows-ucrt64-result"]
        gate = next(step for step in steps(aggregate) if "name: Require expected producers and route\n" in step)
        for cpp, paths in product(("false", "true", "", "unknown"), ("false", "", "unknown")):
            context = self.context(changes={"base": {"ref": {"from": "devel"}}}, paths=paths, cpp=cpp)
            with self.subTest(cpp=cpp, paths=paths):
                self.assertTrue(self.admitted(producer, context))
                self.assertTrue(self.admitted(aggregate, context))
                self.assertTrue(self.expression(value(gate, "CI_ENABLED", 10), context))
                self.assertFalse(self.admitted(producer, {**context, "github.base_ref": "devel"}))
                self.assertFalse(self.admitted(producer, {**context, "github.event.changes.base": ""}))
                self.assertFalse(self.admitted(aggregate, {**context, "github.event.changes.base": ""}))


class WorkflowCacheCleanupPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = (ROOT / ".github/workflows/cache-cleanup.yml").read_text(encoding="utf-8")

    def test_only_cache_writer_completions_trigger_serialized_cleanup(self):
        triggers = self.workflow.split("\non:\n", 1)[1].split("\npermissions:\n", 1)[0]
        events = "\n".join(line for line in triggers.splitlines() if not line.lstrip().startswith("#"))
        self.assertEqual(events.strip(),
                         "workflow_run:\n"
                         "    workflows: [CI, AddressSanitizer, Performance observations]\n"
                         "    types: [completed]")
        self.assertEqual(value(self.workflow, "group", 2), "cache-cleanup")
        self.assertEqual(value(self.workflow, "cancel-in-progress", 2), "false")

    def test_completion_list_covers_every_cache_saving_workflow(self):
        writers = set()
        for path in (ROOT / ".github/workflows").glob("*.yml"):
            workflow = path.read_text(encoding="utf-8")
            # Include combined restore/save actions and action-managed package
            # caches as well as explicit save steps, across every platform lane.
            saves = re.search(r"^\s+(?:- )?uses: actions/cache(?:/save)?@", workflow, re.MULTILINE)
            package_cache = any(
                setting.strip() != "false"
                for setting in re.findall(r"^\s+cache: ([^\n#]+)", workflow, re.MULTILINE)
            )
            if saves or package_cache:
                writers.add(value(workflow, "name", 0))
        self.assertEqual(writers, {"CI", "AddressSanitizer", "Performance observations"})
        completions = value(self.workflow, "workflows", 4).strip("[]").split(", ")
        self.assertEqual(set(completions), writers)

    def test_cleanup_filter_accepts_only_main_and_devel_push_completions(self):
        expression = value(jobs(self.workflow)["cleanup"], "if", 4)
        for event in ("workflow_run", "push", "pull_request", "workflow_dispatch"):
            for source_event in ("push", "pull_request", "schedule", "workflow_dispatch"):
                for branch in ("main", "devel", "feature"):
                    for ref in ("refs/heads/main", "refs/heads/devel", "refs/heads/feature"):
                        actual = expression
                        for key, replacement in (
                            ("github.event.workflow_run.head_branch", branch),
                            ("github.event.workflow_run.event", source_event),
                            ("github.event_name", event), ("github.ref", ref),
                        ):
                            actual = actual.replace(key, repr(replacement))
                        actual = actual.replace("&&", "and").replace("||", "or")
                        expected = (event == "workflow_run" and source_event == "push"
                                    and branch in ("main", "devel"))
                        with self.subTest(event=event, source_event=source_event, branch=branch, ref=ref):
                            self.assertEqual(eval(actual, {"__builtins__": {}}), expected)


class WorkflowAttributionPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = (ROOT / ".github/workflows/ucrt-attribution.yml").read_text(encoding="utf-8")
        triggers = cls.workflow.split("\non:\n", 1)[1].split("\npermissions:\n", 1)[0]
        cls.events = dict(re.findall(
            r"^  (\w+):\n(.*?)(?=^  \w+:|\Z)", triggers, re.MULTILINE | re.DOTALL,
        ))
        cls.input_paths = [
            ".github/workflows/ucrt-attribution.yml",
            "tools/ci/ucrt_attribution.sh",
            "tools/ci/ci_shell.py",
            "tools/ci/test_ucrt_attribution.py",
            "tools/ci/test_ucrt_gcc_cohort.py",
            "tools/ci/check_worldgen_probe_smoke.py",
            "tools/ci/test_check_worldgen_probe_smoke.py",
            "test/diagnostics/worldgen_probe.cpp",
        ]

    def test_only_input_changes_trigger_devel_pull_requests(self):
        pr = self.events["pull_request"]
        self.assertEqual(value(pr, "branches", 4), "[devel]")
        self.assertEqual(value(pr, "types", 4), "[opened, synchronize, reopened, ready_for_review]")
        # Preserve the diagnostic's narrow admission filter; routing must not
        # introduce costly cohort runs on general engine changes.
        self.assertEqual(re.findall(r"^      - (.+)$", pr, re.MULTILINE), self.input_paths)
        self.assertIsNone(value(pr, "paths-ignore", 4))

    def test_side_lanes_reuse_the_trusted_classifier_and_required_skip_pattern(self):
        performance = (ROOT / ".github/workflows/performance.yml").read_text(encoding="utf-8")
        reference = jobs(performance)["changes"]
        for filename in ("ci.yml", "asan.yml", "ucrt-attribution.yml"):
            workflow = (ROOT / ".github/workflows" / filename).read_text(encoding="utf-8")
            classifier = jobs(workflow)["changes"]
            with self.subTest(workflow=filename):
                # CI adds a separately tested UCRT64 output/step; its shared
                # trusted classifier must still match every sibling exactly.
                if filename == "ci.yml":
                    self.assertEqual(classifier.count("      - name: Classify UCRT64 paths\n"), 1)
                    self.assertEqual(value(classifier, "name", 4), "Classify changed paths" + CI_METADATA_SUFFIX)
                    self.assertEqual(value(classifier, "if", 4), ci_event_condition())
                    classifier = classifier.replace(CI_METADATA_SUFFIX, "", 1).replace(
                        "    if: " + ci_event_condition() + "\n", "", 1)
                    classifier = classifier.replace(
                        "      needs_ucrt64: ${{ steps.ucrt64.outputs.needs_ucrt64 }}\n", "", 1
                    ).split("\n      - name: Classify UCRT64 paths\n", 1)[0]
                # The layout reader includes comments preceding the next job.
                self.assertEqual(classifier.split("\n  #", 1)[0].strip(), reference.strip())
                for output in ("needs_cpp", "needs_python"):
                    self.assertEqual(value(classifier, output, 6), value(reference, output, 6))
        # Required checks already report path-routed skips without changing names.
        ci = jobs((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
        for job_id in ("gns-compile-probe", "diligent-rhi"):
            self.assertEqual(value(ci[job_id], "needs", 4), "changes")
            self.assertIn("needs.changes.outputs.needs_cpp != 'false'", value(ci[job_id], "if", 4))
        runner = jobs(performance)["runner-contract"]
        self.assertEqual(value(runner, "name", 4), "Runner contract / ${{ matrix.os }}")
        self.assertEqual(value(runner, "os", 8), "[ubuntu-24.04, windows-2025, macos-15]")

    def test_side_lane_conditions_fail_open_and_preserve_non_pr_routes(self):
        performance = (ROOT / ".github/workflows/performance.yml").read_text(encoding="utf-8")
        for workflow, job_id, python_inputs in (
                (performance, "runner-contract", True), (self.workflow, "attribution", False)):
            job = jobs(workflow)[job_id]
            self.assertEqual(value(job, "needs", 4), "changes")
            condition = value(job, "if", 4)
            expected = "${{ !cancelled() && (github.event_name != 'pull_request' || "
            if not python_inputs:
                expected += "(github.event.pull_request.draft == false && ("
            expected += "needs.changes.result != 'success' || needs.changes.outputs.needs_cpp != 'false'"
            if python_inputs:
                expected += " || needs.changes.outputs.needs_python != 'false'"
            self.assertEqual(condition, expected + (") }}" if python_inputs else "))) }}"))
            # Finite cross-product covers partial/unknown output and failed,
            # skipped or cancelled classification, including stale false output.
            for event in ("pull_request", "push", "schedule", "workflow_dispatch"):
                for result in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                    for cpp in ("true", "false", "", "unknown"):
                        for python in ("true", "false", "", "unknown"):
                            for cancelled in (False, True):
                                rendered = condition
                                for key, item in (("github.event_name", event),
                                                  ("needs.changes.outputs.needs_cpp", cpp),
                                                  ("needs.changes.outputs.needs_python", python)):
                                    rendered = rendered.replace(key, repr(item))
                                for draft in (True, False, None):
                                    # GitHub's missing boolean property is not false.
                                    draft_rendered = rendered.replace(
                                        "github.event.pull_request.draft == false", repr(draft is False))
                                    with self.subTest(job=job_id, event=event, result=result,
                                                      cpp=cpp, python=python, cancelled=cancelled, draft=draft):
                                        routed = (result != "success" or cpp != "false"
                                                  or (python_inputs and python != "false"))
                                        ready = python_inputs or draft is False
                                        self.assertEqual(evaluate_condition(
                                            draft_rendered, {"changes": result}, cancelled=cancelled),
                                            not cancelled and (event != "pull_request" or (ready and routed)))

    def test_side_lane_input_groups_cover_native_toolchain_and_python_contracts(self):
        import ci_paths
        for path in ("src/world.cpp", "include/world.hpp", "CMakeLists.txt", "CMakePresets.json",
                     "cmake/toolchain.cmake", "vendor/library/source.c", "vcpkg.json",
                     "vcpkg-configuration.json", "deps/packages.txt", "scripts/packages.txt",
                     ".github/workflows/performance.yml", ".github/workflows/ucrt-attribution.yml",
                     *self.input_paths, "tools/CMakeLists.txt", "test/CMakeLists.txt", "new-tool"):
            with self.subTest(path=path):
                self.assertTrue(ci_paths.needs_cpp(path))
        for path, python_lane in (("docs/development.md", True), ("README.md", True),
                                  ("docs/diagram.svg", False)):
            with self.subTest(path=path):
                self.assertFalse(ci_paths.needs_cpp(path))
                self.assertEqual(ci_paths.needs_python(path), python_lane)
        # Covered performance modules can skip native work but must retain the
        # runner's Python contract coverage on every OS.
        self.assertFalse(ci_paths.needs_cpp("tools/perf/render_budget.py"))
        self.assertTrue(ci_paths.needs_python("tools/perf/render_budget.py"))

    def test_devel_pushes_also_cover_probe_build_definitions(self):
        push = self.events["push"]
        self.assertEqual(value(push, "branches", 4), "[devel]")
        self.assertEqual(
            re.findall(r"^      - (.+)$", push, re.MULTILINE),
            self.input_paths[:-1] + ["tools/CMakeLists.txt", "test/CMakeLists.txt"] + self.input_paths[-1:],
        )

    def test_weekly_schedule_and_manual_dispatch_remain_available(self):
        self.assertEqual(set(self.events), {"pull_request", "push", "schedule", "workflow_dispatch"})
        self.assertEqual(self.events["schedule"].strip(), '- cron: "43 4 * * 0"')
        self.assertEqual(self.events["workflow_dispatch"].strip(), "")

    def test_concurrency_separates_schedules_pushes_and_manual_runs(self):
        # Evaluate the actual routing expressions for both possible default refs.
        cases = [
            ("push", "refs/heads/devel", 1, "attribution-refs/heads/devel-push", False),
            ("push", "refs/heads/devel", 2, "attribution-refs/heads/devel-push", False),
            ("schedule", "refs/heads/devel", 3, "attribution-refs/heads/devel", True),
            ("schedule", "refs/heads/main", 4, "attribution-refs/heads/main", True),
            ("push", "refs/heads/main", 5, "attribution-refs/heads/main-sha-5", False),
            ("workflow_dispatch", "refs/heads/devel", 6, "attribution-manual-6", False),
            ("workflow_dispatch", "refs/heads/devel", 7, "attribution-manual-7", False),
            ("pull_request", "refs/pull/example/merge", 8, "attribution-refs/pull/example/merge", True),
            ("pull_request", "refs/pull/example/merge", 9, "attribution-refs/pull/example/merge", True),
        ]
        for event, ref, run_id, group, cancel in cases:
            for key, expected in (("group", group), ("cancel-in-progress", cancel)):
                expression = value(self.workflow, key, 2).removeprefix("${{ ").removesuffix(" }}")
                for name, replacement in (
                    ("event_name", event), ("ref", ref), ("workflow", "attribution"),
                    ("sha", "sha"), ("run_id", run_id),
                ):
                    expression = expression.replace("github." + name, repr(replacement))
                expression = expression.replace("&&", "and").replace("||", "or")
                with self.subTest(event=event, ref=ref, run_id=run_id, key=key):
                    self.assertEqual(eval(expression, {"__builtins__": {}, "format": str.format}), expected)

    def test_revision_recording_handles_missing_pull_request_metadata(self):
        step = next(step for step in steps(jobs(self.workflow)["attribution"])
                    if step.startswith("      - name: Record revisions and runner image\n"))
        self.assertEqual(value(step, "shell", 8), "pwsh")
        self.assertEqual(value(step, "HEAD_SHA", 10), "${{ github.event.pull_request.head.sha }}")
        self.assertEqual(value(step, "MERGE_SHA", 10), "${{ github.sha }}")
        script = value(step, "run", 8)
        self.assertIn("$headSha = if ($env:HEAD_SHA) { $env:HEAD_SHA } else { 'not a pull request' }", script)
        self.assertIn('"| pull request head sha | $headSha |"', script)
        self.assertIn('"| checked out sha | $env:MERGE_SHA |"', script)


class PolicyRouteCase(NamedTuple):
    parameters: tuple[str, ...]
    accepted: bool
    command: tuple[str, ...]
    cwd: str
    environment: tuple[tuple[str, str], ...]


def policy_run_route_case(case):
    return subprocess.run(case.command, cwd=case.cwd, env=dict(case.environment),
                          capture_output=True, text=True, timeout=10)


def policy_route_results(cases, workers):
    """Keep at most two submitted cases; report outcomes in original roster order."""
    if type(workers) is not int or workers not in (1, 2):
        raise ValueError("route case workers must be 1 or 2")
    if workers == 1:
        for case in cases:
            future = Future()
            try:
                future.set_result(policy_run_route_case(case))
            except Exception as error:
                future.set_exception(error)
            yield case, future
        return
    # The window bounds descriptors, futures and owned subprocesses to two.
    # Closing the iterator drains the executor before its temporary files go away.
    iterator = iter(cases)
    with ThreadPoolExecutor(max_workers=workers) as executor:
        pending = deque((case, executor.submit(policy_run_route_case, case))
                        for case in islice(iterator, workers))
        while pending:
            case, future = pending.popleft()
            wait((future,))
            yield case, future
            for following in islice(iterator, 1):
                pending.append((following, executor.submit(policy_run_route_case, following)))


# Keep launch-heavy cases separate without changing any case or subtest.
# Selection scans the finite unittest roster once; no subprocess is started here.
POLICY_PARTITIONS = tuple(str(index) for index in range(13))
POLICY_ROUTE_CASE = "WorkflowDraftPolicyTests.test_native_aggregates_preserve_route_checks"
POLICY_CASE_PARTITIONS = {
    "WorkflowDraftPolicyTests.test_aggregates_cancel_at_each_phase_and_fail_invalid_dependencies": "1",
    "WorkflowDraftPolicyTests.test_asan_aggregate_accepts_only_success_or_the_original_path_skip": "2",
    "WorkflowDraftPolicyTests.test_ci_and_asan_bash_scripts_parse_without_executing_native_commands": "3",
    "WorkflowShellDiagnosticTests.test_actual_windows_probe_accepts_legacy_msys_and_documented_cygwin": "6",
    "WorkflowShellDiagnosticTests.test_actual_windows_probe_refuses_other_identities_despite_msystem": "7",
    "WorkflowDraftPolicyTests.test_bootstrap_pins_full_events_main_prs_and_missing_or_invalid_outputs": "8",
    POLICY_ROUTE_CASE: "9",
    "WorkflowUcrtClassifierPolicyTests.test_windows_intentional_skip_gate_uses_real_workflow_inputs_and_refuses_uncertainty": "12",
}
POLICY_CLASS_PARTITIONS = {
    "WorkflowBaseEditPolicyTests": "0",
    "WorkflowDraftPolicyTests": "0",
    "WorkflowRequiredAggregatePolicyTests": "10",
    "WorkflowUcrtClassifierPolicyTests": "11",
    "WorkflowExpressionPolicyTests": "0",
    "WorkflowCacheCleanupPolicyTests": "0",
    "WorkflowAttributionPolicyTests": "0",
    "WorkflowPartitionSelectionTests": "0",
    "WorkflowShellTests": "4",
    "WorkflowShellDiagnosticTests": "5",
}


def policy_cases(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from policy_cases(item)
        else:
            yield item


def policy_partition_suite(loader, module, partition):
    if partition not in POLICY_PARTITIONS:
        raise ValueError("unknown policy partition")
    if any(value not in POLICY_PARTITIONS for value in (
            *POLICY_CLASS_PARTITIONS.values(), *POLICY_CASE_PARTITIONS.values())):
        raise ValueError("invalid policy partition mapping")
    cases = list(policy_cases(loader.loadTestsFromModule(module)))
    seen = set()
    selected = []
    for case in cases:
        key = f"{type(case).__name__}.{case._testMethodName}"
        if key in seen:
            raise ValueError("duplicate policy case: " + key)
        seen.add(key)
        class_partition = POLICY_CLASS_PARTITIONS.get(type(case).__name__)
        if class_partition is None:
            raise ValueError("unregistered policy class: " + type(case).__name__)
        if POLICY_CASE_PARTITIONS.get(key, class_partition) == partition:
            selected.append(case)
    if not POLICY_CASE_PARTITIONS.keys() <= seen:
        raise ValueError("missing dedicated policy case")
    if not selected:
        raise ValueError("empty policy partition")
    return unittest.TestSuite(selected)


class PolicyTimingResult(unittest.TextTestResult):
    def startTest(self, test):
        self.case_started = time.perf_counter()
        super().startTest(test)

    def stopTest(self, test):
        super().stopTest(test)
        self.stream.writeln(f"  {test.id()}: {time.perf_counter() - self.case_started:.3f}s")


def policy_main(argv=None):
    # With no partition, retain unittest's complete CLI and discovery semantics.
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--partition", choices=POLICY_PARTITIONS, action="append")
    parser.add_argument("--list-tests", action="store_true")
    parser.add_argument("--case-workers", choices=("1", "2"), action="append")
    args, remaining = parser.parse_known_args(argv)
    if args.case_workers is not None:
        route_partition = POLICY_CASE_PARTITIONS.get(
            POLICY_ROUTE_CASE, POLICY_CLASS_PARTITIONS["WorkflowDraftPolicyTests"])
        if len(args.case_workers) != 1 or args.partition != [route_partition]:
            parser.error("one --case-workers option requires the route-grid partition " + route_partition)
    if args.partition is None:
        if args.list_tests:
            parser.error("--list-tests requires --partition")
        unittest.main(argv=[sys.argv[0], *remaining])
        return 0
    if len(args.partition) != 1 or remaining:
        parser.error("one partition is required, without unittest selectors")
    try:
        suite = policy_partition_suite(unittest.TestLoader(), sys.modules[__name__], args.partition[0])
    except ValueError as error:
        parser.error(str(error))
    if args.list_tests:
        for case in policy_cases(suite):
            print(case.id())
        return 0
    for case in policy_cases(suite):
        if f"{type(case).__name__}.{case._testMethodName}" == POLICY_ROUTE_CASE:
            case.route_case_workers = int(args.case_workers[0]) if args.case_workers else 1
    result = unittest.TextTestRunner(verbosity=2, resultclass=PolicyTimingResult).run(suite)
    return int(not result.wasSuccessful())


class WorkflowPartitionSelectionTests(unittest.TestCase):
    def test_cli_partition_listing_ignores_shared_unittest_filters(self):
        module = sys.modules[__name__]
        with patch.object(unittest.defaultTestLoader, "testNamePatterns", ["*does_not_exist*"]):
            for partition in POLICY_PARTITIONS:
                with self.subTest(partition=partition), patch.object(sys, "stdout", new_callable=io.StringIO) as output:
                    self.assertEqual(policy_main(["--partition", partition, "--list-tests"]), 0)
                    expected = policy_partition_suite(unittest.TestLoader(), module, partition)
                    self.assertEqual(output.getvalue().splitlines(), [case.id() for case in policy_cases(expected)])

    @staticmethod
    def route_fixture(directory, index, script="print('complete')"):
        summary = (Path(directory) / f"summary-{index}").as_posix()
        return PolicyRouteCase((str(index),), True,
                               (sys.executable, "-I", "-B", "-c", script, str(index), directory),
                               directory, tuple(dict(os.environ, GITHUB_STEP_SUMMARY=summary,
                                                     POLICY_CASE=str(index)).items()))

    def test_route_window_overlaps_owned_children_with_bounded_ordered_consumption(self):
        script = textwrap.dedent('''
            import os, sys, time
            from pathlib import Path
            index, root = sys.argv[1], Path(sys.argv[2])
            def await_file(name):
                deadline = time.monotonic() + 5
                while not (root / name).exists():
                    if time.monotonic() >= deadline:
                        raise TimeoutError(name)
                    time.sleep(.005)
            summary = Path(os.environ['GITHUB_STEP_SUMMARY'])
            assert not summary.exists()
            summary.write_text(os.environ['POLICY_CASE'], encoding='utf-8')
            if index == '0':
                (root / 'first-ready').write_text('ready')
                await_file('second-done')
            elif index == '1':
                await_file('first-ready')
                (root / 'second-done').write_text('done')
            print(index)
        ''')
        generated = []
        with tempfile.TemporaryDirectory() as directory:
            def cases():
                for index in range(4):
                    generated.append(index)
                    yield self.route_fixture(directory, index, script)
            with closing(policy_route_results(cases(), 2)) as results:
                first, future = next(results)
                self.assertEqual(generated, [0, 1])
                self.assertEqual(first.parameters, ("0",))
                process = future.result()
                self.assertEqual(process.returncode, 0, process.stderr)
                self.assertTrue((Path(directory) / 'second-done').is_file())
                outputs = [process.stdout.strip()]
                for index, (case, future) in enumerate(results, 1):
                    self.assertEqual(len(generated), min(index + 2, 4))
                    self.assertEqual(case.parameters, (str(index),))
                    process = future.result()
                    self.assertEqual(process.returncode, 0, process.stderr)
                    outputs.append(process.stdout.strip())
            self.assertEqual(outputs, ["0", "1", "2", "3"])
            self.assertEqual([(Path(directory) / f"summary-{index}").read_text() for index in range(4)],
                             ["0", "1", "2", "3"])
            self.assertEqual(len({dict(self.route_fixture(directory, index).environment)["GITHUB_STEP_SUMMARY"]
                                  for index in range(4)}), 4)

    def test_actual_route_descriptors_isolate_summary_environment_and_working_directory(self):
        descriptors = []
        collect = policy_route_results
        def sample(cases, workers):
            current = list(islice(cases, 2))
            descriptors.extend(current)
            yield from collect(current, workers)
        case = WorkflowDraftPolicyTests("test_native_aggregates_preserve_route_checks")
        case.workflows = {filename: (ROOT / ".github/workflows" / filename).read_text(encoding="utf-8")
                          for filename in GUARDED}
        case.route_case_workers = 2
        environment = dict(os.environ)
        cwd = Path.cwd()
        result = unittest.TestResult()
        with patch.object(sys.modules[__name__], "policy_route_results", side_effect=sample):
            case.run(result)
        self.assertEqual(result.errors, [], result.errors)
        self.assertEqual(result.failures, [], result.failures)
        self.assertEqual(len(descriptors), 6)
        summaries = [Path(dict(item.environment)["GITHUB_STEP_SUMMARY"]) for item in descriptors]
        self.assertEqual(len(set(summaries)), 6)
        self.assertEqual(len({path.parent for path in summaries}), 1)
        self.assertTrue(all(path.is_absolute() for path in summaries))
        self.assertFalse(summaries[0].parent.exists())
        self.assertTrue(all(item.cwd == str(ROOT) for item in descriptors))
        self.assertEqual(dict(os.environ), environment)
        self.assertEqual(Path.cwd(), cwd)
        for item in descriptors:
            self.assertIsInstance(item.environment, tuple)
            self.assertIsInstance(item.command, tuple)
            with self.assertRaises(AttributeError):
                item.cwd = "changed"

    def test_route_serial_path_empty_rosters_and_invalid_bounds(self):
        for workers in (1, 2):
            self.assertEqual(list(policy_route_results((), workers)), [])
        for workers in (0, -1, 3, 504, True, None, "2", 2.0):
            with self.subTest(workers=workers), self.assertRaises(ValueError):
                list(policy_route_results((), workers))
        generated = []
        with tempfile.TemporaryDirectory() as directory:
            def cases():
                for index in range(3):
                    generated.append(index)
                    yield self.route_fixture(directory, index)
            with patch.object(sys.modules[__name__], "ThreadPoolExecutor", side_effect=AssertionError("serial executor")):
                with closing(policy_route_results(cases(), 1)) as results:
                    for index, (case, future) in enumerate(results):
                        self.assertEqual(generated, list(range(index + 1)))
                        self.assertEqual(case.parameters, (str(index),))
                        self.assertEqual(future.result().returncode, 0)

    def test_route_subtests_keep_exit_exception_identity_and_later_case_coverage(self):
        import threading
        reporting_thread = threading.get_ident()
        seen = []
        errors = []
        # Use real owned children with a short controlled timeout; this tests
        # outcome transport, not the runtime of the workflow grid.
        def launch(case):
            return subprocess.run(case.command, cwd=case.cwd, env=dict(case.environment),
                                  capture_output=True, text=True, timeout=.2 if case.parameters == ("2",) else 10)
        for workers in (1, 2):
            with self.subTest(workers=workers), tempfile.TemporaryDirectory() as directory:
                cases = [self.route_fixture(directory, 0, "import sys; print('refused'); sys.exit(7)"),
                         self.route_fixture(directory, 1),
                         self.route_fixture(directory, 2, "import time; time.sleep(30)"),
                         self.route_fixture(directory, 3)]
                cases[1] = cases[1]._replace(command=(str(Path(directory) / "missing-executable"),))
                seen.clear()
                errors.clear()
                class ExercisesSubtests(unittest.TestCase):
                    def runTest(inner):
                        with closing(policy_route_results(cases, workers)) as results:
                            for case, future in results:
                                with inner.subTest(case=case.parameters):
                                    seen.append((case.parameters[0], threading.get_ident()))
                                    try:
                                        process = future.result()
                                    except Exception as error:
                                        errors.append((error, future.exception()))
                                        raise
                                    inner.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                result = unittest.TestResult()
                with patch.object(sys.modules[__name__], "policy_run_route_case", side_effect=launch):
                    ExercisesSubtests().run(result)
                self.assertEqual(seen, [(str(index), reporting_thread) for index in range(4)])
                self.assertEqual(len(result.failures), 1, result.failures)
                self.assertEqual(len(result.errors), 2, result.errors)
                self.assertIsInstance(errors[0][0], FileNotFoundError)
                self.assertIsInstance(errors[1][0], subprocess.TimeoutExpired)
                self.assertEqual(errors[1][0].cmd, cases[2].command)
                for original, transported in errors:
                    self.assertIs(original, transported)

    def test_route_close_and_descriptor_failure_drain_owned_children(self):
        from queue import Queue
        completed = Queue()
        launch = policy_run_route_case
        def record(case):
            process = launch(case)
            completed.put((case.parameters, process.returncode))
            return process
        for broken in (False, True):
            with self.subTest(broken=broken), tempfile.TemporaryDirectory() as directory:
                def cases():
                    yield self.route_fixture(directory, 0)
                    yield self.route_fixture(directory, 1)
                    if broken:
                        raise OSError("descriptor input unavailable")
                with patch.object(sys.modules[__name__], "policy_run_route_case", side_effect=record):
                    with closing(policy_route_results(cases(), 2)) as results:
                        _, future = next(results)
                        self.assertEqual(future.result().returncode, 0)
                        if broken:
                            with self.assertRaisesRegex(OSError, "descriptor input unavailable"):
                                next(results)
                self.assertCountEqual([completed.get_nowait(), completed.get_nowait()],
                                      [(("0",), 0), (("1",), 0)])
                self.assertTrue(completed.empty())

    def test_case_workers_are_local_to_selected_route_case_and_listing_is_inert(self):
        partition = POLICY_CASE_PARTITIONS[POLICY_ROUTE_CASE]
        for option, expected in (([], 1), (["--case-workers", "1"], 1), (["--case-workers", "2"], 2)):
            case = WorkflowDraftPolicyTests("test_native_aggregates_preserve_route_checks")
            other = WorkflowDraftPolicyTests("test_ready_and_draft_transitions_trigger_existing_pr_routes")
            suite = unittest.TestSuite([case, other])
            with patch.object(sys.modules[__name__], "policy_partition_suite", return_value=suite), patch.object(
                    unittest, "TextTestRunner") as runner:
                runner.return_value.run.return_value.wasSuccessful.return_value = True
                self.assertEqual(policy_main(["--partition", partition, *option]), 0)
                self.assertEqual(case.route_case_workers, expected)
                self.assertFalse(hasattr(other, "route_case_workers"))
                runner.return_value.run.assert_called_once_with(suite)
        self.assertFalse(hasattr(WorkflowDraftPolicyTests("test_native_aggregates_preserve_route_checks"),
                                 "route_case_workers"))
        with patch.object(unittest, "TextTestRunner") as runner, patch.object(
                sys.modules[__name__], "policy_run_route_case") as launch, patch.object(
                sys, "stdout", new_callable=io.StringIO) as output:
            self.assertEqual(policy_main(["--partition", partition, "--case-workers", "2", "--list-tests"]), 0)
            self.assertIn("test_native_aggregates_preserve_route_checks", output.getvalue())
            runner.assert_not_called()
            launch.assert_not_called()

    def test_case_workers_follow_route_case_partition_including_class_default(self):
        for partition in ("0", "8", "9"):
            dedicated = dict(POLICY_CASE_PARTITIONS)
            if partition == "0":
                dedicated.pop(POLICY_ROUTE_CASE)
            else:
                dedicated[POLICY_ROUTE_CASE] = partition
            with self.subTest(partition=partition), patch.dict(POLICY_CASE_PARTITIONS, dedicated, clear=True):
                case = WorkflowDraftPolicyTests("test_native_aggregates_preserve_route_checks")
                suite = unittest.TestSuite([case])
                with patch.object(sys.modules[__name__], "policy_partition_suite", return_value=suite), patch.object(
                        unittest, "TextTestRunner") as runner:
                    runner.return_value.run.return_value.wasSuccessful.return_value = True
                    self.assertEqual(policy_main(["--partition", partition, "--case-workers", "2"]), 0)
                    self.assertEqual(case.route_case_workers, 2)
                    runner.return_value.run.assert_called_once_with(suite)
                for other in POLICY_PARTITIONS:
                    if other == partition:
                        continue
                    with self.subTest(other=other), patch.object(
                            sys.modules[__name__], "policy_partition_suite") as select, patch.object(
                            sys, "stderr", new_callable=io.StringIO) as output, self.assertRaises(SystemExit) as error:
                        policy_main(["--partition", other, "--case-workers", "2"])
                    self.assertEqual(error.exception.code, 2)
                    self.assertIn("requires the route-grid partition " + partition, output.getvalue())
                    select.assert_not_called()

    def test_case_worker_cli_refuses_invalid_duplicate_and_unreserved_options(self):
        partition = POLICY_CASE_PARTITIONS[POLICY_ROUTE_CASE]
        invalid = [["--partition", partition, "--case-workers", value]
                   for value in ("0", "3", "-1", "1.0", "nan", "inf", "01", "02", "", "2" * 5000)]
        invalid += [["--partition", partition, "--case-workers"],
                    ["--partition", partition, "--case-workers", "2", "--case-workers", "2"],
                    ["--partition", partition, "--case-workers", "1", "--case-workers", "2"],
                    ["--case-workers", "2"], ["--case-workers", "1"],
                    ["--partition", partition, "--partition", partition, "--case-workers", "2"],
                    ["--partition", partition, "--case-workers", "2", "WorkflowDraftPolicyTests"]]
        invalid += [["--partition", other, "--case-workers", workers]
                    for other in POLICY_PARTITIONS if other != partition for workers in ("1", "2")]
        with patch.object(unittest, "main") as direct, patch.object(
                sys.modules[__name__], "policy_partition_suite") as select:
            for argv in invalid:
                with self.subTest(argv=argv), patch.object(sys, "stderr", new_callable=io.StringIO), self.assertRaises(SystemExit) as error:
                    policy_main(argv)
                self.assertEqual(error.exception.code, 2)
            direct.assert_not_called()
            select.assert_not_called()

    def test_existing_roster_is_retained(self):
        expected = {
            "WorkflowShellTests": (
                "test_windows_candidates_prefer_git_over_wsl_for_all_git_layouts",
                "test_active_msys_keeps_its_path_shell_and_deduplicates_git_siblings",
                "test_lookup_excludes_empty_and_relative_path_entries_and_cwd_results",
                "test_native_windows_resolution_uses_absolute_git_path_as_one_argument",
                "test_windows_rejects_wsl_collision_and_tries_git_siblings",
                "test_posix_lookup_preserves_path_selection_and_does_not_query_git",
                "test_overrides_require_absolute_regular_executable_files_without_fallback",
                "test_probe_refuses_partial_output_wsl_errors_deadline_and_unusable_binaries",
                "test_probe_does_not_run_inherited_shell_startup_hooks",
                "test_resolved_bash_executes_literal_arguments_cwd_crlf_and_exit_status",
            ),
            "WorkflowShellDiagnosticTests": (
                "test_failure_reports_actual_absolute_discovery_and_allowlisted_platform_context",
                "test_nested_discovery_keeps_failure_observations_isolated",
                "test_windows_failure_retains_discovery_order_drive_paths_and_probe_budget",
                "test_validation_failures_name_the_last_stage_without_extra_checks_or_fallback",
                "test_validation_exceptions_report_stage_and_codes_without_exception_payload",
                "test_probe_refusals_preserve_raw_bytes_returncode_and_resolved_path",
                "test_launch_and_deadline_failures_report_codes_and_available_partial_bytes",
                "test_failure_bounds_apply_before_formatting_all_context_candidates_and_output",
                "test_success_stays_silent_after_failed_probe_and_keeps_exact_success_contract",
                "test_actual_probe_distinguishes_empty_bash_version_on_both_platform_branches",
                "test_actual_windows_guard_reports_empty_and_non_msys_intrinsics",
                "test_actual_refusal_observes_changed_decoded_quotes_without_evaluating_command",
                "test_actual_probe_success_retains_only_exact_magic_on_both_platform_branches",
                "test_actual_windows_probe_accepts_legacy_msys_and_documented_cygwin",
                "test_actual_windows_probe_refuses_other_identities_despite_msystem",
                "test_actual_documented_cygwin_identity_still_refuses_empty_bash_version",
                "test_actual_abort_without_guard_observations_leaves_body_stage_unknown",
                "test_actual_guard_observation_write_failures_never_reach_success_magic",
                "test_actual_guard_observations_filter_hooks_and_do_not_dump_environment",
                "test_actual_guard_observations_bound_intrinsics_and_decoded_command_and_escape_controls",
                "test_byte_observation_keeps_display_bound_before_formatting_and_retains_command_tail",
                "test_refusal_observations_cannot_make_unknown_output_or_guard_status_successful",
            ),
            "WorkflowExpressionPolicyTests": (
                "test_status_functions_are_only_used_in_if_values",
                "test_status_expression_contexts",
            ),
            "WorkflowDraftPolicyTests": (
                "test_ready_and_draft_transitions_trigger_existing_pr_routes",
                "test_guard_runs_first_under_existing_check_name_without_draft_job_skip",
                "test_actual_guard_script_rejects_drafts_but_allows_ready_and_non_pr_events",
                "test_failed_guard_cannot_be_bypassed_by_build_or_evidence_steps",
                "test_unconditional_always_on_shard_build_fails_draft_policy",
                "test_other_heavy_jobs_still_skip_drafts_and_keep_routed_skips",
                "test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence",
                "test_native_matrices_aggregates_limits_and_full_qualification_route",
                "test_native_cache_writers_and_attempt_specific_retained_evidence",
                "test_gcc_preserves_prefab_netsoak_packaging_and_diagnostics",
                "test_acceptance_execution_uses_verified_union_and_retains_networking_evidence",
                "test_ci_and_asan_bash_scripts_parse_without_executing_native_commands",
                "test_asan_aggregate_requires_inventory_floor_after_merge",
                "test_asan_aggregate_accepts_stronger_inventory_checks_after_merge",
                "test_asan_aggregate_rejects_removed_or_replaced_required_tests",
                "test_asan_aggregate_rejects_invalid_inventory_and_coverage_commands",
                "test_asan_aggregate_only_rerun_downloads_all_original_shards",
                "test_asan_single_shard_rerun_downloads_latest_and_unchanged_shards",
                "test_asan_rerun_evidence_retained_for_full_30_day_window",
                "test_aggregate_status_conditions_use_valid_contexts",
                "test_aggregates_cancel_at_each_phase_and_fail_invalid_dependencies",
                "test_asan_aggregate_accepts_only_success_or_the_original_path_skip",
                "test_native_aggregates_refuse_invalid_dependencies_before_the_route_gate",
                "test_ucrt_cache_guards_keep_saves_on_trusted_pushes_and_bypass_captures",
                "test_concurrency_keeps_pr_cancellation_and_batches_only_devel_pushes",
                "test_policy_suite_is_part_of_cheap_inventory_check",
                "test_freeze_audit_comment_is_identical_in_every_workflow",
            ),
            "WorkflowRequiredAggregatePolicyTests": (
                "test_required_aggregates_cancel_running_jobs_but_fail_cancelled_dependencies",
                "test_required_aggregate_graph_rejects_cancellation_regressions",
            ),
            "WorkflowUcrtClassifierPolicyTests": (
                "test_ucrt_route_seeds_devel_and_preserves_other_event_and_path_guards",
            ),
            "WorkflowBaseEditPolicyTests": (
                "test_edited_without_base_change_skips_every_job_and_uses_distinct_names",
                "test_edited_base_change_preserves_normal_ready_draft_and_path_routing",
                "test_metadata_edit_concurrency_is_unique_and_cannot_cancel_validation",
                "test_retarget_to_main_runs_ucrt64_without_label_or_sensitive_paths",
            ),
            "WorkflowCacheCleanupPolicyTests": (
                "test_only_cache_writer_completions_trigger_serialized_cleanup",
                "test_completion_list_covers_every_cache_saving_workflow",
                "test_cleanup_filter_accepts_only_main_and_devel_push_completions",
            ),
            "WorkflowAttributionPolicyTests": (
                "test_only_input_changes_trigger_devel_pull_requests",
                "test_devel_pushes_also_cover_probe_build_definitions",
                "test_weekly_schedule_and_manual_dispatch_remain_available",
                "test_concurrency_separates_schedules_pushes_and_manual_runs",
                "test_revision_recording_handles_missing_pull_request_metadata",
            ),
        }
        module = sys.modules[__name__]
        for name, methods in expected.items():
            with self.subTest(test_class=name):
                self.assertLessEqual(set(methods), set(unittest.defaultTestLoader.getTestCaseNames(getattr(module, name))))
        self.assertEqual(sum(map(len, expected.values())), 76)

    def test_partitions_cover_discovered_old_and_new_cases_exactly_once(self):
        module = sys.modules[__name__]
        loader = unittest.TestLoader()
        complete = [case.id() for case in policy_cases(loader.loadTestsFromModule(module))]
        selected = []
        for partition in POLICY_PARTITIONS:
            cases = list(policy_cases(policy_partition_suite(loader, module, partition)))
            self.assertTrue(cases)
            selected.extend(case.id() for case in cases)
        self.assertEqual(len(selected), len(set(selected)))
        self.assertCountEqual(selected, complete)
        self.assertTrue(any("WorkflowPartitionSelectionTests." in name for name in selected))
        self.assertEqual(set(POLICY_CLASS_PARTITIONS),
                         {type(case).__name__ for case in policy_cases(loader.loadTestsFromModule(module))})
        # New methods in known classes enter the same exhaustive selection.
        class Extended(WorkflowShellTests):
            def test_added_case(self):
                pass
        Extended.__name__ = "WorkflowShellTests"
        extra = Extended("test_added_case")
        augmented = unittest.TestSuite([loader.loadTestsFromModule(module), extra])
        with patch.object(loader, "loadTestsFromModule", return_value=augmented):
            cases = list(policy_cases(policy_partition_suite(loader, module, "4")))
        self.assertIn(extra, cases)

    def test_selection_refuses_invalid_empty_duplicate_missing_and_unknown_rosters(self):
        module = sys.modules[__name__]
        loader = unittest.TestLoader()
        for invalid in ("", "-1", "13", "01", "0/13", None, 0):
            with self.subTest(partition=invalid), self.assertRaises(ValueError):
                policy_partition_suite(loader, module, invalid)
        complete = list(policy_cases(loader.loadTestsFromModule(module)))
        dedicated = next(case for case in complete if type(case).__name__ + "." + case._testMethodName in POLICY_CASE_PARTITIONS)
        class Unknown(unittest.TestCase):
            def test_unknown(self):
                pass
        for cases, partition, message in (
            ([], "0", "missing dedicated"),
            ([*complete, complete[0]], "0", "duplicate"),
            ([case for case in complete if case is not dedicated], "0", "missing dedicated"),
            ([*complete, Unknown("test_unknown")], "0", "unregistered"),
            ([case for case in complete if type(case).__name__ != "WorkflowShellTests"], "4", "empty"),
        ):
            with self.subTest(message=message), patch.object(loader, "loadTestsFromModule", return_value=unittest.TestSuite(cases)):
                with self.assertRaisesRegex(ValueError, message):
                    policy_partition_suite(loader, module, partition)

    def test_selection_refuses_unassigned_classes_and_invalid_mapping_targets(self):
        module = sys.modules[__name__]
        for mapping in (POLICY_CLASS_PARTITIONS, POLICY_CASE_PARTITIONS):
            for invalid in ("", "13", "unknown", None, 0):
                with self.subTest(mapping=list(mapping), invalid=invalid), patch.dict(
                        mapping, {next(iter(mapping)): invalid}):
                    with self.assertRaisesRegex(ValueError, "invalid policy partition mapping"):
                        policy_partition_suite(unittest.TestLoader(), module, "0")
        for name in POLICY_CLASS_PARTITIONS:
            remaining = {key: value for key, value in POLICY_CLASS_PARTITIONS.items() if key != name}
            with self.subTest(missing_class=name), patch.dict(POLICY_CLASS_PARTITIONS, remaining, clear=True):
                with self.assertRaisesRegex(ValueError, "unregistered policy class: " + name):
                    policy_partition_suite(unittest.TestLoader(), module, "0")

    def test_remaining_behavioural_groups_have_separate_complete_partitions(self):
        module = sys.modules[__name__]
        groups = {
            "10": ("WorkflowRequiredAggregatePolicyTests", {
                "test_required_aggregates_cancel_running_jobs_but_fail_cancelled_dependencies",
                "test_required_aggregate_graph_rejects_cancellation_regressions",
                "test_required_aggregate_verdicts_run_after_failure_and_cancellation",
                "test_required_aggregate_verdict_policy_rejects_weakened_conditions",
                "test_required_aggregate_verdict_grid_keeps_bounded_launches_and_step_isolation",
            }),
            "11": ("WorkflowUcrtClassifierPolicyTests", {
                "test_ucrt_route_seeds_devel_and_preserves_other_event_and_path_guards",
                "test_ucrt64_path_classifier_uses_merge_base_and_fails_open",
                "test_ucrt64_content_classifier_fails_open_on_git_errors",
                "test_ucrt64_classifier_fixtures_use_portable_subprocesses",
                "test_windows_docs_skip_precedes_checkout_and_keeps_unknown_classification_native",
                "test_windows_standalone_contract_step_runs_once_on_shard_one_and_full_capture",
            }),
            "12": ("WorkflowUcrtClassifierPolicyTests", {
                "test_windows_intentional_skip_gate_uses_real_workflow_inputs_and_refuses_uncertainty",
            }),
        }
        for partition, (name, methods) in groups.items():
            with self.subTest(partition=partition):
                cases = list(policy_cases(policy_partition_suite(unittest.TestLoader(), module, partition)))
                self.assertEqual({type(case).__name__ for case in cases}, {name})
                self.assertEqual({case._testMethodName for case in cases}, methods)

    def test_launch_heavy_cases_keep_individual_partitions(self):
        module = sys.modules[__name__]
        for partition, method in (
            ("1", "test_aggregates_cancel_at_each_phase_and_fail_invalid_dependencies"),
            ("2", "test_asan_aggregate_accepts_only_success_or_the_original_path_skip"),
            ("3", "test_ci_and_asan_bash_scripts_parse_without_executing_native_commands"),
            ("6", "test_actual_windows_probe_accepts_legacy_msys_and_documented_cygwin"),
            ("7", "test_actual_windows_probe_refuses_other_identities_despite_msystem"),
            ("8", "test_bootstrap_pins_full_events_main_prs_and_missing_or_invalid_outputs"),
            ("9", "test_native_aggregates_preserve_route_checks"),
            ("12", "test_windows_intentional_skip_gate_uses_real_workflow_inputs_and_refuses_uncertainty"),
        ):
            with self.subTest(partition=partition):
                cases = list(policy_cases(policy_partition_suite(unittest.TestLoader(), module, partition)))
                self.assertEqual([case._testMethodName for case in cases], [method])

    def test_cmake_registers_every_partition_once_with_unchanged_deadlines(self):
        cmake = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        registrations = re.findall(r"add_test\(\s*NAME ([\w]+)\s+COMMAND ([^)]*)\)", cmake)
        actual = [(name, " ".join(command.split())) for name, command in registrations
                  if "ci/test_workflow_policy.py" in command]
        expected = []
        for partition in POLICY_PARTITIONS:
            name = "WorkflowPolicyContract" + ("" if partition == "0" else "Part" + partition)
            expected.append((name, '\"${Python3_EXECUTABLE}\" -I -B '
                             '\"${CMAKE_CURRENT_SOURCE_DIR}/ci/test_workflow_policy.py\" --partition ' + partition
                             + (' --case-workers 2' if partition == POLICY_CASE_PARTITIONS[POLICY_ROUTE_CASE] else '')))
            properties = re.findall(r"set_tests_properties\(" + name + r"\s+PROPERTIES ([^)]*)\)", cmake)
            self.assertEqual([" ".join(item.split()) for item in properties], ['LABELS "ci;tools" TIMEOUT 30'
                             + (' PROCESSORS 2' if partition == POLICY_CASE_PARTITIONS[POLICY_ROUTE_CASE] else '')])
        self.assertCountEqual(actual, expected)
        self.assertEqual(len(actual), len(set(actual)))

    def test_direct_workflow_discovery_and_path_routes_keep_complete_suite(self):
        import ci_paths
        self.assertEqual(ci_paths.PYTHON_TESTS.count("tools/ci/test_workflow_policy.py"), 1)
        commands = ci_paths.python_test_commands(ROOT)
        self.assertEqual(commands.count(("tools/ci/test_workflow_policy.py",)), 1)
        self.assertIn(("tools/ci/test_replay_source_contract.py",), commands)
        for path in ("tools/ci/test_workflow_policy.py", "tools/CMakeLists.txt"):
            self.assertTrue(ci_paths.needs_cpp(path))
        self.assertTrue(ci_paths.needs_python("tools/ci/test_workflow_policy.py"))
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        inventory = jobs(workflow)["public-tree"]
        self.assertRegex(inventory, r"(?m)^          python3 tools/ci/test_workflow_policy.py$")
        self.assertNotRegex(inventory, r"test_workflow_policy.py[^\n]*--partition")
        discovered = unittest.TestLoader().discover(str(ROOT / "tools/ci"), pattern="test_workflow_policy.py")
        expected = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
        discovered_cases = list(policy_cases(discovered))
        expected_cases = list(policy_cases(expected))
        def roster(cases):
            return [(type(case).__name__, case._testMethodName) for case in cases]
        self.assertCountEqual(roster(discovered_cases), roster(expected_cases))
        # Package imports can add several module components to unittest IDs.
        with patch.object(expected_cases[0], "id", return_value="qualified." + expected_cases[0].id()):
            self.assertCountEqual(roster(discovered_cases), roster(expected_cases))

    def test_cli_listing_and_refusals_use_actual_selection(self):
        for partition in POLICY_PARTITIONS:
            with self.subTest(partition=partition), patch.object(sys, "stdout", new_callable=io.StringIO) as output:
                self.assertEqual(policy_main(["--partition", partition, "--list-tests"]), 0)
                expected = policy_partition_suite(unittest.TestLoader(), sys.modules[__name__], partition)
                self.assertEqual(output.getvalue().splitlines(), [case.id() for case in policy_cases(expected)])
        for argv in (["--partition"], ["--partition", "13"], ["--partition", "01"],
                     ["--partition", "0", "--partition", "1"], ["--list-tests"],
                     ["--partition", "0", "WorkflowShellTests"], ["--partition", "0", "--unknown"]):
            with self.subTest(argv=argv), patch.object(sys, "stderr", new_callable=io.StringIO), self.assertRaises(SystemExit) as error:
                policy_main(argv)
            self.assertEqual(error.exception.code, 2)
        with patch.object(unittest, "main") as run:
            self.assertEqual(policy_main([]), 0)
            run.assert_called_once_with(argv=[sys.argv[0]])

    def test_partition_runner_propagates_case_failure_and_reports_duration(self):
        class Fails(unittest.TestCase):
            def test_failure(self):
                self.fail("partition failure")
        suite = unittest.TestSuite([Fails("test_failure")])
        with patch.dict(os.environ, {}), patch.object(sys, "stderr", new_callable=io.StringIO) as output, patch.object(
                sys.modules[__name__], "policy_partition_suite", return_value=suite):
            self.assertEqual(policy_main(["--partition", "0"]), 1)
        self.assertIn("partition failure", output.getvalue())
        self.assertRegex(output.getvalue(), r"test_failure: [0-9]+\.[0-9]{3}s")
        timing = PolicyTimingResult(io.StringIO(), True, 2)
        with patch.object(time, "perf_counter", side_effect=OSError("timer unavailable")):
            with self.assertRaisesRegex(OSError, "timer unavailable"):
                timing.startTest(Fails("test_failure"))
        self.assertEqual(timing.testsRun, 0)

    def test_qualified_import_keeps_complete_partition_roster_and_listing(self):
        import importlib.util
        name = "policy_selection.qualified"
        spec = importlib.util.spec_from_file_location(name, Path(__file__))
        module = importlib.util.module_from_spec(spec)
        # Reuse the already vetted shell; this checks module/selection semantics.
        with patch.dict(sys.modules, {name: module}), patch.object(shell, "bash_executable", return_value=BASH):
            spec.loader.exec_module(module)
            complete = list(module.policy_cases(unittest.TestLoader().loadTestsFromModule(module)))
            selected = []
            for partition in module.POLICY_PARTITIONS:
                selected.extend(module.policy_cases(module.policy_partition_suite(unittest.TestLoader(), module, partition)))
            self.assertEqual(len(selected), len({case.id() for case in selected}))
            self.assertCountEqual([case.id() for case in selected], [case.id() for case in complete])
            expected = policy_cases(unittest.TestLoader().loadTestsFromModule(sys.modules[__name__]))
            self.assertCountEqual([(type(case).__name__, case._testMethodName) for case in complete],
                                  [(type(case).__name__, case._testMethodName) for case in expected])
            with patch.object(sys, "stdout", new_callable=io.StringIO) as output:
                self.assertEqual(module.policy_main(["--partition", "0", "--list-tests"]), 0)
            part_zero = module.policy_partition_suite(unittest.TestLoader(), module, "0")
            self.assertEqual(output.getvalue().splitlines(), [case.id() for case in module.policy_cases(part_zero)])


if __name__ == "__main__":
    raise SystemExit(policy_main())

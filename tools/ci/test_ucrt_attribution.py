#!/usr/bin/env python3
"""Exercise the complete attribution script with isolated command stubs."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("ucrt_attribution.sh")
BASH = None
COMMANDS = (
    "awk", "basename", "cat", "cmake", "cmp", "cp", "curl", "date", "dirname",
    "find", "g++", "grep", "head", "mkdir", "mv", "ninja", "pacman", "pgrep",
    "pkill", "rm", "sed", "sha256sum", "sleep", "tail", "tr", "tasklist.exe",
    "taskkill.exe", "wmic", "which",
)

# Every command used by the shell is a Bash function calling this dispatcher.
# It uses only Python's standard library; it never forwards to a host executable.
STUB = r'''
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys

sys.stdout.reconfigure(newline="\n")
sys.stderr.reconfigure(newline="\n")

root = Path.cwd().resolve()
name, *args = sys.argv[1:]
with (root / "calls.jsonl").open("a", encoding="utf-8") as stream:
    stream.write(json.dumps([name, *args]) + "\n")
scenario = os.environ["STUB_SCENARIO"]

def local(value):
    path = Path(value).resolve()
    path.relative_to(root)
    return path

def input_text():
    return sys.stdin.read()

if name == "mkdir":
    for arg in args:
        if not arg.startswith("-"):
            local(arg).mkdir(parents=True, exist_ok=True)
elif name == "rm":
    for arg in args:
        if not arg.startswith("-"):
            path = local(arg)
            if path.is_dir():
                shutil.rmtree(path)
            elif path.exists():
                path.unlink()
elif name == "mv":
    source, destination = [arg for arg in args if not arg.startswith("-")]
    local(source).replace(local(destination))
elif name == "cp":
    source, destination = [arg for arg in args if not arg.startswith("-")]
    shutil.copyfile(local(source), local(destination))
elif name == "cmp":
    source, destination = [arg for arg in args if not arg.startswith("-")]
    sys.exit(0 if local(source).read_bytes() == local(destination).read_bytes() else 1)
elif name == "cat":
    for arg in args:
        sys.stdout.write(local(arg).read_text(encoding="utf-8"))
elif name == "date":
    print("1000.000000000" if args == ["+%s.%N"] else
          "1000" if args == ["+%s"] else "2000-01-01T00:00:00Z")
elif name in ("basename", "dirname"):
    print(Path(args[0]).name if name == "basename" else str(Path(args[0]).parent))
elif name == "pacman":
    state_file = root / "package-state.json"
    if state_file.exists():
        packages = json.loads(state_file.read_text(encoding="utf-8"))
        prefix = "mingw-w64-ucrt-x86_64-"
        if args[0] == "-Q":
            selected = packages if len(args) == 1 else {args[1]: packages[args[1]]} if args[1] in packages else {}
            for package, version in sorted(selected.items()):
                print(package + " " + version)
            sys.exit(0 if selected else 1)
        if args[:2] != ["-U", "--noconfirm"]:
            raise AssertionError("unexpected package operation: " + repr(args))
        incoming = {}
        for archive in args[2:]:
            path = local(archive)
            if not path.is_file() or not local(archive + ".sig").is_file():
                raise AssertionError("unstaged package or signature: " + archive)
            for package in sorted(packages, key=len, reverse=True):
                start = package + "-"
                if path.name.startswith(start) and path.name.endswith("-any.pkg.tar.zst"):
                    incoming[package] = path.name[len(start):-len("-any.pkg.tar.zst")]
                    break
            else:
                raise AssertionError("attempt to introduce absent package: " + archive)
        proposed = {**packages, **incoming}
        if incoming.get(prefix + "cc-libs") == "16.2.0-4":
            if scenario == "cohort_unknown_dependency":
                print("unknown dependency refusal", file=sys.stderr)
                sys.exit(1)
            if proposed.get(prefix + "libatomic") != "16.2.0-4":
                print("libatomic=16.2.0-4 is required", file=sys.stderr)
                sys.exit(1)
        state_file.write_text(json.dumps(proposed), encoding="utf-8")
        sys.exit(0)
    if args[0] != "-Q":
        raise AssertionError("unexpected package transaction in baseline-only fixture")
    package = "mingw-w64-ucrt-x86_64-crt"
    if len(args) == 1 or args[1] == package:
        print(package + " 1.0-1")
    else:
        sys.exit(1)
elif name == "curl":
    if scenario == "staging_failure":
        print("fixture download refused", file=sys.stderr)
        sys.exit(7)
    local(args[args.index("-o") + 1]).write_bytes(b"fixture archive")
elif name == "sha256sum":
    for arg in args:
        print(hashlib.sha256(local(arg).read_bytes()).hexdigest() + "  " + arg)
elif name == "awk":
    if args[-1] == '{print $2}':
        for line in input_text().splitlines():
            print(line.split()[1])
    elif "BEGIN" in args[-1]:
        print("0.000")
    elif args[0] == "-F\",\"":
        for line in input_text().splitlines():
            parts = line.strip('"').split('\",\"')
            print(parts[1] + " " + parts[0])
    else:
        raise AssertionError("unexpected awk arguments: " + repr(args))
elif name == "sed":
    expression = args[1] if args[0] == "-n" else args[0]
    data = input_text()
    if expression == "2p":
        lines = data.splitlines()
        if len(lines) >= 2:
            print(lines[1])
    elif expression == 's/[[:space:]]*$//':
        sys.stdout.write(data.rstrip() + "\n")
    elif "surface ready" in expression:
        data = local(args[-1]).read_text(encoding="utf-8")
        for match in re.finditer(r"surface ready \(took ([0-9]+)ms\)", data):
            print(match.group(1))
    else:
        raise AssertionError("unexpected sed expression: " + expression)
elif name in ("head", "tail"):
    lines = input_text().splitlines(keepends=True)
    sys.stdout.write("".join(lines[:1] if name == "head" else lines[-1:]))
elif name == "tr":
    sys.stdout.write(input_text().replace("\r", ""))
elif name == "grep":
    if args[0] == "-qw":
        data = local(args[2]).read_text(encoding="utf-8")
        sys.exit(0 if re.search(r"\b" + re.escape(args[1]) + r"\b", data) else 1)
    elif args[0] == "-qE":
        sys.exit(0 if re.search(r'^"[^"]+","[0-9]+"', input_text()) else 1)
    else:
        raise AssertionError("unexpected grep arguments: " + repr(args))
elif name == "cmake":
    if args == ["--version"]:
        print("cmake fixture")
    elif "--preset" in args:
        local(args[args.index("-B") + 1]).mkdir(parents=True, exist_ok=True)
    elif "--target" in args:
        target = args[args.index("--target") + 1]
        if target == "help":
            print("initial_world_loading_perf_test")
            if scenario != "missing_target":
                print("worldgen_probe")
            if scenario.startswith("help_"):
                print("fixture target listing failure", file=sys.stderr)
                sys.exit(int(scenario.split("_")[1]))
        elif target in ("initial_world_loading_perf_test", "worldgen_probe"):
            directory = local(args[1]) / "bin"
            directory.mkdir(parents=True, exist_ok=True)
            (directory / target).write_text("fixture executable", encoding="utf-8")
        else:
            raise AssertionError("unexpected target: " + target)
    else:
        raise AssertionError("unexpected cmake arguments: " + repr(args))
elif name == "probe":
    local(args[args.index("--json") + 1]).write_text('{"fixture": true}', encoding="utf-8")
elif name == "perf":
    print("surface ready (took 12ms)")
elif name == "pgrep":
    sys.exit(1)
elif name in ("pkill", "taskkill.exe"):
    raise AssertionError("unexpected process termination")
elif name == "tasklist.exe":
    print('"fixture.exe","42","Console","1","0 K"')
elif name == "wmic":
    print("Name\nFixture CPU")
elif name == "which":
    print("fixture-compiler")
elif name in ("g++", "ninja"):
    print("fixture tool")
elif name in ("timeout", "find", "sleep"):
    pass
else:
    raise AssertionError("unstubbed command: " + name)
'''


class AttributionTests(unittest.TestCase):
    def run_script(self, scenario, missing=None, cells="0", packages=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            if packages is not None:
                state = {"mingw-w64-ucrt-x86_64-" + name: version
                         for name, version in packages.items()}
                (root / "package-state.json").write_text(json.dumps(state), encoding="utf-8")
            isolated_path = root / "empty-bin"
            isolated_path.mkdir()
            script = root / "ucrt_attribution.sh"
            shutil.copyfile(SCRIPT, script)
            dispatcher = root / "stub.py"
            dispatcher.write_text(STUB, encoding="utf-8")
            prefix = root / "prefix"
            (prefix / "bin").mkdir(parents=True)
            for dll in ("libwinpthread-1.dll", "libstdc++-6.dll", "libgcc_s_seh-1.dll"):
                (prefix / "bin" / dll).write_bytes(b"fixture DLL")
            environment_file = root / "stub-env.sh"
            python = shlex.quote(Path(sys.executable).as_posix())
            dispatch = shlex.quote(dispatcher.as_posix())
            functions = [f'_stub() {{ {python} {dispatch} "$@"; }}']
            for command in COMMANDS:
                if command != missing:
                    functions.append(f'{command}() {{ _stub {shlex.quote(command)} "$@"; }}')
            # Dispatch only to the explicit functions above or measured stubs.
            # No executable lookup can reach the host through the empty PATH.
            functions.append('''timeout() {
    _stub timeout "$@" || return "$?"
    shift 2
    case "$1" in
        */worldgen_probe) shift; _stub probe "$@" ;;
        */initial_world_loading_perf_test) shift; _stub perf "$@" ;;
        *) "$@" ;;
    esac
}''')
            environment_file.write_bytes(("\n".join(functions) + "\n").encode("utf-8"))
            # Keep only OS variables Python/Bash need on Windows; discard shell
            # startup hooks and CI/package settings inherited from the host.
            environment = {key: value for key, value in os.environ.items()
                           if key.upper() in ("SYSTEMROOT", "WINDIR", "COMSPEC", "TEMP", "TMP")}
            environment.update(PATH=isolated_path.as_posix(),
                               BASH_ENV=environment_file.as_posix(),
                               STUB_SCENARIO=scenario, MSYSTEM="UCRT64",
                               MSYSTEM_PREFIX=prefix.as_posix(), CELLS=cells,
                               GITHUB_STEP_SUMMARY=(root / "step-summary.md").as_posix())
            result = subprocess.run([BASH, "--noprofile", "--norc", script.as_posix()],
                                    cwd=root, env=environment, capture_output=True,
                                    text=True, timeout=20)
            artifacts = {path.relative_to(root).as_posix(): path.read_text(encoding="utf-8")
                         for path in (root / "diag").rglob("*") if path.is_file()}
            state_path = root / "package-state.json"
            if state_path.exists():
                artifacts["fixture/package-state.json"] = state_path.read_text(encoding="utf-8")
            calls_path = root / "calls.jsonl"
            calls = [json.loads(line) for line in calls_path.read_text(encoding="utf-8").splitlines()]
            step_summary = (root / "step-summary.md").read_text(encoding="utf-8")
            return result, artifacts, calls, step_summary

    def assert_finalized(self, artifacts, step_summary, status):
        self.assertIn(f"Experiment status: {status}", artifacts["diag/summary.md"])
        self.assertEqual(artifacts["diag/summary.md"], step_summary)
        self.assertIn("diag/rows.md", artifacts)

    def test_missing_cmp_fails_before_baseline_and_finalizes(self):
        result, artifacts, calls, step_summary = self.run_script("good", missing="cmp")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assert_finalized(artifacts, step_summary, "INFRASTRUCTURE_FAILED")
        self.assertIn("missing required commands: cmp", artifacts["diag/reason.txt"])
        self.assertFalse(any(call[0] in ("pacman", "curl", "cmake", "pgrep", "pkill", "tasklist.exe")
                             for call in calls))

    def test_staging_failure_is_nonzero_and_finalizes(self):
        result, artifacts, calls, step_summary = self.run_script("staging_failure")
        self.assertNotEqual(result.returncode, 0, result.stderr)
        self.assert_finalized(artifacts, step_summary, "STAGING_FAILED")
        self.assertTrue(any(call[0] == "curl" for call in calls))
        self.assertFalse(any(call[0] in ("cmake", "probe", "perf") for call in calls))
        self.assertEqual(artifacts["diag/rows.md"], "")

    def test_failed_help_ignores_partial_target_listing(self):
        for code, status in ((7, "BUILD_FAILED"), (124, "TIMED_OUT"), (137, "TIMED_OUT")):
            with self.subTest(code=code):
                result, artifacts, calls, step_summary = self.run_script(f"help_{code}")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_finalized(artifacts, step_summary, "COMPLETE")
                cell = json.loads(artifacts["diag/cell-0/status.json"])
                self.assertEqual(cell["status"], status)
                self.assertEqual(cell["probe_status"], status)
                self.assertEqual(cell["perf_status"], "NOT_RUN")
                self.assertIn("worldgen_probe", artifacts["diag/cell-0/targets.txt"])
                self.assertIn("fixture target listing failure", artifacts["diag/cell-0/targets.stderr.txt"])
                self.assertFalse(any(call[0] in ("probe", "perf") for call in calls))
                builds = [call for call in calls if call[0] == "cmake" and "--target" in call]
                self.assertEqual(len(builds), 1)
                self.assertIn("help", builds[0])

    def test_successful_listing_without_probe_is_not_ok(self):
        result, artifacts, calls, step_summary = self.run_script("missing_target")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_finalized(artifacts, step_summary, "COMPLETE")
        cell = json.loads(artifacts["diag/cell-0/status.json"])
        self.assertEqual(cell["status"], "TARGET_MISSING")
        self.assertEqual(cell["probe_status"], "TARGET_MISSING")
        self.assertEqual(cell["perf_status"], "OK")
        self.assertTrue(any(call[0] == "perf" for call in calls))
        self.assertFalse(any(call[0] == "probe" for call in calls))

    def test_good_path_records_measurements_and_finalizes(self):
        result, artifacts, calls, step_summary = self.run_script("good")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_finalized(artifacts, step_summary, "COMPLETE")
        cell = json.loads(artifacts["diag/cell-0/status.json"])
        self.assertEqual((cell["status"], cell["probe_status"], cell["perf_status"]), ("OK", "OK", "OK"))
        self.assertEqual(cell["prep_ms"], 12)
        self.assertIsInstance(cell["wall_seconds"], (int, float))
        self.assertIn("staged runtime dll sha256", artifacts["diag/cell-0/identity.txt"])
        self.assertEqual(sum(call[0] == "cmp" for call in calls), 3)
        measured = [call[0] for call in calls if call[0] in ("probe", "perf")]
        self.assertEqual(measured, ["probe", "perf"])



if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--bash")
    options, remaining = parser.parse_known_args()
    BASH = options.bash or shutil.which("bash")
    if not BASH:
        raise SystemExit("Bash is required for attribution regression tests")
    unittest.main(argv=[sys.argv[0], *remaining])

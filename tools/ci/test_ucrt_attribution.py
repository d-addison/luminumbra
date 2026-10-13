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
BASH = shutil.which("bash")
COMMANDS = (
    "awk", "basename", "cat", "cmake", "cmp", "cp", "curl", "date", "dirname",
    "find", "g++", "grep", "head", "mkdir", "mv", "ninja", "pacman", "pgrep",
    "pkill", "rm", "sed", "sha256sum", "sleep", "tail", "tr", "tasklist.exe",
    "taskkill.exe", "wmic", "which",
)

# Slow filesystem operations share one Python dispatcher for the whole run.
# Hot queries, filters and call recording use Bash builtins below; no mock
# forwards to a host executable.
STUB = r'''
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

root = Path.cwd().resolve()
name, *args = sys.argv[1:]
scenario = os.environ["STUB_SCENARIO"]

def local(value):
    path = Path(value).resolve()
    path.relative_to(root)
    return path

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
elif name == "pacman":
    state_file = root / "package-state.json"
    if state_file.exists():
        packages = json.loads(state_file.read_text(encoding="utf-8"))
        prefix = "mingw-w64-ucrt-x86_64-"
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
        (root / "package-snapshot.txt").write_text(
            "".join(package + " " + version + "\n"
                    for package, version in sorted(proposed.items())), encoding="utf-8")
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
else:
    raise AssertionError("unstubbed command: " + name)
'''

# NUL-framed requests/responses preserve spaces, quotes, newlines and Windows
# paths without eval or shell escaping. One interpreter serves the entire run,
# including all file operations and each checked package transaction.
WORKER = r'''
import contextlib
import io
import sys
import traceback

code = compile(STUB, "stub.py", "exec")
reader, writer = sys.stdin.buffer, sys.stdout.buffer

def field():
    data = bytearray()
    while True:
        char = reader.read(1)
        if not char:
            if data:
                raise EOFError("truncated mock request")
            return None
        if char == b"\0":
            return data.decode("utf-8")
        data.extend(char)

while True:
    count = field()
    if count is None:
        break
    args = [field() for _ in range(int(count))]
    output, error = io.StringIO(), io.StringIO()
    status = 0
    with contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
        try:
            sys.argv = ["stub.py", *args]
            exec(code, {})
        except SystemExit as exc:
            status = exc.code or 0
        except Exception:
            traceback.print_exc()
            status = 1
    for value in (str(status), output.getvalue(), error.getvalue()):
        writer.write(value.encode("utf-8") + b"\0")
    writer.flush()
'''

BASH_STUB = r'''
_record() {
    # One append per call, including calls made in pipeline subshells.
    printf '%s\0' "$#" "$@" >> calls.bin
}
_stub() {
    _record "$@"
    local name="$1" line arg package version found=1 expression number=0 last=""
    shift
    case "$name" in
        pacman)
            if [[ "$1" != -Q ]]; then
                _python_dispatch pacman "$@"
                return "$?"
            fi
            while read -r package version; do
                version="${version%$'\r'}"
                if [[ $# == 1 || "$package" == "$2" ]]; then
                    printf '%s %s\n' "$package" "$version"
                    found=0
                fi
            done < package-snapshot.txt
            return "$found"
            ;;
        date)
            case "$*" in
                '+%s.%N') printf '1000.000000000\n' ;;
                '+%s') printf '1000\n' ;;
                *) printf '2000-01-01T00:00:00Z\n' ;;
            esac
            ;;
        pgrep) return 1 ;;
        find|sleep) : ;;
        pkill|taskkill.exe)
            printf 'unexpected process termination\n' >&2; return 1 ;;
        tasklist.exe) printf '"fixture.exe","42","Console","1","0 K"\n' ;;
        wmic) printf 'Name\nFixture CPU\n' ;;
        which) printf 'fixture-compiler\n' ;;
        g++|ninja) printf 'fixture tool\n' ;;
        perf) printf 'surface ready (took 12ms)\n' ;;
        basename) arg="${1%/}"; printf '%s\n' "${arg##*/}" ;;
        dirname)
            arg="${1%/}"
            if [[ "$arg" == */* ]]; then
                printf '%s\n' "${arg%/*}"
            else
                printf '.\n'
            fi
            ;;
        cat)
            for arg in "$@"; do
                {
                    while IFS= read -r line; do printf '%s\n' "$line"; done
                    [[ -z "$line" ]] || printf '%s' "$line"
                } < "$arg" || return "$?"
            done
            return 0
            ;;
        awk)
            if [[ "${@: -1}" == '{print $2}' ]]; then
                while read -r package version last; do
                    printf '%s\n' "$version"
                done
            elif [[ "${@: -1}" == *BEGIN* ]]; then
                printf '0.000\n'
            elif [[ "$1" == '-F","' ]]; then
                while IFS= read -r line || [[ -n "$line" ]]; do
                    line="${line#\"}"; package="${line%%\",\"*}"
                    line="${line#*\",\"}"; version="${line%%\",\"*}"
                    printf '%s %s\n' "$version" "$package"
                done
            else
                printf 'unexpected awk arguments\n' >&2; return 1
            fi
            ;;
        sed)
            [[ "$1" != -n ]] || shift
            expression="$1"; shift
            # Choose the source before reading. A file operand must never wait
            # for the unrelated stdin inherited from the test runner.
            if (( $# )); then
                for arg in "$@"; do _sed_filter "$expression" < "$arg" || return "$?"; done
            else
                _sed_filter "$expression"
            fi
            ;;
        head|tail|tr)
            while IFS= read -r line || [[ -n "$line" ]]; do
                if [[ "$name" == tr ]]; then
                    printf '%s\n' "${line//$'\r'/}"
                elif [[ "$name" == head && "$number" == 0 ]]; then
                    printf '%s\n' "$line"
                fi
                last="$line"; number=$((number + 1))
            done
            if [[ "$name" == tail && "$number" != 0 ]]; then printf '%s\n' "$last"; fi
            ;;
        grep)
            if [[ "$1" == -qw ]]; then
                expression="(^|[^[:alnum:]_])$2([^[:alnum:]_]|$)"
                while IFS= read -r line || [[ -n "$line" ]]; do
                    if [[ "$line" =~ $expression ]]; then found=0; fi
                done < "$3"
            elif [[ "$1" == -qE ]]; then
                expression='^"[^"]+","[0-9]+"'
                while IFS= read -r line || [[ -n "$line" ]]; do
                    if [[ "$line" =~ $expression ]]; then found=0; fi
                done
            else
                printf 'unexpected grep arguments\n' >&2; return 1
            fi
            return "$found"
            ;;
        timeout)
            shift 2
            case "$1" in
                */worldgen_probe) shift; _stub probe "$@" ;;
                */initial_world_loading_perf_test) shift; _stub perf "$@" ;;
                *) "$@" ;;
            esac
            ;;
        *) _python_dispatch "$name" "$@" ;;
    esac
}
_sed_filter() {
    local expression="$1" line number=0 match='surface ready \(took ([0-9]+)ms\)'
    while IFS= read -r line || [[ -n "$line" ]]; do
        number=$((number + 1))
        case "$expression" in
            2p) if (( number == 2 )); then printf '%s\n' "$line"; fi ;;
            's/[[:space:]]*$//')
                while [[ "$line" == *[[:space:]] ]]; do line="${line%?}"; done
                printf '%s\n' "$line"
                ;;
            *'surface ready'*)
                while [[ "$line" =~ $match ]]; do
                    printf '%s\n' "${BASH_REMATCH[1]}"
                    line="${line#*"${BASH_REMATCH[0]}"}"
                done
                ;;
            *) printf 'unexpected sed expression: %s\n' "$expression" >&2; return 1 ;;
        esac
    done
}
'''


def stub_environment(python, dispatcher, missing=None):
    functions = [f'''coproc MOCK_WORKER {{ {shlex.quote(python)} {shlex.quote(dispatcher)}; }}
# Duplicate coproc descriptors so pipeline/command-substitution subshells can
# use them. Only filesystem mocks use this synchronous channel; their consumers
# and every other command in a pipeline are handled entirely by Bash.
exec 8>&${{MOCK_WORKER[1]}} 9<&${{MOCK_WORKER[0]}}
exec {{MOCK_WORKER[1]}}>&- {{MOCK_WORKER[0]}}<&-
_python_dispatch() {{
    local status output error
    printf '%s\\0' "$#" "$@" >&8
    IFS= read -r -d '' status <&9 || return 1
    IFS= read -r -d '' output <&9 || return 1
    IFS= read -r -d '' error <&9 || return 1
    printf '%s' "$output"
    printf '%s' "$error" >&2
    return "$status"
}}''', BASH_STUB]
    for command in (*COMMANDS, "timeout"):
        if command != missing:
            functions.append(f'{command}() {{ _stub {shlex.quote(command)} "$@"; }}')
    return "\n".join(functions) + "\n"


def read_calls(path):
    fields = path.read_bytes().decode("utf-8").split("\0")
    calls = []
    index = 0
    while index < len(fields) - 1:
        count = int(fields[index])
        calls.append(fields[index + 1:index + 1 + count])
        index += count + 1
    if index != len(fields) - 1 or fields[-1]:
        raise AssertionError("truncated mock call record")
    return calls


class AttributionTests(unittest.TestCase):
    def run_script(self, scenario, missing=None, cells="0", packages=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            if packages is not None:
                state = {"mingw-w64-ucrt-x86_64-" + name: version
                         for name, version in packages.items()}
                (root / "package-state.json").write_text(json.dumps(state), encoding="utf-8")
            else:
                state = {"mingw-w64-ucrt-x86_64-crt": "1.0-1"}
            (root / "package-snapshot.txt").write_text(
                "".join(package + " " + version + "\n"
                        for package, version in sorted(state.items())), encoding="utf-8")
            isolated_path = root / "empty-bin"
            isolated_path.mkdir()
            script = root / "ucrt_attribution.sh"
            shutil.copyfile(SCRIPT, script)
            dispatcher = root / "stub.py"
            dispatcher.write_text("STUB = " + repr(STUB) + "\n" + WORKER, encoding="utf-8")
            prefix = root / "prefix"
            (prefix / "bin").mkdir(parents=True)
            for dll in ("libwinpthread-1.dll", "libstdc++-6.dll", "libgcc_s_seh-1.dll"):
                (prefix / "bin" / dll).write_bytes(b"fixture DLL")
            environment_file = root / "stub-env.sh"
            # Dispatch only to the explicit functions above or measured stubs.
            # No executable lookup can reach the host through the empty PATH.
            environment_file.write_bytes(stub_environment(
                Path(sys.executable).as_posix(), dispatcher.as_posix(), missing).encode("utf-8"))
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
                                    stdin=subprocess.DEVNULL, text=True, timeout=20)
            artifacts = {path.relative_to(root).as_posix(): path.read_text(encoding="utf-8")
                         for path in (root / "diag").rglob("*") if path.is_file()}
            state_path = root / "package-state.json"
            if state_path.exists():
                artifacts["fixture/package-state.json"] = state_path.read_text(encoding="utf-8")
            artifacts["fixture/package-snapshot.txt"] = (root / "package-snapshot.txt").read_text(encoding="utf-8")
            calls = read_calls(root / "calls.bin")
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


class MockTests(unittest.TestCase):
    def environment(self, root):
        environment = {key: value for key, value in os.environ.items()
                       if key.upper() in ("SYSTEMROOT", "WINDIR", "COMSPEC", "TEMP", "TMP")}
        isolated_path = root / "empty-bin"
        isolated_path.mkdir()
        environment.update(PATH=isolated_path.as_posix(), STUB_SCENARIO="good")
        return environment

    def test_hot_calls_use_builtins_with_one_batched_python_worker(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dispatcher = root / "stub.py"
            dispatcher.write_text("STUB = " + repr(STUB) + "\n" + WORKER, encoding="utf-8")
            (root / "package-snapshot.txt").write_bytes(b"fixture-package 1.0-1\r\n")
            (root / "targets.txt").write_bytes(b"worldgen_probe\r\n")
            python = shlex.quote(Path(sys.executable).as_posix())
            # Inspect and exercise the same generated functions as the full
            # script. Count interpreter launches independently of call records,
            # and make dispatching any hot operation to Python fail the test.
            functions = stub_environment("fixture_python", dispatcher.as_posix())
            self.assertEqual(functions.count("fixture_python"), 1)
            self.assertIn("coproc MOCK_WORKER { fixture_python ", functions)
            shell = f'''fixture_python() {{
    printf 'startup\\n' >> interpreter-starts.txt
    {python} "$@"
}}
''' + functions + r'''
_python_dispatch() { printf 'hot call reached Python\n' >&2; return 99; }
target() { _record target "$@"; return 7; }
set -euo pipefail
for (( index=0; index<4; index++ )); do
    [[ "$(pacman -Q fixture-package)" == 'fixture-package 1.0-1' ]]
    [[ "$(pacman -Q)" == 'fixture-package 1.0-1' ]]
    if pacman -Q absent; then exit 1; else [[ "$?" == 1 ]]; fi
    [[ "$(date +%s)" == 1000 ]]
    [[ "$(date +%s.%N)" == 1000.000000000 ]]
    if pgrep -x ninja; then exit 1; else [[ "$?" == 1 ]]; fi
    [[ "$(printf 'package 1.0-1\n' | awk '{print $2}')" == 1.0-1 ]]
    [[ "$(awk -v a=1000 -v b=1000 'BEGIN { printf "%.3f", b-a }')" == 0.000 ]]
    [[ "$(tasklist.exe //FO CSV //NH | awk -F'","' '{print $2 " " $1}')" == '42 fixture.exe' ]]
    [[ "$(printf 'Name\r\nFixture CPU  \r\n' | tr -d '\r' | sed -n '2p' | sed 's/[[:space:]]*$//')" == 'Fixture CPU' ]]
    [[ "$(printf 'first\nsecond\n' | head -n 1)" == first ]]
    [[ "$(printf 'first\nsecond\n' | tail -n 1)" == second ]]
    grep -qw worldgen_probe targets.txt
    tasklist.exe //FO CSV //NH | grep -qE '^"[^"]+","[0-9]+"'
    if timeout --kill-after=30s 1s target 'space and "quote"' 'C:/path with spaces' $'line\nnext' ''; then
        exit 1
    else
        [[ "$?" == 7 ]]
    fi
done
'''
            script = root / "hot-calls.sh"
            script.write_bytes(shell.encode("utf-8"))
            result = subprocess.run([BASH, "--noprofile", "--norc", script.as_posix()],
                                    cwd=root, env=self.environment(root), stdin=subprocess.DEVNULL,
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((root / "interpreter-starts.txt").read_text(encoding="utf-8"), "startup\n")
            calls = read_calls(root / "calls.bin")
            self.assertEqual(sum(call[0] == "pacman" for call in calls), 12)
            self.assertEqual(sum(call[0] == "pgrep" for call in calls), 4)
            targets = [call for call in calls if call[0] == "target"]
            self.assertEqual(targets, [["target", 'space and "quote"',
                                       "C:/path with spaces", "line\nnext", ""]] * 4)

    def test_file_operand_sed_does_not_read_open_stdin(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            # Include spaces in the Windows-compatible operand and CRLF input.
            (root / "performance log.txt").write_bytes(
                b"surface ready (took 12ms)\r\nsurface ready (took 34ms)\r\n")
            script = root / "sed-file.sh"
            script.write_bytes((BASH_STUB + r'''
_python_dispatch() { printf 'sed reached Python\n' >&2; return 99; }
_stub sed -n 's/.*surface ready (took \([0-9][0-9]*\)ms).*/\1/p' 'performance log.txt'
''').encode("utf-8"))
            process = subprocess.Popen([BASH, "--noprofile", "--norc", script.as_posix()],
                                       cwd=root, env=self.environment(root), stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                # communicate() closes stdin, which would hide the original
                # bug. Keep the writer open and send no bytes until Bash exits.
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                self.fail("file-operand sed waited for unrelated stdin")
            else:
                output, error = process.communicate()
                self.assertEqual(process.returncode, 0, error.decode("utf-8"))
                self.assertEqual(output, b"12\n34\n")
                self.assertEqual(read_calls(root / "calls.bin"), [[
                    "sed", "-n", r"s/.*surface ready (took \([0-9][0-9]*\)ms).*/\1/p",
                    "performance log.txt"]])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--bash")
    options, remaining = parser.parse_known_args()
    BASH = options.bash or shutil.which("bash")
    if not BASH:
        raise SystemExit("Bash is required for attribution regression tests")
    unittest.main(argv=[sys.argv[0], *remaining])

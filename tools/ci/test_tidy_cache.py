#!/usr/bin/env python3
"""Tests for scripts/tidy_cache.py: a cache hit must be impossible unless the
whole analysis input is identical to an earlier passing run."""

import importlib.util
import concurrent.futures
import contextlib
import io
import json
import ntpath
import os
import shlex
import shutil
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
from unittest.mock import patch

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
import os, sys
args = sys.argv[1:]
if args == ["--version"]:
    print("faketidy " + os.environ.get("FAKE_TIDY_VERSION", "1"))
    sys.exit(0)
target = args[-1]
with open(os.environ["FAKE_TIDY_LOG"], "a", encoding="utf-8") as log:
    log.write(target + "\\n")
swap = os.environ.get("FAKE_TIDY_DB_FROM")
if swap:
    import shutil
    shutil.copyfile(swap, os.environ["FAKE_TIDY_DB"])
mutate = os.environ.get("FAKE_TIDY_MUTATE")
if mutate:
    with open(mutate, "a", encoding="utf-8") as handle:
        handle.write("// changed while the analysis was running\\n")
if "TIDY_FAIL" in open(target, encoding="utf-8").read():
    print("fake diagnostic in " + target)
    sys.exit(1)
sys.exit(0)
"""


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
        original_run = subprocess.run

        def run_fixture(argv, **kwargs):
            # Preserve tool-file hashing, argv, output and exit status; only the
            # fixture launch needs an interpreter instead of a POSIX shebang.
            self.assertIn(Path(argv[0]), (self.cc, self.tidy))
            return original_run([sys.executable, "-I", "-B", *argv], **kwargs)

        out, err = io.StringIO(), io.StringIO()
        with patch.dict(os.environ, self.env, clear=True), patch.object(
            MODULE.subprocess, "run", side_effect=run_fixture
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
        with patch.object(MODULE.subprocess, "run", return_value=result) as version, \
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
        with patch.object(MODULE.subprocess, "run", return_value=result) as version, \
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
        with patch.object(MODULE.subprocess, "run", return_value=result) as version, \
                patch.object(MODULE, "sha256_file", wraps=MODULE.sha256_file) as content:
            before = self.tools.describe(str(self.tool), str(self.root))
            os.utime(self.tool, ns=(self.tick + 1, self.tick + 1))
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
                patch.object(MODULE.subprocess, "run", return_value=result) as version, \
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
                patch.object(MODULE.subprocess, "run", return_value=result) as version, \
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
        with patch.object(MODULE.subprocess, "run", return_value=result) as version:
            self.assertIn(MODULE.sha256_bytes(b"old tool"),
                          self.tools.describe(str(self.tool), str(self.root)))
            self.assertEqual(version.call_count, 1)

    def test_revalidation_failures_cannot_reuse_stale_identity(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        for failure in (PermissionError("unreadable"), OSError("partial read")):
            with self.subTest(failure=failure), \
                    patch.object(MODULE.subprocess, "run", return_value=result) as version, \
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
                with patch.object(MODULE.subprocess, "run", return_value=success):
                    before = self.tools.describe(str(self.tool), str(self.root))
                self.write_tool(b"new tool")
                with patch.object(MODULE.subprocess, "run", side_effect=[failure, success]) as version:
                    with self.assertRaises(MODULE.CacheMiss):
                        self.tools.describe(str(self.tool), str(self.root))
                    after = self.tools.describe(str(self.tool), str(self.root))
                    self.assertNotEqual(after, before)
                    self.assertIn(MODULE.sha256_bytes(b"new tool"), after)
                    self.assertEqual(self.tools.describe(str(self.tool), str(self.root)), after)
                    self.assertEqual(version.call_count, 2)

    def test_identity_observed_after_tick_closes_uses_fingerprint(self):
        result = subprocess.CompletedProcess([], 0, b"tool version\n", b"")
        with patch.object(MODULE.subprocess, "run", return_value=result) as version, \
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
                        patch.object(MODULE.subprocess, "run", return_value=result) as version, \
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
                patch.object(MODULE.subprocess, "run", return_value=result) as version, \
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

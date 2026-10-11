#!/usr/bin/env python3
"""Tests for scripts/tidy_cache.py: a cache hit must be impossible unless the
whole analysis input is identical to an earlier passing run."""

import importlib.util
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "tidy_cache.py"
SPEC = importlib.util.spec_from_file_location("tidy_cache", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

FAKE_COMPILER = f"""#!{sys.executable}
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

FAKE_TIDY = f"""#!{sys.executable}
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


def write_executable(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class TidyCacheTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="tidy-cache-test-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.src = self.root / "src"
        self.build = self.root / "build"
        self.system = self.root / "sysinc"
        self.cache = self.root / "cache"
        for directory in (self.src, self.build, self.system):
            directory.mkdir()
        self.cc = self.root / "fakecc"
        self.tidy = self.root / "faketidy"
        write_executable(self.cc, FAKE_COMPILER)
        write_executable(self.tidy, FAKE_TIDY)
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
            sys.executable, str(SCRIPT), "run", "--build-dir", str(self.build),
            "--cache-dir", str(self.cache), "--mode", mode, "--jobs", "2",
            "--clang-tidy", str(self.tidy), "--config", config,
            "--tool-file", str(tool_file or self.tool_file), "--", *map(str, files),
        ]
        return subprocess.run(command, env=self.env, capture_output=True, text=True, check=False)

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
        stale.mkdir()
        (stale / ("f" * 64 + ".json")).write_text("{}", encoding="utf-8")
        cutoff = time.time() + 5
        os.utime(record, (cutoff + 10, cutoff + 10))
        os.utime(stale / ("f" * 64 + ".json"), (cutoff - 100, cutoff - 100))
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "prune", "--cache-dir", str(self.cache), "--before", str(cutoff)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(record.exists())
        self.assertFalse((stale / ("f" * 64 + ".json")).exists())


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

#!/usr/bin/env python3
"""Regression tests for stable tidy partitions and the required aggregate check.

Shell integration uses only a recording stub: no compiler or analyzer is run.
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("tidy_cache", REPO_ROOT / "scripts/tidy_cache.py")
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ShardPartitionTests(unittest.TestCase):
    def test_partitions_are_disjoint_and_cover_all_units(self):
        files = [f"/checkout/src/unit{index}.cpp" for index in range(458)]
        shards = [set(MODULE.select_shard(files, "/checkout", (index, 3))) for index in (1, 2, 3)]
        self.assertEqual(set.union(*shards), set(files))
        self.assertEqual(sum(map(len, shards)), len(files))
        self.assertTrue(all(shards))

    def test_assignments_ignore_checkout_order_and_other_units(self):
        files = [f"src/unit{index}.cpp" for index in range(50)]
        for index in (1, 2, 3):
            original = MODULE.select_shard(["/first/" + path for path in files], "/first", (index, 3))
            moved = MODULE.select_shard(
                ["/second/" + path for path in reversed(files)] + ["/second/src/new.cpp"],
                "/second", (index, 3),
            )
            self.assertEqual(
                [path.removeprefix("/first/") for path in original],
                [path.removeprefix("/second/") for path in moved if not path.endswith("/new.cpp")],
            )
            subset = files[::2]
            self.assertEqual(
                MODULE.select_shard(["/first/" + path for path in subset], "/first", (index, 3)),
                [path for path in original if path.removeprefix("/first/") in subset],
            )

    def test_default_partition_sorts_and_deduplicates(self):
        self.assertEqual(MODULE.select_shard(["/repo/b.cpp", "/repo/a.cpp", "/repo/b.cpp"],
                                           "/repo", (1, 1)), ["/repo/a.cpp", "/repo/b.cpp"])
        self.assertEqual(MODULE.select_shard([], "/repo", (2, 3)), [])

    def test_validates_one_based_integer_arguments(self):
        self.assertEqual(MODULE.parse_shard("1/1"), (1, 1))
        self.assertEqual(MODULE.parse_shard("2/3"), (2, 3))
        self.assertEqual(MODULE.parse_shard("02/03"), (2, 3))
        for value in ("", "1", "1/", "/3", "0/3", "1/0", "4/3", "-1/3", "+1/3",
                      "1.0/3", "one/3", "1/2/3", "1 /3", "1/3\n", "١/٣"):
            with self.subTest(value=value), self.assertRaises(argparse.ArgumentTypeError):
                MODULE.parse_shard(value)

    def test_cli_preserves_spaces_and_newlines_with_nul_separators(self):
        files = ["/repo/src/space name.cpp", "/repo/test/new\nline.cpp"]
        result = subprocess.run(
            [sys.executable, "-I", str(REPO_ROOT / "scripts/tidy_cache.py"), "shard",
             "--repo-root", "/repo", "--", *files], capture_output=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(b"\0")[:-1], [os.fsencode(path) for path in sorted(files)])


class ShardShellTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tidy-shard-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "scripts").mkdir()
        for name in ("tidy.sh", "tidy_cache.py"):
            shutil.copyfile(REPO_ROOT / "scripts" / name, self.root / "scripts" / name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        stub = self.bin / "clang-tidy-18"
        stub.write_text(
            f"#!{sys.executable}\nimport json, os, sys\n"
            "with open(os.environ['STUB_LOG'], 'a', encoding='utf-8') as log:\n"
            "    log.write(json.dumps(sys.argv[1:]) + '\\n')\n"
            "sys.exit(int(os.environ.get('STUB_EXIT', '0')))\n", encoding="utf-8",
        )
        stub.chmod(0o755)
        self.log = self.root / "calls.jsonl"
        # A closed PATH guarantees that fallback tests cannot execute a real analyzer.
        for command in ("bash", "nproc", "dirname", "python3", "sort", "xargs", "date", "git"):
            (self.bin / command).symlink_to(shutil.which(command))
        self.env = dict(os.environ, PATH=str(self.bin),
                        STUB_LOG=str(self.log))
        self.files = ["src/luminumbra_client/core/RuntimeScenarioConfig.cpp",
                      "src/luminumbra_common/net/GnsTransport.cpp",
                      "src/regular.cpp", "test/space name.cpp", "tools/probe.cpp",
                      "src/luminumbra_client/ui/gl3/vendor.cpp", "vendor/external.cpp"]
        for path in self.files:
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("int declaration();\n", encoding="utf-8")
        build = self.root / "build/tidy"
        build.mkdir(parents=True)
        (build / "compile_commands.json").write_text(
            json.dumps([{"file": str(self.root / path)} for path in self.files]), encoding="utf-8",
        )

    def run_gate(self, *args):
        if self.log.exists():
            self.log.unlink()
        result = subprocess.run(["bash", str(self.root / "scripts/tidy.sh"), "--jobs", "1", *args],
                                env=self.env, capture_output=True, text=True, check=False)
        calls = [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()] \
            if self.log.exists() else []
        return result, calls

    def assert_selected(self, calls, expected):
        seen = []
        for call in calls:
            config = json.loads(next(arg.removeprefix("--config=") for arg in call if arg.startswith("--config=")))
            units = [str(Path(arg).relative_to(self.root)) for arg in call if arg.endswith(".cpp")]
            seen.extend(units)
            for unit in units:
                tracked = unit in self.files[:2]
                self.assertEqual(config["Checks"].endswith(
                    ",-cppcoreguidelines-avoid-non-const-global-variables"), tracked)
        self.assertCountEqual(seen, expected)

    def test_default_and_all_shards_keep_scope_and_tracked_global_configuration(self):
        result, calls = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("shard 1/1, 5 translation unit(s)", result.stdout)
        self.assert_selected(calls, self.files[:5])
        for index in (1, 2, 3):
            expected = MODULE.select_shard([str(self.root / path) for path in self.files[:5]],
                                           str(self.root), (index, 3))
            result, calls = self.run_gate("--shard", f"{index}/3")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"shard {index}/3, {len(expected)} translation unit(s)", result.stdout)
            self.assert_selected(calls, [str(Path(path).relative_to(self.root)) for path in expected])

    def test_empty_shard_succeeds_without_analyzer_calls(self):
        populated = {index for index in range(1, 21) if MODULE.select_shard(
            [str(self.root / path) for path in self.files[:5]], str(self.root), (index, 20))}
        empty = next(index for index in range(1, 21) if index not in populated)
        result, calls = self.run_gate("--shard", f"{empty}/20")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("0 translation unit(s)", result.stdout)
        self.assertIn("has nothing to analyse", result.stdout)
        self.assertEqual(calls, [])

    def test_bad_shard_exits_two_before_analysis(self):
        for args in (("--shard",), ("--shard", "0/3"), ("--shard", "4/3"),
                     ("--shard", "text"), ("--shard", "1/0"), ("--shard", "")):
            with self.subTest(args=args):
                result, calls = self.run_gate(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(calls, [])

    def test_missing_full_scope_still_fails_discovery(self):
        (self.root / "build/tidy/compile_commands.json").write_text("[]", encoding="utf-8")
        result, calls = self.run_gate("--shard", "2/3")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("No first-party translation units found", result.stderr)
        self.assertEqual(calls, [])

    def test_changed_filter_precedes_partition_and_includes_untracked_units(self):
        def git(*args):
            subprocess.run(["git", "-C", str(self.root), *args], check=True, capture_output=True)
        git("init", "--quiet")
        git("add", "src", "test")
        git("-c", "user.name=Test", "-c", "user.email=test@example.org", "commit", "--quiet", "-m", "Fixture")
        changed = self.files[0]
        (self.root / changed).write_text("int changed();\n", encoding="utf-8")
        # tools/probe.cpp remains untracked; unrelated tracked units stay excluded.
        for index in (1, 2, 3):
            expected = MODULE.select_shard([str(self.root / path) for path in (changed, self.files[4])],
                                           str(self.root), (index, 3))
            result, calls = self.run_gate("--changed-from", "HEAD", "--shard", f"{index}/3")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assert_selected(calls, [str(Path(path).relative_to(self.root)) for path in expected])

    def test_partition_helper_failure_cannot_report_an_empty_success(self):
        helper = self.root / "scripts/tidy_cache.py"
        helper.write_text(helper.read_text(encoding="utf-8").replace(
            "def shard_command(args: argparse.Namespace) -> int:",
            "def shard_command(args: argparse.Namespace) -> int:\n    if args.files:\n        return 7",
        ), encoding="utf-8")
        result, calls = self.run_gate("--shard", "2/3")
        self.assertEqual(result.returncode, 7, result.stderr)
        self.assertEqual(calls, [])


    def selection_file(self, data):
        manifest = self.root / "selected.files0"
        manifest.write_bytes(data)
        return str(manifest)

    def test_files0_preserves_unusual_names_relative_db_paths_and_tracked_config(self):
        unusual = "test/hash # dollar $ newline\nname.cpp"
        (self.root / unusual).write_text("fixture")
        build = self.root / "build/tidy"
        database = build / "compile_commands.json"
        entries = json.loads(database.read_text())
        entries.append({"file": "../../" + unusual, "directory": str(build)})
        database.write_text(json.dumps(entries))
        chosen = [self.files[0], unusual]
        manifest = self.selection_file(b"\0".join(os.fsencode(p) for p in chosen + chosen) + b"\0")
        result, calls = self.run_gate("--files0-from", manifest)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_selected(calls, chosen)
        self.assertTrue(all("-quiet" in call and "-p" in call for call in calls))

    def test_files0_empty_is_explicit_success_and_missing_or_malformed_input_fails(self):
        result, calls = self.run_gate("--files0-from", self.selection_file(b""))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls, [])
        for data in (b"src/regular.cpp", b"src/regular.cpp\0\0", b"\0",
                     b"src/missing.cpp\0", b"../outside.cpp\0", b"vendor/external.cpp\0",
                     b"src/luminumbra_client/ui/gl3/vendor.cpp\0"):
            with self.subTest(data=data):
                result, calls = self.run_gate("--files0-from", self.selection_file(data))
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(calls, [])
        result, calls = self.run_gate("--files0-from", str(self.root / "absent.files0"))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_files0_refuses_missing_database_entries_and_malformed_database(self):
        manifest = self.selection_file(os.fsencode(self.root / "src/regular.cpp") + b"\0")
        db = self.root / "build/tidy/compile_commands.json"
        for data in ("[]", "not json", '[{"directory": null}]'):
            db.write_text(data)
            result, calls = self.run_gate("--files0-from", manifest)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(calls, [])

    def test_files0_argument_refusals_happen_before_tidy(self):
        manifest = self.selection_file(b"src/regular.cpp\0")
        for args in (("--files0-from",), ("--files0-from", ""),
                     ("--files0-from", manifest, "--changed-from", "HEAD"),
                     ("--jobs", "0"), ("--jobs", "00"), ("--jobs", "-1")):
            result, calls = self.run_gate(*args)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertEqual(calls, [])

    def test_files0_tidy_failure_propagates_and_fallback_binary_is_supported(self):
        manifest = self.selection_file(b"src/regular.cpp\0")
        self.env["STUB_EXIT"] = "7"
        result, calls = self.run_gate("--files0-from", manifest)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assert_selected(calls, ["src/regular.cpp"])
        self.env.pop("STUB_EXIT")
        (self.bin / "clang-tidy-18").rename(self.bin / "clang-tidy")
        result, calls = self.run_gate("--files0-from", manifest)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("binary clang-tidy", result.stdout)
        self.assert_selected(calls, ["src/regular.cpp"])


class ShardWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflow = (REPO_ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        cls.jobs = dict(re.findall(r"(?ms)^  (tidy(?:-shard)?):\n(.*?)(?=^  [\w-]+:\n|\Z)", cls.workflow))

    def test_three_standard_runners_keep_full_push_analysis_and_separate_caches(self):
        job = self.jobs["tidy-shard"]
        self.assertIn("name: Static analysis / clang-tidy shard ${{ matrix.shard }}/3\n", job)
        self.assertIn("runs-on: ubuntu-24.04\n", job)
        self.assertIn("timeout-minutes: 60\n", job)
        self.assertRegex(job, r"strategy:\n      fail-fast: false\n      matrix:\n        shard: \[1, 2, 3\]")
        self.assertIn("--shard ${{ matrix.shard }}/3", job)
        self.assertIn("TIDY_CACHE_MODE: ${{ github.event_name == 'pull_request' && 'readwrite' || 'writeonly' }}", job)
        self.assertEqual(job.count("key: tidy-cache-${{ runner.os }}-shard${{ matrix.shard }}of3-${{ github.sha }}"), 2)
        self.assertIn("restore-keys: tidy-cache-${{ runner.os }}-shard${{ matrix.shard }}of3-\n", job)
        self.assertIn("if: github.event_name == 'pull_request'\n", job)
        self.assertIn("github.event_name == 'push' && (github.ref == 'refs/heads/main' || github.ref == 'refs/heads/devel')", job)

    def test_required_name_dependency_and_draft_guards(self):
        aggregate = self.jobs["tidy"]
        guard = "github.event_name != 'pull_request' || github.event.pull_request.draft == false"
        self.assertIn("    name: Static analysis / clang-tidy\n", aggregate)
        self.assertIn("    needs: [changes, tidy-shard]\n", aggregate)
        self.assertIn("    if: always() && (" + guard + ")\n", aggregate)
        self.assertIn("    needs: changes\n", self.jobs["tidy-shard"])
        self.assertIn("    if: ${{ !cancelled() && needs.changes.outputs.needs_cpp != 'false' && (" + guard + ") }}\n",
                      self.jobs["tidy-shard"])
        self.assertIn("CI_PATHS_RESULT: ${{ needs.changes.result }}", aggregate)
        self.assertIn("NEEDS_CPP: ${{ needs.changes.outputs.needs_cpp }}", aggregate)
        self.assertIn("    runs-on: ubuntu-24.04\n", aggregate)
        self.assertIn("    timeout-minutes: 5\n", aggregate)
        self.assertIn("TIDY_SHARD_RESULT: ${{ needs.tidy-shard.result }}", aggregate)

    def test_aggregate_shell_accepts_only_success(self):
        script = self.jobs["tidy"].split("        run: |\n", 1)[1]
        script = "\n".join(line[10:] for line in script.splitlines() if line.startswith("          "))
        self.assertTrue(script.strip())
        for status in ("success", "failure", "cancelled", "skipped", ""):
            with self.subTest(status=status):
                result = subprocess.run(["bash", "-e", "-c", script],
                                        env=dict(os.environ, TIDY_SHARD_RESULT=status, CI_PATHS_RESULT="success", NEEDS_CPP="true"),
                                        capture_output=True, text=True, check=False)
                self.assertEqual(result.returncode == 0, status == "success", result.stderr)

    def test_routed_skip_requires_successful_explicit_non_native_classification(self):
        script = self.jobs["tidy"].split("        run: |\n", 1)[1]
        script = "\n".join(line[10:] for line in script.splitlines() if line.startswith("          "))
        with tempfile.TemporaryDirectory() as directory:
            summary = Path(directory) / "summary"
            for classification in ("success", "failure", "cancelled", "skipped", ""):
                for cpp in ("true", "false", ""):
                    for status in ("success", "failure", "cancelled", "skipped", ""):
                        routed = classification == "success" and cpp == "false" and status == "skipped"
                        with self.subTest(classification=classification, cpp=cpp, status=status):
                            summary.unlink(missing_ok=True)
                            result = subprocess.run(
                                ["bash", "-e", "-c", script],
                                env=dict(os.environ, TIDY_SHARD_RESULT=status,
                                         CI_PATHS_RESULT=classification, NEEDS_CPP=cpp,
                                         GITHUB_STEP_SUMMARY=str(summary)),
                                capture_output=True, text=True, check=False,
                            )
                            self.assertEqual(result.returncode == 0, status == "success" or routed,
                                             result.stderr)
                            self.assertEqual(summary.exists(), routed)
                            if routed:
                                self.assertEqual(summary.read_text(), result.stdout)
                                self.assertIn("route: skipped", result.stdout)

    def test_public_tree_runs_shard_regressions(self):
        self.assertIn("          python3 tools/ci/test_tidy_shard.py\n", self.workflow)


if __name__ == "__main__":
    unittest.main()

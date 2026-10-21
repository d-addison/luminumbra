#!/usr/bin/env python3
"""Check effective CTest registrations and hosted coverage without native work."""

import importlib.util
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_paths", ROOT / "tools/ci/ci_paths.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
SPEC = importlib.util.spec_from_file_location("workflow_policy", ROOT / "tools/ci/test_workflow_policy.py")
policy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(policy)


class RegistrationTests(unittest.TestCase):
    def test_compiler_free_project_discovers_the_shared_production_registrations(self):
        # Configure only LANGUAGES NONE: no compiler probes, dependencies,
        # target builds or test execution. CTest itself supplies the model.
        with tempfile.TemporaryDirectory(prefix="local check discovery ") as temporary:
            build = Path(temporary) / "build with spaces"
            subprocess.run(["cmake", "-S", str(ROOT / "tools/ci/contracts"), "-B", str(build),
                            "-G", "Ninja", f"-DPython3_EXECUTABLE={sys.executable}"],
                           check=True, capture_output=True, text=True)
            result = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                                    check=True, capture_output=True, text=True)
            discovery = json.loads(result.stdout)
            tests = {test["name"]: test for test in discovery["tests"]}
            self.assertEqual(set(tests), {
                "ChangedTranslationUnitsContract", "OfflineDependenciesContract",
                "OfflineDependenciesContractPart1", "OfflineDependenciesContractPart2"})
            self.assertEqual(len(discovery["tests"]), len(tests))
            for name, script, arguments in (
                ("ChangedTranslationUnitsContract", "test_changed_tus.py", []),
                ("OfflineDependenciesContract", "test_offline_deps.py", ["--partition", "0"]),
                ("OfflineDependenciesContractPart1", "test_offline_deps.py", ["--partition", "1"]),
                ("OfflineDependenciesContractPart2", "test_offline_deps.py", ["--partition", "2"]),
            ):
                with self.subTest(name=name):
                    test = tests[name]
                    command = test["command"]
                    self.assertEqual(Path(command[0]).resolve(), Path(sys.executable).resolve())
                    self.assertEqual(command[1:3], ["-I", "-B"])
                    self.assertEqual(Path(command[3]).resolve(), ROOT / "tools/ci" / script)
                    self.assertEqual(command[4:], arguments)
                    properties = {item["name"]: item["value"] for item in test["properties"]}
                    self.assertEqual(properties["TIMEOUT"], 30)
                    self.assertEqual(properties["LABELS"], ["ci", "tools"])
                    self.assertFalse(properties.get("DISABLED", False))
                    self.assertNotIn("SKIP_RETURN_CODE", properties)
                    trace = discovery["backtraceGraph"]
                    node = trace["nodes"][test["backtrace"]]
                    self.assertEqual(Path(trace["files"][node["file"]]).resolve(),
                                     ROOT / "tools/ci/local_check_contracts.cmake")
            cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
            self.assertNotRegex(cache, r"(?m)^CMAKE_(?:C|CXX)_COMPILER:")
        production = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/ci/local_check_contracts.cmake")', production)
        for name in tests:
            self.assertNotRegex(production, rf"\bNAME\s+{name}\b")

    def test_both_hosted_routes_execute_registered_deadlines_and_keep_failure_evidence(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        jobs = policy.jobs(workflow)
        for job, python in (("linux", "python3"), ("windows-msvc", "python")):
            with self.subTest(job=job):
                steps = {step.splitlines()[0].removeprefix("      - name: "): step
                         for step in policy.steps(jobs[job])}
                run = steps["Run registered local-check deadlines"]
                self.assertIsNone(policy.value(run, "if", 8))
                self.assertIsNone(policy.value(run, "continue-on-error", 8))
                self.assertEqual(policy.value(run, "shell", 8), "bash")
                self.assertEqual(policy.value(run, "id", 8), "local-contracts")
                configure = "cmake -S tools/ci/contracts -B build/local-check-contracts -G Ninja"
                if job == "windows-msvc":
                    configure += ' "-DPython3_EXECUTABLE=$CI_PYTHON_EXECUTABLE"'
                    self.assertEqual(policy.value(run, "CI_PYTHON_EXECUTABLE", 10),
                                     "${{ steps.python.outputs.python-path }}")
                self.assertEqual(policy.value(run, "run", 8).splitlines(), [
                    configure,
                    "ctest --test-dir build/local-check-contracts --show-only=json-v1 > build/local-check-contracts/ctest-discovery.json",
                    'ctest --test-dir build/local-check-contracts --no-tests=error --parallel 4 --output-on-failure --output-junit "$LOCAL_CHECK_JUNIT"',
                    f'{python} -I -B tools/ci/check_ctest_junit.py "$LOCAL_CHECK_JUNIT" --minimum-tests 4 --require-test ChangedTranslationUnitsContract --require-test OfflineDependenciesContract --require-test OfflineDependenciesContractPart1 --require-test OfflineDependenciesContractPart2',
                ])
                # With no step condition, false/empty classifier output cannot
                # bypass CTest. Existing job-level draft guards still apply.
                upload = steps["Upload local-check deadline evidence"]
                self.assertEqual(policy.value(upload, "if", 8), "${{ always() && (" + policy.READY + ") }}")
                for path in ("ctest-discovery.json", "ctest.junit.xml", "Testing/Temporary"):
                    self.assertIn("build/local-check-contracts/" + path, upload)
                self.assertIn("${{ github.sha }}", upload)
                self.assertLess(jobs[job].index(run), jobs[job].index(upload))
        self.assertNotIn("paths:", workflow.split("permissions:", 1)[0])

    def test_local_check_artifacts_are_distinct_for_both_platforms_and_every_shard(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        jobs = policy.jobs(workflow)
        names = []
        for job, platform in (("linux", "linux"), ("windows-msvc", "windows")):
            with self.subTest(job=job):
                steps = {step.splitlines()[0].removeprefix("      - name: "): step
                         for step in policy.steps(jobs[job])}
                upload = steps["Upload local-check deadline evidence"]
                name = policy.value(upload, "name", 10)
                self.assertEqual(name, platform + "-local-check-contracts-${{ github.sha }}-"
                                 "${{ github.run_id }}-${{ github.run_attempt }}-shard-${{ matrix.shard }}")
                matrix = policy.value(jobs[job], "shard", 8)
                selections = [json.loads(value) for value in re.findall(r"fromJSON\('([^']+)'\)", matrix)]
                self.assertEqual(selections, [[0], [1, 2]])
                for selection in selections:
                    for shard in selection:
                        rendered = name
                        for key, value in (("github.sha", "a" * 40), ("github.run_id", "7"),
                                           ("github.run_attempt", "1"), ("matrix.shard", str(shard))):
                            rendered = rendered.replace("${{ " + key + " }}", value)
                        self.assertNotIn("${{", rendered)
                        names.append(rendered)
        self.assertEqual(len(names), 6)
        self.assertEqual(len(set(names)), len(names))

    def test_local_check_junit_producer_checker_and_upload_share_an_absolute_path(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        jobs = policy.jobs(workflow)
        expected = "${{ github.workspace }}/build/local-check-contracts/ctest.junit.xml"
        for job, python in (("linux", "python3"), ("windows-msvc", "python")):
            with self.subTest(job=job):
                steps = {step.splitlines()[0].removeprefix("      - name: "): step
                         for step in policy.steps(jobs[job])}
                run = steps["Run registered local-check deadlines"]
                self.assertEqual(policy.value(run, "LOCAL_CHECK_JUNIT", 10), expected)
                commands = policy.value(run, "run", 8).splitlines()
                producer, = [line for line in commands if "--output-junit" in line]
                checker, = [line for line in commands if "check_ctest_junit.py" in line]
                self.assertIn('--output-junit "$LOCAL_CHECK_JUNIT"', producer)
                self.assertIn('check_ctest_junit.py "$LOCAL_CHECK_JUNIT"', checker)
                arguments = shlex.split(producer)
                self.assertEqual(arguments[arguments.index("--output-junit") + 1], "$LOCAL_CHECK_JUNIT")
                self.assertEqual(shlex.split(checker)[:5],
                                 [python, "-I", "-B", "tools/ci/check_ctest_junit.py", "$LOCAL_CHECK_JUNIT"])
                upload = steps["Upload local-check deadline evidence"]
                self.assertEqual(policy.value(upload, "path", 10).splitlines(), [
                    "build/local-check-contracts/ctest-discovery.json", expected,
                    "build/local-check-contracts/Testing/Temporary",
                ])
                for workspace, path_type in (("/work/tree with spaces", PurePosixPath),
                                             ("D:/work/tree with spaces", PureWindowsPath),
                                             ("D:/WORKTR~1/tree", PureWindowsPath)):
                    with self.subTest(workspace=workspace):
                        path = expected.replace("${{ github.workspace }}", workspace)
                        self.assertTrue(path_type(path).is_absolute())
                        self.assertEqual(path_type(path).name, "ctest.junit.xml")

    def test_registration_regression_is_in_both_test_plans_and_requires_ci_coverage(self):
        path = "tools/ci/test_local_check_contracts.py"
        self.assertIn((path,), gate.python_test_commands(ROOT))
        production = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(production, r"NAME LocalCheckRegistrationContract\s+COMMAND")
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/ci/test_local_check_contracts.py", production)
        for changed in (path, "tools/ci/local_check_contracts.cmake", "tools/ci/contracts/CMakeLists.txt"):
            with self.subTest(changed=changed):
                self.assertTrue(gate.needs_cpp(changed))


if __name__ == "__main__":
    unittest.main()

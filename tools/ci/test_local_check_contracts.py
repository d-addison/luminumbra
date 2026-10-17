#!/usr/bin/env python3
"""Check effective CTest registrations and hosted coverage without native work."""

import importlib.util
import json
from pathlib import Path
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
                "ChangedTranslationUnitsContract", "OfflineDependenciesContract"})
            for name, script in (("ChangedTranslationUnitsContract", "test_changed_tus.py"),
                                 ("OfflineDependenciesContract", "test_offline_deps.py")):
                with self.subTest(name=name):
                    test = tests[name]
                    command = test["command"]
                    self.assertEqual(Path(command[0]).resolve(), Path(sys.executable).resolve())
                    self.assertEqual(command[1:3], ["-I", "-B"])
                    self.assertEqual(Path(command[3]).resolve(), ROOT / "tools/ci" / script)
                    self.assertEqual(len(command), 4)
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
                self.assertEqual(policy.value(run, "run", 8).splitlines(), [
                    "cmake -S tools/ci/contracts -B build/local-check-contracts -G Ninja",
                    "ctest --test-dir build/local-check-contracts --show-only=json-v1 > build/local-check-contracts/ctest-discovery.json",
                    "ctest --test-dir build/local-check-contracts --no-tests=error --parallel 4 --output-on-failure --output-junit ctest.junit.xml",
                    f"{python} -I -B tools/ci/check_ctest_junit.py build/local-check-contracts/ctest.junit.xml --minimum-tests 2 --require-test ChangedTranslationUnitsContract --require-test OfflineDependenciesContract",
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

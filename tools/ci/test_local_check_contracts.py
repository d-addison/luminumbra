#!/usr/bin/env python3
"""Check effective CTest registrations and hosted coverage without native work."""

import ast
import importlib.util
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_paths", ROOT / "tools/ci/ci_paths.py")
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
SPEC = importlib.util.spec_from_file_location("workflow_policy", ROOT / "tools/ci/test_workflow_policy.py")
policy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(policy)


class RegistrationTests(unittest.TestCase):
    def test_borrowed_source_contracts_share_an_effective_resource_lock(self):
        production = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        start = production.index("    add_test(\n        NAME RmlUiSourceScriptContract")
        end = production.index("    add_test(\n        NAME SlowestTestsScriptContract", start)
        # Evaluate the exact production registration block with real CMake/CTest.
        # This synthetic project has no targets, compilers or dependency setup.
        with tempfile.TemporaryDirectory(prefix="source lock discovery ") as temporary:
            root = Path(temporary)
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(SourceLockContracts LANGUAGES NONE)\nenable_testing()\n"
                + production[start:end], encoding="utf-8")
            build = root / "build"
            subprocess.run(["cmake", "-S", str(root), "-B", str(build), "-G", "Ninja",
                            f"-DPython3_EXECUTABLE={sys.executable}"],
                           check=True, capture_output=True, text=True)
            result = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                                    check=True, capture_output=True, text=True)
            tests = {test["name"]: test for test in json.loads(result.stdout)["tests"]}
            expected = {"RmlUiSourceScriptContract", "RmlUiSourceCommandContract",
                        "LocalRmlUiRecipeContract", "LocalRmlUiRecoveryFailureContract"}
            expected.update("RmlUiSourceFixture_" + fixture for fixture in (
                'every_changed_file_preserves_operations_defaults_and_enum_declarations',
                'files_mixed_missing_extra_and_partial_refuse_untouched',
                'git_pin_index_modes_flags_alternates_and_config_refusals',
                'interruption_at_each_publication_boundary_never_marks_incomplete_ready',
                'mode_and_unreadable_refuse',
                'nonordinary_files_and_links',
                'operation_assertions_use_strict_utf8_with_a_cp1252_default'))
            expected.update("RmlUiSourceRecovery_" + recovery for recovery in (
                "borrowed_archive_without_git_returns_its_original_metadata",
                "borrowed_recovery_refuses_unknown_partial_and_changed_metadata",
                "setup_final_verification_failure_returns_the_complete_input",
                "teardown_refusal_preserves_ancestor_and_can_retry_both_locations"))
            expected.update("LocalRmlUiRecovery_" + boundary for boundary in
                            ("beforepatch", "afterpatch", "beforerename", "afterrename", "receipt", "configure"))
            self.assertEqual(set(tests), expected)
            # Expand every class/method selector against the actual source AST.
            # Registration must cover every case exactly once, including future cases.
            for script in ("test_rmlui_source.py", "test_local_rmlui_recipe.py"):
                tree = ast.parse((ROOT / "tools/ci" / script).read_text(encoding="utf-8"))
                cases = {node.name + "." + method.name for node in tree.body
                         if isinstance(node, ast.ClassDef) for method in node.body
                         if isinstance(method, ast.FunctionDef) and method.name.startswith("test_")}
                selected = []
                for test in tests.values():
                    if Path(test["command"][3]).name == script:
                        for selector in test["command"][4:]:
                            matches = sorted(case for case in cases
                                             if case == selector or case.startswith(selector + "."))
                            self.assertTrue(matches, selector)
                            selected.extend(matches)
                self.assertCountEqual(selected, cases)
            for test in tests.values():
                properties = {item["name"]: item["value"] for item in test["properties"]}
                self.assertEqual(properties["RESOURCE_LOCK"], ["rmlui_offline_input"])
                self.assertFalse(properties.get("RUN_SERIAL", False))
                self.assertEqual(properties["TIMEOUT"], 60)
                self.assertFalse(properties.get("DISABLED", False))
                self.assertNotIn("SKIP_RETURN_CODE", properties)
            self.assertNotRegex((build / "CMakeCache.txt").read_text(encoding="utf-8"),
                                r"(?m)^CMAKE_(?:C|CXX)_COMPILER:")

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
                    f"cmake -S tools/ci/contracts -B build/local-check-contracts -G Ninja \"-DPython3_EXECUTABLE=$({python} -I -B -c 'import sys; print(sys.executable)')\"",
                    "ctest --test-dir build/local-check-contracts --show-only=json-v1 > build/local-check-contracts/ctest-discovery.json",
                    'ctest --test-dir build/local-check-contracts --no-tests=error --parallel 4 --output-on-failure --output-junit "$LOCAL_CHECK_JUNIT"',
                    f'{python} -I -B tools/ci/check_ctest_junit.py "$LOCAL_CHECK_JUNIT" --minimum-tests 2 --require-test ChangedTranslationUnitsContract --require-test OfflineDependenciesContract',
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

    def test_hosted_route_contract_rejects_missing_and_wrong_platform_interpreters(self):
        workflow_path = ROOT / ".github/workflows/ci.yml"
        workflow = workflow_path.read_text(encoding="utf-8")
        jobs = policy.jobs(workflow)
        read_text = Path.read_text
        for job, python, other_python in (("linux", "python3", "python"),
                                          ("windows-msvc", "python", "python3")):
            steps = {step.splitlines()[0].removeprefix("      - name: "): step
                     for step in policy.steps(jobs[job])}
            run = steps["Run registered local-check deadlines"]
            configure = policy.value(run, "run", 8).splitlines()[0]
            without_binding, binding = configure.rsplit(' "-DPython3_EXECUTABLE=', 1)
            self.assertTrue(binding)
            interpreter = f"$({python} -I -B -c "
            self.assertEqual(configure.count(interpreter), 1)
            wrong_binding = configure.replace(interpreter, f"$({other_python} -I -B -c ", 1)
            for defect, replacement in (("missing", without_binding), ("wrong platform", wrong_binding)):
                with self.subTest(job=job, defect=defect):
                    changed_run = run.replace(configure, replacement, 1)
                    self.assertNotEqual(changed_run, run)
                    self.assertEqual(workflow.count(run), 1)
                    candidate = workflow.replace(run, changed_run, 1)

                    def read_workflow(path, *args, **kwargs):
                        if path == workflow_path:
                            return candidate
                        return read_text(path, *args, **kwargs)

                    # Feed the mutated shipping step through the existing exact
                    # route contract and real policy parser; never run its shell.
                    result = unittest.TestResult()
                    with patch.object(Path, "read_text", read_workflow):
                        inner_case = RegistrationTests(
                            "test_both_hosted_routes_execute_registered_deadlines_and_keep_failure_evidence"
                        )
                        inner_case.maxDiff = None
                        inner_case.run(result)
                    self.assertEqual(result.testsRun, 1)
                    self.assertEqual(result.errors, [])
                    self.assertEqual(result.skipped, [])
                    self.assertEqual(len(result.failures), 1)
                    rejected, message = result.failures[0]
                    self.assertIn(f"job='{job}'", rejected.id())
                    self.assertIn("-DPython3_EXECUTABLE=", message)

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

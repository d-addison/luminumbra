#!/usr/bin/env python3
"""Offline policy tests for draft guards and existing workflow routing.

Read the workflows' indented job/step blocks using only the standard library.
This is deliberately scoped to their current YAML layout, not a YAML parser.
"""

import fnmatch
import os
import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GUARDED = {
    "ci.yml": ("linux", "Linux / GCC"),
    "asan.yml": ("asan", "Linux / GCC ASan"),
    "performance.yml": ("preview-frame", "Preview frame / software OpenGL"),
}
READY = "github.event_name != 'pull_request' || github.event.pull_request.draft == false"
ASAN_MINIMUM_TESTS = 2320
ASAN_REQUIRED_TESTS = (
    "ConsoleFixture.CompleteInventoryDefaultsBindingsAndUnavailableRows",
    "UiFixture.DefaultPassiveHudRendersWithoutShellOrFocusAndCanBeDisabled",
    "DeveloperPersistence.DirectConstructionMatchesFactoryDefaultsAndInitializesRecords",
)
ASAN_MERGE_COMMAND = "python3 tools/ci/asan_shards.py merge --directory build/asan-shards --output build/debug-asan/ctest.junit.xml"
UCRT_ROUTE = (
    "github.event_name == 'pull_request' || github.ref_name == 'main' || "
    "(github.event_name == 'push' && github.ref == 'refs/heads/devel') || "
    "(github.event_name == 'workflow_dispatch' && inputs.qualification_capture)"
)
SERVER_ROUTE = (
    "((github.event_name == 'pull_request' && github.base_ref == 'main') || "
    "(github.event_name == 'push' && "
    "(github.ref == 'refs/heads/main' || github.ref == 'refs/heads/devel') && "
    "github.event.before != '0000000000000000000000000000000000000000'))"
)


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


def guarded_jobs(workflows):
    for filename, (job_id, check_name) in GUARDED.items():
        yield filename, job_id, check_name, jobs(workflows[filename])[job_id]
    yield ("asan.yml", "asan-shard", "Linux / GCC ASan shard ${{ matrix.shard }}/2",
           jobs(workflows["asan.yml"])["asan-shard"])


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


class WorkflowDraftPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workflows = {
            filename: (ROOT / ".github/workflows" / filename).read_text(encoding="utf-8")
            for filename in GUARDED
        }

    def test_ready_and_draft_transitions_trigger_existing_pr_routes(self):
        for filename, workflow in self.workflows.items():
            with self.subTest(workflow=filename):
                pr = re.split(r"\n  (?=\S)", workflow.split("  pull_request:\n", 1)[1], maxsplit=1)[0]
                self.assertEqual(
                    value(pr, "types", 4),
                    "[opened, synchronize, reopened, ready_for_review, converted_to_draft]",
                )
                self.assertEqual(value(pr, "branches", 4), "[main, devel]")

    def test_guard_runs_first_under_existing_check_name_without_draft_job_skip(self):
        for filename, job_id, check_name, job in guarded_jobs(self.workflows):
            with self.subTest(workflow=filename, job=job_id):
                self.assertEqual(value(job, "name", 4), check_name)
                self.assertEqual(value(job, "runs-on", 4), "ubuntu-24.04")
                self.assertEqual(value(job, "needs", 4),
                                 "[changes, asan-shard]" if job_id == "asan" else "changes")
                condition = "always()" if job_id == "asan" else "!cancelled()"
                if filename == "performance.yml" or job_id == "asan-shard":
                    condition += (
                        " && ((github.event_name == 'pull_request' && "
                        "toJSON(github.event.pull_request.draft) != 'false') || "
                        "needs.changes.outputs.needs_cpp != 'false')"
                    )
                self.assertEqual(value(job, "if", 4), "${{ " + condition + " }}")
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
                        result = subprocess.run(
                            ["bash", "--noprofile", "--norc", "-e", "-c", script],
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
                    if job_id == "asan" and step.startswith("      - name: Reject cancelled workflow\n"):
                        self.assertEqual(condition, "${{ cancelled() }}")
                        self.assertIsNone(value(step, "continue-on-error", 8))
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
                                 "${{ !cancelled() && needs.changes.outputs.needs_cpp != 'false' && (" + READY + ") }}")
        self.assertEqual(value(ci["windows-msvc"], "needs", 4), "changes")
        self.assertEqual(value(ci["windows-msvc"], "if", 4), "${{ !cancelled() && (" + READY + ") }}")
        self.assertEqual(value(ci["tidy"], "needs", 4), "[changes, tidy-shard]")
        self.assertEqual(value(ci["tidy"], "if", 4), "always() && (" + READY + ")")
        self.assertEqual(value(ci["windows-ucrt64"], "if", 4), "${{ !cancelled() && needs.changes.outputs.needs_cpp != 'false' && " + f"({READY}) && ({UCRT_ROUTE})" + " }}")
        perf = jobs(self.workflows["performance.yml"])
        self.assertEqual(value(perf["server-smoke-comparison"], "if", 4), "!cancelled() && needs.changes.outputs.needs_cpp != 'false' && " + f"({READY}) && {SERVER_ROUTE}")
        self.assertEqual(
            value(perf["qualification-capture"], "if", 4),
            "${{ !cancelled() && needs.changes.outputs.needs_cpp != 'false' && "
            "github.event_name == 'workflow_dispatch' && inputs.qualification_capture }}",
        )
        for job in (ci["lint"], ci["public-tree"], perf["runner-contract"]):
            self.assertIsNone(value(job, "if", 4))

    def test_asan_matrix_and_aggregate_preserve_limits_coverage_and_evidence(self):
        workflow = self.workflows["asan.yml"]
        blocks = jobs(workflow)
        self.assertEqual(set(blocks), {"changes", "asan-shard", "asan"})
        shard, aggregate = blocks["asan-shard"], blocks["asan"]
        self.assertEqual(value(shard, "name", 4), "Linux / GCC ASan shard ${{ matrix.shard }}/2")
        self.assertEqual(value(shard, "needs", 4), "changes")
        self.assertEqual(value(shard, "if", 4), "${{ !cancelled() && ((github.event_name == 'pull_request' && toJSON(github.event.pull_request.draft) != 'false') || needs.changes.outputs.needs_cpp != 'false') }}")
        self.assertEqual(value(shard, "runs-on", 4), "ubuntu-24.04")
        self.assertEqual(value(shard, "timeout-minutes", 4), "60")
        self.assertEqual(value(shard, "fail-fast", 6), "false")
        self.assertEqual(value(shard, "shard", 8), "[1, 2]")
        self.assertIsNone(value(shard, "continue-on-error", 4))
        self.assertEqual(steps(shard)[0], steps(aggregate)[0])
        self.assertEqual(value(aggregate, "timeout-minutes", 4), "5")
        shard_steps = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(shard)}
        aggregate_steps = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(aggregate)}
        self.assertEqual(value(shard_steps["Test"], "id", 8), "tests")
        self.assertEqual(value(shard_steps["Test"], "run", 8),
                         'ctest --preset debug-asan -LE manual --show-only=json-v1 > build/debug-asan/ctest.inventory.json\n'
                         'ctest --preset debug-asan --show-only=json-v1 > build/debug-asan/ctest.all.json\n'
                         '# Alternate independent tests; keep fixture setup/consumers in one shard.\n'
                         'python3 tools/ci/asan_shards.py plan --shard ${{ matrix.shard }} --directory build/debug-asan\n'
                         '# Recheck the ordered roster in the same context immediately before CTest.\n'
                         'env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11 LIBGL_ALWAYS_SOFTWARE=1 \\\n'
                         '  xvfb-run -a python3 tools/ci/asan_shards.py run --directory build/debug-asan')
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
                         rf"^{re.escape(inventory_prefix)}[0-9]+(?: --require-test [\w./-]+)*$")
        self.assertGreaterEqual(int(inventory_command.split()[4]), ASAN_MINIMUM_TESTS)
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
                         "asan-test-evidence-${{ github.sha }}-shard-${{ matrix.shard }}")
        self.assertEqual(value(diagnostics, "overwrite", 10), "true")
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
                    "lowered": re.sub(r"--minimum-tests [0-9]+",
                                      f"--minimum-tests {ASAN_MINIMUM_TESTS - 1}", inventory, count=1),
                    "commented": inventory.replace("python3", "# python3", 1),
                    "before-merge": "",
                }[mutation]
                mutated = workflow.replace(inventory, replacement, 1)
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
                    r"(?m)^(          python3 tools/ci/check_ctest_junit.py "
                    r"build/debug-asan/ctest.junit.xml --minimum-tests )[0-9]+([^\n]*)$",
                    rf"\g<1>{floor}\g<2> --require-test AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes"
                    " --require-test AmbientInputBounds.UnpaddedTwoElementInputFaultsAtActiveIsa",
                    workflow,
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
            ("missing-floor", inventory, re.sub(r" --minimum-tests [0-9]+", "", inventory)),
            ("invalid-floor", inventory, re.sub(r"--minimum-tests [0-9]+", "--minimum-tests unknown", inventory)),
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
        for attempt, producers in ((1, (1, 2)), (2, rerun_shards)):
            for shard in producers:
                name = render(value(upload, "name", 10), attempt, shard)
                if name in artifacts:
                    self.assertEqual(value(upload, "overwrite", 10), "true")
                artifacts[name] = (attempt, shard)
        artifacts["asan-junit-foreign-10-shard-1"] = (1, 1)
        artifacts["asan-junit-" + "a" * 40 + "-9-shard-2"] = (1, 2)
        selected = {name: producer for name, producer in artifacts.items()
                    if fnmatch.fnmatchcase(name, render(value(download, "pattern", 10), 2))}
        self.assertEqual(len(selected), 2, selected)
        self.assertEqual(sorted(selected.values(), key=lambda item: item[1]),
                         [(2 if shard in rerun_shards else 1, shard) for shard in (1, 2)])
        script = render(value(merge, "run", 8), 2)
        for name, (_, shard) in selected.items():
            self.assertIn(f'mv "build/asan-shards/{name}" "build/asan-shards/shard-{shard}"',
                          script.replace("$shard", str(shard)))
        self.assertEqual(value(download, "if", 8), "needs.asan-shard.result == 'success'")

    def test_asan_aggregate_only_rerun_downloads_both_original_shards(self):
        self.assert_asan_rerun_artifacts(())

    def test_asan_single_shard_rerun_downloads_latest_and_unchanged_shards(self):
        self.assert_asan_rerun_artifacts((2,))

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
            "build/debug-asan/qualification-shard.json",
        ])

    def test_asan_aggregate_schedules_under_cancellation_and_every_dependency_result(self):
        aggregate = jobs(self.workflows["asan.yml"])["asan"]
        expression = value(aggregate, "if", 4).removeprefix("${{ ").removesuffix(" }}")
        # Evaluate the workflow's status-only condition, including Actions'
        # implicit success() if no status function is present. A regression to
        # !cancelled() or the default success() must refuse these scenarios.
        for cancelled in (False, True):
            for changes in ("success", "failure", "cancelled", "skipped"):
                for shard in ("success", "failure", "cancelled", "skipped"):
                    with self.subTest(cancelled=cancelled, changes=changes, shard=shard):
                        status = {"always": True, "cancelled": cancelled,
                                  "success": not cancelled and changes == shard == "success",
                                  "failure": "failure" in (changes, shard)}
                        rendered, count = re.subn(r"\b(always|cancelled|success|failure)\(\)",
                                                 lambda match: str(status[match[1]]), expression)
                        rendered = rendered.replace("!", "not ").replace("&&", " and ").replace("||", " or ")
                        runs = (count > 0 or status["success"]) and eval(rendered, {"__builtins__": {}}, {})
                        self.assertTrue(runs, expression)

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
                       if step.startswith("      - name: Reject cancelled workflow\n"))
        self.assertEqual(value(refusal, "if", 8), "${{ cancelled() }}")
        self.assertIsNone(value(refusal, "continue-on-error", 8))
        refusal_script = value(refusal, "run", 8)
        self.assertIsNotNone(refusal_script)
        with tempfile.TemporaryDirectory() as directory:
            for cancelled in (False, True):
                for classification in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                    for cpp in ("true", "false", ""):
                        for result in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                            with self.subTest(cancelled=cancelled, classification=classification,
                                              cpp=cpp, result=result):
                                process = subprocess.run(
                                    ["bash", "--noprofile", "--norc", "-e", "-o", "pipefail", "-c",
                                     (refusal_script + "\n" if cancelled else "") + script],
                                    env=dict(os.environ, NEEDS_CPP=cpp, ASAN_SHARD_RESULT=result,
                                             CI_PATHS_RESULT=classification,
                                             GITHUB_STEP_SUMMARY=str(Path(directory) / "summary")),
                                    capture_output=True, text=True, timeout=10,
                                )
                                accepted = not cancelled and (
                                    result == "success" or (classification == "success" and
                                                            cpp == "false" and result == "skipped"))
                                self.assertEqual(process.returncode == 0, accepted, process.stdout + process.stderr)
                                if cancelled:
                                    self.assertIn("ASan workflow was cancelled", process.stderr)

    def test_ucrt_route_seeds_devel_and_preserves_other_event_and_path_guards(self):
        job = jobs(self.workflows["ci.yml"])["windows-ucrt64"]
        self.assertEqual(value(job, "needs", 4), "changes")
        expression = value(job, "if", 4).removeprefix("${{ ").removesuffix(" }}")
        for event in ("pull_request", "push", "workflow_dispatch", "schedule"):
            for branch in ("main", "devel", "feature"):
                for draft in (True, False, None):
                    for cpp in ("true", "false", ""):
                        for cancelled in (False, True):
                            for capture in (False, True):
                                expected = (
                                    not cancelled and cpp != "false"
                                    and (event != "pull_request" or draft is False)
                                    and (event == "pull_request" or branch == "main"
                                         or (event == "push" and branch == "devel")
                                         or (event == "workflow_dispatch" and capture))
                                )
                                actual = expression.replace("!cancelled()", repr(not cancelled))
                                for key, replacement in (
                                    ("github.event_name", event), ("github.ref_name", branch),
                                    ("github.ref", f"refs/heads/{branch}"),
                                    ("github.event.pull_request.draft", draft),
                                    ("needs.changes.outputs.needs_cpp", cpp),
                                    ("inputs.qualification_capture", capture),
                                ):
                                    actual = actual.replace(key, repr(replacement))
                                actual = actual.replace("== false", "== False").replace(
                                    "&&", "and"
                                ).replace("||", "or")
                                with self.subTest(event=event, branch=branch, draft=draft,
                                                  cpp=cpp, cancelled=cancelled, capture=capture):
                                    self.assertEqual(eval(actual, {"__builtins__": {}}), expected)

    def test_ucrt_cache_guards_keep_saves_on_trusted_pushes_and_bypass_captures(self):
        job = jobs(self.workflows["ci.yml"])["windows-ucrt64"]
        named = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(job)}
        trusted_push = (
            "github.event_name == 'push' && "
            "(github.ref == 'refs/heads/main' || github.ref == 'refs/heads/devel')"
        )
        capture = "github.event_name == 'workflow_dispatch' && inputs.qualification_capture"
        self.assertEqual(value(named["Set up MSYS2 UCRT64"], "cache", 10),
                         "${{ " + trusted_push + " }}")
        disable = named["Disable compiler cache for qualification captures"]
        self.assertEqual(value(disable, "if", 8), capture)
        self.assertIn('CCACHE_DISABLE=1', value(disable, "run", 8))
        self.assertEqual(value(named["Restore compiler cache"], "if", 8),
                         "${{ !(" + capture + ") }}")
        for name in ("Clean compiler cache before saving", "Save compiler cache"):
            with self.subTest(step=name):
                self.assertEqual(
                    value(named[name], "if", 8),
                    "${{ always() && " + trusted_push + " && !(" + capture + ") && "
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
                self.assertEqual(value(workflow, "group", 2), group)
                self.assertEqual(value(workflow, "cancel-in-progress", 2), cancellation)

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
        self.assertEqual(re.findall(r"^      - (.+)$", pr, re.MULTILINE), self.input_paths)

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


if __name__ == "__main__":
    unittest.main()

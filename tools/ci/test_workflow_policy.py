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
UCRT_ROUTE = (
    "github.event_name == 'pull_request' || github.ref_name == 'main' || "
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
        self.assertIn("python3 tools/ci/asan_shards.py merge --directory build/asan-shards --output build/debug-asan/ctest.junit.xml", check)
        self.assertIn("python3 tools/ci/check_ctest_junit.py build/debug-asan/ctest.junit.xml --minimum-tests 2260", check)
        self.assertIn("--expect-labels asan --require-probe asan", check)
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
        self.assertEqual(value(step, "NEEDS_CPP", 10), "${{ needs.changes.outputs.needs_cpp }}")
        self.assertEqual(value(step, "ASAN_CANCELLED", 10), "${{ cancelled() }}")
        self.assertEqual(value(step, "if", 8), "always() && (" + READY + ")")
        self.assertIsNone(value(step, "continue-on-error", 8))
        with tempfile.TemporaryDirectory() as directory:
            for cancelled in ("false", "true", ""):
                for cpp in ("true", "false", ""):
                    for result in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                        with self.subTest(cancelled=cancelled, cpp=cpp, result=result):
                            process = subprocess.run(
                                ["bash", "--noprofile", "--norc", "-e", "-o", "pipefail", "-c", value(step, "run", 8)],
                                env=dict(os.environ, NEEDS_CPP=cpp, ASAN_SHARD_RESULT=result,
                                         ASAN_CANCELLED=cancelled,
                                         GITHUB_STEP_SUMMARY=str(Path(directory) / "summary")),
                                capture_output=True, text=True, timeout=10,
                            )
                            accepted = cancelled == "false" and (
                                result == "success" or (cpp == "false" and result == "skipped"))
                            self.assertEqual(process.returncode == 0, accepted, process.stdout + process.stderr)

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
        for filename, workflow in self.workflows.items():
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


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Offline policy tests for draft guards and existing workflow routing.

Read the workflows' indented job/step blocks using only the standard library.
This is deliberately scoped to their current YAML layout, not a YAML parser.
"""

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
        for filename, (job_id, check_name) in GUARDED.items():
            with self.subTest(workflow=filename):
                job = jobs(self.workflows[filename])[job_id]
                self.assertEqual(value(job, "name", 4), check_name)
                self.assertEqual(value(job, "runs-on", 4), "ubuntu-24.04")
                self.assertEqual(value(job, "needs", 4), "changes")
                condition = "!cancelled()"
                if filename != "ci.yml":
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
            for filename, (job_id, _) in GUARDED.items():
                script = value(steps(jobs(self.workflows[filename])[job_id])[0], "run", 8)
                self.assertIsNotNone(script)
                for event, draft, expected in cases:
                    with self.subTest(workflow=filename, event=event, draft=draft):
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
        for filename, (job_id, _) in GUARDED.items():
            for step in steps(jobs(self.workflows[filename])[job_id])[1:]:
                with self.subTest(workflow=filename, step=step.splitlines()[0]):
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


if __name__ == "__main__":
    unittest.main()

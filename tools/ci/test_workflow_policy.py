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
    "ci.yml": ("linux-result", "Linux / GCC"),
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
    ci = jobs(workflows["ci.yml"])
    for job_id, name in (("linux", value(ci["linux"], "name", 4)),
                         ("windows-msvc-result", "Windows / MSVC"),
                         ("windows-ucrt64-result", "Windows / MSYS2 UCRT64")):
        yield "ci.yml", job_id, name, ci[job_id]


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
                                 "[changes, asan-shard]" if job_id == "asan" else
                                 f"[changes, {job_id.removesuffix('-result')}]" if job_id.endswith("-result") else "changes")
                condition = "always()" if job_id == "asan" or job_id.endswith("-result") else "!cancelled()"
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
                         'python3 tools/ci/ctest_shards.py plan --profile asan --shard ${{ matrix.shard }} --directory build/debug-asan --require-suite SimBudgetWorldParity. --require-suite AmbientDeterminismReceipt.\n'
                         '# Recheck the ordered roster in the same context immediately before CTest.\n'
                         'env -u WAYLAND_DISPLAY XDG_SESSION_TYPE=x11 LIBGL_ALWAYS_SOFTWARE=1 \\\n'
                         '  xvfb-run -a python3 tools/ci/ctest_shards.py run --profile asan --directory build/debug-asan')
        self.assertNotIn("check_ctest_junit.py", shard)
        self.assertNotIn("compare_ambient_receipts.py", shard)
        check = aggregate_steps["Merge shard JUnit and check inventory"]
        self.assertEqual(value(check, "id", 8), "tests")
        self.assertIn("python3 tools/ci/ctest_shards.py merge --profile asan --directory build/asan-shards --output build/debug-asan/ctest.junit.xml", check)
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

    def test_native_matrices_aggregates_limits_and_full_qualification_route(self):
        ci = jobs(self.workflows["ci.yml"])
        matrix = "${{ github.event_name == 'workflow_dispatch' && inputs.qualification_capture && fromJSON('[0]') || fromJSON('[1,2]') }}"
        for job_id, profile, title, runner, limit, floor in (
                ("linux", "gcc", "Linux / GCC", "ubuntu-24.04", "60", "2280"),
                ("windows-msvc", "msvc", "Windows / MSVC", "windows-2025", "90", "2260"),
                ("windows-ucrt64", "ucrt64", "Windows / MSYS2 UCRT64", "windows-2025", "85", "2260")):
            with self.subTest(job=job_id):
                producer, aggregate = ci[job_id], ci[job_id + "-result"]
                self.assertEqual(value(producer, "shard", 8), matrix)
                self.assertEqual(value(producer, "fail-fast", 6), "false")
                self.assertEqual(value(producer, "timeout-minutes", 4), limit)
                self.assertEqual(value(producer, "runs-on", 4), runner)
                self.assertIn(title + " full qualification", value(producer, "name", 4))
                self.assertIn(title + " shard {0}/2", value(producer, "name", 4))
                self.assertEqual(value(aggregate, "name", 4), title)
                self.assertEqual(value(aggregate, "if", 4), "${{ always() }}")
                self.assertEqual(value(aggregate, "needs", 4), f"[changes, {job_id}]")
                self.assertEqual(value(aggregate, "timeout-minutes", 4), "5")
                self.assertEqual(self.workflows["ci.yml"].count("    name: " + title + "\n"), 1)
                self.assertIsNone(value(producer, "continue-on-error", 4))
                self.assertIsNone(value(aggregate, "continue-on-error", 4))
                for command in ("plan", "run"):
                    self.assertIn(f"ctest_shards.py {command} --profile {profile}", producer)
                self.assertIn("ctest --preset release -LE manual --show-only=json-v1", producer)
                self.assertIn("ctest --preset release --show-only=json-v1", producer)
                self.assertIn("--require-suite SimBudgetWorldParity. --require-suite AmbientDeterminismReceipt.", producer)
                gate = next(step for step in steps(aggregate) if "name: Require expected producers and route\n" in step)
                self.assertEqual(value(gate, "SHARD_RESULT", 10), "${{ needs." + job_id + ".result }}")
                for key, expression in (("NEEDS_CPP", "needs.changes.outputs.needs_cpp"),
                                        ("CI_CANCELLED", "cancelled()"), ("CI_EVENT", "github.event_name"),
                                        ("CI_DRAFT", "github.event.pull_request.draft")):
                    self.assertEqual(value(gate, key, 10), "${{ " + expression + " }}")
                self.assertIn('ctest_shards.py gate --result "$SHARD_RESULT" --cpp "$NEEDS_CPP"', gate)
                merge = next(step for step in steps(aggregate) if "name: Merge shard JUnit and check inventory\n" in step)
                self.assertIn(f"ctest_shards.py merge --profile {profile}", merge)
                self.assertIn("--minimum-tests " + floor, merge)
                self.assertIn('if [ "$CI_QUALIFICATION" = true ]; then full=(--full); fi', merge)
                self.assertIn('"${full[@]}"', merge)
                self.assertNotIn("merge-multiple: true", aggregate)
                self.assertNotIn("actions/cache", aggregate)

    def test_native_cache_writers_and_attempt_specific_retained_evidence(self):
        ci = jobs(self.workflows["ci.yml"])
        for job_id, profile in (("linux", "gcc"), ("windows-msvc", "msvc"), ("windows-ucrt64", "ucrt64")):
            producer, aggregate = ci[job_id], ci[job_id + "-result"]
            producer_steps = {step.splitlines()[0].removeprefix("      - name: "): step for step in steps(producer)}
            for step in ("Clean compiler cache before saving", "Save compiler cache"):
                self.assertTrue(value(producer_steps[step], "if", 8).startswith("${{ matrix.shard == 1 && "))
            self.assertNotIn("matrix.shard", producer_steps["Restore compiler cache"])
            upload = producer_steps["Upload shard JUnit"]
            self.assertEqual(value(upload, "retention-days", 10), "30")
            self.assertEqual(value(upload, "if-no-files-found", 10), "error")
            self.assertEqual(value(upload, "path", 10).splitlines(), [
                "build/release/ctest.junit.xml", "build/release/ctest.inventory.json", "build/release/ctest.partition.json"])
            self.assertEqual(value(upload, "name", 10), profile + "-junit-${{ github.sha }}-${{ github.run_id }}-shard-${{ matrix.shard }}-attempt-${{ github.run_attempt }}")
            self.assertIn("pattern: " + profile + "-junit-${{ github.sha }}-${{ github.run_id }}-shard-*-attempt-*", aggregate)
            self.assertIn("--attempt ${{ github.run_attempt }}", aggregate)
        self.assertIn("cache: ${{ matrix.shard == 1 && github.event_name == 'push'", ci["windows-ucrt64"])

    def test_gcc_preserves_prefab_netsoak_packaging_and_diagnostics(self):
        linux = jobs(self.workflows["ci.yml"])["linux"]
        self.assertIn("-DLUMINUMBRA_ENABLE_PREFAB_RUNTIME=ON", linux)
        self.assertIn("--require-suite PrefabRuntimeTest. --require-suite PrefabDigest.", linux)
        prefab = next(step for step in steps(linux) if "name: Qualify installed prefab runtime\n" in step)
        self.assertIn("steps.tests.outputs.prefab == 'true'", value(prefab, "if", 8))
        self.assertIn("--ctest-report build/release/ctest.junit.xml", prefab)
        netsoak = next(step for step in steps(linux) if "name: Test over-the-wire networking\n" in step)
        self.assertIn("matrix.shard != 2", value(netsoak, "if", 8))
        self.assertIn("-R '^NetSoak$'", netsoak)
        self.assertIn("--parallel 1 --output-junit netsoak.junit.xml", netsoak)
        for text in ("build/package-off", "build/package-linux", "build/release/Testing/Temporary",
                     "build/release/test-artifacts", "include-hidden-files: true"):
            self.assertIn(text, linux)
        self.assertIn("python3 -I -B tools/ci/test_ctest_shards.py", jobs(self.workflows["ci.yml"])["public-tree"])

    def test_ci_and_asan_bash_scripts_parse_without_executing_native_commands(self):
        for filename in ("ci.yml", "asan.yml"):
            for job_id, job in jobs(self.workflows[filename]).items():
                windows = value(job, "runs-on", 4).startswith("windows-")
                for step in steps(job):
                    script = value(step, "run", 8)
                    shell = value(step, "shell", 8) or ("pwsh" if windows else "bash")
                    if script is None or not shell.startswith(("bash", "msys2")):
                        continue
                    script = re.sub(r"\$\{\{.*?\}\}", "1", script)
                    with self.subTest(workflow=filename, job=job_id, step=step.splitlines()[0]):
                        result = subprocess.run(["bash", "--noprofile", "--norc", "-n", "-c", script],
                                                capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 0, result.stderr)

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
            "build/debug-asan/ctest.partition.json",
            "build/debug-asan/qualification-shard.json",
        ])

    def test_asan_aggregate_schedules_under_cancellation_and_every_dependency_result(self):
        aggregate = jobs(self.workflows["asan.yml"])["asan"]
        self.assert_aggregate_schedules(aggregate)

    def test_native_aggregates_schedule_under_cancellation_and_every_dependency_result(self):
        ci = jobs(self.workflows["ci.yml"])
        for job in ("linux-result", "windows-msvc-result", "windows-ucrt64-result"):
            with self.subTest(job=job):
                self.assert_aggregate_schedules(ci[job])

    def assert_aggregate_schedules(self, aggregate):
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

"""Path gate coverage, rename safety, and standalone Python runner contracts."""

from pathlib import Path
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ci_paths as gate
from ci_shell import bash_executable


ROOT = Path(__file__).resolve().parents[2]


BASH = bash_executable()


def run_block(workflow: str, step: str) -> str:
    section = workflow.split(f"      - name: {step}\n", 1)[1]
    lines = section.split("        run: |\n", 1)[1].splitlines()
    body = []
    for line in lines:
        if line and not line.startswith("          "):
            break
        body.append(line[10:])
    return "\n".join(body)


class PathGateTests(unittest.TestCase):
    def test_rmlui_manifest_requires_python_and_native_lanes(self):
        manifest = "vendor/rmlui/rmlui-6.1-source.json"
        for paths in ([manifest], ["docs/dependency-source.md", manifest],
                      ["cmake/patches/rmlui-6.1-source.json", manifest]):
            with self.subTest(paths=paths), patch.object(gate, "changed_paths", return_value=paths):
                self.assertTrue(gate.needs_python(manifest))
                self.assertTrue(gate.needs_cpp(manifest))
                self.assertEqual(gate.classify(ROOT, "pull_request", "base", "head",
                                               "owner/repo", "owner/repo"), (True, True))
        for path in ("vendor/rmlui/other.json", "vendor/other/rmlui-6.1-source.json"):
            with self.subTest(path=path):
                self.assertFalse(gate.needs_python(path))
                self.assertTrue(gate.needs_cpp(path))

    def setUp(self):
        history = patch.object(gate.subprocess, "check_output", return_value="false\n")
        self.history = history.start()
        self.addCleanup(history.stop)

    def test_cli_emits_all_lanes_for_non_pr_event(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            with patch.object(sys, "argv", [
                "ci_paths.py", "classify", "--event", "push", "--output", str(output),
            ]):
                self.assertEqual(gate.main(), 0)
            self.assertEqual(output.read_text(encoding="utf-8"), "needs_cpp=true\nneeds_python=true\n")

    def test_python_runner_executes_plan_and_propagates_failures(self):
        commands = [("tools/ci/test_check_docs.py",)]
        with patch.object(sys, "argv", ["ci_paths.py", "python-tests"]), patch.object(
            gate, "python_test_commands", return_value=commands
        ), patch.object(gate.subprocess, "run") as run:
            self.assertEqual(gate.main(), 0)
            run.assert_called_once_with(
                [sys.executable, "-B", "tools/ci/test_check_docs.py"], cwd=ROOT, check=True
            )
            run.side_effect = subprocess.CalledProcessError(1, "python")
            with self.assertRaises(subprocess.CalledProcessError):
                gate.main()

    def test_docs_and_covered_python_skip_builds(self):
        for path in (
            "docs/development.md", "docs/assets/example.png", "README.md", "Doxyfile",
            "tools/perf/check_residency.py", "tools/persistence/save_receipt.py",
            "tools/rendering/acceptance/scene_matrix.py", "tools/audio/bank_validate.py",
            "tools/blender/authoring/extension/recipes.py",
        ):
            with self.subTest(path=path):
                self.assertFalse(gate.needs_cpp(path))

    def test_native_build_inputs_and_unknown_paths_keep_builds(self):
        for path in (
            "src/main.cpp", "include/api.hpp", "test/unit.cpp", "tools/probe.cxx",
            "tools/CMakeLists.txt", "CMakeLists.txt", "CMakePresets.json", "cmake/options.cmake",
            "res/shaders/terrain.vert", "res/test.dat", "data/common/test.json",
            "config/settings.json", "third_party/foo.h", "vendor/CMakeLists.txt",
            "vcpkg.json", "deps/lock.json", "worlds/smoke.json", ".clang-tidy",
            ".github/workflows/docs.yml", "scripts/tidy_cache.py",
            "tools/static_preview/run_software_gl.py", "test/persistence/region_recording_overwrite.py",
            ".gitattributes", "tools/ci/qualify_prefab_runtime.py",
            "tools/ci/test_qualify_prefab_runtime.py",
            "tools/perf/capture_sim_budget.py", "tools/perf/fixture_hash.py",
            "tools/perf/render_contract.py", "tools/perf/perf.py",
            "tools/perf/test_render_runner.py", "tools/perf/synthetic_render_content.py",
            "tools/blender/geonodes_bake.py",
            "tools/blender/authoring/service/package.py",
            "tools/blender/authoring/service/launch.py",
            "tools/blender/authoring/service/luminumbra_author/prefab.py",
            "tools/blender/authoring/service/luminumbra_author/service.py",
            "tools/blender/authoring/service/tests/prefab_fixture.py",
            "test/authoring/installed_prefab_runtime.py",
            "tools/ci/new_subdirectory/native_probe.py", "unknown/file.txt",
            "tools/blender/authoring/service/tests/native_prefab.py",
            "tools/blender/authoring/probes/run_native.py",
        ):
            with self.subTest(path=path):
                self.assertTrue(gate.needs_cpp(path))

    def test_non_pr_events_always_run_everything_without_diff(self):
        with patch.object(gate, "changed_paths") as diff:
            for event in ("push", "workflow_dispatch", "schedule", ""):
                self.assertEqual(gate.classify(ROOT, event, "", "", "owner/repo", "owner/repo"), (True, True))
            diff.assert_not_called()

    def test_pr_classification_and_mixed_changes(self):
        for paths, expected in (
            (["docs/development.md"], (False, True)),
            (["tools/audio/bank_validate.py"], (False, True)),
            (["tools/ci/check_docs.py"], (True, True)),
            (["src/main.cpp"], (True, False)),
            (["docs/index.md", "src/main.cpp", "tools/ci/check_docs.py"], (True, True)),
            (["tools/perf/test_render_runner.py"], (True, True)),
            (["src/deleted.cpp", "docs/renamed.txt"], (True, False)),
            ([".github/workflows/cache-cleanup.yml"], (True, True)),
            (["scripts/README.md"], (True, True)),
        ):
            with self.subTest(paths=paths), patch.object(gate, "changed_paths", return_value=paths):
                self.assertEqual(gate.classify(ROOT, "pull_request", "base", "head", "owner/repo", "owner/repo"), expected)

    def test_ci_changes_override_allowlist_and_force_full_native_matrix(self):
        for path in (
            "tools/ci/ci_paths.py", "tools/ci/ci_shell.py", "tools/ci/test_ci_paths.py", "tools/ci/check_docs.py",
            "tools/ci/README.md", "tools/ci/nested/new_file.txt",
            ".github/workflows/ci.yml", ".github/workflows/new.yml",
            "scripts/tidy_cache.py", "scripts/README.md",
            *gate.CI_ENTRYPOINTS,
        ):
            with self.subTest(path=path), patch.object(gate, "changed_paths", return_value=[path]):
                self.assertTrue(gate.needs_cpp(path))
                self.assertTrue(gate.needs_python(path))
                self.assertEqual(gate.classify(ROOT, "pull_request", "base", "head", "owner/repo", "owner/repo"), (True, True))

    def test_every_workflow_script_entrypoint_requires_native_builds(self):
        for workflow in (ROOT / ".github/workflows").glob("*.yml"):
            paths = re.findall(r"(?:tools|scripts|test)/[A-Za-z0-9_./-]+\.(?:py|sh|ps1)\b", workflow.read_text(encoding="utf-8"))
            for path in paths:
                with self.subTest(workflow=workflow.name, path=path):
                    self.assertTrue(gate.needs_cpp(path))

    def test_workflow_bootstrap_uses_base_and_fails_open_without_executing_head(self):
        for name in ("ci", "asan", "performance"):
            workflow = (ROOT / f".github/workflows/{name}.yml").read_text(encoding="utf-8")
            script = run_block(workflow, "Classify merge-base diff (all lanes for non-PR events)")
            for sensitive, missing, failed in ((False, False, False), (True, False, False),
                                               (False, True, False), (False, False, True)):
                with self.subTest(workflow=name, sensitive=sensitive, missing=missing, failed=failed), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    fixtures = (
                        'git() { case "$1" in\n'
                        'diff) test "$FAKE_SENSITIVE" = false;;\n'
                        'show) test "$FAKE_MISSING" = false || return 1; cat "$FAKE_BASE_SCRIPT";;\n'
                        '*) return 1;;\nesac; }\n'
                        'python3() { "$FAKE_PYTHON" -I -B "$@"; }\n'
                    )
                    base = root / "base.py"
                    base.write_text(
                        "import os, sys\n"
                        "assert sys.argv[1:] == ['classify', '--root', os.environ['GITHUB_WORKSPACE']]\n"
                        "if os.environ['FAKE_FAILED'] == 'true': raise SystemExit(1)\n"
                        "with open(os.environ['GITHUB_OUTPUT'], 'a') as f:\n"
                        "    f.write('needs_cpp=false\\nneeds_python=true\\n')\n", encoding="utf-8"
                    )
                    # A PR tries to exempt itself; executing it would leave a marker.
                    head = root / "tools/ci/ci_paths.py"
                    head.parent.mkdir(parents=True)
                    marker = root / "head-executed"
                    head.write_text(f"from pathlib import Path\nPath({str(marker)!r}).touch()\n", encoding="utf-8")
                    output = root / "output"
                    env = dict(os.environ,
                               CI_EVENT="pull_request", CI_REPOSITORY="owner/repo",
                               CI_HEAD_REPOSITORY="owner/repo", CI_BASE_SHA="base", CI_HEAD_SHA="head",
                               RUNNER_TEMP=root.as_posix(), GITHUB_WORKSPACE=root.as_posix(), GITHUB_OUTPUT=output.as_posix(),
                               FAKE_BASE_SCRIPT=base.as_posix(), FAKE_PYTHON=Path(sys.executable).as_posix(),
                               FAKE_SENSITIVE=str(sensitive).lower(),
                               FAKE_MISSING=str(missing).lower(), FAKE_FAILED=str(failed).lower())
                    subprocess.run([BASH, "-e", "-o", "pipefail", "-c", fixtures + script], env=env, check=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    self.assertFalse(marker.exists())
                    expected = "needs_cpp=true\nneeds_python=true\n" if sensitive or missing or failed else "needs_cpp=false\nneeds_python=true\n"
                    self.assertEqual(output.read_text(encoding="utf-8"), expected)

    def test_required_jobs_run_python_plan_on_mixed_changes_and_publish_route(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        for job in ("linux", "windows-msvc"):
            # Split on a job boundary, not nested YAML mappings.
            body = re.split(r"\n  [\w-]+:\n", workflow.split(f"  {job}:\n", 1)[1])[0]
            for step in ("Install standalone Python test dependencies",
                         "Run standalone Python contracts (including CTest-registered suites)"):
                condition = body.split(f"      - name: {step}\n", 1)[1].splitlines()[0].strip()
                expression = condition.removeprefix("if: ${{ ").removesuffix(" }}")
                for cpp, python, expected in (("true", "true", True), ("false", "true", True),
                                               ("true", "false", False), ("false", "false", True),
                                               ("", "", True)):
                    actual = expression.replace("needs.changes.outputs.needs_cpp", repr(cpp)).replace(
                        "needs.changes.outputs.needs_python", repr(python)
                    ).replace("||", "or").replace("&&", "and")
                    with self.subTest(job=job, step=step, cpp=cpp, python=python):
                        self.assertEqual(eval(actual, {"__builtins__": {}}), expected)
            self.assertIn("route: ${{ steps.route.outputs.route }}", body)
            self.assertIn("tools/ci/ci_paths.py python-tests", body)
            self.assertIn('echo "$message" >> "$GITHUB_STEP_SUMMARY"', body)
            self.assertIn("native build skipped by Classify changed paths", body)

    def test_route_is_visible_in_log_summary_and_step_output(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        for job in ("linux", "windows-msvc"):
            script = run_block(workflow.split(f"  {job}:\n", 1)[1], "Report validation route")
            for cpp, route in (("false", "python-only"), ("true", "native"), ("", "native")):
                with self.subTest(job=job, cpp=cpp), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory)
                    env = dict(os.environ, NEEDS_CPP=cpp, GITHUB_OUTPUT=(root / "output").as_posix(),
                               GITHUB_STEP_SUMMARY=(root / "summary").as_posix())
                    result = subprocess.run([BASH, "-e", "-c", script], env=env, check=True,
                                            stdout=subprocess.PIPE, text=True)
                    self.assertEqual((root / "output").read_text(encoding="utf-8"), f"route={route}\n")
                    self.assertEqual((root / "summary").read_text(encoding="utf-8"), result.stdout)
                    self.assertIn(f"route: {route}", result.stdout)
                    if cpp == "false":
                        self.assertIn("native build skipped by Classify changed paths", result.stdout)

    def test_job_routes_keep_draft_guards_and_fail_open_native_analysis(self):
        workflows = {
            name: (ROOT / f".github/workflows/{name}.yml").read_text(encoding="utf-8")
            for name in ("ci", "asan", "performance")
        }
        routes = (
            ("ci", "linux", "guarded-python"),
            ("ci", "windows-msvc", "ready-python"),
            ("ci", "gns-compile-probe", "ready-native"),
            ("ci", "diligent-rhi", "ready-native"),
            ("ci", "tidy-shard", "ready-native"),
            ("ci", "tidy", "aggregate"),
            ("ci", "linux-result", "required-aggregate"),
            ("ci", "windows-msvc-result", "required-aggregate"),
            ("ci", "windows-ucrt64-result", "required-aggregate"),
            ("asan", "asan-shard", "guarded-native"),
            ("asan", "asan", "required-aggregate"),
            ("performance", "preview-frame", "guarded-native"),
        )
        for workflow, job, route in routes:
            body = re.split(r"\n  [\w-]+:\n", workflows[workflow].split(f"  {job}:\n", 1)[1])[0]
            condition = re.search(r"^    if: (.*)$", body, re.MULTILINE)[1]
            expression = condition.removeprefix("${{ ").removesuffix(" }}")
            # Missing/failed classifier output is represented by the empty string.
            for event in ("pull_request", "push", "schedule", "workflow_dispatch"):
                for draft in (True, False, None):
                    for cpp in ("true", "false", ""):
                        for cancelled in (False, True):
                            ready = event != "pull_request" or draft is False
                            if route == "required-aggregate":
                                expected = not cancelled
                            elif route == "aggregate":
                                expected = not cancelled and ready
                            elif route == "guarded-python":
                                expected = not cancelled
                            elif route == "guarded-native":
                                expected = not cancelled and (not ready or cpp != "false")
                            else:
                                expected = not cancelled and ready and (
                                    route == "ready-python" or cpp != "false"
                                )
                            actual = re.sub(r"needs\.[\w-]+\.result", repr("success"), expression).replace(
                                "toJSON(github.event.pull_request.draft)", repr(json.dumps(draft))
                            ).replace("!cancelled()", repr(not cancelled)).replace(
                                "always()", "True"
                            ).replace("github.event_name", repr(event)).replace(
                                "github.event.pull_request.draft", repr(draft)
                            ).replace("needs.changes.result", repr("success")).replace(
                                "needs.asan-shard.result", repr("success")
                            ).replace("needs.tidy-shard.result", repr("success")
                            ).replace("needs.changes.outputs.needs_cpp", repr(cpp)).replace(
                                "== false", "== False"
                            ).replace("!= false", "!= False").replace("&&", "and").replace("||", "or")
                            with self.subTest(workflow=workflow, job=job, event=event,
                                              draft=draft, cpp=cpp, cancelled=cancelled):
                                self.assertEqual(eval(actual, {"__builtins__": {}}), expected)

    def test_required_aggregate_graph_keeps_dependency_failures_reachable(self):
        for filename, producer, aggregate in (
            ("ci", "linux", "linux-result"),
            ("ci", "windows-msvc", "windows-msvc-result"),
            ("ci", "windows-ucrt64", "windows-ucrt64-result"),
            ("ci", "tidy-shard", "tidy"),
            ("asan", "asan-shard", "asan"),
        ):
            workflow = (ROOT / f".github/workflows/{filename}.yml").read_text(encoding="utf-8")
            body = re.split(r"\n  [\w-]+:\n", workflow.split(f"  {aggregate}:\n", 1)[1])[0]
            condition = re.search(r"^    if: (.*)$", body, re.MULTILINE)[1]
            for classification in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                for result in ("success", "failure", "cancelled", "skipped", "", "unknown"):
                    invalid = classification != "success" or result not in ("success", "skipped")
                    for workflow_cancelled in (False, True):
                        expression = condition.removeprefix("${{ ").removesuffix(" }}")
                        expression = expression.replace("!cancelled()", repr(not workflow_cancelled))
                        expression = expression.replace("always()", "True").replace("failure()", repr("failure" in (classification, result)))
                        expression = expression.replace("needs.changes.result", repr(classification))
                        expression = expression.replace(f"needs.{producer}.result", repr(result))
                        expression = expression.replace("github.event_name", repr("push"))
                        expression = expression.replace("github.event.pull_request.draft", "False")
                        expression = expression.replace("== false", "== False").replace("&&", "and").replace("||", "or")
                        with self.subTest(job=aggregate, classification=classification,
                                          result=result, cancelled=workflow_cancelled):
                            self.assertEqual(bool(eval(expression, {"__builtins__": {}})),
                                             not workflow_cancelled or invalid)

    def test_new_files_in_covered_directories_require_builds(self):
        for path in gate.PYTHON_MODULES | gate.AUTHORING_PYTHON_FILES:
            unknown = str(Path(path).parent / "new_native_tool.py")
            with self.subTest(path=unknown):
                self.assertTrue(gate.needs_cpp(unknown))

    def test_installed_prefab_service_modules_require_builds(self):
        # The gate packages every service module; helper tests alone cannot cover
        # its execution with the native processor and installed ECS consumer.
        for module in (ROOT / "tools/blender/authoring/service/luminumbra_author").glob("*.py"):
            with self.subTest(path=module):
                self.assertTrue(gate.needs_cpp(module.relative_to(ROOT).as_posix()))

    def test_fork_or_missing_repository_identity_runs_everything(self):
        for repository, head_repository in (
            ("owner/repo", "fork/repo"), ("", "owner/repo"), ("owner/repo", ""), ("", ""),
        ):
            with self.subTest(repository=repository, head_repository=head_repository), patch.object(
                gate, "changed_paths", return_value=["docs/development.md"]
            ) as diff:
                self.assertEqual(gate.classify(
                    ROOT, "pull_request", "base", "head", repository, head_repository,
                ), (True, True))
                diff.assert_not_called()
        self.history.assert_not_called()

    def test_shallow_or_unavailable_history_runs_everything(self):
        for outcome in ("true\n", "", "unexpected\n", OSError("missing git"),
                        subprocess.CalledProcessError(1, "git")):
            with self.subTest(outcome=outcome), patch.object(gate, "changed_paths") as diff:
                self.history.side_effect = outcome if isinstance(outcome, Exception) else None
                self.history.return_value = outcome
                self.assertEqual(gate.classify(
                    ROOT, "pull_request", "base", "head", "owner/repo", "owner/repo",
                ), (True, True))
                self.history.assert_called_with(
                    ["git", "rev-parse", "--is-shallow-repository"], cwd=ROOT, text=True,
                )
                diff.assert_not_called()

    def test_pr_cli_passes_repository_identity(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(gate, "changed_paths", return_value=[
            "docs/development.md",
        ]):
            output = Path(directory) / "output"
            with patch.object(sys, "argv", [
                "ci_paths.py", "classify", "--event", "pull_request",
                "--base", "base", "--head", "head", "--repository", "owner/repo",
                "--head-repository", "owner/repo", "--output", str(output),
            ]):
                self.assertEqual(gate.main(), 0)
            self.assertEqual(output.read_text(encoding="utf-8"), "needs_cpp=false\nneeds_python=true\n")

    def test_unavailable_or_empty_diff_fails_open(self):
        for error in (OSError("missing git"), subprocess.CalledProcessError(1, "git")):
            with patch.object(gate, "changed_paths", side_effect=error):
                self.assertEqual(gate.classify(ROOT, "pull_request", "base", "head", "owner/repo", "owner/repo"), (True, True))
        with patch.object(gate, "changed_paths", return_value=[]):
            self.assertEqual(gate.classify(ROOT, "pull_request", "base", "head", "owner/repo", "owner/repo"), (True, True))
        self.assertEqual(gate.classify(ROOT, "pull_request", "", "head", "owner/repo", "owner/repo"), (True, True))

    def test_diff_uses_merge_base_and_preserves_deleted_and_unusual_names(self):
        with patch.object(gate.subprocess, "check_output", side_effect=[
            "ancestor\n", b"src/old.cpp\0docs/new name\nfile.md\0",
        ]) as git:
            self.assertEqual(gate.changed_paths(ROOT, "base", "head"), [
                "src/old.cpp", "docs/new name\nfile.md",
            ])
            self.assertEqual(git.call_args_list[0].args[0], ["git", "merge-base", "base", "head"])
            self.assertEqual(git.call_args_list[1].args[0], [
                "git", "diff", "--name-only", "--no-renames", "-z", "ancestor", "head", "--",
            ])

    def test_windows_and_posix_plans_have_identical_coverage(self):
        with patch.object(gate.os, "name", "posix"):
            posix_commands = gate.python_test_commands(ROOT)
        with patch.object(gate.os, "name", "nt"):
            commands = gate.python_test_commands(ROOT)
        self.assertEqual(commands, posix_commands)
        self.assertIn(("tools/ci/test_tidy_cache.py",), commands)
        for test in gate.PYTHON_TESTS:
            self.assertIn((test,), commands)

    def test_standalone_plan_covers_ctest_python_contracts_without_native_runner(self):
        commands = gate.python_test_commands(ROOT)
        for test in (
            "tools/ci/test_ci_paths.py", "tools/ci/test_qualification_capture.py",
            "tools/ci/test_asan_shards.py", "tools/ci/test_ctest_shards.py",
            "tools/ci/test_workflow_policy.py",
            "tools/perf/test_capture_sim_budget.py", "tools/perf/test_foliage_evidence.py",
            "tools/perf/test_terrain_aovs.py", "tools/perf/test_render_runner_evidence.py",
            "tools/release/test_sbom.py", "tools/persistence/test_save_receipt.py",
            "tools/rendering/acceptance/test_scene_matrix.py",
        ):
            with self.subTest(test=test):
                self.assertIn((test,), commands)
        self.assertIn(("tools/timelapse.py", "--selftest"), commands)
        self.assertNotIn(("tools/perf/test_render_runner.py",), commands)
        for command in commands:
            self.assertTrue((ROOT / command[0]).is_file(), command)


if __name__ == "__main__":
    unittest.main()

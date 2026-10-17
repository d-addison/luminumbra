#!/usr/bin/env python3
"""Offline routing and persistent Linux runner preparation contracts."""

import importlib.util
import itertools
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("workflow_policy", Path(__file__).with_name("test_workflow_policy.py"))
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)
SELECTED = {"ci.yml": {"linux", "tidy-shard", "lint", "public-tree"}, "asan.yml": {"asan"}}
PACKAGES = (
    "build-essential ccache cmake extra-cmake-modules libfreetype6-dev "
    "libgl1-mesa-dev libglu1-mesa-dev libwayland-dev libxkbcommon-dev "
    "ninja-build pkg-config python3-numpy python3-pil python3-jsonschema "
    "wayland-protocols xauth xorg-dev xvfb clang clang-tidy clang-format-18"
).split()
TOOLS = "dpkg-query git gcc g++ make ccache cmake ctest ninja pkg-config python3 Xvfb xvfb-run xauth clang clang++ clang-tidy clang-format-18".split()


def select_runner(expression, variable="", event="push", head="", repository="owner/repo"):
    """Evaluate the workflow's actual &&/|| expression, including its JSON array.

    These nonempty arrays and strings have the same truthiness as Actions values;
    Actions string comparisons are case insensitive. No matrix, needs or runner
    context is supplied: runs-on must be decidable before a runner is allocated.
    """
    if not expression.startswith("${{ "):
        return expression
    expression = expression.removeprefix("${{ ").removesuffix(" }}")
    for context, replacement in (
        ("github.event.pull_request.head.repo.full_name", head.lower()),
        ("github.repository", repository.lower()), ("github.event_name", event.lower()),
        ("vars.LUM_SELF_HOSTED", variable.lower()),
    ):
        expression = expression.replace(context, repr(replacement))
    expression = expression.replace("&&", "and").replace("||", "or")
    return eval(expression, {"__builtins__": {}, "fromJSON": json.loads})


class RunnerRoutingTests(unittest.TestCase):
    def selected_jobs(self):
        for filename, selected in SELECTED.items():
            workflow = (ROOT / ".github/workflows" / filename).read_text(encoding="utf-8")
            for name, body in POLICY.jobs(workflow).items():
                if name in selected:
                    yield filename, name, body

    def test_identical_expression_and_only_requested_jobs_are_opted_in(self):
        found = set()
        for path in (ROOT / ".github/workflows").glob("*.yml"):
            for name, body in POLICY.jobs(path.read_text(encoding="utf-8")).items():
                runner = POLICY.value(body, "runs-on", 4)
                if "LUM_SELF_HOSTED" in runner or "self-hosted" in runner:
                    found.add((path.name, name))
                    self.assertEqual(runner, POLICY.SELF_HOSTED_RUNS_ON)
        self.assertEqual(found, {(filename, name) for filename, names in SELECTED.items() for name in names})

    def test_unset_variable_and_kill_switch_always_select_hosted(self):
        for filename, name, body in self.selected_jobs():
            for variable, event, head in itertools.product(
                ("", "false", "0", "true ", "unknown"),
                ("push", "workflow_dispatch", "pull_request", "schedule", "workflow_run"),
                ("owner/repo", "fork/repo", ""),
            ):
                with self.subTest(workflow=filename, job=name, variable=variable, event=event, head=head):
                    self.assertEqual(select_runner(POLICY.value(body, "runs-on", 4), variable, event, head), "ubuntu-24.04")

    def test_fork_prs_and_untrusted_or_unknown_events_cannot_select_self_hosted(self):
        for filename, name, body in self.selected_jobs():
            expression = POLICY.value(body, "runs-on", 4)
            for event, head, repository in itertools.product(
                ("pull_request", "pull_request_target", "workflow_run", "schedule", "merge_group", "repository_dispatch", ""),
                ("owner/repo", "fork/repo", ""), ("owner/repo", ""),
            ):
                expected = (["self-hosted", "Linux", "X64", "lum-vq1"]
                            if event == "pull_request" and head == repository and head else "ubuntu-24.04")
                with self.subTest(workflow=filename, job=name, event=event, head=head, repository=repository):
                    self.assertEqual(select_runner(expression, "true", event, head, repository), expected)

    def test_push_missing_pr_context_manual_and_same_repository_pr_select_host(self):
        for filename, name, body in self.selected_jobs():
            for variable, (event, head) in itertools.product(
                ("true", "TRUE"), (("push", ""), ("workflow_dispatch", ""), ("pull_request", "owner/repo")),
            ):
                with self.subTest(workflow=filename, job=name, event=event):
                    self.assertEqual(select_runner(POLICY.value(body, "runs-on", 4), variable, event, head),
                                     ["self-hosted", "Linux", "X64", "lum-vq1"])

    def test_preparation_precedes_consumers_and_all_apt_steps_are_hosted_only(self):
        for filename, name, body in self.selected_jobs():
            steps = POLICY.steps(body)
            prepare = next(step for step in steps if "name: Prepare and verify self-hosted runner\n" in step)
            self.assertEqual(POLICY.value(prepare, "if", 8), "runner.environment == 'self-hosted'")
            self.assertEqual(POLICY.value(prepare, "run", 8), "bash tools/ci/prepare_self_hosted.sh")
            index = steps.index(prepare)
            self.assertIn("uses: actions/checkout@", steps[index - 1])
            self.assertEqual(index, 2 if name in ("linux", "asan") else 1)
            for step in steps:
                if "sudo apt-get" not in step:
                    continue
                condition = POLICY.value(step, "if", 8)
                self.assertIn("runner.environment == 'github-hosted'", condition)
                # Evaluate the actual step guard with the ordinary path gate open.
                for environment in ("self-hosted", "github-hosted", ""):
                    actual = condition.removeprefix("${{ ").removesuffix(" }}")
                    for key, replacement in (("runner.environment", environment),
                                             ("needs.changes.outputs.needs_cpp", "true"),
                                             ("needs.changes.outputs.needs_python", "true")):
                        actual = actual.replace(key, repr(replacement))
                    actual = actual.replace("&&", "and").replace("||", "or")
                    self.assertEqual(eval(actual, {"__builtins__": {}}), environment == "github-hosted")

    def test_matrix_and_skipped_job_contexts_do_not_affect_runner_selection(self):
        ci = POLICY.jobs((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
        self.assertIn("shard: [1, 2, 3]", ci["tidy-shard"])
        self.assertIn("--parallel 4", ci["tidy-shard"])
        for _, _, body in self.selected_jobs():
            expression = POLICY.value(body, "runs-on", 4)
            self.assertNotRegex(expression, r"\b(?:matrix|needs|runner|steps|inputs)\.")
            # Job if is evaluated before matrix expansion; no runner is allocated
            # for a skipped job. Its runs-on expression is still well formed.
            self.assertEqual(select_runner(expression), "ubuntu-24.04")

    def test_performance_matrix_and_display_jobs_stay_hosted(self):
        perf = POLICY.jobs((ROOT / ".github/workflows/performance.yml").read_text(encoding="utf-8"))
        self.assertEqual(POLICY.value(perf["runner-contract"], "runs-on", 4), "${{ matrix.os }}")
        for name in ("preview-frame", "server-smoke-comparison", "qualification-capture"):
            self.assertEqual(POLICY.value(perf[name], "runs-on", 4), "ubuntu-24.04")

    def test_caches_keep_paths_caps_and_push_only_writes(self):
        for filename, name, body in self.selected_jobs():
            for step in POLICY.steps(body):
                if "uses: actions/cache/save@" in step:
                    condition = POLICY.value(step, "if", 8)
                    self.assertIn("github.event_name == 'push'", condition)
                    self.assertIn("github.ref == 'refs/heads/main' || github.ref == 'refs/heads/devel'", condition)
                if "compiler cache" in step and "uses: actions/cache/" in step:
                    self.assertEqual(POLICY.value(step, "path", 10), "~/.cache/ccache")
            if name in ("linux", "asan"):
                self.assertEqual(POLICY.value(body, "CCACHE_MAXSIZE", 6), "400M" if name == "linux" else "600M")

    def test_suite_is_registered_in_both_workflow_and_python_plan(self):
        path = "tools/ci/test_self_hosted_runner.py"
        ci = POLICY.jobs((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
        self.assertIn(f"python3 {path}\n", ci["public-tree"])
        spec = importlib.util.spec_from_file_location("ci_paths", ROOT / "tools/ci/ci_paths.py")
        paths = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(paths)
        self.assertIn(path, paths.PYTHON_TESTS)
        self.assertIn((path,), paths.python_test_commands(ROOT))
        docs = (ROOT / "docs/development.md").read_text(encoding="utf-8")
        self.assertIn("`LUM_SELF_HOSTED` is an opt-in and kill switch", docs)


class RunnerPreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="runner-preparation-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.checkout = self.root / "checkout"
        self.checkout.mkdir()
        self.runner_temp = self.root / "runner-temp"
        self.runner_temp.mkdir()
        self.env_file = self.root / "environment"
        self.path_file = self.root / "path"
        self.package_log = self.root / "packages"
        self.outside = self.root / "unrelated"
        self.outside.write_text("keep", encoding="utf-8")
        (self.checkout / ".gitignore").write_text("build/\n.tidy-cache/\n", encoding="utf-8")
        for args in (("init", "--quiet"), ("add", ".gitignore")):
            subprocess.run(["git", "-C", str(self.checkout), *args], check=True, capture_output=True)
        for name in ("build", ".tidy-cache"):
            (self.checkout / name).mkdir()
            (self.checkout / name / "stale").touch()
        self.env = dict(os.environ, RUNNER_TEMP=self.runner_temp.as_posix(),
                        GITHUB_ENV=self.env_file.as_posix(), GITHUB_PATH=self.path_file.as_posix(),
                        PACKAGE_LOG=self.package_log.as_posix(), MISSING_PACKAGES="", MISSING_TOOLS="",
                        PACKAGE_STATUS="install ok installed", REFUSE_ROOT="false", CHANGE_DIRECTORY="false")

    def prepare(self):
        # Record provisioning queries without depending on the test machine's
        # installed toolchain. The real git clean runs in a disposable checkout.
        # Normalize the fake root to Bash's spelling for Windows test hosts too.
        fixture = r'''
        export GITHUB_WORKSPACE="$PWD"
        if [[ "$CHANGE_DIRECTORY" == true ]]; then mkdir child; cd child; fi
        git() {
            if [[ "$3" == rev-parse ]]; then
                if [[ "$REFUSE_ROOT" == true ]]; then echo "$GITHUB_WORKSPACE/other"; else echo "$GITHUB_WORKSPACE"; fi
            else builtin command git "$@"; fi
        }
        dpkg-query() {
            echo "$3" >> "$PACKAGE_LOG"
            if [[ " $MISSING_PACKAGES " == *" $3 "* ]]; then return 1; fi
            printf '%s' "$PACKAGE_STATUS"
        }
        command() {
            if [[ "$1" == -v ]]; then
                if [[ " $MISSING_TOOLS " == *" $2 "* ]]; then return 1; fi
                printf '/usr/bin/true\n'
            else builtin command "$@"; fi
        }
        source "$1"
        '''
        return subprocess.run(["bash", "--noprofile", "--norc", "-e", "-c", fixture,
                               "prepare", (ROOT / "tools/ci/prepare_self_hosted.sh").as_posix()],
                              cwd=self.checkout, env=self.env, capture_output=True, text=True, timeout=10)

    def environment(self):
        return dict(line.split("=", 1) for line in self.env_file.read_text(encoding="utf-8").splitlines())

    def test_removes_only_checkout_leftovers_and_isolates_home_temp_and_cache(self):
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.checkout / "build").exists())
        self.assertFalse((self.checkout / ".tidy-cache").exists())
        self.assertTrue((self.checkout / ".gitignore").is_file())
        self.assertEqual(self.outside.read_text(encoding="utf-8"), "keep")
        self.assertEqual(self.package_log.read_text(encoding="utf-8").splitlines(), PACKAGES)
        first = self.environment()
        self.assertEqual(first["CCACHE_DIR"], first["HOME"] + "/.cache/ccache")
        self.assertEqual(first["XDG_CACHE_HOME"], first["HOME"] + "/.cache")
        self.assertEqual(first["PYTHONNOUSERSITE"], "1")
        self.assertEqual(first["TMPDIR"], first["TMP"])
        self.assertEqual(first["TMPDIR"], first["TEMP"])
        self.assertEqual(first["CCACHE_TEMPDIR"], first["TMPDIR"])
        self.assertNotIn("CCACHE_MAXSIZE", first)
        for key in ("HOME", "TMPDIR", "XDG_RUNTIME_DIR"):
            # Bash can emit a drive path in a different spelling on Windows.
            self.assertIn("/runner-temp/lum-ci.", first[key])
        # Simulate a PR-written cache and a second job on the same instance.
        # Resolve via the known fixture directory on both POSIX and Windows.
        homes = list(self.runner_temp.glob("lum-ci.*/home"))
        self.assertEqual(len(homes), 1)
        private_bin = homes[0].parent / "bin"
        self.assertTrue((private_bin / "clang-format").exists())
        self.assertTrue((private_bin / "python").exists())
        self.assertEqual(self.path_file.read_text(encoding="utf-8").strip(), first["HOME"].removesuffix("/home") + "/bin")
        self.assertEqual((homes[0] / ".config/ccache/ccache.conf").read_bytes(), b"")
        (homes[0] / ".cache/ccache/pr-entry").touch()
        self.env_file.unlink()
        self.path_file.unlink()
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(first["HOME"], self.environment()["HOME"])
        homes = list(self.runner_temp.glob("lum-ci.*/home"))
        self.assertEqual(len(homes), 2)
        self.assertEqual(sum((home / ".cache/ccache/pr-entry").exists() for home in homes), 1)

    def test_reports_all_missing_packages_and_commands_before_environment_writes(self):
        self.env.update(MISSING_PACKAGES=" ".join(PACKAGES), MISSING_TOOLS=" ".join(TOOLS))
        result = self.prepare()
        self.assertEqual(result.returncode, 1)
        for kind, names in (("package", PACKAGES), ("command", TOOLS)):
            for name in names:
                self.assertIn(f"{kind}:{name}", result.stderr)
        self.assertEqual(self.package_log.read_text(encoding="utf-8").splitlines(), PACKAGES)
        self.assertFalse(self.env_file.exists())
        self.assertFalse(self.path_file.exists())
        self.assertEqual(list(self.runner_temp.iterdir()), [])

    def test_partial_unknown_and_removed_package_statuses_fail_closed(self):
        for status in ("", "install ok", "install ok unpacked", "deinstall ok config-files", "unknown"):
            with self.subTest(status=status):
                self.env["PACKAGE_STATUS"] = status
                result = self.prepare()
                self.assertEqual(result.returncode, 1)
                for package in PACKAGES:
                    self.assertIn(f"package:{package}", result.stderr)
                self.assertFalse(self.env_file.exists())

    def test_wrong_checkout_root_refuses_before_cleaning(self):
        self.env["REFUSE_ROOT"] = "true"
        result = self.prepare()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Expected the checkout root", result.stderr)
        self.assertTrue((self.checkout / "build/stale").is_file())
        self.assertEqual(self.outside.read_text(encoding="utf-8"), "keep")
        self.assertFalse(self.package_log.exists())
        self.assertFalse(self.env_file.exists())

    def test_unavailable_runner_temp_fails_without_publishing_environment(self):
        self.env["RUNNER_TEMP"] = (self.root / "missing/runner-temp").as_posix()
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.env_file.exists())
        self.assertFalse(self.path_file.exists())

    def test_subdirectory_working_directory_refuses_before_cleaning(self):
        self.env["CHANGE_DIRECTORY"] = "true"
        result = self.prepare()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Expected the checkout root", result.stderr)
        self.assertTrue((self.checkout / "build/stale").is_file())
        self.assertFalse(self.package_log.exists())
        self.assertFalse(self.env_file.exists())


if __name__ == "__main__":
    unittest.main()

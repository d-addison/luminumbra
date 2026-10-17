#!/usr/bin/env python3
"""Offline routing and persistent Linux runner preparation contracts."""

import importlib.util
import itertools
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("workflow_policy", Path(__file__).with_name("test_workflow_policy.py"))
POLICY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POLICY)
HOOK_SPEC = importlib.util.spec_from_file_location("self_hosted_job_hook", Path(__file__).with_name("self_hosted_job_hook.py"))
HOOK = importlib.util.module_from_spec(HOOK_SPEC)
HOOK_SPEC.loader.exec_module(HOOK)
SELECTED = {"ci.yml": {"linux", "tidy-shard", "lint", "public-tree"}, "asan.yml": {"asan"}}
PACKAGES = (
    "build-essential ccache cmake extra-cmake-modules libfreetype6-dev "
    "libgl1-mesa-dev libglu1-mesa-dev libwayland-dev libxkbcommon-dev "
    "ninja-build pkg-config python3-numpy python3-pil python3-jsonschema "
    "wayland-protocols xauth xorg-dev xvfb clang clang-tidy clang-format-18"
).split()
TOOLS = "dpkg-query git gcc g++ make ccache cmake ctest ninja pkg-config python3 Xvfb xvfb-run xauth clang clang++ clang-tidy clang-format-18".split()


def bash_executable(windows=None, git=None):
    """Native Windows needs Git Bash, not the System32 WSL launcher."""
    if windows is None:
        windows = os.name == "nt"
    if windows:
        executable = git or shutil.which("git")
        if executable:
            for parent in Path(executable).resolve().parents:
                candidate = parent / "bin/bash.exe"
                if candidate.is_file():
                    return str(candidate)
        raise RuntimeError("Git for Windows Bash is required for Linux shell fixtures")
    executable = shutil.which("bash")
    if executable is None:
        raise RuntimeError("Bash is required for Linux shell fixtures")
    return executable


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
                ("pull_request", "pull_request_target", "workflow_run", "schedule", "workflow_dispatch", "merge_group", "repository_dispatch", ""),
                ("owner/repo", "fork/repo", ""), ("owner/repo", ""),
            ):
                expected = (["self-hosted", "Linux", "X64", "lum-vq1"]
                            if event == "pull_request" and head == repository and head else "ubuntu-24.04")
                with self.subTest(workflow=filename, job=name, event=event, head=head, repository=repository):
                    self.assertEqual(select_runner(expression, "true", event, head, repository), expected)

    def test_push_missing_pr_context_and_same_repository_pr_select_host(self):
        for filename, name, body in self.selected_jobs():
            for variable, (event, head) in itertools.product(
                ("true", "TRUE"), (("push", ""), ("pull_request", "owner/repo")),
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

    def test_caches_match_compiler_paths_keep_caps_and_push_only_writes(self):
        for filename, name, body in self.selected_jobs():
            keys = []
            for step in POLICY.steps(body):
                if "uses: actions/cache/save@" in step:
                    condition = POLICY.value(step, "if", 8)
                    self.assertIn("github.event_name == 'push'", condition)
                    self.assertIn("github.ref == 'refs/heads/main' || github.ref == 'refs/heads/devel'", condition)
                if "compiler cache" in step and "uses: actions/cache/" in step:
                    self.assertEqual(POLICY.value(step, "path", 10), ".cache/ccache")
                    keys.append(POLICY.value(step, "key", 10))
                    if "restore-keys:" in step:
                        self.assertIn("ccache-checkout-v2-", step)
                        self.assertNotIn("ccache-${{", step)
            if name in ("linux", "asan"):
                self.assertEqual(POLICY.value(body, "CCACHE_MAXSIZE", 6), "400M" if name == "linux" else "600M")
                self.assertEqual(POLICY.value(body, "CCACHE_DIR", 6), "${{ github.workspace }}/.cache/ccache")
                self.assertEqual(len(keys), 2)
                self.assertEqual(keys[0], keys[1])
                self.assertIn("ccache-checkout-v2-", keys[0])

    def test_suite_is_registered_in_both_workflow_and_python_plan(self):
        path = "tools/ci/test_self_hosted_runner.py"
        ci = POLICY.jobs((ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
        self.assertIn(f"python3 {path}\n", ci["public-tree"])
        spec = importlib.util.spec_from_file_location("ci_paths", ROOT / "tools/ci/ci_paths.py")
        paths = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(paths)
        self.assertIn(path, paths.PYTHON_TESTS)
        self.assertIn((path,), paths.python_test_commands(ROOT))
        cmake = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/ci/test_self_hosted_runner.py"', cmake)
        docs = (ROOT / "docs/development.md").read_text(encoding="utf-8")
        self.assertIn("`LUM_SELF_HOSTED` is an opt-in and kill switch", docs)


class RunnerPreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="runner-preparation-")
        self.addCleanup(self.temp.cleanup)
        # Resolve existing parents before exporting paths; Windows TEMP may use
        # an 8.3 alias while Git Bash reports the corresponding long cwd.
        self.root = Path(self.temp.name).resolve()
        self.checkout = self.root / "_work/luminumbra/luminumbra"
        self.checkout.mkdir(parents=True)
        self.runner_temp = self.root / "_work/_temp"
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
                        PACKAGE_STATUS="install ok installed", REFUSE_ROOT="false", CHANGE_DIRECTORY="false",
                        REFUSE_LINK="false")
        for key in ("BASH_ENV", "ENV", "CDPATH", "GIT_DIR", "GIT_WORK_TREE"):
            self.env.pop(key, None)

    def prepare(self):
        # Record provisioning queries without depending on the test machine's
        # installed toolchain. The real git clean runs in a disposable checkout.
        # Normalize the fake root to Bash's spelling for Windows test hosts too.
        fixture = r'''
        : "${MISSING_PACKAGES=}" "${MISSING_TOOLS=}" "${PACKAGE_STATUS=}"
        # Convert native drive/8.3 spellings through the shell that uses them.
        # In particular, never let a WSL launcher interpret a Windows cwd.
        if builtin command -v cygpath >/dev/null; then
            for key in RUNNER_TEMP GITHUB_ENV GITHUB_PATH PACKAGE_LOG; do
                printf -v "$key" '%s' "$(cygpath -u -- "${!key}")"
                export "$key"
            done
            script=$(cygpath -u -- "$1")
        else script=$1; fi
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
        # Provisioning links are modelled inside this subprocess. Git for
        # Windows need not have symlink privileges to check the shell contract.
        ln() {
            if [[ "$REFUSE_LINK" == true ]]; then return 1; fi
            builtin command cp /usr/bin/true "$3"
        }
        source "$script"
        printf 'fixture-workspace=%s\n' "$GITHUB_WORKSPACE"
        '''
        return subprocess.run([bash_executable(), "--noprofile", "--norc", "-e", "-c", fixture,
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
        workspace = next(line.split("=", 1)[1] for line in result.stdout.splitlines()
                         if line.startswith("fixture-workspace="))
        self.assertEqual(first["CCACHE_DIR"], workspace + "/.cache/ccache")
        self.assertEqual(first["XDG_CACHE_HOME"], first["HOME"] + "/.cache")
        self.assertEqual(first["PYTHONNOUSERSITE"], "1")
        self.assertEqual(first["TMPDIR"], first["TMP"])
        self.assertEqual(first["TMPDIR"], first["TEMP"])
        self.assertEqual(first["CCACHE_TEMPDIR"], first["TMPDIR"])
        self.assertNotIn("CCACHE_MAXSIZE", first)
        for key in ("HOME", "TMPDIR", "XDG_RUNTIME_DIR"):
            # Bash can emit a drive path in a different spelling on Windows.
            self.assertIn("/_temp/lum-ci.", first[key])
        # Simulate a PR-written cache and a second job on the same instance.
        # Resolve via the known fixture directory on both POSIX and Windows.
        homes = list(self.runner_temp.glob("lum-ci.*/home"))
        self.assertEqual(len(homes), 1)
        private_bin = homes[0].parent / "bin"
        self.assertTrue((private_bin / "clang-format").exists())
        self.assertTrue((private_bin / "python").exists())
        self.assertEqual(self.path_file.read_text(encoding="utf-8").strip(), first["HOME"].removesuffix("/home") + "/bin")
        self.assertEqual((homes[0] / ".config/ccache/ccache.conf").read_bytes(), b"")
        cache = self.checkout / ".cache/ccache"
        (cache / "pr-entry").touch()
        self.env_file.unlink()
        self.path_file.unlink()
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(first["HOME"], self.environment()["HOME"])
        self.assertEqual(first["CCACHE_DIR"], self.environment()["CCACHE_DIR"])
        homes = list(self.runner_temp.glob("lum-ci.*/home"))
        self.assertEqual(len(homes), 2)
        self.assertFalse((cache / "pr-entry").exists())
        # Model Actions restoring to its checkout-relative path. The compiler
        # directory is that same stable location on a later job too.
        (cache / "restored-entry").write_text("restored", encoding="utf-8")
        restored = subprocess.run([bash_executable(), "--noprofile", "--norc", "-c",
                                   'cat -- "$1/restored-entry"', "cache", self.environment()["CCACHE_DIR"]],
                                  env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(restored.returncode, 0, restored.stderr)
        self.assertEqual(restored.stdout, "restored")

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
        self.assertTrue((self.checkout / "build/stale").is_file())

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
        self.env["RUNNER_TEMP"] = (self.root / "missing/_temp").as_posix()
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.env_file.exists())
        self.assertFalse(self.path_file.exists())

    def test_link_creation_denied_does_not_publish_environment(self):
        self.env["REFUSE_LINK"] = "true"
        result = self.prepare()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.env_file.exists())
        self.assertFalse(self.path_file.exists())

    def test_git_bash_selection_ignores_wsl_launcher_and_handles_spaces(self):
        install = self.root / "Program Files/Git"
        (install / "cmd").mkdir(parents=True)
        (install / "bin").mkdir()
        (install / "bin/bash.exe").touch()
        self.assertEqual(bash_executable(windows=True, git=str(install / "cmd/git.exe")),
                         str((install / "bin/bash.exe").resolve()))
        with self.assertRaises(RuntimeError):
            bash_executable(windows=True, git=str(self.root / "Windows/System32/git.exe"))
        attributes = (ROOT / ".gitattributes").read_text(encoding="utf-8")
        for name in ("prepare_self_hosted.sh", "self_hosted_job_hook.sh"):
            self.assertIn(f"tools/ci/{name} text eol=lf\n", attributes)

    def test_subdirectory_working_directory_refuses_before_cleaning(self):
        self.env["CHANGE_DIRECTORY"] = "true"
        result = self.prepare()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Expected the checkout root", result.stderr)
        self.assertTrue((self.checkout / "build/stale").is_file())
        self.assertFalse(self.package_log.exists())
        self.assertFalse(self.env_file.exists())


class HostHookTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="host-hook-")
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "event.json"
        self.repo = {"full_name": "d-addison/luminumbra"}
        self.environment = {"GITHUB_REPOSITORY": self.repo["full_name"], "GITHUB_EVENT_NAME": "pull_request"}
        self.payload = {"repository": self.repo, "number": 7,
                        "pull_request": {"number": 7, "head": {"repo": self.repo, "sha": "a" * 40},
                                         "base": {"repo": self.repo}}}

    def validate(self, payload=None):
        self.path.write_text(json.dumps(self.payload if payload is None else payload), encoding="utf-8")
        HOOK.validate_event(self.environment, HOOK.read_json(self.path))

    def test_same_repository_pr_passes_actual_payload(self):
        self.validate()

    def test_fork_denied_even_with_changed_workflow_and_forged_input(self):
        self.payload["pull_request"]["head"]["repo"] = {"full_name": "external/luminumbra"}
        self.payload["inputs"] = {"repository": self.repo["full_name"], "trusted": True}
        self.payload["workflow"] = {"runs-on": ["self-hosted", "Linux", "X64", "lum-vq1"]}
        with self.assertRaisesRegex(HOOK.Denied, "external or missing PR head"):
            self.validate()

    def test_repository_identity_is_exact_in_environment_and_payload(self):
        for value in ("", "external/luminumbra", "D-Addison/luminumbra", None):
            with self.subTest(value=value):
                original = self.environment["GITHUB_REPOSITORY"]
                self.environment["GITHUB_REPOSITORY"] = value
                with self.assertRaises(HOOK.Denied):
                    self.validate()
                self.environment["GITHUB_REPOSITORY"] = original
                with self.assertRaises(HOOK.Denied):
                    self.validate(dict(self.payload, repository={"full_name": value}))

    def test_pr_missing_partial_and_wrong_identity_denies(self):
        changes = [None, {}, {"head": {}}, dict(self.payload["pull_request"], number=8),
                   dict(self.payload["pull_request"], head={"repo": self.repo}),
                   dict(self.payload["pull_request"], base={"repo": {"full_name": "external/repo"}})]
        for pr in changes:
            with self.subTest(pr=pr), self.assertRaises(HOOK.Denied):
                self.validate(dict(self.payload, pull_request=pr))
        self.payload["number"] = 1
        self.payload["pull_request"]["number"] = True
        with self.assertRaises(HOOK.Denied):
            self.validate()

    def non_pr(self, event):
        self.environment["GITHUB_EVENT_NAME"] = event
        if event == "push":
            return {"repository": self.repo, "ref": "refs/heads/devel", "after": "b" * 40}
        if event == "schedule":
            return {"repository": self.repo, "schedule": "43 4 * * 0"}
        return {"repository": self.repo, "action": "completed", "workflow_run": {
            "repository": self.repo, "head_repository": self.repo, "id": 12,
            "head_sha": "c" * 40, "head_branch": "devel", "event": "push"}}

    def test_supported_non_pr_events_require_complete_payloads(self):
        for event in ("push", "schedule", "workflow_run"):
            payload = self.non_pr(event)
            with self.subTest(event=event):
                self.validate(payload)
            for key in payload:
                partial = dict(payload)
                del partial[key]
                with self.subTest(event=event, missing=key), self.assertRaises(HOOK.Denied):
                    self.validate(partial)
            with self.assertRaises(HOOK.Denied):
                self.validate(dict(payload, repository={"full_name": "fork/repo"}))
        for key, value in (("ref", "refs/heads/"), ("ref", "devel"), ("ref", 7),
                           ("after", "a"), ("after", None)):
            payload = self.non_pr("push")
            payload[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(HOOK.Denied):
                self.validate(payload)
        for schedule in ("", " ", None, 7):
            with self.subTest(schedule=schedule), self.assertRaises(HOOK.Denied):
                self.validate(dict(self.non_pr("schedule"), schedule=schedule))
        run = self.non_pr("workflow_run")
        for key in run["workflow_run"]:
            changed = json.loads(json.dumps(run))
            del changed["workflow_run"][key]
            with self.subTest(missing=key), self.assertRaises(HOOK.Denied):
                self.validate(changed)
        for key, value in (("head_repository", {"full_name": "fork/repo"}), ("id", True),
                           ("head_sha", "0"), ("event", "unknown"), ("head_branch", "")):
            changed = json.loads(json.dumps(run))
            changed["workflow_run"][key] = value
            with self.subTest(key=key), self.assertRaises(HOOK.Denied):
                self.validate(changed)

    def test_unknown_and_missing_event_denies(self):
        for event in ("", None, "workflow_dispatch", "pull_request_target", "repository_dispatch", "unknown"):
            self.environment["GITHUB_EVENT_NAME"] = event
            with self.subTest(event=event), self.assertRaises(HOOK.Denied):
                self.validate()

    def test_missing_unreadable_malformed_partial_and_oversized_payload_denies(self):
        with self.assertRaises(OSError):
            HOOK.read_json(self.path)
        for data in (b"", b"{", b"[]", b"null", b'{"repository":{},"repository":{}}',
                     b'{"number":NaN}', b'{"number":Infinity}', b"\xff", b" " * (HOOK.MAX_JSON + 1)):
            self.path.write_bytes(data)
            with self.subTest(size=len(data)), self.assertRaises((ValueError, OSError)):
                HOOK.read_json(self.path)
        self.path.write_text("{}", encoding="utf-8")
        with patch.object(HOOK.os, "open", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                HOOK.read_json(self.path)
        with patch.object(HOOK.os, "stat", return_value=SimpleNamespace(st_mode=stat.S_IFLNK)):
            with self.assertRaisesRegex(HOOK.Denied, "symlink"):
                HOOK.read_json(self.path)
        with patch.object(HOOK.os, "fstat", return_value=SimpleNamespace(st_mode=stat.S_IFIFO, st_size=0)):
            with self.assertRaisesRegex(HOOK.Denied, "invalid JSON file"):
                HOOK.read_json(self.path)

    def test_host_paths_and_account_must_match_fixed_configuration(self):
        config = {"runner_uid": 1234, "work_root": "/srv/luminumbra/runner-1/_work", "metadata_verified": True}
        environment = {"GITHUB_WORKSPACE": config["work_root"] + "/luminumbra/luminumbra",
                       "RUNNER_TEMP": config["work_root"] + "/_temp",
                       "GITHUB_EVENT_PATH": config["work_root"] + "/_temp/_github_workflow/event.json"}
        # PurePosixPath keeps Linux host path tests meaningful on native Windows.
        from pathlib import PurePosixPath
        with patch.object(HOOK.os, "getuid", return_value=1234, create=True), patch.object(HOOK, "Path", PurePosixPath):
            self.assertEqual(str(HOOK.validate_paths(config, environment)[1]), environment["GITHUB_WORKSPACE"])
            for key in environment:
                for value in ("", "/", "/usr/bin", environment[key] + "/../other", "C:/runner/_work"):
                    with self.subTest(key=key, value=value), self.assertRaises(HOOK.Denied):
                        HOOK.validate_paths(config, dict(environment, **{key: value}))
            for root in ("", "/", "/usr/bin/_work", "/var/_work", "/srv/lum/../other/_work", "/srv/lum/_work\n"):
                with self.subTest(root=root), self.assertRaises(HOOK.Denied):
                    HOOK.validate_paths(dict(config, work_root=root), environment)
            for uid in (0, True, 999, "1234"):
                with self.subTest(uid=uid), self.assertRaises(HOOK.Denied):
                    HOOK.validate_paths(dict(config, runner_uid=uid), environment)
            for verified in (False, None, 1, "true"):
                with self.subTest(verified=verified), self.assertRaises(HOOK.Denied):
                    HOOK.validate_paths(dict(config, metadata_verified=verified), environment)
            for key in config:
                missing = dict(config)
                del missing[key]
                with self.subTest(key=key), self.assertRaises(HOOK.Denied):
                    HOOK.validate_paths(missing, environment)

    def test_installation_rejects_job_writable_group_writable_and_symlink_paths(self):
        with patch.object(HOOK.os, "lstat", return_value=SimpleNamespace(st_uid=0, st_mode=stat.S_IFREG | 0o644)):
            HOOK.protected_install(self.path)
        for uid, mode in ((1234, stat.S_IFREG | 0o644), (0, stat.S_IFREG | 0o664),
                          (0, stat.S_IFREG | 0o646), (0, stat.S_IFLNK | 0o777)):
            with self.subTest(uid=uid, mode=mode), patch.object(HOOK.os, "lstat", return_value=SimpleNamespace(st_uid=uid, st_mode=mode)):
                with self.assertRaises(HOOK.Denied):
                    HOOK.protected_install(self.path)

    def test_directory_walk_retains_validated_handle_and_closes_failures(self):
        from pathlib import PurePosixPath
        # Linux directory-handle semantics are modelled in this interpreter on
        # every platform. The policy itself refuses non-Linux production hosts.
        with patch.object(HOOK.os, "O_DIRECTORY", 65536, create=True), \
                patch.object(HOOK.os, "O_NOFOLLOW", 131072, create=True):
            for uid, succeeds in ((1234, True), (999, False)):
                with self.subTest(uid=uid), patch.object(HOOK.os, "open", side_effect=[10, 11, 12]) as opened, \
                        patch.object(HOOK.os, "close") as close, \
                        patch.object(HOOK.os, "fstat", return_value=SimpleNamespace(st_uid=uid)):
                    if succeeds:
                        self.assertEqual(HOOK.open_directory(PurePosixPath("/srv/_work"), 1234), 12)
                        self.assertEqual(close.call_args_list, [unittest.mock.call(10), unittest.mock.call(11)])
                    else:
                        with self.assertRaises(HOOK.Denied):
                            HOOK.open_directory(PurePosixPath("/srv/_work"), 1234)
                        self.assertEqual(close.call_args_list[-1], unittest.mock.call(12))
                    self.assertEqual(opened.call_args_list[-1],
                                     unittest.mock.call("_work", os.O_RDONLY | 65536 | 131072, dir_fd=11))
            with patch.object(HOOK.os, "open", side_effect=[10, PermissionError("directory denied")]), \
                    patch.object(HOOK.os, "close") as close, self.assertRaises(PermissionError):
                HOOK.open_directory(PurePosixPath("/srv/_work"), 1234)
            close.assert_called_once_with(10)

    def test_admission_denial_precedes_cleanup_and_missing_payload_never_falls_back(self):
        # These mocks operate in this interpreter, where admit actually runs.
        # No subprocess is expected to inherit a Python mock.
        installed = Path(self.temp.name) / "self_hosted_job_hook.py"
        with patch.object(HOOK.sys, "platform", "linux"), patch.object(HOOK, "protected_install"), \
                patch.object(HOOK, "validate_paths", return_value=(Path(self.temp.name), self.path, 1234)), \
                patch.object(HOOK, "open_directory", return_value=15), patch.object(HOOK.os, "close"), \
                patch.object(HOOK, "clean_contents") as clean:
            for error in (PermissionError("denied"), FileNotFoundError("missing"), ValueError("partial")):
                with patch.object(HOOK, "read_json", side_effect=[{}, error]), self.assertRaises((OSError, ValueError)):
                    HOOK.admit(self.environment, installed)
                clean.assert_not_called()
            fork = json.loads(json.dumps(self.payload))
            fork["pull_request"]["head"]["repo"] = {"full_name": "fork/repo"}
            with patch.object(HOOK, "read_json", side_effect=[{}, fork]), self.assertRaises(HOOK.Denied):
                HOOK.admit(self.environment, installed)
            clean.assert_not_called()
            with patch.object(HOOK, "read_json", side_effect=[{}, self.payload]), \
                    patch.object(HOOK.os, "fstat", return_value=SimpleNamespace(st_dev=1)):
                HOOK.admit(self.environment, installed)
            clean.assert_called_once_with(15, 1234, 1, [HOOK.MAX_ENTRIES])

    def test_cleanup_unlinks_only_validated_entries_and_has_global_budget(self):
        info = SimpleNamespace(st_mode=stat.S_IFLNK, st_uid=1234, st_dev=1, st_ino=2)
        scan = unittest.mock.MagicMock()
        scan.__enter__.return_value = [SimpleNamespace(name="link")]
        with patch.object(HOOK.os, "scandir", return_value=scan), \
                patch.object(HOOK.os, "stat", return_value=info), patch.object(HOOK.os, "unlink") as unlink:
            HOOK.clean_contents(15, 1234, 1, [1])
            unlink.assert_called_once_with("link", dir_fd=15)
            unlink.reset_mock()
            for budget, uid, device, depth in (([0], 1234, 1, 0), ([1], 999, 1, 0),
                                                ([1], 1234, 2, 0), ([1], 1234, 1, HOOK.MAX_DEPTH + 1)):
                with self.subTest(budget=budget, uid=uid), self.assertRaises(HOOK.Denied):
                    HOOK.clean_contents(15, uid, device, budget, depth)
                unlink.assert_not_called()

    def test_recursive_cleanup_rechecks_identity_and_closes_directory_on_failure(self):
        directory = SimpleNamespace(st_mode=stat.S_IFDIR, st_uid=1234, st_dev=1, st_ino=2)
        file = SimpleNamespace(st_mode=stat.S_IFREG, st_uid=1234, st_dev=1, st_ino=3)
        replaced = SimpleNamespace(st_mode=stat.S_IFDIR, st_uid=1234, st_dev=1, st_ino=4)

        def scan(name):
            result = unittest.mock.MagicMock()
            result.__enter__.return_value = [SimpleNamespace(name=name)]
            return result

        for opened, current, budget, succeeds in ((directory, directory, [2], True),
                                                  (replaced, directory, [2], False),
                                                  (directory, replaced, [2], False),
                                                  (directory, directory, [1], False)):
            stats = [directory, file, file, current]
            with self.subTest(opened=opened, current=current, budget=budget), \
                    patch.object(HOOK.os, "scandir", side_effect=[scan("nested"), scan("item")]), \
                    patch.object(HOOK.os, "O_DIRECTORY", 65536, create=True), \
                    patch.object(HOOK.os, "O_NOFOLLOW", 131072, create=True), \
                    patch.object(HOOK.os, "stat", side_effect=stats), patch.object(HOOK.os, "open", return_value=16), \
                    patch.object(HOOK.os, "fstat", return_value=opened), patch.object(HOOK.os, "close") as close, \
                    patch.object(HOOK.os, "unlink") as unlink, patch.object(HOOK.os, "rmdir") as rmdir:
                if succeeds:
                    HOOK.clean_contents(15, 1234, 1, budget)
                    unlink.assert_called_once_with("item", dir_fd=16)
                    rmdir.assert_called_once_with("nested", dir_fd=15)
                else:
                    with self.assertRaises(HOOK.Denied):
                        HOOK.clean_contents(15, 1234, 1, budget)
                    rmdir.assert_not_called()
                close.assert_called_once_with(16)

    def test_checkout_copy_cannot_be_used_as_installed_hook(self):
        result = subprocess.run([sys.executable, "-I", "-B", str(ROOT / "tools/ci/self_hosted_job_hook.py")],
                                env=dict(os.environ, **self.environment), capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Self-hosted job denied:", result.stderr)

    def test_shell_hook_uses_only_fixed_host_policy_and_no_checkout_commands(self):
        script = (ROOT / "tools/ci/self_hosted_job_hook.sh").read_text(encoding="utf-8")
        self.assertIn('exec /usr/bin/python3 -I -B "${0%/*}/self_hosted_job_hook.py"', script)
        self.assertNotIn("GITHUB_WORKSPACE", script)
        self.assertNotIn("source ", script)


if __name__ == "__main__":
    unittest.main()

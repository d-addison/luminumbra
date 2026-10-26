#!/usr/bin/env python3
"""Exercise the documented owned-source boundary without CMake or compilers."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

MODULE_PATH = Path(__file__).with_name("rmlui_source.py")
LOADER = importlib.util.spec_from_file_location("source_contract", Path(__file__).with_name("test_rmlui_source.py"))
source = importlib.util.module_from_spec(LOADER)
LOADER.loader.exec_module(source)
M = source.M
ROOT = source.ROOT
file_identity = source.file_identity
remove_source_fixture = source.remove_source_fixture


class LocalSourceRecipe(unittest.TestCase):
    # Reuse the full immutable input fixture, without inheriting its test cases.
    setUpClass = classmethod(source.SourceFiles.setUpClass.__func__)
    tearDownClass = classmethod(source.SourceFiles.tearDownClass.__func__)

    def test_documented_local_recipe_requires_both_owned_disconnected_sources(self):
        self.exercise_recipe(phase="enrollment")

    def test_worker_configures_both_owned_disconnected_sources(self):
        self.exercise_recipe(phase="configure")

    def test_worker_refuses_invalid_gcc_receipts(self):
        self.exercise_recipe(phase="gcc_receipts")

    def test_worker_refuses_invalid_tidy_receipts(self):
        self.exercise_recipe(phase="tidy_receipts")

    def exercise_recipe(self, failure=None, phase="configure"):
        docs = (ROOT / "docs/ci/local-checks.md").read_text(encoding="utf-8")
        owner_section, worker_section = docs.split("## Worker recipe\n", 1)
        owner = owner_section.split("```bash\n")[-1].split("\n```", 1)[0]
        worker = worker_section.split("```bash\n", 1)[1].split("\n```", 1)[0]
        self.assertIn("rmlui_source.py init", owner)
        self.assertIn("rmlui_source.py download", owner)
        self.assertIn("rmlui_source.py verify", owner)
        self.assertNotIn("rmlui_source.py download", worker)
        self.assertIn('"tools/ci/test_local_rmlui_recipe.py"',
                      (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8"))
        registration = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('NAME LocalRmlUiRecipeContract', registration)
        self.assertIn('"${CMAKE_CURRENT_SOURCE_DIR}/ci/test_local_rmlui_recipe.py"', registration)
        self.assertIn('tools/ci/ci_paths.py python-tests',
                      (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8"))
        # Run the actual source boundary and configure function. Generic staging
        # has its own real-Git regression; no CMake or compiler is launched here.
        prefix = worker.split("nice -n 10 python3 -I -B tools/ci/offline_deps.py stage", 1)[0]
        configure = worker.split("configure_local() {", 1)[1].split(
            "\nnice -n 10 cmake --build", 1)[0]
        worker_boundary = prefix + "configure_local() {" + configure
        sys.path.insert(0, str(MODULE_PATH.parent))
        from ci_shell import bash_executable
        directory = Path(tempfile.mkdtemp(prefix="local-source-recipe-")).resolve(strict=True)
        identity = file_identity(directory)
        first = M.Workspace(directory / "scratch/local-gcc", self.spec)
        second = M.Workspace(directory / "scratch/local-tidy", self.spec)
        borrowed = source.BorrowedSource(self.workspace.stage, self.spec)
        self.recipe_recovery = (borrowed, directory, identity, first, second)
        try:
            for name in ("tools/ci/rmlui_source.py", "tools/ci/offline_deps.py",
                         "tools/ci/path_safety.py", "vendor/rmlui/rmlui-6.1-source.json",
                         "cmake/patches/rmlui-6.1-shadow.patch"):
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes((ROOT / name).read_bytes())
            (directory / "boundary.py").write_text(textwrap.dedent('''\
                import importlib.util
                import json
                import os
                from pathlib import Path
                import shutil
                import subprocess
                import sys

                loader = importlib.util.spec_from_file_location("source", "tools/ci/rmlui_source.py")
                module = importlib.util.module_from_spec(loader)
                loader.loader.exec_module(module)
                spec = module.Specification(Path("vendor/rmlui/rmlui-6.1-source.json"),
                                            Path("cmake/patches/rmlui-6.1-shadow.patch"))
                args = sys.argv[1:]
                with Path("calls.jsonl").open("a", encoding="utf-8") as stream:
                    stream.write(json.dumps(args) + "\\n")
                if args[0] == "download":
                    workspace = module.Workspace(Path(args[args.index("--root") + 1]), spec)
                    workspace.preflight(False)
                    module.write_exclusive(workspace.admission, b"fixture delivery\\n")
                    workspace.stage.parent.mkdir(exist_ok=True)
                    pristine = Path(os.environ["PRISTINE_STAGE"])
                    if pristine.exists():
                        os.rename(pristine, workspace.stage)
                    else:
                        first = module.Workspace(Path("scratch/local-gcc"), spec)
                        shutil.copytree(first.source, workspace.stage)
                        module.git(workspace.stage, "apply", "--reverse", "--unidiff-zero",
                                   "-p1", "--", str(spec.patch.resolve()))
                    # Only transport is replaced: verify the entire immutable
                    # input and run the unchanged publication/receipt machinery.
                    failure = os.environ.get("PUBLICATION_FAILURE")
                    real_git, real_rename, real_write = module.git, module.os.rename, module.write_exclusive
                    def command(path, *arguments, **kwargs):
                        if failure == "beforepatch" and arguments[0] == "apply":
                            raise RuntimeError("injected beforepatch")
                        result = real_git(path, *arguments, **kwargs)
                        if (failure == "afterpatch" and arguments[0] == "apply"
                                and "--check" not in arguments):
                            raise RuntimeError("injected afterpatch")
                        return result
                    def rename(before, after):
                        if failure == "beforerename":
                            raise RuntimeError("injected beforerename")
                        real_rename(before, after)
                        if failure == "afterrename":
                            raise RuntimeError("injected afterrename")
                    def write(path, data):
                        if failure == "receipt":
                            real_write(path, b"{partial")
                            raise RuntimeError("injected receipt")
                        real_write(path, data)
                    module.git, module.os.rename, module.write_exclusive = command, rename, write
                    workspace.publish(workspace.owner())
                else:
                    if os.environ.get("PUBLICATION_FAILURE") == "configure":
                        raise RuntimeError("injected configure")
                    build = args[args.index("-B") + 1]
                    base = next(value.split("=", 1)[1] for value in args
                                if value.startswith("-DFETCHCONTENT_BASE_DIR="))
                    assert module.canonical(base) == module.canonical(Path(build) / "_deps")
                    assert "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" in args
                    assert not any("FETCHCONTENT_SOURCE_DIR_RMLUI" in value for value in args)
                    sys.exit(subprocess.run([sys.executable, "-I", "-B", "tools/ci/rmlui_source.py",
                                             "preflight", "--root", build,
                                             "--fully-disconnected"], check=False).returncode)
                '''), encoding="utf-8")
            harness = '''\
nice() { shift 2; "$@"; }
python3() {
  if [ "$3" = tools/ci/rmlui_source.py ] && [ "$4" = download ]; then
    "$FIXTURE_PYTHON" -I -B boundary.py "${@:4}"
  else
    "$FIXTURE_PYTHON" "$@"
  fi
}
cmake() { "$FIXTURE_PYTHON" -I -B boundary.py configure "$@"; }
'''
            environment = dict(os.environ, FIXTURE_PYTHON=Path(sys.executable).as_posix(),
                               PRISTINE_STAGE=self.workspace.stage.as_posix(),
                               LUMINUMBRA_DEPS_CACHE=directory.as_posix(),
                               PUBLICATION_FAILURE=failure or "")
            def run(fragment):
                return subprocess.run([bash_executable(), "-e", "-o", "pipefail", "-c", harness + fragment],
                                      cwd=directory, env=environment, capture_output=True, text=True, check=False)
            if failure:
                # Failure cases enter the real publication boundary directly;
                # the normal recipe case below retains every refusal assertion.
                first.initialize()
                result = subprocess.run(
                    [sys.executable, "-I", "-B", "boundary.py", "download", "--root", str(first.root)],
                    cwd=directory, env=environment, capture_output=True, text=True, check=False)
                if failure == "configure":
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    second.initialize()
                    result = subprocess.run(
                        [sys.executable, "-I", "-B", "boundary.py", "download", "--root", str(second.root)],
                        cwd=directory, env=environment, capture_output=True, text=True, check=False)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    result = run(worker_boundary)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("injected " + failure, result.stderr)
                raise RuntimeError("injected publication " + failure)
            if phase == "enrollment":
                # Preparation must finish before enrollment, including its fresh
                # stage/init checks. An occupied init path cannot leave an owner.
                initial = directory / "scratch/offline-init.cmake"
                initial.parent.mkdir()
                initial.write_bytes(b"retained initial cache")
                self.assertNotEqual(run(owner).returncode, 0)
                self.assertFalse(first.root.exists())
                self.assertFalse(second.root.exists())
                self.assertEqual(initial.read_bytes(), b"retained initial cache")
                initial.unlink()
                # Unowned empty/nonempty roots are never enrolled by the recipe.
                first.root.mkdir()
                for occupied in (False, True):
                    if occupied:
                        (first.root / "retained").write_bytes(b"unknown state")
                    result = run(owner)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertFalse(first.owner_path.exists())
                    self.assertFalse(second.root.exists())
                    if occupied:
                        self.assertEqual((first.root / "retained").read_bytes(), b"unknown state")
                (first.root / "retained").unlink()
                first.root.rmdir()
                self.assertNotEqual(run(worker_boundary).returncode, 0)
                return
            result = run(owner)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if phase.endswith("_receipts"):
                workspace = first if phase == "gcc_receipts" else second
                workspace.verify()
                receipt = workspace.receipt.read_bytes()
                for invalid in (None, b"partial", M.encoded({})):
                    workspace.receipt.unlink()
                    if invalid is not None:
                        workspace.receipt.write_bytes(invalid)
                    result = run(worker_boundary)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    workspace.receipt.write_bytes(receipt)
                return
            result = run(worker_boundary)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            calls = [json.loads(line) for line in (directory / "calls.jsonl").read_text(encoding="utf-8").splitlines()]
            self.assertEqual([call[0] for call in calls], ["download", "download", "configure", "configure"])
            self.assertEqual([call[call.index("-B") + 1] for call in calls[2:]],
                             ["scratch/local-gcc", "scratch/local-tidy"])
            helper = directory / "tools/ci/rmlui_source.py"
            helper.write_bytes(helper.read_bytes() + b"\n# changed helper identity\n")
            self.assertNotEqual(run(worker_boundary).returncode, 0)
        finally:
            borrowed.restore(first.stage, first.source)
            remove_source_fixture(directory, identity, (first.source, first.stage, second.source, second.stage))

class LocalRecipeRecovery(unittest.TestCase):
    setUpClass = classmethod(source.SourceFiles.setUpClass.__func__)
    tearDownClass = classmethod(source.SourceFiles.tearDownClass.__func__)
    exercise_recipe = LocalSourceRecipe.exercise_recipe

    def assert_failed_publication_returns_input(self, failure):
        identity = file_identity(self.workspace.stage)
        seal = source.BorrowedSource.snapshot(self.workspace.stage)
        with self.assertRaisesRegex(RuntimeError, "injected publication " + failure):
            self.exercise_recipe(failure)
        self.assertEqual(file_identity(self.workspace.stage), identity)
        self.assertEqual(source.BorrowedSource.snapshot(self.workspace.stage), seal)
        M.verify_git(self.workspace.stage, self.spec)
        self.spec.verify_files(self.workspace.stage, False)
        self.assertFalse(self.recipe_recovery[1].exists())

    def test_publication_beforepatch_returns_borrowed_input(self):
        self.assert_failed_publication_returns_input("beforepatch")

    def test_publication_afterpatch_returns_borrowed_input(self):
        self.assert_failed_publication_returns_input("afterpatch")

    def test_publication_beforerename_returns_borrowed_input(self):
        self.assert_failed_publication_returns_input("beforerename")

    def test_publication_afterrename_returns_borrowed_input(self):
        self.assert_failed_publication_returns_input("afterrename")

    def test_publication_receipt_returns_borrowed_input(self):
        self.assert_failed_publication_returns_input("receipt")

    def test_publication_configure_returns_borrowed_input(self):
        self.assert_failed_publication_returns_input("configure")

    def test_failed_restoration_preserves_recoverable_input_and_both_errors(self):
        real_git = M.git
        def command(path, *args, **kwargs):
            if "--reverse" in args:
                raise OSError("injected restoration refusal")
            return real_git(path, *args, **kwargs)
        try:
            with mock.patch.object(M, "git", side_effect=command):
                with self.assertRaisesRegex(AssertionError,
                                            "original failure:.*afterpatch.*restoration failure:.*refusal"):
                    self.exercise_recipe("afterpatch")
            borrowed, directory, identity, first, second = self.recipe_recovery
            self.assertTrue(directory.exists())
            self.assertEqual(file_identity(first.stage), borrowed.identity)
            self.assertFalse(self.workspace.stage.exists())
            self.spec.verify_files(first.stage, True)
        finally:
            borrowed, directory, identity, first, second = self.recipe_recovery
            borrowed.restore(first.stage, first.source)
            remove_source_fixture(directory, identity, (first.stage, first.source, second.stage, second.source))


if __name__ == "__main__":
    unittest.main()

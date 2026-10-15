"""Static pin diagnostics and guarded launches of stdlib-only game fixtures."""
from copy import deepcopy
from pathlib import Path, PureWindowsPath
import errno
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
from luminumbra_author.contracts import Toolchain, TOOLCHAIN, Refusal, canonical, digest
from luminumbra_author.__main__ import dispatch
from luminumbra_author.meshgen_scripts import ScriptInventory


class ScriptPins(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.game = self.root/"game"; self.game.mkdir()
        self.installed = self.root/"installed"; self.installed.mkdir()
        self.processor = self.installed/"processor"; self.processor.write_bytes(b"never executed")
        self.manifest = self.installed/"toolchain.json"
        self.document = {"schema": TOOLCHAIN, "id": "test.compiler", "processor": "processor",
                         "files": {"processor": digest(self.processor.read_bytes())}}
        self.script = self.game/"scripts/finish.py"
        self.script.parent.mkdir()
        self.script.write_text("import bpy\nfrom helper import run\n# Never imported or executed.\n")
        self.helper = self.game/"scripts/helper.py"
        self.helper.write_text("import math\ndef run(): pass\n")

    def pin(self):
        self.document["scripts"] = {"finisher": {"path": "scripts/finish.py", "sha256": digest(self.script.read_bytes()),
            "modules": {"scripts/helper.py": digest(self.helper.read_bytes())}}}
        self.write()
        return Toolchain(self.manifest)

    def write(self):
        self.manifest.write_bytes(canonical(self.document))

    def assert_inventory_handle_closed(self, inventory):
        if os.name == "posix":
            with self.assertRaises(OSError) as caught:
                os.fstat(inventory.root_fd)
            self.assertEqual(caught.exception.errno, errno.EBADF)
        elif os.name == "nt":
            import ctypes
            from ctypes import wintypes
            get_info = ctypes.WinDLL("kernel32", use_last_error=True).GetHandleInformation
            get_info.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
            get_info.restype = wintypes.BOOL
            flags = wintypes.DWORD()
            self.assertFalse(get_info(inventory.root_handle, ctypes.byref(flags)))
            self.assertEqual(ctypes.get_last_error(), 6)  # ERROR_INVALID_HANDLE
        self.assertFalse(inventory._close_root.alive)

    def test_inventory_close_releases_handle_and_is_idempotent(self):
        self.pin()
        inventory = ScriptInventory(self.game, self.document["scripts"]["finisher"])
        self.addCleanup(inventory.close)
        self.assertEqual(inventory.source(self.helper), self.helper.read_bytes())
        inventory.close()
        self.assert_inventory_handle_closed(inventory)
        inventory.close()
        self.assert_inventory_handle_closed(inventory)
        with self.assertRaises(Refusal) as caught:
            inventory.source(self.helper)
        self.assertEqual(caught.exception.finding["rule_id"], "toolchain.script_path")

    def test_inventory_context_releases_handle_on_success_and_exception(self):
        self.pin()
        for raises in (False, True):
            with self.subTest(raises=raises):
                try:
                    with ScriptInventory(self.game, self.document["scripts"]["finisher"]) as inventory:
                        self.assertEqual(inventory.source(self.helper), self.helper.read_bytes())
                        if raises:
                            raise RuntimeError("leave the inventory context")
                except RuntimeError:
                    if not raises:
                        raise
                self.assert_inventory_handle_closed(inventory)
                inventory.close()
                self.assert_inventory_handle_closed(inventory)

    def test_replacement_skip_closes_inventory_before_temporary_directory(self):
        opened = []
        inventory_type = ScriptInventory
        def track_inventory(*args):
            inventory = inventory_type(*args)
            case.addCleanup(inventory.close)
            opened.append(inventory)
            return inventory
        case = ScriptPins("test_root_symlink_replacement_between_containment_and_open_refused")
        result = unittest.TestResult()
        with mock.patch(__name__ + ".ScriptInventory", side_effect=track_inventory), \
                mock.patch.object(Path, "symlink_to", side_effect=OSError(errno.EPERM, "no symlink privilege")):
            case.run(result)
        self.assertEqual(len(result.skipped), 1)
        self.assertEqual(result.errors, [])
        self.assertEqual(result.failures, [])
        self.assertEqual(len(opened), 1)
        self.assert_inventory_handle_closed(opened[0])
        self.assertFalse(case.root.exists())

    def test_inventory_initialization_refusal_releases_handle(self):
        self.pin()
        opened = []
        def refuse_entry(inventory, path):
            opened.append(inventory)
            raise Refusal("toolchain.script_path", "Entry refused after opening root.")
        with mock.patch.object(ScriptInventory, "canonical_path", autospec=True, side_effect=refuse_entry):
            with self.assertRaises(Refusal):
                with ScriptInventory(self.game, self.document["scripts"]["finisher"]):
                    self.fail("Refused inventory cannot enter its context")
        self.assertEqual(len(opened), 1)
        self.assert_inventory_handle_closed(opened[0])

    def test_verify_scripts_releases_inventory_on_success_and_refusal(self):
        toolchain = self.pin()
        from luminumbra_author import meshgen_scripts
        opened = []
        inventory_type = ScriptInventory
        def track_inventory(*args):
            inventory = inventory_type(*args)
            self.addCleanup(inventory.close)
            opened.append(inventory)
            return inventory
        with mock.patch.object(meshgen_scripts, "ScriptInventory", side_effect=track_inventory):
            toolchain.verify_scripts(self.game)
            self.assert_inventory_handle_closed(opened[-1])
            self.helper.write_text("changed companion\n")
            with self.assertRaises(Refusal) as caught:
                toolchain.verify_scripts(self.game)
            self.assertEqual(caught.exception.finding["rule_id"], "toolchain.changed")
            self.assert_inventory_handle_closed(opened[-1])
        self.assertEqual(len(opened), 2)

    def test_old_toolchain_valid_but_mg2_requires_role(self):
        self.write()
        toolchain = Toolchain(self.manifest)
        toolchain.verify()
        with self.assertRaises(Refusal) as caught: toolchain.verify_scripts(self.game)
        self.assertEqual(caught.exception.finding["rule_id"], "config.pins_missing")

    def test_explicit_project_root_and_rechecks(self):
        toolchain = self.pin()
        result = toolchain.verify_scripts(self.game)
        self.assertEqual(result["path"].resolve(strict=True), self.script.resolve(strict=True))
        self.assertEqual(set(result["modules"]), {"scripts/helper.py"})
        with self.assertRaises(Refusal): toolchain.verify_scripts(self.installed)
        self.helper.write_text("changed companion module")
        with self.assertRaises(Refusal) as caught: toolchain.verify_scripts(self.game)
        self.assertEqual(caught.exception.finding["rule_id"], "toolchain.changed")
        toolchain = self.pin()
        self.script.write_text("changed script")
        with self.assertRaises(Refusal): toolchain.verify_scripts(self.game)

    def test_transitive_unpinned_import_is_refused(self):
        (self.script.parent/"other.py").write_text("value = 1\n")
        self.helper.write_text("import other\n")
        toolchain = self.pin()
        with self.assertRaises(Refusal) as caught: toolchain.verify_scripts(self.game)
        self.assertEqual(caught.exception.finding["rule_id"], "config.pins_missing")
        self.document["scripts"]["finisher"]["modules"]["scripts/other.py"] = digest((self.script.parent/"other.py").read_bytes())
        self.write()
        Toolchain(self.manifest).verify_scripts(self.game)

    def test_package_initializer_and_relative_import_inventory(self):
        package = self.game/"companion"; package.mkdir()
        init = package/"__init__.py"; init.write_text("from . import helper\n")
        helper = package/"helper.py"; helper.write_text("import math\n")
        self.script.write_text("import companion.helper\n")
        toolchain = self.pin()
        with self.assertRaises(Refusal): toolchain.verify_scripts(self.game)
        record = self.document["scripts"]["finisher"]
        record["modules"].update({"companion/__init__.py": digest(init.read_bytes()),
                                  "companion/helper.py": digest(helper.read_bytes())})
        self.write()
        Toolchain(self.manifest).verify_scripts(self.game)

    def test_unknown_dependency_and_dynamic_imports_refused(self):
        for source in ("import unknown_ml_package\n", "__import__('helper')\n",
                       "import importlib\nimportlib.import_module('helper')\n",
                       "from importlib import import_module as load\nload('helper')\n",
                       "load = __import__\nload('helper')\n",
                       "import importlib.util\nspec = importlib.util.spec_from_file_location('other', 'other.py')\n",
                       "exec(open('other.py').read())\n"):
            self.script.write_text(source)
            toolchain = self.pin()
            with self.subTest(source=source), self.assertRaises(Refusal): toolchain.verify_scripts(self.game)

    def launch(self, cwd=None, environment=None, project_root=None):
        return subprocess.run([sys.executable, "-I", "-S", "-B", str(SERVICE / "launch.py"), "--scripts",
            "--project", str(self.game if project_root is None else project_root), "--toolchain", str(self.manifest)],
            env=environment or dict(os.environ, PYTHONPATH=str(self.game), PYTHONDONTWRITEBYTECODE="1"),
            cwd=cwd or self.root, capture_output=True, text=True, timeout=15)

    def check_root_alias(self, alias, source_path):
        self.helper.write_text("print('aliased pinned source')\n")
        self.script.write_text("import importlib.machinery\nimport importlib.util\n"
            f"loader = getattr(importlib.machinery, 'Source' + 'FileLoader')('other', {str(source_path)!r})\n"
            "spec = getattr(importlib.util, 'spec_from_' + 'loader')('other', loader)\n"
            "module = getattr(importlib.util, 'module_from_' + 'spec')(spec)\n"
            "getattr(loader, 'exec_' + 'module')(module)\nimport helper\n")
        toolchain = self.pin()
        self.assertEqual(toolchain.verify_scripts(alias)["path"].resolve(strict=True), self.script.resolve(strict=True))
        result = self.launch(project_root=alias)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(result.stdout, "aliased pinned source\n" * 2)
        inventory = ScriptInventory(alias, self.document["scripts"]["finisher"])
        self.addCleanup(inventory.close)
        self.assertEqual(inventory.source(source_path), self.helper.read_bytes())
        unpinned = source_path.with_name("unpinned.py")
        unpinned.write_bytes(self.helper.read_bytes())
        with self.assertRaises(Refusal) as caught: inventory.source(unpinned)
        self.assertEqual(caught.exception.finding["rule_id"], "config.pins_missing")
        self.helper.write_text("changed after verification\n")
        with self.assertRaises(Refusal) as caught: inventory.source(source_path)
        self.assertEqual(caught.exception.finding["rule_id"], "toolchain.changed")

    def test_relative_project_root_and_companion_alias(self):
        # Required on every platform; a relative spelling must resolve just as
        # the explicitly supplied project root does, including in direct loads.
        previous = Path.cwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous)
        self.check_root_alias(Path("game"), Path("game/scripts/helper.py"))

    if os.name != "nt":
        def test_symlinked_project_root_alias(self):
            alias = self.root / "project-alias"
            alias.symlink_to(self.game, target_is_directory=True)
            self.check_root_alias(alias, alias / "scripts/helper.py")

    if os.name == "nt":
        def test_windows_companion_junctions_are_refused(self):
            self.pin()
            inventory = ScriptInventory(self.game, self.document["scripts"]["finisher"])
            self.addCleanup(inventory.close)
            outside = self.root / "outside"
            outside.mkdir()
            (outside / "helper.py").write_bytes(self.helper.read_bytes())
            for name, target, member in (("outside-alias", outside, "helper.py"),
                                         ("root-alias", self.game, "scripts/helper.py")):
                link = self.game / name
                # Directory junctions do not require Windows symlink privilege.
                subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(link), str(target)],
                               check=True, capture_output=True, text=True, timeout=15)
                try:
                    with self.subTest(target=target), self.assertRaises(Refusal) as caught:
                        inventory.source(link / member)
                    self.assertEqual(caught.exception.finding["rule_id"], "path.symlink")
                finally:
                    link.rmdir()

        def test_windows_short_project_root_alias(self):
            import ctypes
            from ctypes import wintypes
            renamed = self.root / "project with a long directory name"
            self.game.rename(renamed)
            self.game = renamed
            self.script = renamed / "scripts/finish.py"
            self.helper = renamed / "scripts/helper.py"
            get_short = ctypes.WinDLL("kernel32", use_last_error=True).GetShortPathNameW
            get_short.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
            get_short.restype = wintypes.DWORD
            long_path = str(renamed.resolve(strict=True))
            size = get_short(long_path, None, 0)
            if not size: raise ctypes.WinError(ctypes.get_last_error())
            buffer = ctypes.create_unicode_buffer(size)
            written = get_short(long_path, buffer, size)
            if not written or written >= size: raise ctypes.WinError(ctypes.get_last_error())
            alias = Path(buffer.value)
            if alias.name == renamed.name:
                self.skipTest("8.3 short names are disabled for new directories on this volume")
            self.assertEqual(alias.resolve(strict=True), renamed.resolve(strict=True))
            self.check_root_alias(alias, alias / "scripts/helper.py")

    def test_companion_aliases_do_not_allow_traversal_or_escape(self):
        self.pin()
        inventory = ScriptInventory(self.game, self.document["scripts"]["finisher"])
        self.addCleanup(inventory.close)
        outside = self.root / "outside.py"
        outside.write_bytes(self.helper.read_bytes())
        for path in (outside, self.game / "scripts/../scripts/helper.py", self.game / "../outside.py"):
            with self.subTest(path=path), self.assertRaises(Refusal) as caught: inventory.source(path)
            self.assertEqual(caught.exception.finding["rule_id"], "toolchain.script_path")
        if os.name == "nt":
            drive = "Z:" if self.game.drive.upper() != "Z:" else "Y:"
            with self.assertRaises((Refusal, OSError)): inventory.source(Path(drive + "/outside.py"))
        else:
            # Both an internal alias and an escaping link must be rejected,
            # even when their target has exactly the expected pinned bytes.
            for target in (self.helper.resolve(strict=True), outside):
                link = self.script.parent / "alias.py"
                link.symlink_to(target)
                with self.subTest(target=target), self.assertRaises(Refusal) as caught: inventory.source(link)
                self.assertEqual(caught.exception.finding["rule_id"], "path.symlink")
                link.unlink()
            alias = self.game / "root-alias"
            alias.symlink_to(self.game.resolve(strict=True), target_is_directory=True)
            with self.assertRaises(Refusal) as caught: inventory.source(alias / "scripts/helper.py")
            self.assertEqual(caught.exception.finding["rule_id"], "path.symlink")

    def check_replacement_after_containment(self, replaced):
        self.pin()
        inventory = ScriptInventory(self.game, self.document["scripts"]["finisher"])
        self.addCleanup(inventory.close)
        outside = self.root / "outside"
        shutil.copytree(replaced, outside)
        # Outside source has identical pins: refusal must establish identity,
        # rather than rely on changed bytes or a probabilistic race.
        probe = self.root / "symlink-probe"
        try:
            probe.symlink_to(outside, target_is_directory=True)
        except OSError as error:
            if (error.errno not in (errno.EPERM, errno.EACCES, errno.ENOSYS, errno.EOPNOTSUPP)
                    and getattr(error, "winerror", None) not in (1, 50, 1314)):
                raise
            self.skipTest(f"Directory symlink creation is unavailable: {error}")
        probe.unlink()
        original = ScriptInventory.canonical_path
        backup = replaced.with_name(replaced.name + "-original")
        blocked = []
        def replace_after_check(current, path):
            checked = original(current, path)
            try:
                replaced.rename(backup)
            except PermissionError as error:
                # Windows refuses to rename a directory while a handle beneath it
                # is open, so the swap itself cannot happen there.
                if getattr(error, "winerror", None) != 5:
                    raise
                blocked.append(error)
                return checked
            try:
                replaced.symlink_to(outside, target_is_directory=True)
            except BaseException:
                backup.rename(replaced)
                raise
            return checked
        try:
            with mock.patch.object(ScriptInventory, "canonical_path", autospec=True, side_effect=replace_after_check):
                try:
                    source = inventory.source(self.helper)
                except Refusal as refusal:
                    self.assertEqual(blocked, [])
                    self.assertEqual(refusal.finding["rule_id"], "path.symlink")
                else:
                    self.assertTrue(blocked, "the replaced tree was read without a refusal")
                    self.assertFalse(replaced.is_symlink())
                    self.assertEqual(source, self.helper.read_bytes())
        finally:
            if replaced.is_symlink():
                replaced.unlink()
                backup.rename(replaced)

    def test_root_symlink_replacement_between_containment_and_open_refused(self):
        self.check_replacement_after_containment(self.game)

    def test_companion_ancestor_symlink_replacement_between_containment_and_open_refused(self):
        self.check_replacement_after_containment(self.script.parent)

    def test_root_ancestor_symlink_replacement_between_containment_and_open_refused(self):
        ancestor = self.root / "project-parent"
        ancestor.mkdir()
        self.game.rename(ancestor / "game")
        self.game = ancestor / "game"
        self.script = self.game / "scripts/finish.py"
        self.helper = self.game / "scripts/helper.py"
        self.check_replacement_after_containment(ancestor)

    def test_all_companions_retain_same_root_identity(self):
        self.pin()
        inventory = ScriptInventory(self.game, self.document["scripts"]["finisher"])
        self.addCleanup(inventory.close)
        self.assertEqual(inventory.source(self.script), self.script.read_bytes())
        replacement = self.root / "replacement"
        shutil.copytree(self.game, replacement)
        backup = self.root / "original"
        self.game.rename(backup)
        replacement.rename(self.game)
        with self.assertRaises(Refusal) as caught:
            inventory.source(self.helper)
        self.assertEqual(caught.exception.finding["rule_id"], "toolchain.script_path")

    def test_isolated_launcher_never_executes_startup_project_sentinels(self):
        self.script.write_text("print('pinned entry')\n")
        self.pin()
        sentinel = self.game / "startup-executed"
        source = f"open({str(sentinel)!r}, 'w').write('untrusted startup')\n"
        for location in (self.root, self.game):
            for name in ("ast.py", "sitecustomize.py", "usercustomize.py"):
                (location / name).write_text(source)
            (location / "startup.pth").write_text("import builtins; " + source)
        # Also plant startup hooks in the user site of a controlled user base;
        # ordinary site-enabled startup would discover these before main().
        userbase = self.root / "userbase"
        environment = dict(os.environ, PYTHONPATH=os.pathsep.join((str(self.game), str(self.root))),
                           PYTHONUSERBASE=str(userbase))
        query = subprocess.run([sys.executable, "-I", "-S", "-B", "-c",
            "import site, sys; site.USER_BASE = sys.argv[1]; print(site.getusersitepackages())", str(userbase)],
            capture_output=True, text=True, timeout=15, check=True)
        usersite = Path(query.stdout.strip()); usersite.mkdir(parents=True)
        for name in ("sitecustomize.py", "usercustomize.py"):
            (usersite / name).write_text(source)
        (usersite / "startup.pth").write_text("import builtins; " + source)
        for cwd in (self.root, self.game):
            result = self.launch(cwd=cwd, environment=environment)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertEqual(result.stdout, "pinned entry\n")
            self.assertFalse(sentinel.exists())
        self.assertFalse(list(self.root.rglob("__pycache__")))

    def test_guarded_entry_refuses_missing_bootstrap_or_project_search_path(self):
        self.script.write_text("raise AssertionError('entry must never execute')\n")
        self.pin()
        # The intentional no--B case may write caches; keep them in this fixture.
        source_root = self.root / "service"
        source_package = source_root / "luminumbra_author"
        source_package.mkdir(parents=True)
        for path in (SERVICE / "luminumbra_author").glob("*.py"):
            shutil.copyfile(path, source_package / path.name)
        command = ("import sys; sys.path.insert(0, sys.argv.pop(1)); "
                   "from luminumbra_author.meshgen_scripts import main; raise SystemExit(main())")
        # Each required interpreter flag and the project-path check is independent.
        for flags, extra in ((["-B"], ""), (["-I", "-B"], ""), (["-S", "-B"], ""),
                             (["-I", "-S"], ""),
                             (["-I", "-S", "-B"], f"sys.path.append({str(self.game)!r}); ")):
            source = command.replace("from luminumbra", extra + "from luminumbra")
            result = subprocess.run([sys.executable, *flags, "-c", source, str(source_root),
                "--project", str(self.game), "--toolchain", str(self.manifest)],
                env={k: v for k, v in os.environ.items() if not k.startswith("PYTHON")},
                cwd=self.root, capture_output=True, text=True, timeout=15)
            with self.subTest(flags=flags, extra=extra):
                self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
                self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.bootstrap")

    def test_runtime_refuses_project_site_processing_before_pth_execution(self):
        sentinel = self.game / "pth-executed"
        for directory in (self.game, self.game / "nested"):
            directory.mkdir(exist_ok=True)
            (directory / "unsafe.pth").write_text(f"import builtins; open({str(sentinel)!r}, 'w').write('ran')\n")
            for call in (f"site.addsitedir({str(directory)!r})",
                         f"site.addpackage({str(directory)!r}, 'unsafe.pth', set())"):
                self.script.write_text("import site\n" + call + "\n")
                self.pin().verify_scripts(self.game)
                result = self.launch()
                with self.subTest(call=call):
                    self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
                    self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.script_inventory")
                    self.assertFalse(sentinel.exists())

    def test_runtime_refuses_source_laundered_through_stdlib_helpers(self):
        sentinel = self.game / "helper-executed"
        other = self.game / "unlisted.py"
        other.write_text(f"open({str(sentinel)!r}, 'w').write('ran')\n")
        # runpy supplies an external stdlib caller; timeit compiles with <timeit>.
        for source in (f"import runpy\nrunpy.run_path({str(other)!r})\n",
                       f"import timeit\ntimeit.timeit({other.read_text()!r}, number=1)\n"):
            self.script.write_text(source)
            self.pin().verify_scripts(self.game)
            result = self.launch()
            with self.subTest(source=source):
                self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
                self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.script_inventory")
                self.assertFalse(sentinel.exists())

    def test_entry_root_is_used_for_transitive_import_and_stdlib_shadows_refused(self):
        package = self.game / "companion"; package.mkdir()
        init = package / "__init__.py"; init.write_text("")
        helper = package / "helper.py"; helper.write_text("import fractions\n")
        self.script.write_text("import companion.helper\n")
        self.pin()
        record = self.document["scripts"]["finisher"]
        record["modules"].update({"companion/__init__.py": digest(init.read_bytes()),
                                  "companion/helper.py": digest(helper.read_bytes())})
        shadow = self.script.parent / "fractions.py"
        shadow.write_text("raise AssertionError('unpinned shadow ran')\n")
        for pinned in (False, True):
            if pinned: record["modules"]["scripts/fractions.py"] = digest(shadow.read_bytes())
            self.write()
            with self.subTest(pinned=pinned), self.assertRaises(Refusal) as caught:
                Toolchain(self.manifest).verify_scripts(self.game)
            self.assertEqual(caught.exception.finding["rule_id"], "toolchain.script_shadow")
            result = self.launch()
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.script_shadow")

    def test_runtime_imports_use_same_roots_packages_and_verified_bytes(self):
        package = self.game / "companion"; package.mkdir()
        init = package / "__init__.py"; init.write_text("from . import helper\n")
        helper = package / "helper.py"; helper.write_text("from helper import run\nrun()\n")
        self.helper.write_text("import fractions\ndef run(): print(fractions.Fraction(1, 2))\n")
        self.script.write_text("import companion.helper\n")
        self.pin()
        record = self.document["scripts"]["finisher"]
        record["modules"].update({"companion/__init__.py": digest(init.read_bytes()),
                                  "companion/helper.py": digest(helper.read_bytes())})
        self.write()
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(result.stdout, "1/2\n")
        self.assertFalse(list(self.game.rglob("__pycache__")))

    def test_runtime_guard_refuses_indirect_unpinned_loaders_even_when_ast_misses_them(self):
        other = self.script.parent / "other.py"
        other.write_text("raise AssertionError('unpinned code executed')\n")
        cases = (
            "import builtins\nload = getattr(builtins, '__im' + 'port__')\nload('other')\n",
            "import importlib.util\nfrom pathlib import Path\n"
            "spec = getattr(importlib.util, 'spec_from_file_' + 'location')('other', Path(__file__).with_name('other.py'))\n"
            "module = getattr(importlib.util, 'module_from_' + 'spec')(spec)\n"
            "getattr(spec.loader, 'exec_' + 'module')(module)\n",
            "import builtins\nfrom pathlib import Path\n"
            "getattr(builtins, 'ex' + 'ec')(Path(__file__).with_name('other.py').read_text())\n")
        for source in cases:
            self.script.write_text(source)
            toolchain = self.pin()
            # These evade diagnostics deliberately; runtime must still refuse.
            toolchain.verify_scripts(self.game)
            result = self.launch()
            with self.subTest(source=source):
                self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
                self.assertEqual(result.stderr, "")
                self.assertIn(json.loads(result.stdout)["findings"][0]["rule_id"],
                              ("config.pins_missing", "toolchain.script_inventory"))
                self.assertNotIn("unpinned code executed", result.stdout)

    def test_runtime_rechecks_source_after_diagnostics_before_import(self):
        # Entry modifies its pinned helper after verification; loader must refuse.
        self.script.write_text("from pathlib import Path\n"
            "Path(__file__).with_name('helper.py').write_text(\"raise AssertionError('changed code executed')\\n\")\n"
            "import helper\n")
        self.pin()
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.changed")

    def test_runtime_ignores_companion_bytecode(self):
        self.helper.write_text("print('verified source')\n")
        self.script.write_text("import helper\n")
        self.pin()
        import py_compile
        replacement = self.script.parent / "replacement.py"
        replacement.write_text("raise AssertionError('unverified bytecode executed')\n")
        py_compile.compile(str(replacement), cfile=str(self.helper.with_suffix('.pyc')), doraise=True)
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(result.stdout, "verified source\n")

    def test_runtime_refuses_direct_bytecode_loader_with_external_code_filename(self):
        import py_compile
        replacement = self.root / "outside.py"
        replacement.write_text("raise AssertionError('unverified bytecode executed')\n")
        py_compile.compile(str(replacement), cfile=str(self.script.parent / "other.pyc"), doraise=True)
        self.script.write_text("import importlib.util\nfrom pathlib import Path\n"
            "spec = getattr(importlib.util, 'spec_from_file_' + 'location')('other', Path(__file__).with_name('other.pyc'))\n"
            "module = getattr(importlib.util, 'module_from_' + 'spec')(spec)\n"
            "getattr(spec.loader, 'exec_' + 'module')(module)\n")
        self.pin().verify_scripts(self.game)
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.script_inventory")
        self.assertNotIn("unverified bytecode executed", result.stderr + result.stdout)

    def test_runtime_refuses_obtained_non_source_loaders_regardless_of_suffix_and_filename(self):
        import py_compile
        sentinel = self.game / "bytecode-executed"
        replacement = self.root / "outside.py"
        replacement.write_text(f"open({str(sentinel)!r}, 'w').write('ran')\n")
        for suffix in (".PYC", ".bin"):
            for filename in (str(replacement), "<string>"):
                bytecode = self.script.parent / ("other" + suffix)
                py_compile.compile(str(replacement), cfile=str(bytecode), dfile=filename, doraise=True)
                self.script.write_text("import importlib.machinery\nimport importlib.util\n"
                    f"loader = getattr(importlib.machinery, 'Sourceless' + 'FileLoader')('other', {str(bytecode)!r})\n"
                    "spec = getattr(importlib.util, 'spec_from_' + 'loader')('other', loader)\n"
                    "module = getattr(importlib.util, 'module_from_' + 'spec')(spec)\n"
                    "getattr(loader, 'exec_' + 'module')(module)\n")
                self.pin().verify_scripts(self.game)
                result = self.launch()
                with self.subTest(suffix=suffix, filename=filename):
                    self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
                    self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.script_inventory")
                    self.assertFalse(sentinel.exists())

    def test_runtime_refuses_project_native_loader_before_loading(self):
        self.script.write_text("import importlib.machinery\nimport importlib.util\n"
            f"loader = getattr(importlib.machinery, 'Extension' + 'FileLoader')('other', {str(self.helper)!r})\n"
            "spec = getattr(importlib.util, 'spec_from_' + 'loader')('other', loader)\n"
            "getattr(importlib.util, 'module_from_' + 'spec')(spec)\n")
        self.pin().verify_scripts(self.game)
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.script_inventory")

    def test_runtime_refuses_custom_non_source_loader_with_pinned_spec_origin(self):
        sentinel = self.game / "custom-loader-executed"
        payload = f"open({str(sentinel)!r}, 'w').write('ran')"
        self.script.write_text("import builtins\nimport importlib.abc\nimport importlib.machinery\nimport importlib.util\n"
            "class OtherLoader(importlib.abc.Loader):\n"
            "    def create_module(self, spec): return None\n"
            "    def exec_module(self, module):\n"
            f"        getattr(builtins, 'ex' + 'ec')({payload!r}, module.__dict__)\n"
            "loader = OtherLoader()\n"
            f"spec = importlib.machinery.ModuleSpec('other', loader, origin={str(self.helper)!r})\n"
            "module = getattr(importlib.util, 'module_from_' + 'spec')(spec)\n"
            "getattr(loader, 'exec_' + 'module')(module)\n")
        self.pin().verify_scripts(self.game)
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "toolchain.script_inventory")
        self.assertFalse(sentinel.exists())

    def test_direct_source_loader_uses_verified_snapshot_and_refuses_unpinned_file(self):
        sentinel = self.game / "source-loader-executed"
        other = self.script.parent / "other.py"
        other.write_text(f"open({str(sentinel)!r}, 'w').write('ran')\n")
        self.helper.write_text("print('direct pinned source')\n")
        for path, expected in ((self.helper, 0), (other, 1)):
            self.script.write_text("import importlib.machinery\nimport importlib.util\n"
                f"loader = getattr(importlib.machinery, 'Source' + 'FileLoader')('other', {str(path)!r})\n"
                "spec = getattr(importlib.util, 'spec_from_' + 'loader')('other', loader)\n"
                "module = getattr(importlib.util, 'module_from_' + 'spec')(spec)\n"
                "getattr(loader, 'exec_' + 'module')(module)\n")
            self.pin().verify_scripts(self.game)
            result = self.launch()
            self.assertEqual(result.returncode, expected, result.stderr + result.stdout)
            if expected == 0:
                self.assertEqual(result.stdout, "direct pinned source\n")
            else:
                self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "config.pins_missing")
            self.assertFalse(sentinel.exists())

    def test_runtime_refuses_symlink_replacement_after_verification(self):
        outside = self.root / "outside.py"; outside.write_bytes(self.helper.read_bytes())
        probe = self.game / "probe.py"
        try: probe.symlink_to(outside)
        except OSError: self.skipTest("Symlinks unavailable")
        probe.unlink()
        self.script.write_text("from pathlib import Path\n"
            "helper = Path(__file__).with_name('helper.py')\nhelper.unlink()\n"
            f"helper.symlink_to({str(outside)!r})\nimport helper\n")
        self.pin()
        result = self.launch()
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "path.symlink")

    def test_explicit_companion_root_controls_namespace_imports(self):
        namespace = self.game / "namespace"; namespace.mkdir()
        helper = namespace / "helper.py"; helper.write_text("print('namespace companion')\n")
        self.script.write_text("import namespace.helper\n")
        self.pin()
        record = self.document["scripts"]["finisher"]
        record["modules"]["namespace/helper.py"] = digest(helper.read_bytes())
        record["import_roots"] = ["."]
        self.write()
        Toolchain(self.manifest).verify_scripts(self.game)
        result = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(result.stdout, "namespace companion\n")

    def test_manifest_changes_refused_before_game_script_use(self):
        toolchain = self.pin()
        self.document["scripts"]["finisher"]["sha256"] = "0"*64
        self.write()
        with self.assertRaises(Refusal) as caught: toolchain.verify_scripts(self.game)
        self.assertEqual(caught.exception.finding["rule_id"], "toolchain.changed")

    def test_script_mapping_strict_keys_hashes_and_paths(self):
        self.pin()
        baseline = deepcopy(self.document)
        mutations = (("sha256", "invalid"), ("path", "../finish.py"), ("path", "/finish.py"),
                     ("path", "scripts\\finish.py"), ("path", "scripts/./finish.py"),
                     ("path", "scripts/not_python.txt"), ("command", "python"), ("modules", []),
                     ("import_roots", ["../escape"]), ("import_roots", ["scripts", "scripts"]),
                     ("import_roots", "scripts"))
        for field, value in mutations:
            self.document = deepcopy(baseline)
            self.document["scripts"]["finisher"][field] = value
            self.write()
            with self.subTest(field=field, value=value), self.assertRaises(Refusal): Toolchain(self.manifest)

    def test_symlink_inventory_member_refused(self):
        toolchain = self.pin()
        outside = self.root/"outside.py"; outside.write_bytes(self.helper.read_bytes())
        self.helper.unlink()
        try: self.helper.symlink_to(outside)
        except OSError: self.skipTest("Symlinks unavailable")
        with self.assertRaises(Refusal) as caught: toolchain.verify_scripts(self.game)
        self.assertEqual(caught.exception.finding["rule_id"], "path.symlink")

    def test_capability_negotiation_advertises_manifest_extension_only(self):
        class Service:
            def capabilities(self):
                return {"schemas": [], "profiles": []}
        features = dispatch(Service(), "capabilities", {})["features"]
        self.assertTrue(features["meshgen_script_pins"])
        self.assertTrue(features["meshgen_configuration"])
        self.assertNotIn("max_tris_setting", features)
        self.assertNotIn("meshgen_execution", features)


class WindowsPathVerification(unittest.TestCase):
    def kernel(self, opened, final, attributes=0x10):
        kernel = mock.Mock()
        kernel.CreateFileW.return_value = 123
        def name(value, buffer, size):
            if buffer is None: return len(value)+1
            buffer.value = value
            return len(value)
        kernel.GetLongPathNameW.side_effect = lambda path, buffer, size: name(opened, buffer, size)
        kernel.GetFinalPathNameByHandleW.side_effect = lambda handle, buffer, size, flags: name(final, buffer, size)
        def information(handle, kind, pointer, size):
            if kind == 9: pointer._obj.attributes = attributes
            return True
        kernel.GetFileInformationByHandleEx.side_effect = information
        return kernel

    def test_short_names_case_separators_and_drive_roots_use_same_comparison(self):
        from luminumbra_author import meshgen_scripts as scripts
        for supplied, expanded, final in (
                ('C:/', 'C:\\', '\\\\?\\C:\\'),
                ('c:/users/LOCALU~1/Local', 'c:\\Users\\Local User\\Local',
                 '\\\\?\\C:\\Users\\Local User\\Local'),
                ('D:/Project/scripts', 'D:\\Project\\scripts', '\\\\?\\d:\\project\\SCRIPTS')):
            with self.subTest(path=supplied):
                kernel = self.kernel(expanded, final)
                with mock.patch.object(scripts, '_windows_path_api', return_value=kernel):
                    handle, identity = scripts._windows_directory(PureWindowsPath(supplied))
                    self.assertEqual(handle, 123)
                    self.assertEqual(scripts._windows_final_path(handle), scripts._windows_long_path(supplied))
                kernel.CloseHandle.assert_not_called()
                self.assertEqual(identity, (0, bytes(16)))
                self.assertEqual(kernel.CreateFileW.call_args.args[2], 3)

    def test_expansion_cannot_accept_a_different_handle_or_reparse_directory(self):
        from luminumbra_author import meshgen_scripts as scripts
        for final, attributes, rule in (
                ('\\\\?\\D:\\Users\\Local User', 0x10, 'toolchain.script_path'),
                ('\\\\?\\C:\\outside', 0x10, 'toolchain.script_path'),
                ('\\\\?\\UNC\\server\\share', 0x10, 'toolchain.script_path'),
                ('\\\\?\\C:\\Users\\Local User', 0x410, 'path.symlink'),
                ('\\\\?\\C:\\Users\\Local User', 0, 'toolchain.script_path')):
            with self.subTest(final=final, attributes=attributes):
                kernel = self.kernel('C:\\Users\\Local User', final, attributes)
                with mock.patch.object(scripts, '_windows_path_api', return_value=kernel):
                    with self.assertRaises(Refusal) as caught:
                        scripts._windows_directory(PureWindowsPath('C:/Users/LOCALU~1'))
                self.assertEqual(caught.exception.finding['rule_id'], rule)
                kernel.CloseHandle.assert_called_once_with(123)
                if attributes & 0x400: kernel.GetLongPathNameW.assert_not_called()

    def test_non_dos_and_traversal_spellings_are_refused_before_expansion(self):
        from luminumbra_author import meshgen_scripts as scripts
        for name in ('C:relative', '/relative', '//server/share/file', '//?/C:/file',
                     'C:/project/../outside', 'C:/project/\x00file'):
            with self.subTest(path=name), mock.patch.object(scripts, '_windows_path_api') as api:
                with self.assertRaises(Refusal) as caught: scripts._windows_long_path(name)
                self.assertEqual(caught.exception.finding['rule_id'], 'toolchain.script_path')
                api.assert_not_called()


if __name__ == "__main__":
    unittest.main()

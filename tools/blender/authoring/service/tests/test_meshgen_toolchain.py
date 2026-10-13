"""Static pin diagnostics and guarded launches of stdlib-only game fixtures."""
from copy import deepcopy
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
from luminumbra_author.contracts import Toolchain, TOOLCHAIN, Refusal, canonical, digest
from luminumbra_author.__main__ import dispatch


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

    def test_old_toolchain_valid_but_mg2_requires_role(self):
        self.write()
        toolchain = Toolchain(self.manifest)
        toolchain.verify()
        with self.assertRaises(Refusal) as caught: toolchain.verify_scripts(self.game)
        self.assertEqual(caught.exception.finding["rule_id"], "config.pins_missing")

    def test_explicit_project_root_and_rechecks(self):
        toolchain = self.pin()
        result = toolchain.verify_scripts(self.game)
        self.assertEqual(result["path"], self.script)
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

    def launch(self, cwd=None, environment=None):
        return subprocess.run([sys.executable, "-I", "-S", "-B", str(SERVICE / "launch.py"), "--scripts",
            "--project", str(self.game), "--toolchain", str(self.manifest)],
            env=environment or dict(os.environ, PYTHONPATH=str(self.game), PYTHONDONTWRITEBYTECODE="1"),
            cwd=cwd or self.root, capture_output=True, text=True, timeout=15)

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


if __name__ == "__main__":
    unittest.main()

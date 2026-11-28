"""Portable regressions for the qualified Blender authoring profile."""
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tomllib
from types import SimpleNamespace
import unittest
from unittest.mock import patch


AUTHORING = Path(__file__).resolve().parents[1]
EXTENSION = AUTHORING / "extension"
EXPORTER_ID = "blender.gltf.5.2.41"
EXPORTER_PIN = "06723e7f58aa29513f951b67f1a5788d74693066c0cae6432da7230b162172fd"


class ProfileContracts(unittest.TestCase):
    def test_manifest_version_range(self):
        with (EXTENSION / "blender_manifest.toml").open("rb") as stream:
            manifest = tomllib.load(stream)
        self.assertEqual(manifest["blender_version_min"], "5.2.4")
        self.assertEqual(manifest["blender_version_max"], "5.2.5")

    def test_manifest_permission_and_tagline_lengths(self):
        with (EXTENSION / "blender_manifest.toml").open("rb") as stream:
            manifest = tomllib.load(stream)
        values = {"tagline": manifest["tagline"], **manifest["permissions"]}
        for name, value in values.items():
            with self.subTest(field=name):
                self.assertIsInstance(value, str)
                self.assertLessEqual(len(value), 64)

    def test_no_stale_profile_references(self):
        stale = (
            "(5, 1, 0)", "(5,1,0)", "5.1.0", "5.1.18", "adfe2921d5f3",
            "5fe9f5ede7e5264b0a1045dc3784e243e645a90cb6073fc73ae56bc7a10de447",
        )
        paths = sorted(EXTENSION.glob("*.py"))
        paths.append(EXTENSION / "blender_manifest.toml")
        paths.extend(sorted((AUTHORING / "extension_tests").glob("native_*.py")))
        for path in paths:
            source = path.read_text(encoding="utf-8")
            for reference in stale:
                with self.subTest(path=path.name, reference=reference):
                    self.assertNotIn(reference, source)

    def test_all_version_comparisons_use_qualified_profile(self):
        required = {
            "__init__.py", "export_worker.py", "native_basis_probe.py",
            "native_evaluated_asset_probe.py", "native_viewport_presentation.py",
            "native_viewport_engine.py",
        }
        paths = [EXTENSION / "__init__.py", EXTENSION / "export_worker.py"]
        paths.extend(sorted((AUTHORING / "extension_tests").glob("native_*.py")))
        checked = set()
        for path in paths:
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                if not isinstance(node, ast.Compare):
                    continue
                operands = [node.left, *node.comparators]
                for index, operand in enumerate(operands):
                    if ast.unparse(operand) != "bpy.app.version":
                        continue
                    checked.add(path.name)
                    with self.subTest(path=path.name, line=node.lineno):
                        self.assertEqual(len(node.ops), 1)
                        self.assertIsInstance(node.ops[0], (ast.Eq, ast.NotEq))
                        self.assertEqual(ast.literal_eval(operands[1 - index]), (5, 2, 4))
        self.assertTrue(required <= checked, f"Missing version gates: {required - checked}")

    def test_evaluated_probe_build_pin(self):
        source = (AUTHORING / "extension_tests/native_evaluated_asset_probe.py").read_text(encoding="utf-8")
        self.assertIn('bpy.app.build_hash == b"cd5dc428cce1"', source)

    def test_exporter_identity_and_encoding(self):
        source = (EXTENSION / "export_worker.py").read_text(encoding="utf-8")
        self.assertIn(f'"id": "{EXPORTER_ID}"', source)
        self.assertIn(f'exporter["sha256"] != "{EXPORTER_PIN}"', source)
        # Encoding is inline in main(), with no importable encoding helper.
        self.assertIn("sort_keys=True", source)
        self.assertIn('separators=(",", ":")', source)
        self.assertIn("ensure_ascii=False", source)
        self.assertIn('.encode("utf-8")', source)
        records = [
            {"path": "z.py", "sha256": "b" * 64},
            {"sha256": "a" * 64, "path": "materials/éclairage.py"},
        ]
        records.sort(key=lambda entry: entry["path"])
        canonical = json.dumps(records, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        escaped = json.dumps(records, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
        self.assertIn("éclairage.py".encode("utf-8"), canonical)
        self.assertNotEqual(hashlib.sha256(canonical).hexdigest(), hashlib.sha256(escaped).hexdigest())


class WorkerVersionGate(unittest.TestCase):
    def setUp(self):
        self.bpy = SimpleNamespace(app=SimpleNamespace(version=(5, 2, 4)))
        # Restore both the module cache and the worker's import-path insertion.
        with patch.dict(sys.modules, {"bpy": self.bpy}), patch.object(sys, "path", list(sys.path)):
            for name in ("logic", "blender_asset"):
                sys.modules.pop(name, None)
            spec = importlib.util.spec_from_file_location("profile_export_worker", EXTENSION / "export_worker.py")
            self.worker = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(self.worker)
        argv = patch.object(sys, "argv", ["export_worker.py", "--", "request.json"])
        argv.start()
        self.addCleanup(argv.stop)
        request = patch.object(self.worker, "read_json", return_value={"project": "."})
        request.start()
        self.addCleanup(request.stop)

    def test_previous_version_is_refused_before_project_access_or_metadata(self):
        self.bpy.app.version = (5, 1, 0)
        with patch.object(self.worker.Path, "resolve") as resolve, patch.object(self.worker, "atomic_json") as publish:
            with self.assertRaisesRegex(RuntimeError, "qualified for Blender 5.2.4 only"):
                self.worker.main()
            resolve.assert_not_called()
            publish.assert_not_called()

    def test_qualified_version_passes_gate(self):
        # Stop at the first operation after the gate, without touching disk.
        with patch.object(self.worker.Path, "resolve", side_effect=RuntimeError("project resolution reached")) as resolve:
            with self.assertRaisesRegex(RuntimeError, "project resolution reached"):
                self.worker.main()
            resolve.assert_called_once_with(strict=True)


if __name__ == "__main__":
    unittest.main()

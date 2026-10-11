"""Portable evaluated-data contract controls; native Blender remains separate."""
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace as NS
import unittest
from unittest.mock import Mock, patch


EXTENSION = Path(__file__).resolve().parents[1] / "extension"


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, EXTENSION / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def material():
    result = Mock(use_nodes=False)
    result.original = result
    return result


def mesh(materials=(), uv=True, index=0):
    return NS(materials=materials, uv_layers=[NS(active_render=True)] if uv else [],
              polygons=[NS(material_index=index)])


class EvaluatedContracts(unittest.TestCase):
    def setUp(self):
        self.graph = object()
        self.bpy = NS(context=NS(evaluated_depsgraph_get=Mock(return_value=self.graph)))
        self.modules = patch.dict(sys.modules, {"bpy": self.bpy})
        self.modules.start()
        self.addCleanup(self.modules.stop)
        self.asset = load("evaluated_asset_under_test", "blender_asset.py")
        self.original, self.replacement = material(), material()
        self.evaluated_mesh = mesh((self.replacement,))
        self.evaluated = NS(to_mesh=Mock(return_value=self.evaluated_mesh), to_mesh_clear=Mock())
        self.obj = NS(type="MESH", modifiers=[NS(type="NODES")], data=mesh((self.original,)),
                      material_slots=[NS(material=self.original)], evaluated_get=Mock(return_value=self.evaluated))
        self.collection = NS(all_objects=[self.obj])

    def test_evaluated_assignment_replaces_original_slot(self):
        self.assertEqual(self.asset.evaluated_static_materials(self.collection), {self.replacement})
        self.obj.evaluated_get.assert_called_once_with(self.graph)
        self.evaluated.to_mesh.assert_called_once_with(preserve_all_data_layers=True, depsgraph=self.graph)
        self.evaluated.to_mesh_clear.assert_called_once_with()

    def test_evaluated_material_is_resolved_to_original_before_mesh_release(self):
        copy = material()
        copy.original = self.replacement
        self.evaluated_mesh.materials = [copy]
        self.assertEqual(self.asset.evaluated_static_materials(self.collection), {self.replacement})

    def test_unsupported_evaluated_shader_cannot_bypass_prefab_validation(self):
        # The former original-slot-only validator returned success for this
        # empty source assignment, ignoring the shader introduced by GN.
        self.obj.material_slots = []
        with self.assertRaisesRegex(ValueError, "Principled material"):
            self.asset.validate_prefab_materials(self.collection)
        self.evaluated.to_mesh_clear.assert_called_once_with()

    def test_unexported_original_shader_is_not_validated(self):
        self.evaluated_mesh.materials = ()
        self.asset.validate_prefab_materials(self.collection)
        self.evaluated.to_mesh_clear.assert_called_once_with()

    def test_unused_original_slot_is_excluded_from_evaluated_assignments(self):
        self.evaluated_mesh.materials = (self.original, self.replacement)
        self.evaluated_mesh.polygons[0].material_index = 1
        self.assertEqual(self.asset.evaluated_static_materials(self.collection), {self.replacement})

    def test_no_modifier_uses_object_material_override(self):
        self.obj.modifiers = []
        self.obj.data.materials = [self.replacement]
        self.assertEqual(self.asset.evaluated_static_materials(self.collection), {self.original})
        self.obj.evaluated_get.assert_not_called()
        self.evaluated.to_mesh_clear.assert_not_called()

    def test_exact_exporter_empty_slot_fallback_preserves_object_material(self):
        self.evaluated_mesh.materials = (None,)
        self.assertEqual(self.asset.evaluated_static_materials(self.collection), {self.original})
        self.evaluated_mesh.materials = (None, self.replacement)
        self.evaluated_mesh.polygons[0].material_index = 1
        self.assertEqual(self.asset.evaluated_static_materials(self.collection), {self.replacement})

    def test_modifier_removed_uv_is_refused_and_mesh_released(self):
        self.evaluated_mesh.uv_layers = []
        with self.assertRaisesRegex(ValueError, "evaluated geometry"):
            self.asset.evaluated_static_materials(self.collection)
        self.evaluated.to_mesh_clear.assert_called_once_with()

    def test_modifier_generated_uv_can_replace_missing_source_uv(self):
        self.obj.data.uv_layers = []
        self.assertEqual(self.asset.evaluated_static_materials(self.collection), {self.replacement})

    def test_missing_render_uv_is_refused(self):
        self.evaluated_mesh.uv_layers[0].active_render = False
        with self.assertRaisesRegex(ValueError, "active render UV"):
            self.asset.evaluated_static_materials(self.collection)
        self.evaluated.to_mesh_clear.assert_called_once_with()

    def test_invalid_evaluated_face_assignment_is_refused(self):
        for index in (-1, 1):
            with self.subTest(index=index):
                self.evaluated_mesh.polygons[0].material_index = index
                with self.assertRaisesRegex(ValueError, "face material assignment"):
                    self.asset.evaluated_static_materials(self.collection)
        self.assertEqual(self.evaluated.to_mesh_clear.call_count, 2)

    def test_failed_mesh_evaluation_releases_owned_temporary(self):
        self.evaluated.to_mesh.side_effect = RuntimeError("evaluation failed")
        with self.assertRaisesRegex(RuntimeError, "evaluation failed"):
            self.asset.evaluated_static_materials(self.collection)
        self.evaluated.to_mesh_clear.assert_called_once_with()

    def test_missing_mesh_is_refused_and_released(self):
        self.evaluated.to_mesh.return_value = None
        with self.assertRaisesRegex(ValueError, "did not produce a mesh"):
            self.asset.evaluated_static_materials(self.collection)
        self.evaluated.to_mesh_clear.assert_called_once_with()

    def test_cancellation_inside_mesh_scope_releases_temporary(self):
        with self.assertRaises(KeyboardInterrupt):
            with self.asset.static_mesh(self.obj, self.graph):
                raise KeyboardInterrupt()
        self.evaluated.to_mesh_clear.assert_called_once_with()


class LoadedSnapshotContracts(unittest.TestCase):
    def setUp(self):
        self.collection = object()
        self.scene = object()
        self.validate = Mock(return_value=("static", set()))
        self.prefab = Mock()
        modules = {"bpy": NS(), "blender_asset": NS(validate_collection=self.validate,
                   validate_prefab_materials=self.prefab), "logic": load("evaluated_worker_logic", "logic.py")}
        with patch.dict(sys.modules, modules):
            old_path = list(sys.path)
            try:
                self.worker = load("evaluated_export_worker", "export_worker.py")
            finally:
                sys.path[:] = old_path

    def test_loaded_prefab_is_revalidated(self):
        request = {"profile": "static", "build_profile": "glb-static-prefab-v1"}
        self.assertEqual(self.worker.validate_snapshot(self.scene, self.collection, request),
                         ("static", "glb-static-prefab-v1"))
        self.validate.assert_called_once_with(self.scene, self.collection)
        self.prefab.assert_called_once_with(self.collection)

    def test_loaded_snapshot_refusal_propagates_before_export(self):
        self.prefab.side_effect = ValueError("unsupported evaluated shader")
        with self.assertRaisesRegex(ValueError, "unsupported evaluated shader"):
            self.worker.validate_snapshot(self.scene, self.collection,
                {"profile": "static", "build_profile": "glb-static-prefab-v1"})

    def test_snapshot_rig_profile_cannot_be_relabelled_static(self):
        self.validate.return_value = ("character", set())
        with self.assertRaisesRegex(RuntimeError, "differs"):
            self.worker.validate_snapshot(self.scene, self.collection, {"profile": "static"})
        self.prefab.assert_not_called()

    def test_geometry_profile_keeps_its_distinct_material_contract(self):
        self.worker.validate_snapshot(self.scene, self.collection, {"profile": "static"})
        self.validate.assert_called_once_with(self.scene, self.collection)
        self.prefab.assert_not_called()

    def test_unknown_profile_is_refused_before_snapshot_evaluation(self):
        for request in ({"profile": "unknown"}, {"profile": "static", "build_profile": "unknown"}):
            with self.subTest(request=request), self.assertRaisesRegex(RuntimeError, "Unsupported"):
                self.worker.validate_snapshot(self.scene, self.collection, request)
        self.validate.assert_not_called()


if __name__ == "__main__":
    unittest.main()

"""Headless operator control flow; Blender's UI and undo stack remain native checks."""
import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace as NS
import unittest
from unittest.mock import Mock, patch


EXTENSION = Path(__file__).resolve().parents[1] / "extension"


class ReviewedOperator(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        package = ModuleType("recipe_operator_test")
        package.__path__ = [str(EXTENSION)]
        modules = {package.__name__: package,
                   "bpy": NS(types=NS(Operator=object), props=NS(StringProperty=Mock())),
                   package.__name__ + ".blender_asset": NS(validate_collection=Mock())}
        patched = patch.dict(sys.modules, modules)
        patched.start()
        self.addCleanup(patched.stop)
        name = package.__name__ + ".blender_recipes"
        spec = importlib.util.spec_from_file_location(name, EXTENSION / "blender_recipes.py")
        self.adapter = importlib.util.module_from_spec(spec)
        sys.modules[name] = self.adapter
        spec.loader.exec_module(self.adapter)
        self.rows, self.blocks = [], {}
        for kind, uid, key, identity in (
                ("collection", 1, "luminumbra.asset_id", "asset.prop"),
                ("mesh", 2, "luminumbra.asset_id", None),
                ("object", 3, "luminumbra.object_id", "")):
            handle = f"{kind}:{uid}"
            self.rows.append(dict(kind=kind, uid=uid, handle=handle, identity=identity,
                                  target=True, editable=True, label=kind, prefix=kind, key=key,
                                  shared_users=["Object:outside"] if kind == "mesh" else []))
            self.blocks[handle] = ({} if identity is None else {key: identity}, key)
        self.scene = NS(lum_author_collection=NS(session_uid=1))
        self.dialog = Mock(return_value={"RUNNING_MODAL"})
        self.context = NS(scene=self.scene, window_manager=NS(invoke_props_dialog=self.dialog))
        self.adapter.records = Mock(side_effect=lambda *_: (self.current_rows(), self.blocks))
        self.adapter.receipt_directory = Mock(return_value=self.root)
        self.adapter.before_apply = Mock()

    def current_rows(self):
        rows = copy.deepcopy(self.rows)
        for value in rows:
            block, key = self.blocks[value["handle"]]
            value["identity"] = block.get(key)
        return rows

    def operator(self):
        operator = self.adapter.LUMINUMBRA_OT_review_id_repairs()
        operator.plan_sha256, operator.request_id = "", ""
        operator.report = Mock()
        return operator

    def invoke(self):
        operator = self.operator()
        self.assertEqual(operator.invoke(self.context, None), {"RUNNING_MODAL"})
        return operator

    def assert_no_application(self, before):
        self.assertEqual(self.current_rows(), before)
        self.assertEqual(self.adapter.state.receipts, {})
        self.adapter.before_apply.assert_not_called()

    def test_review_draw_and_cancel_preserve_ids_and_release_only_reviewed_plan(self):
        before = self.current_rows()
        other = self.adapter.plan(self.scene, self.scene.lum_author_collection)
        operator = self.invoke()
        self.dialog.assert_called_once_with(operator, width=700, confirm_text="Apply Reviewed Repairs")
        labels = []
        layout = NS(label=lambda **kwargs: labels.append(kwargs["text"]), box=lambda: layout)
        operator.layout = layout
        operator.draw(self.context)
        for change in operator._plan["changes"]:
            self.assertIn("After: " + change["after"], labels)
        self.assertIn("Also used by: Object:outside", labels)
        self.assertEqual(json.loads((self.root / ("plan-" + operator.plan_sha256 + ".json")).read_text()),
                         operator._plan)
        operator.cancel(self.context)
        operator.cancel(self.context)
        self.assertEqual(set(self.adapter.state.plans), {other["plan_sha256"]})
        self.assert_no_application(before)
        self.assertEqual(operator.execute(self.context), {"CANCELLED"})
        self.assert_no_application(before)

    def test_empty_reviews_do_not_consume_pending_plan_capacity(self):
        for value in self.rows:
            block, key = self.blocks[value["handle"]]
            block[key] = value["kind"] + ".unique"
        before = self.current_rows()
        for index in range(33):
            with self.subTest(review=index):
                # Different labels create distinct, still empty review snapshots.
                self.rows[0]["label"] = str(index)
                operator = self.operator()
                self.assertEqual(operator.invoke(self.context, None), {"CANCELLED"})
                operator.report.assert_called_once_with({"INFO"}, "All asset identities are already distinct")
                self.assertEqual(self.adapter.state.plans, {})
        self.rows[0]["label"] = before[0]["label"]
        self.dialog.assert_not_called()
        self.assert_no_application(before)

    def test_dialog_refusal_and_errors_release_review_but_preserve_other_plans(self):
        before = self.current_rows()
        other = self.adapter.plan(self.scene, self.scene.lum_author_collection)
        for failure in (None, OSError("dialog unavailable"), ValueError("invalid dialog"),
                        RuntimeError("window closed")):
            with self.subTest(failure=failure):
                self.dialog.return_value = {"CANCELLED"}
                self.dialog.side_effect = failure
                operator = self.operator()
                self.assertEqual(operator.invoke(self.context, None), {"CANCELLED"})
                self.assertEqual(set(self.adapter.state.plans), {other["plan_sha256"]})
                self.assert_no_application(before)

    def test_failed_plan_publication_never_leaves_an_applicable_review(self):
        before = self.current_rows()
        other = self.adapter.plan(self.scene, self.scene.lum_author_collection)
        with patch.object(self.adapter, "atomic_json", side_effect=OSError("plan storage failed")):
            operator = self.operator()
            self.assertEqual(operator.invoke(self.context, None), {"CANCELLED"})
        self.assertEqual(set(self.adapter.state.plans), {other["plan_sha256"]})
        self.dialog.assert_not_called()
        self.assertEqual(len(list(self.root.iterdir())), 1)
        self.assert_no_application(before)

    def test_execute_publishes_exact_ids_and_replay_does_not_reapply_undo(self):
        before = self.current_rows()
        operator = self.invoke()
        self.assertEqual(operator.execute(self.context), {"FINISHED"})
        applied = self.current_rows()
        for change in operator._plan["changes"]:
            block, key = self.blocks[change["handle"]]
            self.assertEqual(block[key], change["after"])
        receipt_path = self.root / (operator.request_id + ".json")
        receipt_bytes = receipt_path.read_bytes()
        receipt = json.loads(receipt_bytes)
        self.assertEqual(receipt["status"], "applied")
        self.assertEqual(receipt["changes"], operator._plan["changes"])
        self.assertEqual(self.adapter.state.plans, {})
        self.adapter.before_apply.assert_called_once_with()
        # Model data restoration and handler invalidation, not Blender's undo stack.
        for rows in (before, applied):
            for value in rows:
                block, key = self.blocks[value["handle"]]
                block.clear()
                if value["identity"] is not None:
                    block[key] = value["identity"]
            self.adapter.state.changed()
            with patch.object(self.adapter, "atomic_json", side_effect=AssertionError("replayed publication")):
                self.assertEqual(operator.execute(self.context), {"FINISHED"})
            self.assertEqual(self.current_rows(), rows)
            self.assertEqual(receipt_path.read_bytes(), receipt_bytes)
        self.adapter.before_apply.assert_called_once_with()

    def test_receipt_failure_restores_absent_and_empty_properties_and_replay_is_terminal(self):
        before = self.current_rows()
        operator = self.invoke()
        with patch.object(self.adapter, "atomic_json", side_effect=OSError("receipt storage failed")):
            self.assertEqual(operator.execute(self.context), {"CANCELLED"})
        self.assertEqual(self.current_rows(), before)
        self.assertNotIn("luminumbra.asset_id", self.blocks["mesh:2"][0])
        self.assertEqual(self.blocks["object:3"][0]["luminumbra.object_id"], "")
        self.assertEqual(self.adapter.state.receipts[operator.request_id]["status"], "failed")
        self.assertFalse((self.root / (operator.request_id + ".json")).exists())
        self.assertEqual(operator.execute(self.context), {"CANCELLED"})
        self.assertEqual(self.current_rows(), before)
        self.adapter.before_apply.assert_called_once_with()

    def test_stale_revision_snapshot_project_and_document_refuse_before_invalidation(self):
        for change in ("revision", "snapshot", "project", "document"):
            with self.subTest(change=change):
                self.adapter.receipt_directory.return_value = self.root
                operator = self.invoke()
                if change == "revision":
                    self.adapter.state.changed()
                elif change == "snapshot":
                    self.rows[1]["shared_users"].append("Object:new")
                elif change == "project":
                    self.adapter.receipt_directory.return_value = self.root / "other"
                else:
                    self.adapter.reset_document()
                before = self.current_rows()
                self.assertEqual(operator.execute(self.context), {"CANCELLED"})
                self.assert_no_application(before)


if __name__ == "__main__":
    unittest.main()

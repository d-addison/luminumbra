import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

path = Path(__file__).resolve().parents[1] / "extension" / "logic.py"
spec = importlib.util.spec_from_file_location("geometry_adapter_logic", path)
logic = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = logic
spec.loader.exec_module(logic)


class EditFeedback(unittest.TestCase):
    def test_atomic_guard_does_not_extend_long_target_filename(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / ("d" * (240 - len(str(root)) - 107))
            parent.mkdir()
            target = parent / ("g" * 100 + ".json")
            logic.atomic_json(target, {"revision": 42})
            self.assertEqual(logic.read_json(target), {"revision": 42})
            self.assertEqual(list(parent.iterdir()), [target])

    def result(self, status="succeeded"):
        return {"ok": True, "result": {"status": status, "job_id": "generation", "outputs": {
            "asset.lmesh": {"triangles": 24, "bytes": 1852}}}}

    def test_debounce_waits_for_last_edit_and_running_build(self):
        state = logic.BuildState()
        state.changed(1)
        self.assertFalse(state.due(1.4))
        state.changed(1.4)
        self.assertFalse(state.due(1.6))
        self.assertTrue(state.due(1.91))
        state.submitted()
        self.assertFalse(state.due(10))
        with self.assertRaises(ValueError):
            state.submitted()

    def test_old_completion_does_not_label_new_edit_as_built(self):
        state = logic.BuildState()
        state.changed(0)
        state.submitted()
        state.changed(1)
        state.completed(self.result())
        self.assertEqual(state.generation, "")
        self.assertTrue(state.dirty)
        self.assertTrue(state.due(2))

    def test_failure_and_cancel_keep_last_successful_generation(self):
        state = logic.BuildState()
        state.submitted()
        state.completed(self.result())
        for response in (self.result("cancelled"), {"ok": False, "message": "Source invalid"}):
            state.changed(1)
            state.submitted()
            state.completed(response)
            self.assertEqual(state.generation, "generation")
            self.assertEqual(state.published_revision, 0)
            self.assertFalse(state.busy)


if __name__ == "__main__":
    unittest.main()

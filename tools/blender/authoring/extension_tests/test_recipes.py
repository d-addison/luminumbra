import copy
import importlib.util
from pathlib import Path
import unittest


path = Path(__file__).resolve().parents[1] / "extension" / "recipes.py"
spec = importlib.util.spec_from_file_location("authoring_recipes", path)
recipes = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipes)


def row(kind, uid, identity, target=True, users=()):
    key = {"collection": "asset", "object": "object", "mesh": "asset", "material": "material"}[kind]
    return {"kind": kind, "uid": uid, "handle": f"{kind}:{uid}", "identity": identity,
            "target": target, "editable": True, "label": f"{kind} {uid}",
            "prefix": "asset" if kind == "collection" else kind,
            "key": f"luminumbra.{key}_id", "shared_users": list(users)}


class ReviewedRecipes(unittest.TestCase):
    def setUp(self):
        self.rows = [row("collection", 1, "asset.prop"), row("object", 2, "object.original"),
                     row("object", 3, "object.original"), row("mesh", 4, None, users=("Object:outside",))]
        self.session = recipes.RecipeSession()
        self.values = {r["handle"]: r["identity"] for r in self.rows}
        self.published = []

    def plan(self):
        return self.session.plan(self.rows, "collection:1")

    def apply(self, plan, request="request_1", write=None, publish=None):
        return self.session.apply(plan["plan_sha256"], request, self.rows,
                                  write or self.values.__setitem__, publish or self.published.append)

    def test_plan_changes_nothing_and_exposes_shared_users_and_exact_replacements(self):
        before = copy.deepcopy(self.rows)
        plan = self.plan()
        self.assertEqual(self.rows, before)
        self.assertEqual(self.session.revision, 0)
        self.assertEqual([r["handle"] for r in plan["changes"]], ["mesh:4", "object:3"])
        self.assertEqual(plan["changes"][0]["shared_users"], ["Object:outside"])
        self.assertNotEqual(plan["changes"][1]["after"], "object.original")
        self.assertEqual(plan["definition_sha256"], recipes.DEFINITION_DIGEST)

    def test_oldest_external_owner_is_preserved_even_when_labels_sort_differently(self):
        self.rows.append(row("object", 0, "object.original", target=False))
        self.rows[1]["label"] = "A first label"
        plan = self.plan()
        self.assertEqual({r["handle"] for r in plan["changes"]}, {"mesh:4", "object:2", "object:3"})

    def test_apply_matches_reviewed_ids_and_receipt_replay_never_repeats_after_undo(self):
        plan = self.plan()
        result = self.apply(plan)
        for change in plan["changes"]:
            self.assertEqual(self.values[change["handle"]], change["after"])
        self.assertEqual(self.values["object:2"], "object.original")
        self.assertEqual(result["after_revision"], 1)
        self.assertEqual(self.session.revision, 1)
        self.assertEqual(len(self.published), 1)
        # Blender's undo restores authored data, not this session's receipt log.
        self.values = {r["handle"]: r["identity"] for r in self.rows}
        self.session.changed()
        self.assertEqual(self.apply(plan), result)
        self.assertEqual(self.values["mesh:4"], None)
        self.assertEqual(len(self.published), 1)
        with self.assertRaises(recipes.RecipeError):
            self.session.apply("different", "request_1", self.rows, self.values.__setitem__, self.published.append)

    def test_returned_plan_cannot_change_stored_review(self):
        plan = self.plan()
        expected = plan["changes"][0]["after"]
        plan["changes"][0]["after"] = "injected"
        result = self.apply(plan)
        self.assertEqual(result["changes"][0]["after"], expected)

    def test_edit_undo_reload_and_changed_dependencies_refuse_without_writes(self):
        for mutation in (lambda: self.session.changed(),
                         lambda: setattr(self, "session", recipes.RecipeSession()),
                         lambda: self.rows[3]["shared_users"].append("Object:new user"),
                         lambda: self.rows.append(row("object", 0, "object.original", False))):
            with self.subTest(mutation=mutation):
                self.setUp()
                plan = self.plan()
                before = dict(self.values)
                mutation()
                with self.assertRaises(recipes.RecipeError):
                    self.apply(plan)
                self.assertEqual(self.values, before)
                self.assertEqual(self.published, [])

    def test_partial_write_failure_restores_preimage_including_failing_write(self):
        plan = self.plan()
        before, calls = dict(self.values), []

        def failing_write(handle, value):
            self.values[handle] = value
            calls.append((handle, value))
            if len(calls) == 2:
                raise RuntimeError("injected write failure")

        with self.assertRaisesRegex(recipes.RecipeError, "injected write failure"):
            self.apply(plan, write=failing_write)
        self.assertEqual(self.values, before)
        self.assertEqual(len(calls), 4)
        self.assertEqual(self.published[-1]["status"], "failed")
        self.assertEqual(self.apply(plan)["status"], "failed")

    def test_receipt_failure_rolls_back_all_edits_and_is_terminal(self):
        plan = self.plan()
        before = dict(self.values)

        def disk_failure(_):
            raise OSError("disk full")

        with self.assertRaisesRegex(recipes.RecipeError, "disk full"):
            self.apply(plan, publish=disk_failure)
        self.assertEqual(self.values, before)
        self.assertEqual(self.apply(plan)["status"], "failed")

    def test_rollback_failure_is_reported_without_atomic_success_claim(self):
        plan = self.plan()

        def cannot_write(_handle, _value):
            raise RuntimeError("datablock no longer writable")

        with self.assertRaises(recipes.RecipeError) as result:
            self.apply(plan, write=cannot_write)
        self.assertEqual(result.exception.rule, "recipe.rollback")
        self.assertEqual(self.published[-1]["status"], "rollback_failed")

    def test_unknown_digest_and_invalid_requests_are_refused(self):
        plan = self.plan()
        for request in ("../escape", "", "a" * 65, None, 12):
            with self.subTest(request=request), self.assertRaises(recipes.RecipeError):
                self.apply(plan, request=request)
        with self.assertRaises(recipes.RecipeError):
            self.apply({"plan_sha256": "unreviewed"})
        self.assertEqual(self.published, [])

    def test_project_scope_and_build_invalidation_precede_scene_writes(self):
        plan = self.session.plan(self.rows, "collection:1", "project-one")
        events = []
        with self.assertRaises(recipes.RecipeError):
            self.session.apply(plan["plan_sha256"], "one", self.rows,
                               lambda *args: events.append("write"), self.published.append,
                               scope="project-two", before_apply=lambda: events.append("invalidate"))
        self.assertEqual(events, [])
        self.session.apply(plan["plan_sha256"], "one", self.rows,
                           lambda *args: events.append("write"), self.published.append,
                           scope="project-one", before_apply=lambda: events.append("invalidate"))
        self.assertEqual(events, ["invalidate", "write", "write"])

    def test_unmarked_ambiguous_read_only_and_large_repairs_are_refused(self):
        for mode in ("unmarked", "ambiguous", "read-only", "nontext", "large"):
            with self.subTest(mode=mode):
                self.setUp()
                if mode == "unmarked":
                    self.rows[0]["identity"] = None
                elif mode == "ambiguous":
                    self.rows.append(row("collection", 9, "asset.prop", False))
                elif mode == "read-only":
                    self.rows[3]["editable"] = False
                elif mode == "nontext":
                    self.rows[3]["identity"] = 12
                else:
                    self.rows.extend(row("object", 100 + i, None) for i in range(25))
                with self.assertRaises(recipes.RecipeError):
                    self.plan()


if __name__ == "__main__":
    unittest.main()

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

    def test_plan_publication_precedes_cache_and_cannot_mutate_review(self):
        published = []
        before = copy.deepcopy(self.rows)

        def publish(value):
            self.assertNotIn(value["plan_sha256"], self.session.plans)
            published.append(copy.deepcopy(value))
            value["changes"][0]["after"] = "changed.by.publisher"

        plan = self.session.plan(self.rows, "collection:1", publish=publish)
        self.assertEqual(published, [plan])
        self.assertEqual(self.rows, before)
        self.assertEqual(self.apply(plan)["changes"], plan["changes"])

    def test_plan_publication_failure_preserves_existing_review_and_session(self):
        # An empty review has the same digest on repeated planning.
        self.rows = [row("collection", 1, "asset.prop")]
        existing = self.plan()
        pending = copy.deepcopy(self.session.plans)

        def failure(_):
            raise OSError("publication refused")

        for rows in (self.rows, self.rows + [row("mesh", 2, None)]):
            with self.subTest(rows=rows):
                with self.assertRaisesRegex(OSError, "publication refused"):
                    self.session.plan(rows, "collection:1", publish=failure)
                self.assertEqual(self.session.plans, pending)
                self.assertEqual(self.session.revision, 0)
                self.assertEqual(self.session.receipts, {})
        self.assertEqual(self.apply(existing)["changes"], [])

    def test_every_snapshot_field_and_missing_record_refuse_before_callbacks(self):
        changes = {"uid": 99, "handle": "mesh:99", "kind": "material", "prefix": "other",
                   "label": "renamed", "key": "other.key", "identity": "mesh.new",
                   "target": False, "editable": False, "shared_users": ["Object:new"],
                   "parent": 19, "data": 20, "collections": [1, 2], "objects": [2, 3]}
        for field, value in list(changes.items()) + [("removed", None), ("document", None),
                                                     ("revision", None)]:
            with self.subTest(field=field):
                self.setUp()
                plan = self.plan()
                if field == "removed":
                    self.rows.pop()
                elif field == "document":
                    self.session.document = "other-document"
                elif field == "revision":
                    self.session.revision += 1
                else:
                    self.rows[3][field] = value
                events = []
                with self.assertRaises(recipes.RecipeError):
                    self.session.apply(plan["plan_sha256"], "refused", self.rows,
                                       lambda *_: events.append("write"),
                                       lambda _: events.append("publish"),
                                       before_apply=lambda: events.append("invalidate"))
                self.assertEqual(events, [])
                self.assertEqual(self.session.receipts, {})

    def test_failure_at_each_apply_stage_restores_preimage_in_reverse_order(self):
        for stage in ("invalidate", "write-first", "write-second", "publish"):
            for partial in (False, True):
                with self.subTest(stage=stage, partial=partial):
                    self.setUp()
                    plan = self.plan()
                    before, events, writes = dict(self.values), [], []
                    failed_index = {"write-first": 1, "write-second": 2}.get(stage)

                    def invalidate():
                        events.append("invalidate")
                        if stage == "invalidate":
                            raise OSError("build invalidation failed")

                    def write(handle, identity):
                        writes.append((handle, identity))
                        if len(writes) == failed_index:
                            if partial:
                                self.values[handle] = identity
                            raise RuntimeError("property write failed")
                        self.values[handle] = identity

                    def publish(value):
                        events.append(value["status"])
                        if stage == "publish" and value["status"] == "applied":
                            raise OSError("receipt publication failed")
                        self.published.append(value)

                    with self.assertRaises(recipes.RecipeError) as error:
                        self.session.apply(plan["plan_sha256"], "request_1", self.rows,
                                           write, publish, before_apply=invalidate)
                    self.assertEqual(error.exception.rule, "recipe.failed")
                    self.assertEqual(self.values, before)
                    attempted = 0 if stage == "invalidate" else failed_index or len(plan["changes"])
                    self.assertEqual(writes[attempted:], [(change["handle"], change["before"])
                                                         for change in reversed(plan["changes"][:attempted])])
                    self.assertEqual(events[0], "invalidate")
                    self.assertEqual(events[-1], "failed")
                    self.assertEqual(self.session.revision, 1)
                    self.assertEqual(self.session.plans, {})
                    self.assertEqual(self.apply(plan), self.published[-1])

    def test_rollback_continues_after_each_restore_failure_and_reports_all_errors(self):
        for failing in ({"mesh:4"}, {"object:3"}, {"mesh:4", "object:3"}):
            with self.subTest(failing=failing):
                self.setUp()
                plan = self.plan()
                attempts = []

                def write(handle, value):
                    attempts.append((handle, value))
                    if len(attempts) > len(plan["changes"]) and handle in failing:
                        raise RuntimeError("cannot restore " + handle)
                    self.values[handle] = value

                def publish(value):
                    if value["status"] == "applied":
                        raise OSError("receipt unavailable")
                    self.published.append(value)

                with self.assertRaises(recipes.RecipeError) as error:
                    self.apply(plan, write=write, publish=publish)
                self.assertEqual(error.exception.rule, "recipe.rollback")
                receipt = self.published[-1]
                self.assertEqual(receipt["status"], "rollback_failed")
                self.assertEqual(receipt["rollback_errors"], ["cannot restore " + change["handle"]
                                 for change in reversed(plan["changes"]) if change["handle"] in failing])
                self.assertEqual(len(attempts), 2 * len(plan["changes"]))
                for change in plan["changes"]:
                    expected = change["after"] if change["handle"] in failing else change["before"]
                    self.assertEqual(self.values[change["handle"]], expected)
                self.assertEqual(self.apply(plan), receipt)
                self.assertEqual(self.session.plans, {})

    def test_receipt_copies_and_record_order_do_not_change_stored_result(self):
        plan = self.plan()
        self.rows.reverse()
        result = self.apply(plan)
        expected = copy.deepcopy(result)
        result["changes"].clear()
        self.published[0]["status"] = "changed"
        replay = self.apply(plan)
        self.assertEqual(replay, expected)
        replay["changes"].clear()
        self.assertEqual(self.apply(plan), expected)

    def test_pending_and_terminal_limits_refuse_without_losing_replay(self):
        for _ in range(32):
            self.plan()
        with self.assertRaises(recipes.RecipeError) as error:
            self.plan()
        self.assertEqual(error.exception.rule, "recipe.limit")
        self.assertEqual(len(self.session.plans), 32)
        self.session.changed()
        for index in range(256):
            plan = self.plan()
            receipt = self.apply(plan, request=str(index))
        self.assertEqual(self.apply(plan, request="255"), receipt)
        plan = self.plan()
        before, published = dict(self.values), copy.deepcopy(self.published)
        with self.assertRaises(recipes.RecipeError) as error:
            self.apply(plan, request="overflow")
        self.assertEqual(error.exception.rule, "recipe.limit")
        self.assertEqual(self.values, before)
        self.assertEqual(self.published, published)


if __name__ == "__main__":
    unittest.main()

"""Bounded, reviewed identity repairs. No Blender API or executable input."""
from copy import deepcopy
import hashlib
import json
import re
import uuid


RECIPE = "authoring.asset.refresh_ids.v1"
DEFINITION = {"id": RECIPE, "version": 1, "parameters": {},
              "max_changes": 24, "max_records": 100000,
              "policy": "preserve-oldest-owner;explicit-shared-users;atomic-ids-only"}
IDENTIFIER = re.compile(r"[A-Za-z][A-Za-z0-9_.:-]{0,127}\Z")
REQUEST_ID = re.compile(r"[A-Za-z0-9_-]{1,64}\Z")


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


DEFINITION_DIGEST = digest(DEFINITION)


class RecipeError(ValueError):
    def __init__(self, rule, message):
        super().__init__(message)
        self.rule = rule


def checked_records(records):
    if len(records) > DEFINITION["max_records"]:
        raise RecipeError("recipe.limit", "Too many datablocks for the bounded ID recipe")
    result = sorted(deepcopy(records), key=lambda row: (row["kind"], row["uid"]))
    handles = set()
    for row in result:
        if row["handle"] in handles:
            raise RecipeError("recipe.identity", "Duplicate datablock handle")
        handles.add(row["handle"])
        if row["identity"] is not None and not isinstance(row["identity"], str):
            raise RecipeError("recipe.identity", "Repair non-text identity properties manually first")
        if row["target"] and not row["editable"]:
            raise RecipeError("recipe.read_only", "Linked or overridden datablocks cannot be repaired")
    return result


class RecipeSession:
    """Concurrency state lives outside Blender undo and changes on document load."""

    def __init__(self):
        self.document = uuid.uuid4().hex
        self.revision = 0
        self.plans = {}
        self.receipts = {}

    def changed(self):
        self.revision += 1
        self.plans.clear()

    def inspect(self, records):
        rows = checked_records(records)
        return {"schema": "luminumbra.authoring.inspection.v1", "document": self.document,
                "revision": self.revision, "recipe": deepcopy(DEFINITION),
                "definition_sha256": DEFINITION_DIGEST,
                "records": [row for row in rows if row["target"]]}

    def plan(self, records, target_handle, scope=""):
        rows = checked_records(records)
        target = next((row for row in rows if row["handle"] == target_handle), None)
        if (target is None or target["kind"] != "collection" or not target["target"]
                or not target["identity"] or not IDENTIFIER.fullmatch(target["identity"])):
            raise RecipeError("recipe.target", "Choose an already marked asset collection")
        if sum(row["kind"] == "collection" and row["identity"] == target["identity"]
               for row in rows) != 1:
            raise RecipeError("recipe.target", "The target asset ID is ambiguous; mark it manually first")
        owners, occupied, changes = {}, set(), []
        for row in rows:
            identity = row["identity"]
            if identity is not None:
                owners.setdefault((row["kind"], identity), row["handle"])
                occupied.add(identity)
        for row in rows:
            if not row["target"]:
                continue
            old = row["identity"]
            if old and IDENTIFIER.fullmatch(old) and owners.get((row["kind"], old)) == row["handle"]:
                continue
            new = row["prefix"] + "." + uuid.uuid4().hex
            while new in occupied:
                new = row["prefix"] + "." + uuid.uuid4().hex
            occupied.add(new)
            changes.append({"handle": row["handle"], "kind": row["kind"], "label": row["label"],
                            "key": row["key"], "before": old, "after": new,
                            "shared_users": row["shared_users"]})
        if len(changes) > DEFINITION["max_changes"]:
            raise RecipeError("recipe.limit", "More than 24 ID changes; use the manual marking workflow")
        if len(self.plans) >= 32:
            raise RecipeError("recipe.limit", "Too many pending plans; edit or reload the document")
        plan = {"schema": "luminumbra.authoring.recipe.plan.v1", "recipe": RECIPE,
                "version": 1, "definition_sha256": DEFINITION_DIGEST, "parameters": {},
                "document": self.document, "revision": self.revision,
                "scope": scope,
                "target_asset_id": target["identity"], "target_handle": target_handle,
                "snapshot_sha256": digest(rows), "changes": changes}
        identity = digest(plan)
        self.plans[identity] = deepcopy(plan)
        return {"plan_sha256": identity, **deepcopy(plan)}

    def apply(self, plan_sha256, request_id, records, write, publish, *, scope="", before_apply=None):
        """Apply a stored reviewed plan; write(handle, value), publish(receipt).

        publish must commit atomically or raise before commit. Every attempted
        property write is rolled back if mutation or receipt publication fails.
        Both callbacks run synchronously on Blender's main thread.
        """
        if not isinstance(request_id, str) or not REQUEST_ID.fullmatch(request_id):
            raise RecipeError("recipe.request", "Invalid recipe request ID")
        if request_id in self.receipts:
            previous = self.receipts[request_id]
            if previous["plan_sha256"] != plan_sha256:
                raise RecipeError("recipe.request", "Request ID already belongs to a different plan")
            return deepcopy(previous)
        if len(self.receipts) >= 256:
            raise RecipeError("recipe.limit", "Recipe session is full; reload before further automation")
        plan = self.plans.get(plan_sha256)
        if plan is None or plan["document"] != self.document or plan["revision"] != self.revision:
            raise RecipeError("recipe.stale", "The scene changed; review a new ID repair plan")
        rows = checked_records(records)
        if digest(rows) != plan["snapshot_sha256"]:
            raise RecipeError("recipe.stale", "Datablock identities or dependencies changed; review again")
        if scope != plan["scope"]:
            raise RecipeError("recipe.stale", "The project changed; review a new ID repair plan")
        before_revision = self.revision
        receipt = {"schema": "luminumbra.authoring.recipe.receipt.v1", "recipe": RECIPE,
                   "definition_sha256": DEFINITION_DIGEST, "request_id": request_id,
                   "plan_sha256": plan_sha256, "document": self.document,
                   "before_revision": before_revision, "after_revision": before_revision + 1,
                   "target_asset_id": plan["target_asset_id"], "changes": deepcopy(plan["changes"]),
                   "status": "applied", "undo_operation": "luminumbra.review_id_repairs"}
        attempted = []
        try:
            if before_apply:
                before_apply()
            for change in plan["changes"]:
                attempted.append(change)
                write(change["handle"], change["after"])
            publish(deepcopy(receipt))
        except Exception as error:
            rollback_errors = []
            for change in reversed(attempted):
                try:
                    write(change["handle"], change["before"])
                except Exception as rollback_error:
                    rollback_errors.append(str(rollback_error))
            receipt["status"] = "rollback_failed" if rollback_errors else "failed"
            receipt["message"] = str(error)
            receipt["rollback_errors"] = rollback_errors
            self.receipts[request_id] = receipt
            self.changed()
            try:
                publish(deepcopy(receipt))
            except Exception:
                pass  # Still terminal in memory; a new document gets a new identity.
            rule = "recipe.rollback" if rollback_errors else "recipe.failed"
            raise RecipeError(rule, "ID repair failed: " + str(error)) from error
        self.receipts[request_id] = receipt
        self.changed()
        return deepcopy(receipt)

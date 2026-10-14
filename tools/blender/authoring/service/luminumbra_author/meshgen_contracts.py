"""Shared backend-only request, candidate, receipt and provenance definitions.

This module validates data only. Storage transactions, policy computation and
worker execution belong to their respective owners. Parent-authored evidence
cannot be replaced with assertions supplied by a worker or request.
"""
from __future__ import annotations

from copy import deepcopy
from pathlib import PurePosixPath
import re

from .contracts import canonical, digest, require
from .meshgen_config import canonical_relative, inert_json
from .meshgen_registry import (ID_SCHEMA, SHA_SCHEMA, STATUSES, WAIVABLE_GATES,
    BOOL, array, enum, number, obj, string, validate_schema, overrides_schema)

PROVENANCE = "luminumbra.authoring.candidate.provenance.v2"
CANDIDATE = "luminumbra.authoring.meshgen.candidate.v1"
RECEIPT = "luminumbra.authoring.meshgen.receipt.v1"
REQUEST = "luminumbra.authoring.meshgen.request.v1"
BACKEND = "luminumbra.meshgen.backend.v1"
RETRYABLE_RULES = ("busy", "backend.unavailable", "stale.plan")
P1_FEATURES = {"backend": True, "ops": False, "raw_bpy": False, "colliders": False,
               "lod": False, "texture": False, "rig": False, "animation": False}
JOB_STATES = ("queued", "running", "finishing", "succeeded", "failed", "cancelled")
# Mutable job/intent/prepared-receipt state belongs beside candidates, never in
# a sealed bundle. Publication order: prepared receipt -> candidate marker ->
# receipt -> receipt hash. Neither receipt nor manifest contains its own hash.
STATE_ROOT = ".luminumbra-author-recipes"
JOB_AREA = "jobs"
CANDIDATE_AREA = "candidates"
MANIFEST_FILE = "manifest.json"
COMMIT_FILE = "candidate.sha256"
RECEIPT_FILE = "receipt.json"
RECEIPT_COMMIT_FILE = "receipt.sha256"
PREPARED_RECEIPT_FILE = "receipt.prepared.json"
SEALED_REQUIRED = frozenset(("candidate.glb", "preview.png", "provenance.json", "stats.json",
    "effective_config.json", "licence_gate.json", "inputs/reference_image", "inputs/raw_backend.glb",
    "inputs/capabilities.json", "inputs/result.json", "inputs/backend.log"))
SEALED_OPTIONAL = frozenset(("inputs/prompt.txt", "inputs/finish.log"))

TEXT = string(maximum=4096)
UINT = number(0, 2**32-1, True)
COUNT = number(0, 16000000, True)
HASH_MAP = obj(dynamic=SHA_SCHEMA, maximum=65536)
JSON_PARAMS = obj(dynamic={}, maximum=128)
BOUNDS = array(number(0, 1000), 3, 3)
FINDING = obj({"rule_id": string(), "severity": enum("error", "warning", "info"), "message": TEXT,
    "field": TEXT, "object_id": {"oneOf": [TEXT, {"type": "null"}]},
    "component_id": {"oneOf": [TEXT, {"type": "null"}]},
    "node_id": {"oneOf": [TEXT, {"type": "null"}]}},
    ("rule_id", "severity", "message", "field", "object_id", "component_id", "node_id"))
ACCEPTANCE_SCHEMA = obj({"human_initiated": {"type": "boolean", "const": True},
    "accepted_by": string(maximum=256), "accepted_at": string(maximum=64),
    "gates": array(enum(*WAIVABLE_GATES), 5, 1)},
    ("human_initiated", "accepted_by", "accepted_at", "gates"))
REQUEST_SCHEMA = obj({"schema": {"type": "string", "const": REQUEST},
    "kind": enum("backend"), "asset_class": ID_SCHEMA, "backend_id": ID_SCHEMA,
    "image_ref": obj({"path": TEXT, "sha256": SHA_SCHEMA}, ("path", "sha256")),
    "declared_source": string(maximum=2048), "prompt": string(maximum=1024), "seed": UINT,
    "params": JSON_PARAMS, "overrides": overrides_schema(),
    "request_id": string(r"generate:[A-Za-z0-9_.:-]{1,119}", 128),
    "accept_flagged": ACCEPTANCE_SCHEMA},
    ("kind", "asset_class", "backend_id", "image_ref", "declared_source", "seed", "request_id"))
MANIFEST_SCHEMA = obj({"schema": {"type": "string", "const": CANDIDATE},
    "members": obj({name: SHA_SCHEMA for name in sorted(SEALED_REQUIRED | SEALED_OPTIONAL)},
                   sorted(SEALED_REQUIRED))}, ("schema", "members"))
COMPONENT = obj({"id": TEXT, "text_sha256": SHA_SCHEMA, "clauses": array(string(), 128), "executed": BOOL},
                ("id", "text_sha256", "clauses", "executed"))
LICENCE = obj({"status": enum(*STATUSES), "shippable": BOOL, "components": array(COMPONENT, 256),
    "dataset_chain_audited": BOOL, "ack_ids": array(TEXT, 256), "sha256": SHA_SCHEMA},
    ("status", "shippable", "components", "dataset_chain_audited", "ack_ids", "sha256"))
FLAGS = obj({"shippable": BOOL, "origin": string(r"backend:[A-Za-z][A-Za-z0-9_-]{0,63}|ops:typed|ops:raw_bpy", 80),
    "waived": array(enum(*WAIVABLE_GATES), 5), "game-repo-only": {"type": "boolean", "const": True},
    "no-training-on-outputs": {"type": "boolean", "const": True}},
    ("shippable", "origin", "waived", "game-repo-only", "no-training-on-outputs"))
ARTIFACT = obj({"sha256": SHA_SCHEMA, "triangles": COUNT, "vertices": COUNT, "bodies": COUNT,
    "watertight": BOOL, "has_uv": BOOL, "has_normals": BOOL, "bounds_m": BOUNDS, "budget_ok": BOOL},
    ("sha256", "triangles", "vertices", "bodies", "watertight", "has_uv", "has_normals", "bounds_m", "budget_ok"))
OBSERVATION = {"oneOf": [TEXT, {"type": "null"}]}
ENVIRONMENT = obj({"os": TEXT, "blender_version": TEXT, "exporter_sha256": SHA_SCHEMA,
    "gpu_name": OBSERVATION, "driver": OBSERVATION, "cuda": OBSERVATION, "torch": OBSERVATION,
    "dtype": OBSERVATION, "batch_size": {"oneOf": [number(1, 1024, True), {"type": "null"}]},
    "python_version": TEXT, "peak_vram_mib": {"oneOf": [number(0, 2**20), {"type": "null"}]}},
    ("os", "blender_version", "exporter_sha256", "gpu_name", "driver", "cuda", "torch", "dtype",
     "batch_size", "python_version", "peak_vram_mib"))
STEP_OUTCOME = obj({"id": TEXT, "params": JSON_PARAMS, "outcome": TEXT, "stats": JSON_PARAMS},
                   ("id", "params", "outcome", "stats"))
FINISH = obj({"blender_version": TEXT, "exporter_sha256": SHA_SCHEMA,
    "steps": array(STEP_OUTCOME, 11), "tool_versions": obj(dynamic=TEXT, maximum=64)},
    ("blender_version", "exporter_sha256", "steps", "tool_versions"))
PROVENANCE_SCHEMA = obj({"schema": {"type": "string", "const": PROVENANCE},
    # Inherited identity, request and raw output fields remain required in v2.
    "recipe": TEXT, "definition_sha256": SHA_SCHEMA, "plan_sha256": SHA_SCHEMA,
    "request_id": REQUEST_SCHEMA["properties"]["request_id"], "document_session": TEXT,
    "model": obj({"name": TEXT, "repo": TEXT, "revision": TEXT,
        "weights_sha256": array(SHA_SCHEMA, 256), "precision": OBSERVATION, "code_commit": TEXT,
        "backend_reported": {"type": "boolean", "const": True}},
        ("name", "repo", "revision", "weights_sha256", "precision", "code_commit", "backend_reported")),
    "license": obj({"id": TEXT, "text_sha256": SHA_SCHEMA, "url": TEXT,
        "territory_restricted": BOOL, "user_acknowledgment": BOOL},
        ("id", "text_sha256", "url", "territory_restricted", "user_acknowledgment")),
    "input": obj({"image_sha256": SHA_SCHEMA, "prompt_sha256": SHA_SCHEMA,
        "preprocessing_model": OBSERVATION, "preprocessing_license": OBSERVATION},
        ("image_sha256", "prompt_sha256", "preprocessing_model", "preprocessing_license")),
    "params": JSON_PARAMS,
    "raw_output": obj({"sha256": SHA_SCHEMA, "bytes": number(1, 256*1024**2, True),
        "triangles": COUNT, "vertices": COUNT}, ("sha256", "bytes", "triangles", "vertices")),
    "finishing": FINISH,
    "config": obj({"config_digest": SHA_SCHEMA, "config_schema_version": {"type": "integer", "const": 1},
        "profile": ID_SCHEMA, "sources": obj(dynamic=enum("L0", "L1", "L2", "L3", "L4"), maximum=4096),
        "effective_config_sha256": SHA_SCHEMA},
        ("config_digest", "config_schema_version", "profile", "sources", "effective_config_sha256")),
    "licence_gate": LICENCE,
    "backend": obj({"id": ID_SCHEMA, "version": TEXT, "protocol": number(1, 1, True),
        "adapter_commit": TEXT, "capability_digest": SHA_SCHEMA, "pins_matched": BOOL},
        ("id", "version", "protocol", "adapter_commit", "capability_digest", "pins_matched")),
    "environment": ENVIRONMENT,
    "request": obj({"image_sha256": SHA_SCHEMA, "declared_source": string(maximum=2048),
        "prompt_digest": SHA_SCHEMA, "prompt_text": string(maximum=1024), "seed": UINT,
        "params": JSON_PARAMS, "max_tris_hint": number(64, 1000000, True), "inputs_retained": BOOL},
        ("image_sha256", "declared_source", "prompt_digest", "seed", "params", "max_tris_hint", "inputs_retained")),
    "finish": FINISH,
    "metrics": obj({"stats_sha256": SHA_SCHEMA}, ("stats_sha256",)),
    "artifact": ARTIFACT, "flags": FLAGS},
    ("schema", "recipe", "definition_sha256", "plan_sha256", "request_id", "document_session", "model",
     "license", "input", "params", "raw_output", "finishing", "config", "licence_gate", "backend", "environment",
     "request", "finish", "metrics", "artifact", "flags"))
RECEIPT_SCHEMA = obj({"schema": {"type": "string", "const": RECEIPT},
    "request_id": string(r"(?:generate|import):[A-Za-z0-9_.:-]{1,119}", 128),
    "plan_digest": SHA_SCHEMA, "config_digest": SHA_SCHEMA, "tier": enum("backend"),
    "origin": FLAGS["properties"]["origin"], "backend": ID_SCHEMA,
    "licence": obj({"status": enum(*STATUSES), "shippable": BOOL, "ack_ids": array(TEXT)},
                   ("status", "shippable", "ack_ids")),
    "candidate": obj({"dir": TEXT, "sha256": SHA_SCHEMA, "artifact_sha256": SHA_SCHEMA,
        "provenance_sha256": SHA_SCHEMA, "tris": COUNT, "vertices": COUNT, "bodies": COUNT,
        "watertight": BOOL, "bounds_m": BOUNDS, "budget_ok": BOOL},
        ("dir", "sha256", "artifact_sha256", "provenance_sha256", "tris", "vertices", "bodies", "watertight", "bounds_m", "budget_ok")),
    "flags": obj({"waived": array(enum(*WAIVABLE_GATES), 5)}, ("waived",)),
    "gates": obj(dynamic=enum("pass", "fail", "na"), maximum=32), "executed": BOOL, "spatially_valid": BOOL,
    "timings": obj(dynamic=number(0, 86400), maximum=32), "errors": array(FINDING, 256)},
    ("schema", "request_id", "plan_digest", "config_digest", "tier", "origin", "backend", "licence", "candidate",
     "flags", "gates", "executed", "spatially_valid", "timings", "errors"))


def validate_request(value, effective=None):
    require(isinstance(value, dict) and value.get("kind") == "backend", "operation.unsupported",
            "P1 accepts backend requests only; ops and scripts are unavailable.")
    inert_json(value)
    validate_schema(value, REQUEST_SCHEMA)
    from .meshgen_registry import config_schema
    validate_schema(value.get("overrides", {}), config_schema(), partial=True)
    canonical_relative(value["image_ref"]["path"])
    source = value["declared_source"]
    require(source == "own_work" or re.fullmatch(r"cc0:https?://\S+|licensed:[A-Za-z0-9_.:-]+", source),
            "request.invalid", "Declare own_work, cc0:<URL>, or licensed:<id>.", "declared_source")
    if "accept_flagged" in value:
        validate_acceptance(value["accept_flagged"])
    if effective is not None:
        require(value["backend_id"] == effective["backend"]["id"], "request.invalid",
                "Request backend must match the resolved configuration.", "backend_id")
        require("target_height_m" in effective["budgets"], "config.type",
                "A generation plan requires a positive target height.", "budgets.target_height_m")
        require(len(value.get("prompt", "")) <= effective["limits"]["prompt_chars"],
                "request.limit", "Prompt exceeds the resolved limit.", "prompt")
        if effective["candidates"]["seed_policy"] == "fixed":
            candidate_seeds(value["seed"], effective["candidates"]["count"])
    return value


def validate_acceptance(value):
    validate_schema(value, ACCEPTANCE_SCHEMA)
    require(bool(value["accepted_by"].strip()) and bool(value["accepted_at"].strip())
            and len(set(value["gates"])) == len(value["gates"]),
            "request.invalid", "Human acceptance must identify the reviewer, time and distinct gates.")
    return value


def accepted_waivers(failed_gates, effective, acceptance=None):
    """Closed waiver contract; on_over_budget alone grants no acceptance."""
    failed = set(failed_gates)
    require(failed <= set(WAIVABLE_GATES), "finish.gate_failed", "An unconditional gate cannot be waived.")
    if not failed:
        return []
    require(effective["policy"]["validation_profile"] == "internal" and acceptance is not None,
            "finish.gate_failed", "Waivers need internal policy and a new human-reviewed plan.")
    validate_acceptance(acceptance)
    require(failed <= set(effective["evaluation"]["waivable"]) and failed <= set(acceptance["gates"]),
            "finish.gate_failed", "Every failed gate needs committed permission and explicit human acceptance.")
    return sorted(failed)


def candidate_seeds(seed, count):
    validate_schema(seed, UINT)
    validate_schema(count, number(1, 4, True))
    require(seed + count - 1 <= 2**32 - 1, "request.invalid", "Fixed candidate seeds overflow; refuse instead of wrapping.")
    return list(range(seed, seed + count))


def validate_backend_params(params, schema):
    """Call only with the capability schema whose entire document is pinned."""
    from .meshgen_registry import check_schema
    if "type" not in schema and "$ref" not in schema and "oneOf" not in schema:
        schema = obj(schema)
    check_schema(schema)
    inert_json(params)
    return validate_schema(params, schema)


def validate_manifest(value):
    validate_schema(value, MANIFEST_SCHEMA)
    members = value["members"]
    require(SEALED_REQUIRED <= members.keys() and members.keys() <= SEALED_REQUIRED | SEALED_OPTIONAL,
            "candidate.members", "Manifest must cover exactly the registered sealed members.")
    for name in members:
        canonical_relative(name)
    return value


def bundle_identity(manifest_bytes):
    """Marker identity hashes actual manifest bytes, independently of artifact."""
    from .contracts import parse_json
    validate_manifest(parse_json(manifest_bytes))
    return digest(manifest_bytes)


def validate_provenance(value):
    inert_json(value)
    validate_schema(value, PROVENANCE_SCHEMA)
    flags = value["flags"]
    gate = value["licence_gate"]
    require(value["finishing"] == value["finish"] and value["params"] == value["request"]["params"]
            and value["input"]["image_sha256"] == value["request"]["image_sha256"]
            and value["input"]["prompt_sha256"] == value["request"]["prompt_digest"],
            "provenance.inconsistent", "Inherited and v2 evidence must agree.")
    require(flags["origin"] == "backend:" + value["backend"]["id"],
            "provenance.inconsistent", "Origin must match the recorded backend.")
    require(not gate["shippable"] or gate["status"] == "shippable", "provenance.inconsistent", "Licence result is inconsistent.")
    require(not flags["shippable"] or (gate["shippable"] and not flags["waived"]),
            "provenance.inconsistent", "Candidate eligibility requires licence eligibility and no waivers.")
    if value["backend"]["id"] == "mock":
        require(not flags["shippable"] and all(value["environment"][key] is None for key in
                ("gpu_name", "driver", "cuda", "torch", "dtype", "batch_size", "peak_vram_mib")),
                "provenance.mock", "Mock candidates are non-shippable and GPU evidence is unavailable.")
    return value


def validate_receipt(value):
    inert_json(value)
    validate_schema(value, RECEIPT_SCHEMA)
    canonical_relative(value["candidate"]["dir"])
    parts = PurePosixPath(value["candidate"]["dir"]).parts
    require(len(parts) == 4 and parts[0] == STATE_ROOT and parts[2] == CANDIDATE_AREA,
            "candidate.path", "Receipt must name a per-document candidate directory.")
    require(value["origin"] == "backend:" + value["backend"], "provenance.inconsistent", "Receipt backend and origin differ.")
    return value


def packaged_schemas():
    from .meshgen_registry import config_schema
    return {"meshgen.config.v1.schema.json": config_schema(),
            "meshgen.request.v1.schema.json": deepcopy(REQUEST_SCHEMA),
            "meshgen.candidate.v1.schema.json": deepcopy(MANIFEST_SCHEMA),
            "meshgen.provenance.v2.schema.json": deepcopy(PROVENANCE_SCHEMA),
            "meshgen.receipt.v1.schema.json": deepcopy(RECEIPT_SCHEMA)}

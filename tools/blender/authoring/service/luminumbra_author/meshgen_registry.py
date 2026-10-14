"""Authoritative stdlib configuration registry and its generated editor schema.

Dynamic maps have a declared value schema; only backend parameters are deferred
until a capability schema is available. No calibration thresholds are invented:
``evaluation.gates`` is closed and empty until thresholds are registered.
"""
from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import math
import re

from .contracts import Refusal, require

CONFIG = "luminumbra.authoring.meshgen.config.v1"
VERSION = 1
STEPS = ("validate_input", "weld", "degenerate", "loose_pieces", "holes",
         "decimate", "normals", "scale_pivot", "uv", "material", "stats")
MANDATORY_STEPS = frozenset(STEPS) - {"loose_pieces", "holes", "uv", "material"}
WAIVABLE_GATES = ("final_tris_within_budget", "bounds_within_budgets",
                  "thin_shell_guard", "uv_present", "uv_overlap_zero")
STATUSES = ("shippable", "internal_ok", "internal_only", "counsel", "blocked")
NAME_PATTERN = r"[A-Za-z][A-Za-z0-9_-]{0,63}"
SHA_PATTERN = r"[0-9a-f]{64}"
MISSING = object()


def obj(properties=None, required=(), dynamic=False, maximum=256):
    return {"type": "object", "properties": properties or {},
            "additionalProperties": dynamic, "required": list(required),
            "maxProperties": maximum}


def string(pattern=None, maximum=1024):
    result = {"type": "string", "maxLength": maximum}
    if pattern is not None:
        result["pattern"] = "^(?:" + pattern + ")$"
    return result


def enum(*values):
    return {"type": "string", "enum": list(values)}


def number(low, high, integer=False):
    return {"type": "integer" if integer else "number", "minimum": low, "maximum": high}


def array(items, maximum=256, minimum=0):
    return {"type": "array", "items": items, "minItems": minimum, "maxItems": maximum}


BOOL = {"type": "boolean"}
SHA_SCHEMA = string(SHA_PATTERN, 64)
ID_SCHEMA = string(NAME_PATTERN, 64)
VECTOR = array(number(0.001, 1000), 3, 3)
TOKEN = {"oneOf": [obj({"env": string(r"[A-Za-z_][A-Za-z0-9_]*", 128)}, ("env",)),
                   obj({"file": string(maximum=4096)}, ("file",))]}
ATTESTATION = {"oneOf": [obj({"value": {"type": "boolean", "const": False}}, ("value",)),
    obj({"value": {"type": "boolean", "const": True}, "attested_by": string(),
         "attested_at": string(), "note_id": ID_SCHEMA, "note_sha256": SHA_SCHEMA},
        ("value", "attested_by", "attested_at", "note_id", "note_sha256"))]}
ACK = obj({"licence_id": string(), "text_sha256": SHA_SCHEMA,
           "acknowledged_by": string(), "acknowledged_at": string(),
           "scope": enum("internal_only")},
          ("licence_id", "text_sha256", "acknowledged_by", "acknowledged_at", "scope"))
COUNSEL = obj({"counsel_note_id": ID_SCHEMA, "covers_licence_ids": array(string(), 128, 1),
               "text_sha256": SHA_SCHEMA, "recorded_by": string(), "recorded_at": string(),
               "outcome": enum("cleared_internal")},
              ("counsel_note_id", "covers_licence_ids", "text_sha256", "recorded_by", "recorded_at", "outcome"))
WAIVER = obj({"backend_id": ID_SCHEMA, "reason": string(), "recorded_by": string(),
              "recorded_at": string()}, ("backend_id", "reason", "recorded_by", "recorded_at"))
PINS = {"oneOf": [obj(), obj({"model_revision": string(r"[0-9a-f]{40}", 40),
                             "weights_sha256": array(SHA_SCHEMA, 256, 1)},
                            ("model_revision", "weights_sha256"))]}
# JSON values are bounded separately. This is the sole deliberately opaque map:
# the broker MUST validate it against the pinned capability parameter schema.
PARAMS = obj(dynamic={}, maximum=128)


@dataclass(frozen=True)
class Field:
    path: str
    schema: dict
    default: object = MISSING
    override: str = "none"
    tighten: str = "lower"
    local: bool = False
    immutable: bool = False


FIELDS = {}


def register(path, schema, default=MISSING, override="none", tighten="lower",
             local=False, immutable=False):
    FIELDS[path] = Field(path, schema, default, override, tighten, local, immutable)


register("schema", {"type": "string", "const": CONFIG}, CONFIG, immutable=True)
register("config_schema_version", {"type": "integer", "const": VERSION}, VERSION, immutable=True)
register("backend.id", ID_SCHEMA, override="request")
register("backend.params", PARAMS, {}, "request")
register("policy.licence.allowed_status", array(enum(*STATUSES[:-1]), 4, 1), ["shippable"])
register("policy.licence.internal_acknowledgements", array(ACK), [])
register("policy.licence.counsel_notes", array(COUNSEL), [])
register("policy.licence.dataset_chain_waivers", array(WAIVER), [])
for key in ("revenue_under_cap", "no_ai_improvement", "registration_done"):
    register("policy.licence.attestations." + key, ATTESTATION, {"value": False})
register("policy.licence.reference_image_provenance",
         enum("refuse_if_ai_training_reserved", "refuse_unknown"), "refuse_if_ai_training_reserved")
register("policy.validation_profile", enum("internal", "shipping"), "internal")
register("policy.locks", array(string(), 256), ["policy.licence", "naming.collection_template",
         "backends.*.endpoint", "agent.tier_b.enabled", "finish.normals.repair_zero"])
register("policy.bounds", obj(dynamic=obj({"min": number(0, 2**40), "max": number(0, 2**40)},
                                         ("min", "max"))), {})
register("backends.*.endpoint", string(r"http://127\.0\.0\.1:[0-9]{2,5}", 64))
register("backends.*.token_source", TOKEN, local=True)
register("backends.*.kind", enum("model", "mock"), "model")
register("backends.*.enabled", BOOL, False, "local_lower_only", local=True)
register("backends.*.pins", PINS, {})
register("backends.*.params", PARAMS, {}, "request")
for name, default, low, high in (("connect_s", 5, 1, 30), ("job_s", 600, 10, 1800),
                                ("finish_s", 180, 10, 600)):
    register("timeouts." + name, number(low, high), default, "request_tighten")
    register("backends.*.timeouts." + name, number(low, high), override="request_tighten", local=True)
for step in STEPS:
    register("timeouts.step_s." + step, number(0.1, 600), 60, "request_tighten")
    register("backends.*.timeouts.step_s." + step, number(0.1, 600), override="request_tighten", local=True)
for name, default, low, high in (("max_extent_m", 200, 1, 1000),
        ("max_tris_final", 8000, 64, 1000000), ("max_tris_prefinish", 4000000, 10000, 4000000)):
    register("budgets." + name, number(low, high, name != "max_extent_m"), default, "request_tighten")
register("budgets.target_height_m", number(0.001, 1000), override="request")
register("budgets.bounds_m", VECTOR, [8.0, 8.0, 8.0], "request_tighten")
for name, ceiling in {"image_bytes": 16*1024**2, "prompt_chars": 1024, "result_bytes": 256*1024**2,
        "image_pixels": 16777216, "image_dimension": 16384, "metadata_bytes": 1024**2,
        "json_depth": 32, "collection_items": 65536, "glb_accessor_elements": 16000000,
        "request_bytes": 65536, "response_bytes": 1024**2, "log_bytes": 16384,
        "outstanding_jobs": 1, "plans": 32, "terminal_requests": 256}.items():
    register("limits." + name, number(1, ceiling, True), ceiling, "request_tighten")
register("finish.steps", array(enum(*STEPS), len(STEPS), len(MANDATORY_STEPS)), list(STEPS), "profile")
for path, default, low, high in (("weld.distance_rel", 1e-5, 0, 1e-3),
        ("degenerate.area_eps_rel", 1e-12, 0, 1e-6), ("loose.min_fraction", 0.01, 0, 0.5),
        ("decimate.hausdorff_max_rel", 0.01, 0.001, 0.1),
        ("decimate.open_sheet_min_retention", 0.8, 0.5, 1),
        ("decimate.thin_shell_p10_min_ratio", 0.5, 0.3, 1),
        ("decimate.min_ray_hit_rate", 0.5, 0.3, 1),
        ("normals.sharp_angle_deg", 40, 5, 85), ("uv.angle_limit_deg", 66, 1, 89),
        ("uv.island_margin", 0.003, 0, 0.05)):
    override = "request_tighten" if path in ("decimate.hausdorff_max_rel",
        "decimate.open_sheet_min_retention", "decimate.thin_shell_p10_min_ratio") else (
        "request" if path == "loose.min_fraction" else "profile")
    register("finish." + path, number(low, high), default, override,
             "higher" if path.endswith(("min_retention", "min_ratio")) else "lower")
for path, default, values, override in (
    ("loose.policy", "keep", ("keep", "drop_small", "keep_largest"), "request"),
    ("decimate.method", "blender_collapse", ("none", "blender_collapse", "meshopt_cli"), "profile"),
    ("decimate.on_over_budget", "refuse", ("refuse", "accept_flagged"), "profile"),
    ("normals.mode", "smooth_angle", ("smooth_angle", "smooth", "flat"), "profile"),
    ("scale.up_axis", "Y", ("Y",), "none"),
    ("scale.pivot", "base_center", ("base_center", "bbox_center", "origin"), "profile"),
    ("uv.method", "smart_project", ("none", "smart_project", "xatlas"), "profile"),
    ("material.mode", "neutral", ("none", "neutral"), "profile")):
    register("finish." + path, enum(*values), default, override)
register("finish.decimate.max_tris", number(64, 1000000, True), override="request_tighten")
register("finish.decimate.max_attempts", number(1, 8, True), 4, "profile")
register("finish.decimate.thin_shell_guard", BOOL, True, "profile")
register("finish.holes.max_edges", number(0, 64, True), 0, "profile")
register("finish.normals.repair_zero", {"type": "boolean", "const": True}, True, immutable=True)
register("finish.scale.scene_scale", {"type": "number", "const": 1.0}, 1.0, immutable=True)
register("finish.uv.max_charts", number(1, 8192, True), 512, "request_tighten")
register("candidates.count", number(1, 4, True), 1, "request")
register("candidates.seed_policy", enum("fixed", "random_recorded"), "fixed", "request")
register("candidates.select", enum("manual", "auto_by_score"), "manual", "request")
for metric in ("chamfer_l1", "chamfer_l2", "f_score", "silhouette_iou", "thin_shell_p10", "uv_stretch", "watertight"):
    register("candidates.score_weights." + metric, number(0, 1), override="request")
register("evaluation.gates", obj(), {})
register("evaluation.waivable", array(enum(*WAIVABLE_GATES), len(WAIVABLE_GATES)), [])
register("evaluation.judge.enabled", BOOL, False, "request")
register("agent.tier_a.enabled", BOOL, True)
register("agent.tier_b.enabled", BOOL, False)
for name, default, low, high in (("qualification_max_age_days", 180, 1, 365),
        ("max_processes", 64, 4, 256), ("scratch_mib", 256, 16, 2048), ("output_mib", 64, 1, 256)):
    register("agent.tier_b." + name, number(low, high, True), default, "profile")
for name, default, low, high, override in (("max_ops", 256, 1, 256, "request_tighten"),
        ("max_script_bytes", 65536, 1, 262144, "none"),
        ("max_voxel_cells", 8000000, 1000, 8000000, "request_tighten")):
    register("agent.ops." + name, number(low, high, True), default, override)
for name, cap in (("max_incomplete", 4), ("max_session_bytes", 8*1024**2), ("max_project_bytes", 32*1024**2)):
    register("agent.staging." + name, number(1, cap, True), cap)
register("workers.blender.path", string(maximum=4096), local=True)
for worker in ("ops", "finisher"):
    register("workers." + worker + ".python", string(maximum=4096), local=True)
    register("workers." + worker + ".memory_mib", number(512, 16384, True), 4096, "request_tighten")
    register("workers." + worker + ".cpu_threads", number(1, 2 if worker == "finisher" else 4, True), 2, "request_tighten")
register("verify.tolerance_rel", number(0.0001, 0.05), 0.005, "request_tighten")
register("naming.collection_template", string(maximum=63), "LUM_CANDIDATE.{job8}")
register("naming.object_template", string(maximum=63), "LUM_CAND.{job8}.{index:02d}")
for name, default, low, high in (("keep_last", 20, 1, 1000), ("max_total_mib", 4096, 64, 262144),
                                ("failed_days", 14, 1, 365)):
    register("retention." + name, number(low, high, True), default)
register("retention.gc", enum("manual"), "manual")
register("logging.keep_prompts", enum("digest", "text"), "digest")
# Later-phase seams are explicitly registered, never executable in P1.
for family in ("lod", "collider", "texture", "rig", "anim"):
    register(family + ".enabled", BOOL, False, "profile")
register("lod.switch_source", enum("farworld"), "farworld", "profile")
register("lod.ratios", array(number(0.001, 1), 2, 2), [0.5, 0.2], "profile")
register("collider.mode", enum("none", "auto"), "none", "profile")
register("collider.primitive_fit_max_error_rel", number(0, 1), 0.05, "profile")
register("collider.hull_max_vertices", number(4, 255, True), 64, "profile")
register("collider.max_total_hulls", number(1, 256, True), 32, "profile")
register("collider.output_naming", string(maximum=63), "LUM_COL.{kind}.{index:02d}", "profile")
register("collider.decompose.tool", enum("coacd", "vhacd"), "coacd", "profile")
register("collider.decompose.threshold", number(0.001, 1), 0.05, "profile")
register("collider.decompose.max_hulls", number(1, 256, True), 16, "profile")
register("collider.decompose.max_hull_vertices", number(4, 255, True), 64, "profile")
register("collider.decompose.decimate", BOOL, True, "profile")
register("collider.decompose.preprocess", enum("on", "off"), "on", "profile")
register("anim.sample_rate_hz", number(1, 240, True), 30, "profile")
for flag in ("partuv", "eval3d", "nuvo"):
    register("experimental." + flag, BOOL, False, "profile")

HARD_CEILINGS = {path: field.schema["maximum"] for path, field in FIELDS.items()
    if path.startswith("limits.") or path in ("budgets.max_tris_prefinish", "timeouts.job_s",
        "timeouts.finish_s", "agent.ops.max_ops", "agent.ops.max_voxel_cells") or path.startswith("agent.staging.")}
register("policy.hard_ceilings", obj({path: {"type": "number", "const": value}
         for path, value in HARD_CEILINGS.items()}, HARD_CEILINGS), HARD_CEILINGS, immutable=True)

# These are provisional internal baselines, not approved shipping budgets.
PROVISIONAL_PROFILES = {
    "default": {"budgets": {"max_tris_final": 8000, "max_tris_prefinish": 4000000,
                            "bounds_m": [8.0, 8.0, 8.0]}, "backend": {"id": "mock"}},
    "rock": {"inherits": "default", "budgets": {"max_tris_final": 6000}, "finish": {"loose": {"policy": "keep"}}},
    "prop": {"inherits": "default", "budgets": {"max_tris_final": 8000}},
    "foliage": {"inherits": "default", "budgets": {"max_tris_final": 4000},
        "finish": {"loose": {"policy": "keep"}, "decimate": {"thin_shell_guard": True, "hausdorff_max_rel": 0.02},
                   "normals": {"mode": "smooth"}}},
    "tree": {"inherits": "default", "budgets": {"max_tris_final": 20000, "bounds_m": [30.0, 30.0, 40.0]},
             "finish": {"decimate": {"thin_shell_guard": True}}},
}


def set_path(target, path, value):
    parts = path.split(".")
    for key in parts[:-1]:
        target = target.setdefault(key, {})
    target[parts[-1]] = deepcopy(value)


def field_for(path):
    parts = path.split(".")
    if len(parts) > 2 and parts[0] == "backends":
        parts[1] = "*"
    return FIELDS.get(".".join(parts))


def defaults():
    result = {"backends": {}, "profiles": {}, "candidates": {"score_weights": {}}, "workers": {"blender": {}}}
    for path, field in FIELDS.items():
        if "*" not in path and field.default is not MISSING:
            set_path(result, path, field.default)
    return result


def _insert_schema(root, path, schema):
    parts = path.split(".")
    for key in parts[:-1]:
        if key == "*":
            root = root.setdefault("additionalProperties", obj())
        else:
            root = root["properties"].setdefault(key, obj())
    root["properties"][parts[-1]] = deepcopy(schema)


def config_schema():
    result = obj()
    backend = obj(required=("endpoint", "token_source"))
    result["properties"]["backends"] = obj(dynamic=backend, maximum=32)
    profile = obj({"inherits": ID_SCHEMA})
    for path, field in FIELDS.items():
        schema = {**field.schema, "x-override": field.override, "x-tighten": field.tighten,
                  "x-locked": field.immutable}
        if field.default is not MISSING:
            schema["default"] = deepcopy(field.default)
        _insert_schema(result, path, schema)
        if field.override in ("profile", "request", "request_tighten") and not path.startswith("backends."):
            _insert_schema(profile, path, schema)
    result["properties"]["profiles"] = obj(dynamic=profile, maximum=32)
    result["required"] = ["schema", "config_schema_version"]
    result["$id"] = CONFIG
    return result


def overrides_schema():
    """Closed configuration shape with the same partial-object rules as Python.

    Union branches and array elements remain complete values, as they do in
    validate_schema(partial=True); only recursively merged objects are partial.
    Layer permissions and tighten-only comparisons still require resolution.
    """
    result = config_schema()
    result.pop("$id")
    result["maxProperties"] = 128

    def partial_objects(schema):
        schema.pop("required", None)
        for child in schema.get("properties", {}).values():
            partial_objects(child)
        extra = schema.get("additionalProperties")
        if isinstance(extra, dict):
            partial_objects(extra)

    partial_objects(result)
    return result


KEYWORDS = frozenset(("$id", "$defs", "$ref", "type", "enum", "const", "oneOf", "minimum", "maximum",
    "maxLength", "maxProperties", "pattern", "items", "minItems", "maxItems",
    "required", "properties", "additionalProperties", "default", "x-override", "x-tighten", "x-locked"))


def check_schema(schema, root=None):
    """Refuse unsupported keywords, including in unvisited branches."""
    require(isinstance(schema, dict) and schema.keys() <= KEYWORDS, "schema.unsupported",
            "Unsupported schema keyword or schema shape.")
    root = schema if root is None else root
    if "type" in schema:
        require(schema["type"] in ("object", "array", "string", "integer", "number", "boolean", "null"),
                "schema.unsupported", "Unsupported schema type.")
    for key in ("properties", "$defs"):
        require(isinstance(schema.get(key, {}), dict), "schema.unsupported", "Schema properties must be a mapping.")
    require(isinstance(schema.get("oneOf", []), list) and ("oneOf" not in schema or bool(schema["oneOf"])),
            "schema.unsupported", "oneOf must contain declared branches.")
    require(schema.get("additionalProperties", False) is False or isinstance(schema.get("additionalProperties"), dict),
            "schema.unsupported", "Dynamic objects need a declared value schema.")
    require("items" not in schema or isinstance(schema["items"], dict), "schema.unsupported", "Array items need a schema.")
    for key in ("minimum", "maximum", "maxLength", "maxProperties", "minItems", "maxItems"):
        if key in schema:
            require(type(schema[key]) is int or (type(schema[key]) is float and math.isfinite(schema[key])),
                    "schema.unsupported", "Schema bounds must be finite numbers.")
    for key in ("required", "enum"):
        require(key not in schema or isinstance(schema[key], list), "schema.unsupported", "Schema lists must be arrays.")
    require(all(isinstance(key, str) for key in schema.get("required", [])),
            "schema.unsupported", "Required fields must be names.")
    if "pattern" in schema:
        require(isinstance(schema["pattern"], str), "schema.unsupported", "Pattern must be text.")
        try:
            re.compile(schema["pattern"])
        except re.error as error:
            raise Refusal("schema.unsupported", "Invalid schema pattern.") from error
    if "$ref" in schema:
        ref = schema["$ref"]
        require(isinstance(ref, str) and re.fullmatch(r"#/\$defs/[A-Za-z0-9_-]+", ref)
                and ref[8:] in root.get("$defs", {}), "schema.unsupported", "Only defined local references are allowed.")
    for key in ("properties", "$defs"):
        for child in schema.get(key, {}).values():
            check_schema(child, root)
    for child in schema.get("oneOf", []):
        check_schema(child, root)
    for key in ("items", "additionalProperties"):
        if isinstance(schema.get(key), dict):
            check_schema(schema[key], root)


def validate_schema(value, schema, path="", *, partial=False, root=None, _depth=0):
    """Small fail-closed JSON Schema subset shared with capability parameters.

    Schema documents are checked in full before matching. Partial validation
    omits object required fields only; scalar and collection bounds still apply.
    """
    if root is None:
        check_schema(schema)
        root = schema
    require(_depth <= 64, "config.out_of_bounds", "Schema/value nesting exceeds the bound.", path)
    if "$ref" in schema:
        return validate_schema(value, root["$defs"][schema["$ref"][8:]], path,
                               partial=partial, root=root, _depth=_depth+1)
    if "oneOf" in schema:
        matches = 0
        for branch in schema["oneOf"]:
            try:
                validate_schema(value, branch, path, root=root, _depth=_depth+1)
                matches += 1
            except Refusal:
                pass
        require(matches == 1, "config.type", "Exactly one declared shape must match.", path)
    kind = schema.get("type")
    valid = {"object": isinstance(value, dict), "array": isinstance(value, list),
             "string": isinstance(value, str), "integer": type(value) is int,
             "number": type(value) in (int, float), "boolean": type(value) is bool,
             "null": value is None}
    require(kind is None or valid.get(kind, False), "config.type", "Wrong configuration type.", path)
    if "const" in schema:
        require(value == schema["const"] and not (type(value) is bool and type(schema["const"]) is not bool),
                "config.out_of_bounds", "Immutable value differs.", path)
    if "enum" in schema:
        require(any(type(value) is type(item) and value == item for item in schema["enum"]),
                "config.out_of_bounds", "Value is outside the declared choices.", path)
    if type(value) in (int, float):
        require((type(value) is int or math.isfinite(value))
                and schema.get("minimum", -math.inf) <= value <= schema.get("maximum", math.inf),
                "config.out_of_bounds", "Number is non-finite or outside bounds.", path)
    if isinstance(value, str):
        require(schema.get("minLength", 0) <= len(value) <= schema.get("maxLength", math.inf)
                and ("pattern" not in schema or re.fullmatch(schema["pattern"], value)),
                "config.out_of_bounds", "String exceeds bounds or has an invalid shape.", path)
    if isinstance(value, dict):
        require(schema.get("minProperties", 0) <= len(value) <= schema.get("maxProperties", math.inf),
                "config.out_of_bounds", "Object has too many or too few fields.", path)
        require(partial or set(schema.get("required", ())) <= value.keys(),
                "config.type", "Required configuration field is missing.", path)
        props = schema.get("properties", {})
        for key, child in value.items():
            child_path = path + "." + key if path else key
            extra = schema.get("additionalProperties", {})
            require(key in props or isinstance(extra, dict), "config.unknown_key", "Unregistered configuration path.", child_path)
            validate_schema(child, props.get(key, extra), child_path, partial=partial, root=root, _depth=_depth+1)
    if isinstance(value, list):
        require(schema.get("minItems", 0) <= len(value) <= schema.get("maxItems", math.inf),
                "config.out_of_bounds", "Array has the wrong number of items.", path)
        for index, child in enumerate(value):
            validate_schema(child, schema.get("items", {}), f"{path}[{index}]", root=root, _depth=_depth+1)
    return value

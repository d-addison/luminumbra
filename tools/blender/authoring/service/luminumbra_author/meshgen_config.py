"""Strict read-only configuration resolution; no service, backend or secret IO.

Committed profiles establish baselines within L1 bounds, even when tree budgets
or foliage guards differ from L0. L3/L4 tighten against the selected baseline.
Sources are separate evidence, never part of the configuration identity.
"""
from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re

from .contracts import canonical, digest, parse_json, read_bounded, relative_path, require
from .meshgen_registry import (CONFIG, VERSION, FIELDS, MISSING, NAME_PATTERN, STEPS,
    MANDATORY_STEPS, defaults, config_schema, field_for, validate_schema, set_path)

PROJECT_CONFIG = "authoring/meshgen/meshgen.config.json"
LOCAL_CONFIG = "authoring/meshgen/meshgen.local.json"
SCHEMA = config_schema()


def inert_json(value, depth=0, *, max_depth=32, max_items=65536):
    """Bound even deferred backend parameter values without accepting secrets."""
    require(depth <= max_depth, "config.out_of_bounds", "JSON nesting limit exceeded.")
    if isinstance(value, dict):
        require(len(value) <= max_items and all(isinstance(k, str) for k in value),
                "config.out_of_bounds", "JSON object exceeds limits.")
        for child in value.values():
            inert_json(child, depth+1, max_depth=max_depth, max_items=max_items)
    elif isinstance(value, list):
        require(len(value) <= max_items, "config.out_of_bounds", "JSON array exceeds limits.")
        for child in value:
            inert_json(child, depth+1, max_depth=max_depth, max_items=max_items)
    else:
        require(value is None or type(value) in (str, int, float, bool),
                "config.type", "Configuration must contain JSON data.")
    if depth == 0:
        canonical(value)


def load_document(path):
    value = parse_json(read_bounded(path, 1024**2))
    inert_json(value)
    return value


def merge(left, right):
    result = deepcopy(left)
    for key, value in right.items():
        result[key] = merge(result[key], value) if isinstance(value, dict) and isinstance(result.get(key), dict) else deepcopy(value)
    return result


def leaves(value, prefix=""):
    if field_for(prefix) is not None or not isinstance(value, dict):
        yield prefix, value
    else:
        for key, child in value.items():
            yield from leaves(child, prefix + "." + key if prefix else key)


def get_path(value, path, default=MISSING):
    for part in path.split("."):
        if not isinstance(value, dict) or part not in value:
            return default
        value = value[part]
    return value


def matches(pattern, path):
    pattern_parts, path_parts = pattern.split("."), path.split(".")
    return len(pattern_parts) <= len(path_parts) and all(
        left == "*" or left == right or right == "*" for left, right in zip(pattern_parts, path_parts))


def validate_locks(locks):
    require(len(set(locks)) == len(locks), "config.type", "Lock patterns must be distinct.", "policy.locks")
    for pattern in locks:
        require(all(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*|\*", part) for part in pattern.split("."))
                and any(matches(pattern, path) for path in FIELDS),
                "config.unknown_key", "Lock pattern must match a registered path.", "policy.locks")


def validate_bounds(bounds):
    for path, bound in bounds.items():
        field = field_for(path)
        require(field is not None and "*" not in path, "config.unknown_key", "Bound must name a concrete registry path.", path)
        schema = field.schema.get("items", field.schema)
        require(schema.get("type") in ("integer", "number") and bound["min"] <= bound["max"]
                and schema["minimum"] <= bound["min"] <= bound["max"] <= schema["maximum"],
                "config.out_of_bounds", "Policy bounds may only narrow registry bounds.", path)


def within_policy(value, bounds):
    for path, bound in bounds.items():
        item = get_path(value, path)
        if item is MISSING:
            continue
        values = item if isinstance(item, list) else [item]
        require(all(bound["min"] <= part <= bound["max"] for part in values),
                "config.out_of_bounds", "Value exceeds committed policy bounds.", path)


def tighter(new, old, direction):
    if old is MISSING:
        return True
    if direction == "subset":
        return set(new) <= set(old)
    if isinstance(new, list) and isinstance(old, list):
        return len(new) == len(old) and all(tighter(a, b, direction) for a, b in zip(new, old))
    return new >= old if direction == "higher" else new <= old


def canonical_relative(name):
    require(isinstance(name, str) and bool(name) and name != "." and not name.startswith("/")
            and ".." not in PurePosixPath(name).parts and "\\" not in name and ":" not in name
            and "\x00" not in name and str(PurePosixPath(name)) == name,
            "config.type", "Use a canonical project-relative path.", "token_source.file")


def validate_semantics(value, project_root=None):
    validate_schema(value, SCHEMA)
    inert_json(value, max_depth=value["limits"]["json_depth"], max_items=value["limits"]["collection_items"])
    validate_locks(value["policy"]["locks"])
    validate_bounds(value["policy"]["bounds"])
    within_policy(value, value["policy"]["bounds"])
    steps = value["finish"]["steps"]
    require(len(set(steps)) == len(steps) and MANDATORY_STEPS <= set(steps)
            and steps == [step for step in STEPS if step in steps],
            "config.override_denied", "Finisher steps must retain mandatory steps and registry order.", "finish.steps")
    require(value["finish"]["decimate"]["max_tris"] <= value["budgets"]["max_tris_final"],
            "config.out_of_bounds", "Decimation target exceeds the authoritative final cap.", "finish.decimate.max_tris")
    for name, backend in value["backends"].items():
        require(re.fullmatch(NAME_PATTERN, name), "config.type", "Invalid backend name.", "backends")
        port = int(backend["endpoint"].rsplit(":", 1)[1])
        require(1 <= port <= 65535, "config.out_of_bounds", "Endpoint port must be 1–65535.", f"backends.{name}.endpoint")
        if "file" in backend["token_source"]:
            canonical_relative(backend["token_source"]["file"])
        require(not (backend["enabled"] and backend["kind"] == "model" and not backend["pins"]),
                "config.pins_missing", "Enabled model backend needs revision and weight pins.", f"backends.{name}.pins")
    for name in value.get("profiles", {}):
        require(re.fullmatch(NAME_PATTERN, name), "config.type", "Invalid profile name.", "profiles")
    backend_id = value["backend"].get("id")
    require(backend_id in value["backends"] and value["backends"][backend_id]["enabled"],
            "config.backend_missing", "Select an enabled project backend.", "backend.id")
    for path, item in leaves(value.get("workers", {}), "workers"):
        field = field_for(path)
        if field and field.local:
            require(Path(item).is_file() and not Path(item).is_symlink(),
                    "config.type", "Worker location must be an existing regular file.", path)
    for key in ("collection_template", "object_template"):
        template = value["naming"][key]
        try:
            rendered = template.format(job8="12345678", index=0)
        except (ValueError, KeyError, IndexError, AttributeError):
            rendered = ""
        require(re.fullmatch(r"[A-Za-z0-9._-]{1,63}", rendered)
                and (key != "collection_template" or rendered.startswith("LUM_CANDIDATE.")),
                "config.type", "Naming template has invalid placeholders, prefix or characters.", "naming." + key)
    require(not value["evaluation"]["judge"]["enabled"] or value["policy"]["validation_profile"] == "internal",
            "config.override_denied", "Advisory judge is internal only.", "evaluation.judge.enabled")
    if value["candidates"]["select"] == "auto_by_score":
        require(sum(value["candidates"]["score_weights"].values()) > 0, "config.type",
                "Automatic scoring needs explicit weights; metric availability is checked at planning.", "candidates.score_weights")


def configuration_digest(value):
    # No secret values are admitted by the schema: token_source contains a
    # reference only. Token rotation is therefore independent of identity.
    content = {key: item for key, item in value.items() if key != "config_digest"}
    require("sources" not in content, "config.unknown_key", "Sources belong outside effective configuration.", "sources")
    validate_schema(content, SCHEMA)
    inert_json(content)
    return digest(canonical(content))


@dataclass(frozen=True)
class ResolvedConfig:
    effective: dict
    sources: dict
    config_digest: str
    findings: list

    @property
    def stored_bytes(self):
        return canonical({**self.effective, "config_digest": self.config_digest}) + b"\n"

    @property
    def stored_sha256(self):
        return digest(self.stored_bytes)

    @property
    def differences(self):
        base = defaults()
        base["finish"]["decimate"]["max_tris"] = base["budgets"]["max_tris_final"]
        return {path: {"default": None if get_path(base, path) is MISSING else get_path(base, path), "effective": value}
                for path, value in leaves(self.effective) if get_path(base, path) != value}


def verify_effective_config(data, stored_sha256, config_digest):
    """Worker-side byte and resolved-identity checks, with no file writes."""
    require(digest(data) == stored_sha256, "config.digest_mismatch", "Stored configuration bytes changed.")
    value = parse_json(data)
    require(isinstance(value, dict) and value.get("config_digest") == config_digest
            and configuration_digest(value) == config_digest, "config.digest_mismatch", "Resolved configuration identity changed.")
    effective = {key: item for key, item in value.items() if key != "config_digest"}
    validate_semantics(effective)
    return effective


def _finding(rule, message, field="", severity="warning"):
    from .contracts import Refusal
    result = Refusal(rule, message, field).finding
    result["severity"] = severity
    return result


def resolve(project, asset_class, local=None, overrides=None, *, project_root=None):
    """Resolve L0→L1→selected L2→L3→L4, validating all committed profiles.

    Passing documents is useful for portable callers. ``load_config`` performs
    bounded, no-symlink project file discovery for the read-only CLI.
    """
    require(isinstance(asset_class, str) and bool(asset_class), "config.profile_missing", "asset_class must explicitly select a profile.", "asset_class")
    require(isinstance(project, dict), "config.type", "Project configuration must be an object.")
    require(project.get("schema") == CONFIG and type(project.get("config_schema_version")) is int
            and project["config_schema_version"] == VERSION, "config.schema_version",
            "Use config schema version 1; migration requires an explicit tool.", "config_schema_version")
    local = {} if local is None else local
    overrides = {} if overrides is None else overrides
    for document in (project, local, overrides):
        inert_json(document)
        validate_schema(document, SCHEMA, partial=True)
    effective = merge(defaults(), {key: value for key, value in project.items() if key != "profiles"})
    effective["profiles"] = deepcopy(project.get("profiles", {}))
    for name, backend in effective["backends"].items():
        for path, field in FIELDS.items():
            if path.startswith("backends.*.") and field.default is not MISSING:
                suffix = path[len("backends.*."):]
                if get_path(backend, suffix) is MISSING:
                    set_path(backend, suffix, field.default)
        backend["timeouts"] = merge(effective["timeouts"], backend.get("timeouts", {}))
    sources = {path: "L0" for path, _ in leaves(effective) if not path.startswith("profiles.")}
    sources.update({path: "L1" for path, _ in leaves(project) if not path.startswith("profiles.")})
    explicit_backend_timeouts = {path for path in sources if sources[path] == "L1"
                                 and path.startswith("backends.") and ".timeouts." in path}

    def inherit_timeouts():
        for backend_name, entry in effective["backends"].items():
            for path, item in leaves(effective["timeouts"], "timeouts"):
                full = f"backends.{backend_name}.{path}"
                if full not in explicit_backend_timeouts:
                    set_path(entry, path, item)
                    sources[full] = sources[path]
    locks = effective["policy"]["locks"]
    validate_locks(locks)
    validate_bounds(effective["policy"]["bounds"])
    for path, _ in leaves(project):
        field = field_for(path)
        require(not (field and field.local and path.startswith("workers.")), "config.override_denied", "Worker paths are L3 only.", path)
    profiles = effective["profiles"]
    for name in profiles:
        require(re.fullmatch(NAME_PATTERN, name), "config.type", "Invalid profile name.", "profiles")

    def chain(name, seen=()):
        require(name not in seen, "config.cycle", "Profile inheritance contains a cycle.", name)
        require(name in profiles, "config.profile_missing", "Profile or parent is missing.", name)
        require(len(seen) < 3, "config.out_of_bounds", "At most three profiles may be resolved.", name)
        parent = profiles[name].get("inherits")
        return (chain(parent, seen + (name,)) if parent is not None else []) + [name]

    def apply(base, layer, level, baseline):
        require("profiles" not in layer and "policy" not in layer,
                "config.override_denied", "Profiles and policy are committed L1 inputs.")
        for path, item in leaves(layer):
            field = field_for(path)
            require(field is not None, "config.unknown_key", "Unregistered override path.", path)
            previous = get_path(base, path)
            require(not any(matches(pattern, path) for pattern in locks) or previous == item,
                    "config.locked", "Path is locked by L1 policy.", path)
            if level == "L2":
                allowed = field.override in ("profile", "request", "request_tighten")
            elif level == "L3":
                allowed = field.local and (not path.startswith("backends.") or path.split(".")[1] in base["backends"])
            else:
                allowed = field.override in ("request", "request_tighten") and not (field.local and path.startswith("workers.") and field.override == "none")
                if path.startswith("backends."):
                    allowed = allowed and path.split(".")[1] in base["backends"]
            require(allowed, "config.override_denied", "Layer cannot set this path.", path)
            if level == "L3" and field.override == "local_lower_only":
                require(previous is True or item is False, "config.override_denied", "Local config may only disable a backend.", path)
            if level in ("L3", "L4") and field.override == "request_tighten":
                old = get_path(baseline, path)
                if path == "finish.decimate.max_tris" and old is MISSING:
                    old = baseline["budgets"]["max_tris_final"]
                require(tighter(item, old, field.tighten), "config.override_denied", "Override may only tighten the selected profile.", path)
                require(tighter(item, previous, field.tighten), "config.override_denied", "Override cannot loosen an earlier layer.", path)
        result = merge(base, layer)
        # A typed token reference is one registry value, represented by a
        # oneOf object. Switching env/file replaces that complete value.
        for path, item in leaves(layer):
            if path.endswith(".token_source"):
                set_path(result, path, item)
        return result

    # Validate dormant profiles too: typos, inherited cycles, forbidden policy
    # and inconsistent decimation targets must not hide until selection.
    for name in profiles:
        candidate = deepcopy(effective)
        for parent in chain(name):
            layer = {key: item for key, item in profiles[parent].items() if key != "inherits"}
            candidate = apply(candidate, layer, "L2", effective)
        if "max_tris" not in candidate["finish"]["decimate"]:
            candidate["finish"]["decimate"]["max_tris"] = candidate["budgets"]["max_tris_final"]
        validate_schema(candidate, SCHEMA)
        within_policy(candidate, effective["policy"]["bounds"])
        require(candidate["finish"]["decimate"]["max_tris"] <= candidate["budgets"]["max_tris_final"],
                "config.out_of_bounds", "Profile decimation target exceeds final cap.", name)
        profile_steps = candidate["finish"]["steps"]
        require(len(set(profile_steps)) == len(profile_steps) and MANDATORY_STEPS <= set(profile_steps)
                and profile_steps == [step for step in STEPS if step in profile_steps],
                "config.override_denied", "Profile finisher steps are missing, repeated or reordered.", name)
    for name in chain(asset_class):
        layer = {key: value for key, value in profiles[name].items() if key != "inherits"}
        effective = apply(effective, layer, "L2", effective)
        sources.update({path: "L2" for path, _ in leaves(layer)})
    inherit_timeouts()
    baseline = deepcopy(effective)
    for level, layer in (("L3", local), ("L4", overrides)):
        effective = apply(effective, layer, level, baseline)
        sources.update({path: level for path, _ in leaves(layer)})
        explicit_backend_timeouts.update(path for path, _ in leaves(layer)
                                        if path.startswith("backends.") and ".timeouts." in path)
    inherit_timeouts()
    if "max_tris" not in effective["finish"]["decimate"]:
        effective["finish"]["decimate"]["max_tris"] = effective["budgets"]["max_tris_final"]
        sources["finish.decimate.max_tris"] = sources["budgets.max_tris_final"]
    # Profile declarations are resolution inputs, not resolved values. Retain
    # the selected profile name in provenance/CLI, outside config identity.
    effective.pop("profiles")
    validate_semantics(effective, project_root)
    findings = [_finding("config.budgets_uncalibrated", "Profile budgets are provisional internal values; shipping calibration remains pending."),
                _finding("config.backend_evidence_unavailable", "Live capability, parameter, pin and licence evidence has not been obtained; this is a configuration planning check.")]
    for key, attestation in effective["policy"]["licence"]["attestations"].items():
        if attestation["value"]:
            path = f"authoring/meshgen/notes/{attestation['note_id']}.md"
            try:
                note = relative_path(project_root, path) if project_root is not None else None
                verified = note is not None and digest(read_bounded(note, 1024**2)) == attestation["note_sha256"]
            except (ValueError, OSError):
                verified = False
            require(verified, "config.attestation_unverified", "True attestation needs a matching committed note file.", "policy.licence.attestations." + key)
    for path, supported in (("finish.decimate.method", ("none", "blender_collapse")), ("finish.uv.method", ("none", "smart_project"))):
        require(get_path(effective, path) in supported, "config.helper_unavailable", "Selected helper is unavailable in P1.", path)
    for family in ("lod", "collider", "texture", "rig", "anim"):
        require(not effective[family]["enabled"], "config.execution_unavailable", "Selected execution belongs to a later phase.", family + ".enabled")
    require(not effective["agent"]["tier_b"]["enabled"], "tier_b.unavailable", "Raw bpy execution is unavailable in P1.")
    require(effective["collider"]["mode"] == "none", "config.execution_unavailable", "Collider execution is unavailable in P1.", "collider.mode")
    require(not any(effective["experimental"].values()), "config.execution_unavailable", "Experimental helpers are unavailable in P1.")
    return ResolvedConfig(effective, sources, configuration_digest(effective), findings)


def load_config(project_root, asset_class, overrides=None):
    root = Path(project_root).resolve(strict=True)
    project = load_document(relative_path(root, PROJECT_CONFIG))
    local_path = root / LOCAL_CONFIG
    local = load_document(relative_path(root, LOCAL_CONFIG)) if local_path.exists() or local_path.is_symlink() else {}
    return resolve(project, asset_class, local, overrides, project_root=root)

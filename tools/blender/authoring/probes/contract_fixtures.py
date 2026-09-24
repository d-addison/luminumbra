"""Materialize draft schema examples, without registering production contracts."""
from common import atomic_json
from contracts import ASSET, GRAPH, PROFILE, RECEIPT, RECIPE
from fixtures import asset, graph


def object_schema(properties, required=None):
    return {"type": "object", "properties": properties,
            "required": list(properties if required is None else required), "additionalProperties": False}


def write(directory):
    directory.mkdir(parents=True, exist_ok=True)
    string = {"type": "string", "minLength": 1}
    identity = {"type": "string", "pattern": "^[A-Za-z][A-Za-z0-9_.:-]{0,127}$"}
    revision = {"type": "integer", "minimum": 0}
    matrix = {"type": "array", "items": {"type": "number"}, "minItems": 16, "maxItems": 16}
    objects = object_schema({"id": identity, "parent": {"type": ["string", "null"]},
                             "asset_id": identity, "transform": matrix, "components": {"const": []},
                             "label": {"type": "string"}},
                            ["id", "parent", "asset_id", "transform", "components"])
    schemas = {}
    schemas["asset"] = object_schema({
        "schema": {"const": ASSET}, "asset_id": identity, "revision": revision,
        "profile": {"const": PROFILE},
        "objects": {"type": "array", "items": objects, "maxItems": 1024},
        "dependencies": {"type": "array", "items": string, "uniqueItems": True, "maxItems": 128},
        "required_schemas": {"type": "array", "items": {"const": ASSET}},
        "annotations": {"type": "object"}},
        ["schema", "asset_id", "revision", "profile", "objects", "dependencies"])
    schemas["recipe"] = object_schema({
        "schema": {"const": RECIPE}, "recipe_id": {"const": "fixture.mark_prefab.v1"},
        "version": {"const": 1}, "expected_scene_revision": revision,
        "target_ids": {"type": "array", "items": identity, "minItems": 1, "uniqueItems": True},
        "parameters": object_schema({"asset_id": identity})})
    ports = {"type": "object", "additionalProperties": string}
    definition = object_schema({"id": identity, "digest": {"type": "string", "pattern": "^[0-9a-f]{64}$"},
                                "inputs": ports, "outputs": ports})
    endpoint = {"type": "array", "items": string, "minItems": 2, "maxItems": 2}
    schemas["graph"] = object_schema({
        "schema": {"const": GRAPH}, "id": identity,
        "definitions": {"type": "array", "items": definition},
        "nodes": {"type": "array", "items": object_schema({"id": identity, "definition": identity})},
        "connections": {"type": "array", "items": object_schema({"from": endpoint, "to": endpoint})},
        "layout": {"type": "object"}}, ["schema", "id", "definitions", "nodes", "connections"])
    schemas["receipt"] = object_schema({
        "schema": {"const": RECEIPT}, "mode": {"const": "MOCK"}, "job_id": string,
        "status": {"enum": ["extracting", "compiling", "publishing", "succeeded", "failed", "cancelled", "stale"]},
        "source": string, "revision": revision, "asset_id": identity,
        "input_hashes": {"type": "object", "additionalProperties": string},
        "build_identity": string, "tool_hashes": {"type": "object", "additionalProperties": string},
        "findings": {"type": "array", "items": {"type": "object"}},
        "engine_executed": {"const": False}, "rendered": {"const": False},
        "recovery": string},
        ["schema", "mode", "job_id", "status", "source", "revision", "asset_id", "input_hashes",
         "build_identity", "tool_hashes", "findings", "engine_executed", "rendered"])
    examples = {"asset": asset(), "graph": graph(), "recipe": {
        "schema": RECIPE, "recipe_id": "fixture.mark_prefab.v1", "version": 1,
        "expected_scene_revision": 1, "target_ids": ["fixture.root"],
        "parameters": {"asset_id": "fixture.prop"}}}
    names = {"asset": ASSET, "graph": GRAPH, "recipe": RECIPE, "receipt": RECEIPT}
    for name, schema in schemas.items():
        schema.update({"$schema": "https://json-schema.org/draft/2020-12/schema",
                       "$id": "urn:" + names[name],
                       "description": "DRAFT MOCK experiment subset. Not a registered engine contract."})
        atomic_json(directory / (name + ".schema.json"), schema)
    for name, example in examples.items():
        atomic_json(directory / (name + ".example.json"), example)
    return schemas, examples

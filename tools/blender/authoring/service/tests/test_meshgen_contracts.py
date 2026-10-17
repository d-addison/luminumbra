"""Shared contracts, waiver grammar, digest identities and provenance fixtures."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from luminumbra_author.contracts import Refusal, canonical, digest
from luminumbra_author.meshgen_config import resolve
from luminumbra_author.meshgen_contracts import (PROVENANCE, CANDIDATE, RECEIPT,
    SEALED_REQUIRED, SEALED_OPTIONAL, MANIFEST_FILE, COMMIT_FILE, JOB_AREA, CANDIDATE_AREA,
    PREPARED_RECEIPT_FILE, RECEIPT_FILE, RECEIPT_COMMIT_FILE, RETRYABLE_RULES, P1_FEATURES,
    validate_request, validate_manifest, bundle_identity, validate_provenance, validate_receipt,
    validate_backend_params, candidate_seeds, accepted_waivers)
from test_meshgen_config import project_fixture
from luminumbra_author.meshgen_registry import validate_schema

HASH = digest(b"fixture")


def request_fixture():
    return {"kind": "backend", "asset_class": "rock", "backend_id": "mock",
        "image_ref": {"path": "references/rock.png", "sha256": HASH},
        "declared_source": "own_work", "seed": 123, "request_id": "generate:test"}


def acceptance_fixture():
    return {"human_initiated": True, "accepted_by": "reviewer", "accepted_at": "2026-10-13T12:00:00Z",
            "gates": ["final_tris_within_budget"]}


def provenance_fixture():
    config = resolve(project_fixture(), "rock")
    finish = {"blender_version": "5.1.0", "exporter_sha256": HASH, "steps": [], "tool_versions": {}}
    return {"schema": PROVENANCE, "recipe": "authoring.asset.generate_candidate.v1",
        "definition_sha256": HASH, "plan_sha256": HASH, "request_id": "generate:test", "document_session": "fixture-session",
        "model": {"name": "synthetic", "repo": "fixture", "revision": "builtin", "weights_sha256": [],
            "precision": None, "code_commit": "builtin", "backend_reported": True},
        "license": {"id": "fixture", "text_sha256": HASH, "url": "fixture", "territory_restricted": False, "user_acknowledgment": False},
        "input": {"image_sha256": HASH, "prompt_sha256": digest(b""), "preprocessing_model": None, "preprocessing_license": None},
        "params": {}, "raw_output": {"sha256": HASH, "bytes": 128, "triangles": 1, "vertices": 3}, "finishing": deepcopy(finish),
        "config": {"config_digest": config.config_digest, "config_schema_version": 1, "profile": "rock", "sources": config.sources,
                   "effective_config_sha256": config.stored_sha256},
        "licence_gate": {"status": "shippable", "shippable": True, "components": [], "dataset_chain_audited": True,
                         "ack_ids": [], "sha256": HASH},
        "backend": {"id": "mock", "version": "1", "protocol": 1, "adapter_commit": "builtin", "capability_digest": HASH, "pins_matched": True},
        "environment": {"os": "simulated", "blender_version": "not-executed", "exporter_sha256": HASH,
            "gpu_name": None, "driver": None, "cuda": None, "torch": None, "dtype": None, "batch_size": None,
            "python_version": "stdlib", "peak_vram_mib": None},
        "request": {"image_sha256": HASH, "declared_source": "own_work", "prompt_digest": digest(b""),
            "seed": 123, "params": {}, "max_tris_hint": 6000, "inputs_retained": True}, "finish": finish,
        "metrics": {"stats_sha256": HASH},
        "artifact": {"sha256": HASH, "triangles": 1, "vertices": 3, "bodies": 1, "watertight": False,
            "has_uv": True, "has_normals": True, "bounds_m": [1, 1, 1], "budget_ok": True},
        "flags": {"shippable": False, "origin": "backend:mock", "waived": [], "game-repo-only": True, "no-training-on-outputs": True}}


def receipt_fixture():
    return {"schema": RECEIPT, "request_id": "generate:test", "plan_digest": HASH, "config_digest": HASH,
        "tier": "backend", "origin": "backend:mock", "backend": "mock",
        "licence": {"status": "shippable", "shippable": True, "ack_ids": []},
        "candidate": {"dir": ".luminumbra-author-recipes/_headless/candidates/test", "sha256": HASH,
            "artifact_sha256": HASH, "provenance_sha256": HASH, "tris": 1, "vertices": 3, "bodies": 1,
            "watertight": False, "bounds_m": [1, 1, 1], "budget_ok": True},
        "flags": {"waived": []}, "gates": {"non_empty": "pass"}, "executed": True,
        "spatially_valid": True, "timings": {"finish_s": 0}, "errors": []}


class Requests(unittest.TestCase):
    def test_packaged_overrides_schema_matches_python_partial_objects(self):
        schema = json.loads((Path(__file__).resolve().parents[1] / "luminumbra_author" /
                             "schemas/meshgen.request.v1.schema.json").read_text())
        for overrides in ({"unknown": 1}, {"finish": {"decimate": {"unknown": 1}}},
                          {"backends": {"mock": {"unknown": 1}}}):
            value = {**request_fixture(), "overrides": overrides}
            for validator in (lambda v: validate_schema(v, schema), validate_request):
                with self.subTest(overrides=overrides, validator=validator), self.assertRaises(Refusal):
                    validator(value)
        value = {**request_fixture(), "overrides": {"backends": {"mock": {"timeouts": {"job_s": 30}}}}}
        validate_schema(value, schema)
        validate_request(value)

    def test_backend_request_and_strict_unknown_fields_no_script_selection(self):
        request = request_fixture()
        validate_request(request)
        for key, value in (("script", "scripts/finish.py"), ("token", "secret"), ("command", "python")):
            with self.subTest(key=key), self.assertRaises(Refusal): validate_request({**request, key: value})
        for kind in ("ops", "script", "collider", "texture", "rig"):
            with self.assertRaises(Refusal) as caught: validate_request({**request, "kind": kind})
            self.assertEqual(caught.exception.finding["rule_id"], "operation.unsupported")
        self.assertEqual(set(key for key, value in P1_FEATURES.items() if value), {"backend"})
        self.assertEqual(RETRYABLE_RULES, ("busy", "backend.unavailable", "stale.plan"))

    def test_request_image_rights_digest_relative_path_and_generation_id(self):
        for key, value in (("asset_class", ""), ("declared_source", ""), ("seed", True),
                           ("seed", 2**32), ("request_id", "import:test")):
            with self.assertRaises(Refusal): validate_request({**request_fixture(), key: value})
        for image in ({"path": "../rock.png", "sha256": HASH}, {"path": "rock.png", "sha256": "unhashed"},
                      {"path": "http://example/rock.png", "sha256": HASH}):
            with self.assertRaises(Refusal): validate_request({**request_fixture(), "image_ref": image})
        for source in ("own_work", "cc0:https://example.test/rock", "licensed:asset.fixture"):
            validate_request({**request_fixture(), "declared_source": source})

    def test_seed_sequence_boundaries_no_wrap(self):
        self.assertEqual(candidate_seeds(123, 4), [123, 124, 125, 126])
        self.assertEqual(candidate_seeds(2**32-1, 1), [2**32-1])
        for seed, count in ((2**32-1, 2), (0, 5), (True, 1)):
            with self.assertRaises(Refusal): candidate_seeds(seed, count)

    def test_plan_binds_resolved_backend_height_prompt_and_seed_limits(self):
        effective = resolve(project_fixture(), "rock").effective
        with self.assertRaises(Refusal): validate_request(request_fixture(), effective)
        effective["budgets"]["target_height_m"] = 1
        validate_request(request_fixture(), effective)
        with self.assertRaises(Refusal): validate_request({**request_fixture(), "backend_id": "other"}, effective)
        effective["limits"]["prompt_chars"] = 1
        with self.assertRaises(Refusal): validate_request({**request_fixture(), "prompt": "long"}, effective)
        effective["candidates"]["count"] = 4
        with self.assertRaises(Refusal): validate_request({**request_fixture(), "seed": 2**32-1}, effective)

    def test_capability_parameter_schemas_unknown_keys_and_keywords(self):
        schema = {"resolution": {"type": "integer", "enum": [512, 1024], "default": 512},
                  "steps": {"type": "integer", "minimum": 10, "maximum": 100}}
        validate_backend_params({"resolution": 512, "steps": 50}, schema)
        for params in ({"extra": 1}, {"steps": True}, {"steps": 101}):
            with self.assertRaises(Refusal): validate_backend_params(params, schema)
        with self.assertRaises(Refusal): validate_backend_params({}, {"x": {"format": "unknown"}})

    def test_human_waiver_all_three_requirements_and_unconditional_refusals(self):
        project = project_fixture()
        project["evaluation"] = {"waivable": ["final_tris_within_budget"]}
        effective = resolve(project, "rock").effective
        with self.assertRaises(Refusal) as caught:
            accepted_waivers(["final_tris_within_budget"], effective, acceptance_fixture())
        self.assertEqual(caught.exception.finding['rule_id'], 'finish.budget_exceeded')
        project['finish'] = {'decimate': {'on_over_budget': 'accept_flagged'}}
        effective = resolve(project, "rock").effective
        self.assertEqual(accepted_waivers(["final_tris_within_budget"], effective, acceptance_fixture()), ["final_tris_within_budget"])
        validate_request({**request_fixture(), "accept_flagged": acceptance_fixture()})
        for acceptance in (None, True, {"gates": ["final_tris_within_budget"]},
                           {**acceptance_fixture(), "human_initiated": False},
                           {**acceptance_fixture(), "gates": ["finite_coordinates"]}):
            with self.assertRaises(Refusal): accepted_waivers(["final_tris_within_budget"], effective, acceptance)
        for gate in ("finite_coordinates", "non_empty", "pre_finish_triangle_cap", "uv_present"):
            with self.assertRaises(Refusal): accepted_waivers([gate], effective, acceptance_fixture())
        effective["policy"]["validation_profile"] = "shipping"
        with self.assertRaises(Refusal): accepted_waivers(["final_tris_within_budget"], effective, acceptance_fixture())
        effective["policy"]["validation_profile"] = "internal"
        effective["evaluation"]["waivable"] = []
        effective["finish"]["decimate"]["on_over_budget"] = "accept_flagged"
        with self.assertRaises(Refusal): accepted_waivers(["final_tris_within_budget"], effective, acceptance_fixture())


class CandidateEvidence(unittest.TestCase):
    def test_packaged_manifest_schema_matches_required_optional_and_closed_members(self):
        schema = json.loads((Path(__file__).resolve().parents[1] / "luminumbra_author" /
                             "schemas/meshgen.candidate.v1.schema.json").read_text())
        value = {"schema": CANDIDATE, "members": {name: HASH for name in SEALED_REQUIRED}}
        for validator in (lambda v: validate_schema(v, schema), validate_manifest):
            validator(value)
            validator({**value, "members": {**value["members"], **dict.fromkeys(SEALED_OPTIONAL, HASH)}})
            for name in SEALED_REQUIRED:
                invalid = deepcopy(value); del invalid["members"][name]
                with self.subTest(member=name, validator=validator), self.assertRaises(Refusal):
                    validator(invalid)
            for name in ("unexpected.py", "receipt.json", "manifest.json", "../escape"):
                invalid = deepcopy(value); invalid["members"][name] = HASH
                with self.subTest(member=name, validator=validator), self.assertRaises(Refusal):
                    validator(invalid)

    def test_manifest_bundle_identity_distinct_from_artifact_and_stored_config(self):
        manifest = {"schema": CANDIDATE, "members": {name: HASH for name in SEALED_REQUIRED}}
        validate_manifest(manifest)
        bundle = bundle_identity(canonical(manifest)+b"\n")
        self.assertNotEqual(bundle, manifest["members"]["candidate.glb"])
        self.assertNotEqual(bundle, bundle_identity(canonical(manifest)))
        resolved = resolve(project_fixture(), "rock")
        self.assertNotEqual(resolved.config_digest, resolved.stored_sha256)

    def test_members_missing_extra_escaped_and_no_self_hash_cycle(self):
        manifest = {"schema": CANDIDATE, "members": {name: HASH for name in SEALED_REQUIRED}}
        for member in SEALED_REQUIRED:
            invalid = deepcopy(manifest); del invalid["members"][member]
            with self.assertRaises(Refusal): validate_manifest(invalid)
        for name in (MANIFEST_FILE, COMMIT_FILE, RECEIPT_FILE, PREPARED_RECEIPT_FILE,
                     RECEIPT_COMMIT_FILE, "../escape", "attestations/metrics.json", "token", "weights.bin"):
            invalid = deepcopy(manifest); invalid["members"][name] = HASH
            with self.assertRaises(Refusal): validate_manifest(invalid)
        for name in SEALED_OPTIONAL:
            manifest["members"][name] = HASH
        validate_manifest(manifest)
        self.assertNotEqual(JOB_AREA, CANDIDATE_AREA)

    def test_v2_preserves_inherited_identity_request_and_raw_output(self):
        value = provenance_fixture()
        validate_provenance(value)
        for key in ("recipe", "definition_sha256", "plan_sha256", "request_id", "document_session",
                    "model", "license", "input", "params", "raw_output", "finishing"):
            invalid = deepcopy(value); del invalid[key]
            with self.subTest(key=key), self.assertRaises(Refusal): validate_provenance(invalid)
        invalid = deepcopy(value); invalid["input"]["image_sha256"] = "0"*64
        with self.assertRaises(Refusal): validate_provenance(invalid)

    def test_secrets_unknown_worker_policy_and_mock_gpu_claims_refused(self):
        for mutation in ("token", "eligible_mock", "mock_gpu", "waived_shipping"):
            value = provenance_fixture()
            if mutation == "token": value["request"]["token"] = "secret"
            if mutation == "eligible_mock": value["flags"]["shippable"] = True
            if mutation == "mock_gpu": value["environment"]["peak_vram_mib"] = 0
            if mutation == "waived_shipping":
                value["backend"]["id"] = "model"; value["flags"].update(origin="backend:model", shippable=True, waived=["uv_present"])
            with self.subTest(mutation=mutation), self.assertRaises(Refusal): validate_provenance(value)

    def test_receipt_paths_structured_errors_and_bool_count_refusal(self):
        value = receipt_fixture()
        validate_receipt(value)
        for path in ("../candidate", "/candidate", ".luminumbra-author-recipes/_headless/jobs/test", "candidate"):
            invalid = deepcopy(value); invalid["candidate"]["dir"] = path
            with self.assertRaises(Refusal): validate_receipt(invalid)
        invalid = deepcopy(value); invalid["candidate"]["tris"] = True
        with self.assertRaises(Refusal): validate_receipt(invalid)
        invalid = deepcopy(value); invalid["errors"] = [{"retryable": True}]
        with self.assertRaises(Refusal): validate_receipt(invalid)


if __name__ == "__main__":
    unittest.main()

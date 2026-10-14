"""Portable registry, layer and read-only CLI evidence; no backend processes."""
from copy import deepcopy
import ast
import importlib.util
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
from luminumbra_author.contracts import Refusal, canonical, digest, parse_json
from luminumbra_author.meshgen_config import (resolve, load_config, load_document, merge,
    configuration_digest, verify_effective_config, PROJECT_CONFIG, LOCAL_CONFIG)
from luminumbra_author.meshgen_registry import (CONFIG, FIELDS, STEPS, PROVISIONAL_PROFILES,
    defaults, config_schema, validate_schema, set_path)
from luminumbra_author.meshgen_contracts import packaged_schemas


def project_fixture():
    return {"schema": CONFIG, "config_schema_version": 1,
        "policy": {"bounds": {"budgets.max_tris_final": {"min": 64, "max": 200000},
                                "timeouts.job_s": {"min": 10, "max": 1800}}},
        "backends": {
            "mock": {"kind": "mock", "enabled": True, "endpoint": "http://127.0.0.1:18800",
                     "token_source": {"env": "LUMINUMBRA_GEN_TOKEN"}, "pins": {}, "timeouts": {"job_s": 60}},
            "direct3d_s2": {"kind": "model", "enabled": False, "endpoint": "http://127.0.0.1:18801",
                "token_source": {"file": "authoring/meshgen/token.d3ds"}, "pins": {},
                "params": {"resolution": 512, "steps": 50, "guidance": 7.0}}},
        "profiles": deepcopy(PROVISIONAL_PROFILES)}


class Configuration(unittest.TestCase):
    def setUp(self):
        self.project = project_fixture()

    def refusal(self, rule, project=None, profile="rock", local=None, overrides=None):
        with self.assertRaises(Refusal) as caught:
            resolve(self.project if project is None else project, profile, local, overrides)
        self.assertEqual(caught.exception.finding["rule_id"], rule, caught.exception.finding)

    def test_design_example_defaults_and_all_provisional_profiles(self):
        # Include the abridged example's complete non-profile values from L0.
        example = merge(defaults(), self.project)
        validate_schema(example, config_schema())
        for profile, budget in (("default", 8000), ("prop", 8000), ("rock", 6000), ("foliage", 4000), ("tree", 20000)):
            with self.subTest(profile=profile):
                result = resolve(example, profile)
                self.assertEqual(result.effective["budgets"]["max_tris_final"], budget)
                self.assertEqual(result.effective["finish"]["decimate"]["max_tris"], budget)
                self.assertIn("config.budgets_uncalibrated", [f["rule_id"] for f in result.findings])
        self.assertEqual(resolve(example, "foliage").effective["finish"]["decimate"]["hausdorff_max_rel"], 0.02)
        self.assertEqual(resolve(example, "tree").effective["budgets"]["bounds_m"], [30, 30, 40])

    def test_layer_precedence_sources_and_selected_baseline(self):
        self.project["budgets"] = {"target_height_m": 1}
        self.project["profiles"]["tree"]["budgets"]["target_height_m"] = 4
        result = resolve(self.project, "tree", {"backends": {"mock": {"timeouts": {"job_s": 40}}}},
                         {"budgets": {"max_tris_final": 15000, "target_height_m": 3}})
        self.assertEqual(result.effective["budgets"]["max_tris_final"], 15000)
        self.assertEqual(result.effective["budgets"]["target_height_m"], 3)
        self.assertEqual(result.effective["backends"]["mock"]["timeouts"]["job_s"], 40)
        for path, layer in (("backend.id", "L2"), ("budgets.max_tris_final", "L4"),
                            ("backends.mock.timeouts.job_s", "L3"), ("limits.image_bytes", "L0")):
            self.assertEqual(result.sources[path], layer)
        self.refusal("config.override_denied", overrides={"budgets": {"max_tris_final": 9000}})
        self.assertEqual(resolve(self.project, "rock", overrides={"finish": {"decimate": {"max_tris": 4000}}}).effective["finish"]["decimate"]["max_tris"], 4000)

    def test_arrays_replace_and_recursive_objects_merge(self):
        result = merge({"a": {"b": 1, "c": [1, 2]}}, {"a": {"c": [3]}})
        self.assertEqual(result, {"a": {"b": 1, "c": [3]}})
        self.assertEqual(resolve(self.project, "tree").effective["budgets"]["bounds_m"], [30, 30, 40])

    def test_inheritance_three_including_selected_cycles_and_missing(self):
        self.project["profiles"]["third"] = {"inherits": "rock"}
        resolve(self.project, "third")
        self.project["profiles"]["fourth"] = {"inherits": "third"}
        self.refusal("config.out_of_bounds", profile="fourth")
        del self.project["profiles"]["fourth"]
        self.project["profiles"]["default"]["inherits"] = "rock"
        self.refusal("config.cycle")
        self.project = project_fixture()
        self.project["profiles"]["rock"]["inherits"] = "missing"
        self.refusal("config.profile_missing")
        self.project["profiles"]["rock"]["inherits"] = ["default", "prop"]
        self.refusal("config.type")

    def test_profile_must_be_explicit(self):
        for name in (None, "", "absent"):
            self.refusal("config.profile_missing", profile=name)

    def test_profile_policy_changes_and_dormant_typos_are_refused(self):
        self.project["profiles"]["unused"] = {"policy": {"validation_profile": "shipping"}}
        self.refusal("config.unknown_key")
        self.project["profiles"]["unused"] = {"finish": {"weld": {"typo": 1}}}
        self.refusal("config.unknown_key")

    def test_every_nested_object_is_closed(self):
        for path in ("policy.licence.unknown", "budgets.extra", "backends.mock.pins.other",
                     "backends.mock.token_source.token", "profiles.rock.finish.normals.extra",
                     "evaluation.gates.unregistered", "rig.arbitrary"):
            value = project_fixture()
            set_path(value, path, 1)
            with self.subTest(path=path), self.assertRaises(Refusal):
                resolve(value, "rock")

    def test_lock_patterns_and_profile_request_locks(self):
        for pattern in ("typo.*", "backends.m*ck.endpoint", "policy..licence", "*.*.*.*.*.*.*"):
            value = project_fixture()
            value["policy"]["locks"] = [pattern]
            self.refusal("config.unknown_key", project=value)
        self.project["policy"]["locks"] = ["budgets.max_tris_final"]
        self.refusal("config.locked")
        self.project["policy"]["locks"] = ["finish.loose"]
        self.refusal("config.locked", overrides={"finish": {"loose": {"policy": "drop_small"}}})
        self.refusal("config.locked", local={"backends": {"mock": {"endpoint": "http://127.0.0.1:12345"}}}, project=project_fixture())

    def test_tightening_vectors_caps_and_minimums(self):
        for path, item in (("budgets.bounds_m", [8, 8, 9]), ("timeouts.job_s", 601),
                           ("finish.decimate.hausdorff_max_rel", 0.02),
                           ("finish.decimate.open_sheet_min_retention", 0.7),
                           ("finish.decimate.thin_shell_p10_min_ratio", 0.4)):
            value = {}; set_path(value, path, item)
            self.refusal("config.override_denied", overrides=value)
        good = {"budgets": {"bounds_m": [4, 5, 6], "max_tris_final": 4000},
                "finish": {"decimate": {"open_sheet_min_retention": 0.9, "thin_shell_p10_min_ratio": 0.7}}}
        resolve(self.project, "rock", overrides=good)
        resolve(self.project, "foliage", overrides={"finish": {"decimate": {"hausdorff_max_rel": 0.015}}})

    def test_local_allowlist_and_backend_lower_only(self):
        for local in ({"budgets": {"max_tris_final": 5000}}, {"policy": {"locks": []}},
                      {"backends": {"mock": {"params": {"steps": 1}}}},
                      {"backends": {"direct3d_s2": {"enabled": True}}},
                      {"backends": {"mock": {"timeouts": {"job_s": 61}}}}):
            self.refusal("config.override_denied", local=local)
        local = {"backends": {"direct3d_s2": {"enabled": False}, "mock": {"token_source": {"file": "private/token"}}}}
        self.assertEqual(resolve(self.project, "rock", local).effective["backends"]["mock"]["token_source"], {"file": "private/token"})
        self.refusal("config.backend_missing", local={"backends": {"mock": {"enabled": False}}})

    def test_timeout_inheritance_remains_connected_after_global_override(self):
        result = resolve(self.project, "rock", overrides={"timeouts": {"connect_s": 2, "step_s": {"uv": 30}}})
        self.assertEqual(result.effective["backends"]["mock"]["timeouts"]["connect_s"], 2)
        self.assertEqual(result.effective["backends"]["mock"]["timeouts"]["step_s"]["uv"], 30)
        self.assertEqual(result.effective["backends"]["mock"]["timeouts"]["job_s"], 60)
        self.refusal("config.override_denied", local={"backends": {"mock": {"timeouts": {"job_s": 30}}}},
                     overrides={"backends": {"mock": {"timeouts": {"job_s": 40}}}})

    def test_bool_as_int_and_wrong_types(self):
        for path, value in (("candidates.count", True), ("limits.image_bytes", False),
                            ("budgets.max_tris_final", 6000.0), ("timeouts.connect_s", True),
                            ("finish.decimate.thin_shell_guard", 1)):
            layer = {}; set_path(layer, path, value)
            self.refusal("config.type", overrides=layer)
        self.project["config_schema_version"] = True
        self.refusal("config.schema_version")

    def test_nonfinite_duplicate_json_and_overflow(self):
        for data in (b'{"a":1,"a":2}', b'{"a":NaN}', b'{"a":Infinity}', b'{"a":1e999}'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                parse_json(data)
        for value in (float("nan"), float("inf"), -float("inf")):
            with self.assertRaises(ValueError):
                resolve(self.project, "rock", overrides={"timeouts": {"job_s": value}})

    def test_exact_positive_finite_vector(self):
        for vector in ([], [1, 2], [1, 2, 3, 4], [0, 1, 2], [-1, 1, 2], [True, 1, 2], [float("inf"), 1, 2]):
            with self.subTest(vector=vector), self.assertRaises(ValueError):
                resolve(self.project, "rock", overrides={"budgets": {"bounds_m": vector}})

    def test_bounds_hard_ceilings_and_refuse_instead_of_clamp(self):
        for path, value in (("limits.image_bytes", 16777217), ("timeouts.job_s", 1801),
                            ("limits.image_pixels", 16777217), ("limits.json_depth", 33),
                            ("workers.finisher.cpu_threads", 5), ("budgets.max_tris_final", 63)):
            layer = {}; set_path(layer, path, value)
            self.refusal("config.out_of_bounds", overrides=layer)
        for bounds in ({"limits.image_bytes": {"min": 1, "max": 16777217}},
                       {"budgets.max_tris_final": {"min": 6000, "max": 5000}},
                       {"finish.steps": {"min": 0, "max": 1}},
                       {"budgets.unknown": {"min": 0, "max": 1}}):
            self.project["policy"]["bounds"] = bounds
            with self.assertRaises(Refusal): resolve(self.project, "rock")
        self.project = project_fixture()
        self.project["policy"]["hard_ceilings"] = {"limits.image_bytes": 1}
        self.refusal("config.out_of_bounds")

    def test_endpoint_loopback_shape_and_port_range(self):
        for endpoint in ("http://localhost:18800", "http://127.0.0.2:18800", "https://127.0.0.1:18800",
                         "http://127.0.0.1:18800/", "http://127.0.0.1:0", "http://127.0.0.1:65536",
                         "http://user@127.0.0.1:18800", "http://[::1]:18800"):
            self.project["backends"]["mock"]["endpoint"] = endpoint
            self.refusal("config.out_of_bounds")
        for port in (1, 9, 65535):
            self.project["backends"]["mock"]["endpoint"] = f"http://127.0.0.1:{port:02d}"
            resolve(self.project, "rock")

    def test_token_reference_shapes_and_paths_without_secret_reads(self):
        for token in ({}, {"env": "1BAD"}, {"env": "GOOD", "file": "private/token"},
                      {"file": "../token"}, {"file": "/token"}, {"file": "a\\token"}, {"token": "secret"}):
            self.project["backends"]["mock"]["token_source"] = token
            with self.assertRaises(Refusal): resolve(self.project, "rock")
        self.project["backends"]["mock"]["token_source"] = {"file": "private/missing-token"}
        with patch("pathlib.Path.open", side_effect=AssertionError("No token IO")):
            resolve(self.project, "rock")

    def test_enabled_model_requires_real_pins_disabled_and_mock_exempt(self):
        resolve(self.project, "rock")
        self.project["backends"]["direct3d_s2"]["enabled"] = True
        self.refusal("config.pins_missing")
        self.project["backends"]["direct3d_s2"]["pins"] = {"model_revision": "a"*40, "weights_sha256": ["b"*64]}
        resolve(self.project, "rock")
        self.project["backends"]["direct3d_s2"]["pins"]["weights_sha256"] = ["placeholder"]
        with self.assertRaises(Refusal): resolve(self.project, "rock")

    def test_steps_and_decimate_cap(self):
        for steps in (list(STEPS)[::-1], list(STEPS) + ["stats"], list(STEPS)[1:]):
            self.project["profiles"]["rock"]["finish"]["steps"] = steps
            with self.assertRaises(Refusal): resolve(self.project, "rock")
        self.project = project_fixture()
        self.project["profiles"]["rock"]["finish"]["steps"] = [step for step in STEPS if step not in ("holes", "loose_pieces")]
        resolve(self.project, "rock")
        self.refusal("config.override_denied", overrides={"finish": {"decimate": {"max_tris": 6001}}})

    def test_explicit_l1_l2_targets_cannot_exceed_independent_final_cap(self):
        for layer in ("L1", "L2"):
            self.project = project_fixture()
            self.project["profiles"] = {"rock": {"backend": {"id": "mock"},
                                                "budgets": {"max_tris_final": 6000}}}
            target = self.project if layer == "L1" else self.project["profiles"]["rock"]
            target.setdefault("finish", {})["decimate"] = {"max_tris": 6001}
            with self.subTest(layer=layer):
                self.refusal("config.out_of_bounds")
            target["finish"]["decimate"]["max_tris"] = 6000
            self.assertEqual(resolve(self.project, "rock").effective["finish"]["decimate"]["max_tris"], 6000)

    def test_later_phase_helpers_and_unregistered_content_fail(self):
        for path, value, rule in (("finish.uv.method", "xatlas", "config.helper_unavailable"),
            ("finish.decimate.method", "meshopt_cli", "config.helper_unavailable"),
            ("texture.enabled", True, "config.execution_unavailable"),
            ("agent.tier_b.enabled", True, "tier_b.unavailable")):
            self.project = project_fixture()
            set_path(self.project, path, value)
            self.refusal(rule)
        self.project = project_fixture()
        self.refusal("config.override_denied", overrides={"texture": {"enabled": True}})

    def test_digest_canonical_sources_separate_token_rotation_and_machine_path(self):
        first = resolve(self.project, "rock")
        reordered = json.loads(json.dumps(self.project, sort_keys=True))
        self.assertEqual(first.config_digest, resolve(reordered, "rock").config_digest)
        with patch.dict(os.environ, {"LUMINUMBRA_GEN_TOKEN": "rotated-secret"}):
            self.assertEqual(first.config_digest, resolve(self.project, "rock").config_digest)
        different = resolve(self.project, "rock", {"backends": {"mock": {"token_source": {"file": "private/token"}}}})
        self.assertNotEqual(first.config_digest, different.config_digest)
        self.assertNotIn("sources", first.effective)
        self.assertNotIn("profiles", first.effective)
        self.project["profiles"]["prop"]["budgets"]["max_tris_final"] = 7000
        self.assertEqual(first.config_digest, resolve(self.project, "rock").config_digest)
        self.assertNotEqual(first.config_digest, first.stored_sha256)
        self.assertEqual(verify_effective_config(first.stored_bytes, first.stored_sha256, first.config_digest), first.effective)
        for data, byte_hash, identity in ((first.stored_bytes.rstrip(), first.stored_sha256, first.config_digest),
                                         (first.stored_bytes, first.stored_sha256, "0"*64)):
            with self.assertRaises(Refusal): verify_effective_config(data, byte_hash, identity)
        with self.assertRaises(Refusal): configuration_digest({**first.effective, "sources": first.sources})

    def test_unsupported_schema_keywords_and_local_references(self):
        for schema in ({"type": "object", "properties": {"x": {"format": "date"}}},
                       {"oneOf": [{"type": "integer"}, {"allOf": []}]}, {"$ref": "https://example/schema"}):
            with self.assertRaises(Refusal): validate_schema({}, schema)
        self.assertEqual(validate_schema(1, {"$defs": {"n": {"type": "integer"}}, "$ref": "#/$defs/n"}), 1)

    def test_dynamic_names_counts_and_empty_policy_override_are_strict(self):
        self.project["profiles"]["bad.name"] = {}
        self.refusal("config.type")
        self.project = project_fixture()
        for index in range(32): self.project["profiles"][f"extra{index}"] = {"inherits": "default"}
        self.refusal("config.out_of_bounds")
        self.project = project_fixture()
        self.refusal("config.override_denied", overrides={"policy": {}})
        self.refusal("config.override_denied", overrides={"profiles": {"empty": {}}})

    def test_no_in_place_mutation(self):
        before = deepcopy(self.project)
        first = resolve(self.project, "tree")
        first.effective["budgets"]["max_tris_final"] = 1
        self.assertEqual(self.project, before)
        self.assertEqual(resolve(self.project, "tree").effective["budgets"]["max_tris_final"], 20000)


class ProjectFiles(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.config = self.root / PROJECT_CONFIG
        self.config.parent.mkdir(parents=True)
        self.config.write_bytes(canonical(project_fixture()))

    def source_cli(self, profile="rock"):
        # -I ignores PYTHON* settings; -S prevents site startup, -B prevents writes.
        environment = dict(os.environ, PYTHONPATH=str(self.root / "source"),
                           PYTHONDONTWRITEBYTECODE="1", PYTHONPYCACHEPREFIX=str(self.root / "bytecode"),
                           TMPDIR=str(self.root), TMP=str(self.root), TEMP=str(self.root))
        return subprocess.run([sys.executable, "-I", "-S", "-B", str(self.root / "source" / "launch.py"),
            "--project", str(self.root / "game"), "--toolchain", str(self.root / "missing-toolchain"),
            "meshgen", "check-config", "--profile", profile, "--print-effective"],
            env=environment, cwd=self.root, capture_output=True, text=True, timeout=15)

    def prepare_source_cli(self):
        # Fresh writable source and cwd are both outside the game project.
        source = self.root / "source" / "luminumbra_author"
        source.mkdir(parents=True)
        for path in (SERVICE / "luminumbra_author").glob("*.py"):
            shutil.copyfile(path, source / path.name)
        shutil.copyfile(SERVICE / "launch.py", source.parent / "launch.py")
        config = self.root / "game" / PROJECT_CONFIG
        config.parent.mkdir(parents=True)
        config.write_bytes(self.config.read_bytes())
        return config

    def snapshot(self):
        return {p.relative_to(self.root).as_posix(): p.read_bytes() if p.is_file() else None
                for p in self.root.rglob("*")}

    def test_isolated_source_cli_has_no_writes_inside_or_outside_project(self):
        self.prepare_source_cli()
        before = self.snapshot()
        for profile, expected in (("rock", 0), ("absent", 1)):
            result = self.source_cli(profile)
            self.assertEqual(result.returncode, expected, result.stderr + result.stdout)
            self.assertEqual(json.loads(result.stdout)["ok"], expected == 0)
            self.assertEqual(self.snapshot(), before)
        self.assertFalse(list(self.root.rglob("__pycache__")))
        self.assertFalse((self.root / "bytecode").exists())

    def test_check_config_launcher_never_executes_project_or_cwd_startup_hooks(self):
        self.prepare_source_cli()
        sentinel = self.root / "startup-executed"
        source = f"open({str(sentinel)!r}, 'w').write('ran')\n"
        for location in (self.root, self.root / "game"):
            for name in ("ast.py", "sitecustomize.py", "usercustomize.py"):
                (location / name).write_text(source)
            (location / "startup.pth").write_text("import builtins; " + source)
        before = self.snapshot()
        result = self.source_cli()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertTrue(json.loads(result.stdout)["ok"])
        self.assertFalse(sentinel.exists())
        self.assertEqual(self.snapshot(), before)

    def test_cli_huge_integer_returns_structured_out_of_bounds(self):
        config = self.prepare_source_cli()
        project = project_fixture()
        project["limits"] = {"image_bytes": 10**400}
        config.write_bytes(canonical(project))
        before = self.snapshot()
        result = self.source_cli()
        self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
        self.assertEqual(result.stderr, "")
        finding = json.loads(result.stdout)["findings"][0]
        self.assertEqual(finding["rule_id"], "config.out_of_bounds")
        self.assertEqual(finding["field"], "limits.image_bytes")
        self.assertEqual(self.snapshot(), before)

    def test_attestation_hash_bound_notes_and_worker_locations(self):
        project = project_fixture()
        notes = self.config.parent / "notes"
        notes.mkdir()
        note = notes / "revenue.md"
        note.write_bytes(b"Human reviewed internal revenue attestation.\n")
        project["policy"]["licence"] = {"attestations": {"revenue_under_cap": {"value": True,
            "attested_by": "reviewer", "attested_at": "2026-10-13", "note_id": "revenue", "note_sha256": digest(note.read_bytes())}}}
        with self.assertRaises(Refusal): resolve(project, "rock")
        resolved = resolve(project, "rock", project_root=self.root)
        self.assertTrue(resolved.effective["policy"]["licence"]["attestations"]["revenue_under_cap"]["value"])
        note.write_bytes(b"changed")
        with self.assertRaises(Refusal) as caught: resolve(project, "rock", project_root=self.root)
        self.assertEqual(caught.exception.finding["rule_id"], "config.attestation_unverified")
        worker = self.root / "worker"
        worker.write_bytes(b"not executed")
        result = resolve(project_fixture(), "rock", {"workers": {"finisher": {"python": str(worker)}}}, project_root=self.root)
        self.assertEqual(result.effective["workers"]["finisher"]["python"], str(worker))
        with self.assertRaises(Refusal): resolve(project_fixture(), "rock", {"workers": {"finisher": {"python": str(worker)+"missing"}}}, project_root=self.root)

    def test_file_layer_duplicate_keys_and_symlinks(self):
        local = self.root / LOCAL_CONFIG
        local.write_bytes(b'{"workers":{},"workers":{}}')
        with self.assertRaises(Refusal): load_config(self.root, "rock")
        local.unlink()
        if hasattr(os, "symlink"):
            try: local.symlink_to(self.config)
            except OSError: self.skipTest("Symlinks unavailable")
            with self.assertRaises(Refusal): load_config(self.root, "rock")

    def test_cli_side_effect_free_success_and_structured_failure(self):
        from luminumbra_author import __main__
        from contextlib import redirect_stdout
        import io
        before = sorted(str(path.relative_to(self.root)) for path in self.root.rglob("*"))
        argv = ["service", "--project", str(self.root), "--toolchain", str(self.root/"missing-toolchain"),
                "meshgen", "check-config", "--profile", "rock", "--print-effective"]
        with patch.object(sys, "argv", argv), patch.object(__main__, "BuildService", side_effect=AssertionError("No build session")), \
             redirect_stdout(io.StringIO()) as output:
            self.assertEqual(__main__.main(), 0)
        result = json.loads(output.getvalue())
        self.assertTrue(result["ok"])
        self.assertEqual(result["result"]["effective_config"]["budgets"]["max_tris_final"], 6000)
        self.assertEqual(result["result"]["backend_validation"], "unavailable")
        self.assertEqual(before, sorted(str(path.relative_to(self.root)) for path in self.root.rglob("*")))
        argv[-2:] = ["absent"]  # Keep --profile and replace its value, remove print flag.
        with patch.object(sys, "argv", argv), redirect_stdout(io.StringIO()) as output:
            self.assertEqual(__main__.main(), 1)
        result = json.loads(output.getvalue())
        self.assertEqual(result["findings"][0]["rule_id"], "config.profile_missing")
        self.assertEqual(set(result["findings"][0]), {"rule_id", "severity", "message", "field", "object_id", "component_id", "node_id"})


class Packaging(unittest.TestCase):
    def test_shipped_modules_include_actual_sibling_imports_and_hashed_code(self):
        spec = importlib.util.spec_from_file_location("service_package", SERVICE/"package.py")
        package = importlib.util.module_from_spec(spec); spec.loader.exec_module(package)
        # Source siblings identify module names; dependencies come from imports
        # in the built archive, including imports inside functions.
        siblings = {path.stem for path in (SERVICE/"luminumbra_author").glob("*.py")}
        with tempfile.TemporaryDirectory() as temporary:
            app = package.build(Path(temporary)/"author.pyz")
            with zipfile.ZipFile(app) as archive:
                shipped = {Path(name).stem for name in archive.namelist()
                           if name.startswith("luminumbra_author/") and name.endswith(".py")}
                for name in sorted(shipped):
                    tree = ast.parse(archive.read(f"luminumbra_author/{name}.py"))
                    for node in ast.walk(tree):
                        imports = []
                        if isinstance(node, ast.Import):
                            imports = [alias.name.removeprefix("luminumbra_author.")
                                       for alias in node.names if alias.name.startswith("luminumbra_author.")]
                        elif isinstance(node, ast.ImportFrom):
                            module = node.module or ""
                            if node.level == 1 or module == "luminumbra_author" or module.startswith("luminumbra_author."):
                                module = module.removeprefix("luminumbra_author").lstrip(".")
                                imports = [module] if module else [alias.name for alias in node.names]
                        for imported in imports:
                            sibling = imported.split(".")[0]
                            if sibling in siblings:
                                with self.subTest(module=name, sibling=sibling):
                                    self.assertIn(sibling, shipped, "Imported sibling is missing from the archive")
                    # Build identities must be able to hash every named resource.
                    for node in ast.walk(tree):
                        if isinstance(node, ast.Assign) and any(
                                isinstance(target, ast.Name) and target.id == "service_hashes" for target in node.targets):
                            for literal in ast.walk(node.value):
                                if isinstance(literal, ast.Constant) and isinstance(literal.value, str) and literal.value.endswith(".py"):
                                    self.assertIn("luminumbra_author/" + literal.value, archive.namelist())

    def test_registry_and_packaged_schema_defaults_are_identical(self):
        validate_schema(defaults(), config_schema())
        resources = {**packaged_schemas(), "meshgen.defaults.v1.json": defaults()}
        for name, value in resources.items():
            self.assertEqual(json.loads((SERVICE/"luminumbra_author"/"schemas"/name).read_text()), value)
        self.assertIn("workers.finisher.memory_mib", FIELDS)
        self.assertIn("workers.finisher.cpu_threads", FIELDS)

    def test_installed_zipapp_contains_resources_and_runs_read_only(self):
        spec = importlib.util.spec_from_file_location("service_package", SERVICE/"package.py")
        package = importlib.util.module_from_spec(spec); spec.loader.exec_module(package)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            import shutil
            source = root/"source"; source.mkdir()
            for name in package.SERVICE_MODULES:
                shutil.copyfile(SERVICE/"luminumbra_author"/name, source/name)
            (source/"finish_candidate.py").write_text("import bpy\n")
            (source/"provenance.json").write_text('{}')
            (source/"candidates").mkdir()
            (source/"candidates/candidate.glb").write_bytes(b"excluded")
            with patch.object(package, "SOURCE_DIRECTORY", source):
                app = package.build(root/"author.pyz")
            with zipfile.ZipFile(app) as archive:
                for name, value in packaged_schemas().items():
                    self.assertEqual(json.loads(archive.read("luminumbra_author/schemas/"+name)), value)
                self.assertFalse(any("provenance.json" in name or "finish_candidate.py" in name for name in archive.namelist()))
            project = root/"game"; config = project/PROJECT_CONFIG
            config.parent.mkdir(parents=True); config.write_bytes(canonical(project_fixture()))
            before = sorted(str(path.relative_to(project)) for path in project.rglob("*"))
            result = subprocess.run([sys.executable, "-I", "-S", "-B", str(app), "--project", str(project), "--toolchain", str(root/"absent"),
                "meshgen", "check-config", "--profile", "tree", "--print-effective"], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertEqual(json.loads(result.stdout)["result"]["effective_config"]["budgets"]["max_tris_final"], 20000)
            self.assertEqual(before, sorted(str(path.relative_to(project)) for path in project.rglob("*")))

    def test_new_runtime_modules_use_only_stdlib_and_service_imports(self):
        import ast
        for name in ("meshgen_registry", "meshgen_config", "meshgen_contracts", "meshgen_scripts",
                     "meshgen_licence", "meshgen_backend", "meshgen_jobs"):
            tree = ast.parse((SERVICE/"luminumbra_author"/(name+".py")).read_text())
            for node in ast.walk(tree):
                names = [item.name for item in node.names] if isinstance(node, ast.Import) else (
                    [node.module] if isinstance(node, ast.ImportFrom) and not node.level else [])
                for imported in names:
                    self.assertIn(imported.split(".")[0], sys.stdlib_module_names)

    def test_engine_extension_archive_excludes_game_scripts_and_provenance(self):
        spec = importlib.util.spec_from_file_location("extension_package", SERVICE.parent/"package_extension.py")
        package = importlib.util.module_from_spec(spec); spec.loader.exec_module(package)
        with tempfile.TemporaryDirectory() as temporary:
            archive_path = package.build(Path(temporary)/"extension.zip")
            with zipfile.ZipFile(archive_path) as archive:
                self.assertFalse(any("meshgen" in name or "provenance" in name or "candidate" in name for name in archive.namelist()))
                self.assertEqual(archive.read("viewport/file_io.py"),
                                 (SERVICE/"luminumbra_author/file_io.py").read_bytes())

    def test_new_files_pass_public_tree_guard(self):
        repo = SERVICE.parents[3]
        spec = importlib.util.spec_from_file_location("public_tree_guard", repo/"tools/ci/validate_public_tree.py")
        module = importlib.util.module_from_spec(spec); sys.modules[spec.name] = module; spec.loader.exec_module(module)
        files = list((SERVICE/"luminumbra_author").glob("meshgen_*.py")) + list((SERVICE/"tests").glob("test_meshgen*.py"))
        for path in files:
            relative = path.relative_to(repo).as_posix()
            self.assertEqual(module.inspect_path(relative) + module.inspect_text(relative, path.read_text()), [], relative)

    def test_existing_ci_discovers_new_service_tests(self):
        workflow = (SERVICE.parents[3]/".github/workflows/blender-authoring.yml").read_text()
        self.assertIn("unittest discover -s tools/blender/authoring/service/tests", workflow)
        self.assertIn("ubuntu-24.04", workflow)
        self.assertIn("windows-2025", workflow)


if __name__ == "__main__":
    unittest.main()

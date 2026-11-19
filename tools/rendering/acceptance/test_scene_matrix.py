#!/usr/bin/env python3
"""Unit tests for scene_matrix.py: schema, generator output, and engine-facing names.

Flags, scene-config keys and isolation tokens are cross-checked against the C++
sources by parsing quoted literals, so a flag the client does not read fails here.
Run from anywhere: python3 -I -m unittest discover -s tools/rendering/acceptance -p 'test_*.py'
"""

import contextlib
import copy
import io
import json
import re
import shlex
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
SRC = REPO_ROOT / "src" / "luminumbra_client"
MATRIX_PATH = HERE / "scene_matrix.json"
VALIDATOR_PATH = REPO_ROOT / "tools" / "perf" / "validate_render_capture.py"

sys.dont_write_bytecode = True  # never leave bytecode in the source tree
sys.path.insert(0, str(HERE))
import scene_matrix as sm  # noqa: E402

# Independent identity oracle: changing a checked-in fixture must be intentional.
CANONICAL_CAMERAS = {
    "elevated": {"pos": [8, 56, 8], "yaw": 35, "pitch": -6, "fov": 110},
    "ground": {"pos": [8, 19.1, 8], "yaw": 35, "pitch": -6, "fov": 110},
    "high": {"pos": [8, 112, 8], "yaw": 35, "pitch": -6, "fov": 110},
    "cave_air": {"pos": [8.469266, -46.070644, 11.695518], "yaw": 0, "pitch": 0, "fov": 110},
}
BASE_WEATHER = {"type": "none", "intensity": 0.0}
WEATHER_VARIANTS = {
    "clear": BASE_WEATHER,
    **{kind: {"type": kind, "intensity": 1.0} for kind in ("rain", "snow", "storm")},
}
ISOLATION_VARIANTS = {
    "no-aerial": [t for t in sm.FULL_LAYERS if t != "aerial"],
    "no-particles": [t for t in sm.FULL_LAYERS if t != "particles"],
    "no-water": [t for t in sm.FULL_LAYERS if t != "water"],
    "terrain-only": ["terrain"],
}
EXPECTED_NAMES = {
    sid + suffix for sid in CANONICAL_CAMERAS
    for suffix in ("", *("." + vid for vid in WEATHER_VARIANTS),
                   *("." + vid for vid in ISOLATION_VARIANTS if sid == "elevated"))
}
EXPECTED_TRANSITIONS = [
    {"id": "surface-camera", "kind": "camera",
     "endpoints": ["ground", "elevated", "high", "elevated", "ground"]},
    {"id": "cave-camera", "kind": "camera", "endpoints": ["ground", "cave_air", "ground"]},
    {"id": "near-far-lod", "kind": "lod", "endpoints": ["elevated", "high", "elevated"]},
    *[{"id": sid + "-weather", "kind": "weather",
       "endpoints": [sid, sid + ".rain", sid + ".snow", sid + ".storm", sid + ".clear"]}
      for sid in CANONICAL_CAMERAS],
]

QUOTED_FLAG = re.compile(r'"(--[a-z0-9-]+)"')


def _client_flag_literals():
    sources = [SRC / "main_client.cpp"]
    sources += sorted((SRC).glob("**/RuntimeScenarioConfig.cpp"))
    found = set()
    for path in sources:
        found.update(QUOTED_FLAG.findall(path.read_text(encoding="utf-8")))
    return found


def _scene_config_literals():
    """Quoted literals inside the --scene-config parse block of main_client.cpp."""
    text = (SRC / "main_client.cpp").read_text(encoding="utf-8")
    start = text.index('GetCommandLineOption(argc, argv, "--scene-config", "")')
    end = text.index("// framescan", start)
    return set(re.findall(r'"([a-z_]+)"', text[start:end]))


def _isolation_literals():
    text = (SRC / "core" / "IsolationConfig.h").read_text(encoding="utf-8")
    return set(re.findall(r'name == "([a-z_]+)"', text))


def _validator_flags():
    text = VALIDATOR_PATH.read_text(encoding="utf-8")
    return set(re.findall(r"add_argument\(['\"](--[a-z0-9-]+)['\"]", text))


def _json_keys(value):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key
            yield from _json_keys(child)
    elif isinstance(value, list):
        for child in value:
            yield from _json_keys(child)


def _load_matrix():
    return json.loads(MATRIX_PATH.read_text(encoding="utf-8"))


def _run(argv):
    stderr = io.StringIO()
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
        code = sm.main(argv)
    return code, stderr.getvalue()


class MatrixSchemaTests(unittest.TestCase):
    def test_checked_in_matrix_is_valid(self):
        sm.validate_matrix(_load_matrix())

    def test_scene_ids_and_world(self):
        data = _load_matrix()
        self.assertEqual(data["schema"], "luminumbra.scene_matrix.v1")
        self.assertEqual(data["world"], {"preset": "default", "world_seed_implied": 424242})
        self.assertEqual(
            [scene["id"] for scene in data["scenes"]],
            ["elevated", "ground", "high", "cave_air"],
        )

    def test_elevated_pose_is_the_g04_pose(self):
        elevated = _load_matrix()["scenes"][0]
        self.assertEqual(elevated["pose_source"], "explicit")
        camera = elevated["scene"]["camera"]
        self.assertEqual(camera, {"pos": [8, 56, 8], "yaw": 35, "pitch": -6, "fov": 110})
        self.assertEqual(elevated["scene"]["time_of_day"], 0.04)

    def test_all_required_scenes_resolve_completely(self):
        data = _load_matrix()
        sm.validate_matrix(data)
        entries, unresolved = sm.plan_entries(data)
        self.assertEqual(unresolved, [])
        self.assertEqual({entry["name"] for entry in entries}, EXPECTED_NAMES)
        for scene in data["scenes"]:
            self.assertEqual(scene["pose_source"], "explicit", scene["id"])
            self.assertEqual(scene["scene"]["camera"], CANONICAL_CAMERAS[scene["id"]])
            self.assertEqual(scene["scene"]["time_of_day"], 0.04)
            self.assertEqual(scene["scene"]["weather"], BASE_WEATHER)
        self.assertEqual(data["transitions"], EXPECTED_TRANSITIONS)

    def test_qualification_workloads_use_the_same_authored_poses(self):
        path = REPO_ROOT / "tools/perf/qualification/matrix.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        workloads = {w["id"]: w for w in data["workloads"]}
        self.assertEqual(data["time_of_day"], 0.04)
        for sid, wid in (("ground", "ground-settled"), ("high", "high-altitude-settled"),
                         ("cave_air", "cave-air-settled")):
            camera = CANONICAL_CAMERAS[sid]
            self.assertEqual(workloads[wid]["pose"],
                             dict(zip(("x", "y", "z"), camera["pos"]),
                                  yaw=camera["yaw"], pitch=camera["pitch"]))

    def test_variants_use_only_accepted_layer_tokens(self):
        accepted = _isolation_literals()
        for scene in _load_matrix()["scenes"]:
            for variant in scene["variants"]:
                for token in variant.get("layers", []) + variant.get("exclude_layers", []):
                    self.assertIn(token, sm.FULL_LAYERS)
                    self.assertIn(token, accepted)

    def test_full_layers_are_isolation_tokens(self):
        self.assertTrue(set(sm.FULL_LAYERS) <= _isolation_literals())

    def test_no_world_seed_flag_anywhere(self):
        self.assertNotIn("--world-seed", MATRIX_PATH.read_text(encoding="utf-8"))

    def test_rejects_unknown_layer_token(self):
        data = _load_matrix()
        data["scenes"][0]["variants"][0]["layers"] = ["terrain", "bogus"]
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(data)

    def test_rejects_wrong_schema_and_duplicate_scene(self):
        data = _load_matrix()
        bad = copy.deepcopy(data)
        bad["schema"] = "luminumbra.scene_matrix.v0"
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(bad)
        dup = copy.deepcopy(data)
        dup["scenes"].append(copy.deepcopy(dup["scenes"][0]))
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(dup)

    def test_rejects_non_finite_and_out_of_range_values(self):
        data = _load_matrix()
        data["scenes"][0]["scene"]["camera"]["pos"][1] = float("nan")
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(data)
        data = _load_matrix()
        data["scenes"][0]["scene"]["camera"]["pitch"] = 120
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(data)
        data = _load_matrix()
        data["scenes"][0]["scene"]["time_of_day"] = 1.0
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(data)

    def test_rejects_recorded_scene_with_pose_and_explicit_without_pose(self):
        data = _load_matrix()
        data["scenes"][1]["pose_source"] = "recorded-evidence"
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(data)
        data = _load_matrix()
        data["scenes"][0]["scene"]["camera"]["pos"] = None
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(data)

    def test_rejects_unknown_weather_type(self):
        data = _load_matrix()
        data["scenes"][0]["variants"][1]["weather"]["type"] = "hail"
        with self.assertRaises(sm.MatrixError):
            sm.validate_matrix(data)

    def test_weather_identity_must_be_complete_in_bases_and_variants(self):
        for value in ({}, {"type": "rain"}, {"intensity": 1.0},
                      {"type": "rain", "intensity": 1.0, "unknown": 1}, [], None):
            for variant in (False, True):
                with self.subTest(value=value, variant=variant):
                    data = _load_matrix()
                    target = (data["scenes"][0]["variants"][0] if variant
                              else data["scenes"][0]["scene"])
                    target["weather"] = value
                    with self.assertRaisesRegex(sm.MatrixError, "scene elevated.*weather"):
                        sm.validate_matrix(data)
        data = _load_matrix()
        del data["scenes"][0]["scene"]["weather"]
        with self.assertRaisesRegex(sm.MatrixError, "scene elevated.*weather"):
            sm.validate_matrix(data)


class FormatTests(unittest.TestCase):
    def test_numbers_round_trip_exactly(self):
        self.assertEqual(float(sm._fmt(56.123456)), 56.123456)
        self.assertEqual(float(sm._fmt(0.9999999)), 0.9999999)
        self.assertEqual(sm._fmt(8), "8")


class VariantSemanticsTests(unittest.TestCase):
    def test_variant_layers_replace_base_layers(self):
        scene = {"id": "x", "pose_source": "explicit", "scene": {}, "base_layers": ["terrain", "water"]}
        self.assertEqual(sm._variant_layers(scene["base_layers"], {"id": "v", "layers": ["terrain"]}), ["terrain"])

    def test_variant_without_layers_inherits_base(self):
        self.assertEqual(
            sm._variant_layers(["terrain", "water"], {"id": "v", "weather": {"type": "none"}}),
            ["terrain", "water"],
        )

    def test_exclude_expands_from_full_list(self):
        layers = sm._variant_layers(["terrain"], {"id": "v", "exclude_layers": ["aerial"]})
        self.assertEqual(layers, [t for t in sm.FULL_LAYERS if t != "aerial"])


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.out = Path(self._tmp.name) / "out"

    def _emit_real(self):
        code, err = _run(["--matrix", str(MATRIX_PATH), "--out", str(self.out)])
        self.assertEqual(code, 0, err)
        return sorted(p.name for p in self.out.glob("*.scene.json"))

    def test_all_required_recipes_emit_exact_requested_identities(self):
        self.assertEqual(self._emit_real(), sorted(name + ".scene.json" for name in EXPECTED_NAMES))
        manifest = json.loads((self.out / "recipes.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "luminumbra.scene_recipes.v1")
        self.assertEqual(manifest["transitions"], EXPECTED_TRANSITIONS)
        self.assertEqual(len(manifest["recipes"]), len(EXPECTED_NAMES))
        self.assertEqual({r["name"] for r in manifest["recipes"]}, EXPECTED_NAMES)
        lines = [shlex.split(line) for line in (self.out / "commands.txt").read_text(
            encoding="utf-8").splitlines() if line and not line.startswith("#")]
        for recipe in manifest["recipes"]:
            sid, _, vid = recipe["name"].partition(".")
            camera = CANONICAL_CAMERAS[sid]
            weather = WEATHER_VARIANTS.get(vid, BASE_WEATHER)
            path = self.out / (recipe["name"] + ".scene.json")
            self.assertEqual(recipe, {
                "name": recipe["name"], "scene": sid, "variant": vid or "base",
                "preset": "default", "seed": 424242, "camera": camera,
                "time_of_day": 0.04, "weather": weather,
                "layers": ISOLATION_VARIANTS.get(vid), "scene_config": path.as_posix(),
            })
            config = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(config, {
                "camera": camera, "time_of_day": 0.04, "weather": weather,
                "screenshot": str(self.out / (recipe["name"] + ".ppm")),
            })
            clients = [argv for argv in lines if "--scene-config" in argv
                       and argv[argv.index("--scene-config") + 1] == path.as_posix()]
            self.assertEqual(len(clients), 2)
            for argv in clients:
                self.assertEqual(argv[argv.index("--world-preset") + 1], "default")
                if recipe["layers"] is None:
                    self.assertNotIn("--isolation-layers", argv)
                else:
                    self.assertEqual(argv[argv.index("--isolation-layers") + 1],
                                     ",".join(recipe["layers"]))
            report = self.out / (recipe["name"] + ".render_benchmark.json")
            validators = [argv for argv in lines if len(argv) > 2
                          and argv[1] == sm.VALIDATOR and argv[2] == report.as_posix()]
            self.assertEqual(len(validators), 1)
            argv = validators[0]
            pos_index = argv.index("--position")
            self.assertEqual([float(v) for v in argv[pos_index + 1:pos_index + 4]], camera["pos"])
            for flag, expected in (("--yaw", camera["yaw"]), ("--pitch", camera["pitch"]),
                                   ("--fov", 110), ("--tod", 0.04)):
                self.assertEqual(float(argv[argv.index(flag) + 1]), expected)
        self.assertTrue((self.out / "commands.txt").is_file())

    def test_elevated_config_contents(self):
        self._emit_real()
        config = json.loads((self.out / "elevated.scene.json").read_text(encoding="utf-8"))
        self.assertEqual(
            config["camera"], {"pos": [8, 56, 8], "yaw": 35, "pitch": -6, "fov": 110}
        )
        self.assertEqual(config["time_of_day"], 0.04)
        self.assertEqual(config["weather"], BASE_WEATHER)
        self.assertTrue(Path(config["screenshot"]).is_absolute())
        self.assertEqual(Path(config["screenshot"]).name, "elevated.ppm")

    def test_storm_variant_and_layer_variants(self):
        self._emit_real()
        storm = json.loads((self.out / "elevated.storm.scene.json").read_text(encoding="utf-8"))
        self.assertEqual(storm["weather"], {"type": "storm", "intensity": 1.0})
        terrain = json.loads((self.out / "elevated.terrain-only.scene.json").read_text(encoding="utf-8"))
        self.assertEqual(terrain["weather"], BASE_WEATHER)
        commands = (self.out / "commands.txt").read_text(encoding="utf-8")
        self.assertIn("--isolation-layers terrain", commands)
        no_aerial = ",".join(t for t in sm.FULL_LAYERS if t != "aerial")
        self.assertIn(f"--isolation-layers {no_aerial}", commands)

    def test_unique_screenshot_paths(self):
        self._emit_real()
        shots = [
            json.loads(p.read_text(encoding="utf-8"))["screenshot"]
            for p in self.out.glob("*.scene.json")
        ]
        self.assertGreater(len(shots), 1)
        self.assertEqual(len(shots), len(set(shots)))

    def test_every_block_is_marked_needs_native_run(self):
        self._emit_real()
        lines = (self.out / "commands.txt").read_text(encoding="utf-8").splitlines()
        marks = [line for line in lines if "NEEDS-NATIVE-RUN" in line]
        self.assertEqual(len(marks), 2 * len(EXPECTED_NAMES))
        for line in marks:
            self.assertTrue(line.startswith("#"), line)

    def test_each_scene_emits_benchmark_and_screenshot_forms(self):
        self._emit_real()
        commands = [
            line
            for line in (self.out / "commands.txt").read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
        self.assertEqual(len(commands), 3 * len(EXPECTED_NAMES))
        benchmark = [c for c in commands if "--render-benchmark" in c]
        self.assertEqual(len(benchmark), len(EXPECTED_NAMES))
        validators = [c for c in commands
                      if shlex.split(c)[:2] == [Path(sys.executable).as_posix(), sm.VALIDATOR]]
        self.assertEqual(len(validators), len(EXPECTED_NAMES))
        for line in validators:
            self.assertIn("--require-settled", line)

    def test_no_world_seed_flag_emitted(self):
        self._emit_real()
        self.assertNotIn("--world-seed", (self.out / "commands.txt").read_text(encoding="utf-8"))

    def test_each_unresolved_required_pose_names_scene_before_writing(self):
        for sid in CANONICAL_CAMERAS:
            with self.subTest(scene=sid):
                data = _load_matrix()
                scene = next(s for s in data["scenes"] if s["id"] == sid)
                scene["pose_source"] = "recorded-evidence"
                scene["scene"]["camera"].update(pos=None, yaw=None, pitch=None)
                scene["scene"]["time_of_day"] = None
                data["scenes"] = [scene]
                data["transitions"] = []
                matrix = Path(self._tmp.name) / "unresolved.json"
                matrix.write_text(json.dumps(data), encoding="utf-8")
                code, err = _run(["--matrix", str(matrix), "--out", str(self.out)])
                self.assertEqual(code, sm.EXIT_UNRESOLVED, err)
                self.assertIn(sid, err)
                self.assertFalse(self.out.exists())
                # Explicitly requested partial output is empty, never a substituted pose.
                code, err = _run(["--matrix", str(matrix), "--out", str(self.out),
                                  "--allow-unresolved"])
                self.assertEqual(code, 0, err)
                self.assertEqual(list(self.out.glob("*.scene.json")), [])
                self.assertEqual(json.loads((self.out / "recipes.json").read_text(
                    encoding="utf-8"))["recipes"], [])
                for path in self.out.iterdir():
                    path.unlink()
                self.out.rmdir()

    def test_each_null_required_identity_names_scene_before_writing(self):
        fields = (("camera", "pos"), ("camera", "yaw"), ("camera", "pitch"),
                  ("camera", "fov"), ("time_of_day",), ("weather",),
                  ("weather", "type"), ("weather", "intensity"),
                  *(("camera", "pos", axis) for axis in range(3)))
        for sid in CANONICAL_CAMERAS:
            for field in fields:
                with self.subTest(scene=sid, field=field):
                    data = _load_matrix()
                    node = next(s for s in data["scenes"] if s["id"] == sid)["scene"]
                    for key in field[:-1]:
                        node = node[key]
                    node[field[-1]] = None
                    matrix = Path(self._tmp.name) / "null-identity.json"
                    matrix.write_text(json.dumps(data), encoding="utf-8")
                    code, err = _run(["--matrix", str(matrix), "--out", str(self.out)])
                    self.assertEqual(code, sm.EXIT_INVALID, err)
                    self.assertIn(f"scene {sid}", err)
                    self.assertFalse(self.out.exists())

    def test_bad_transitions_are_refused_before_writing(self):
        invalid = [None, {}, {"id": None, "kind": "camera", "endpoints": ["ground", "high"]},
                   {"id": "bad", "kind": "unknown", "endpoints": ["ground", "high"]},
                   {"id": "bad", "kind": "camera", "endpoints": None},
                   {"id": "bad", "kind": "camera", "endpoints": []},
                   {"id": "bad", "kind": "camera", "endpoints": ["ground"]},
                   {"id": "bad", "kind": "camera", "endpoints": ["ground", "missing"]},
                   {"id": "bad", "kind": "camera", "endpoints": ["ground", None]}]
        for transition in invalid:
            with self.subTest(transition=transition):
                data = _load_matrix()
                data["transitions"] = [transition]
                matrix = Path(self._tmp.name) / "bad-transition.json"
                matrix.write_text(json.dumps(data), encoding="utf-8")
                code, err = _run(["--matrix", str(matrix), "--out", str(self.out)])
                self.assertEqual(code, sm.EXIT_INVALID, err)
                self.assertIn("transition", err)
                self.assertFalse(self.out.exists())
        data = _load_matrix()
        for transitions in (None, [data["transitions"][0]] * 2):
            data["transitions"] = transitions
            with self.assertRaises(sm.MatrixError):
                sm.validate_matrix(data)

    def test_refusals_preserve_existing_output(self):
        self.out.mkdir()
        sentinel = self.out / "recipes.json"
        sentinel.write_bytes(b"prior evidence\n")
        bad = _load_matrix()
        bad["scenes"][1]["scene"]["camera"]["yaw"] = None
        partial = Path(self._tmp.name) / "partial.json"
        partial.write_text("{", encoding="utf-8")
        invalid = Path(self._tmp.name) / "invalid.json"
        invalid.write_text(json.dumps(bad), encoding="utf-8")
        missing = Path(self._tmp.name) / "missing.json"
        for path in (missing, partial, invalid):
            with self.subTest(path=path.name):
                code, err = _run(["--matrix", str(path), "--out", str(self.out)])
                self.assertEqual(code, sm.EXIT_INVALID, err)
                self.assertEqual(list(self.out.iterdir()), [sentinel])
                self.assertEqual(sentinel.read_bytes(), b"prior evidence\n")

    def test_legacy_matrix_without_transitions_emits_no_transition_claim(self):
        data = _load_matrix()
        del data["transitions"]
        matrix = Path(self._tmp.name) / "legacy.json"
        matrix.write_text(json.dumps(data), encoding="utf-8")
        code, err = _run(["--matrix", str(matrix), "--out", str(self.out)])
        self.assertEqual(code, 0, err)
        manifest = json.loads((self.out / "recipes.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["transitions"], [])
        self.assertEqual({r["name"] for r in manifest["recipes"]}, EXPECTED_NAMES)

    def test_unknown_layer_token_is_rejected_by_generator(self):
        data = _load_matrix()
        data["scenes"][0]["variants"][0]["layers"] = ["terrain", "nope"]
        matrix = Path(self._tmp.name) / "bad.json"
        matrix.write_text(json.dumps(data), encoding="utf-8")
        code, err = _run(["--matrix", str(matrix), "--out", str(self.out)])
        self.assertEqual(code, 1)
        self.assertIn("nope", err)
        self.assertFalse((self.out / "commands.txt").exists())

    def test_build_dir_is_substituted(self):
        build_dir = self.out.parent / "lum build"
        code, _ = _run(
            [
                "--matrix",
                str(MATRIX_PATH),
                "--out",
                str(self.out),
                "--allow-unresolved",
                "--build-dir",
                str(build_dir),
            ]
        )
        self.assertEqual(code, 0)
        text = (self.out / "commands.txt").read_text(encoding="utf-8")
        commands = [shlex.split(line) for line in text.splitlines()
                    if line and not line.startswith("#")]
        clients = [argv for argv in commands if "--scene-config" in argv]
        self.assertEqual(len(clients), 2 * len(EXPECTED_NAMES))
        for argv in clients:
            self.assertEqual(argv[0], (build_dir / "bin" / sm.CLIENT_BINARY).as_posix())
            self.assertTrue(Path(argv[argv.index("--scene-config") + 1]).is_absolute())


class EngineContractTests(unittest.TestCase):
    """Every emitted token must be read by the engine or validator sources."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.out = Path(self._tmp.name) / "out"
        code, err = _run(["--matrix", str(MATRIX_PATH), "--out", str(self.out)])
        self.assertEqual(code, 0, err)

    def _command_lines(self):
        return [
            shlex.split(line)
            for line in (self.out / "commands.txt").read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]

    def test_emitted_client_and_validator_flags_are_literals(self):
        client = _client_flag_literals()
        validator = _validator_flags()
        self.assertIn("--scene-config", client)
        self.assertIn("--render-benchmark", client)
        self.assertIn("--require-settled", validator)
        seen = set()
        for argv in self._command_lines():
            if argv[0].endswith(sm.CLIENT_BINARY):
                allowed = client
            elif argv[0] == Path(sys.executable).as_posix():
                allowed = validator
            else:
                self.fail(f"unexpected command {argv[0]}")
            for token in argv[1:]:
                if token.startswith("--"):
                    seen.add(token)
                    self.assertIn(token, allowed, token)
        self.assertNotIn("--world-seed", seen)
        self.assertIn("--isolation-layers", seen)

    def test_scene_config_keys_are_read_by_main_client(self):
        literals = _scene_config_literals()
        for path in self.out.glob("*.scene.json"):
            config = json.loads(path.read_text(encoding="utf-8"))
            for key in _json_keys(config):
                self.assertIn(key, literals, f"{path.name}: {key}")

    def test_seed_and_weather_identities_match_client_inputs(self):
        text = (SRC / "main_client.cpp").read_text(encoding="utf-8")
        self.assertIn('traversal ? std::to_string(traversal->seed) : "424242"', text)
        self.assertTrue(set(sm.WEATHER_TYPES) <= _scene_config_literals())

    def test_isolation_tokens_are_accepted_by_isolation_config(self):
        accepted = _isolation_literals()
        for argv in self._command_lines():
            for index, token in enumerate(argv):
                if token == "--isolation-layers":
                    for layer in argv[index + 1].split(","):
                        self.assertIn(layer, accepted)


if __name__ == "__main__":
    unittest.main()

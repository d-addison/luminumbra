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

    def test_all_scenes_use_fov_110_and_unresolved_pose_is_null(self):
        for scene in _load_matrix()["scenes"]:
            self.assertEqual(scene["scene"]["camera"]["fov"], 110, scene["id"])
            if scene["pose_source"] == "recorded-evidence":
                self.assertIsNone(scene["scene"]["camera"]["pos"], scene["id"])
                self.assertIsNone(scene["scene"]["camera"]["yaw"], scene["id"])
                self.assertIsNone(scene["scene"]["camera"]["pitch"], scene["id"])

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
        data["scenes"][1]["scene"]["camera"]["pos"] = [1, 2, 3]
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
        code, err = _run(["--matrix", str(MATRIX_PATH), "--out", str(self.out), "--allow-unresolved"])
        self.assertEqual(code, 0, err)
        return sorted(p.name for p in self.out.glob("*.scene.json"))

    def test_resolved_outputs_written_and_unresolved_skipped(self):
        names = self._emit_real()
        self.assertEqual(
            names,
            sorted(
                [
                    "elevated.scene.json",
                    "elevated.clear.scene.json",
                    "elevated.storm.scene.json",
                    "elevated.no-aerial.scene.json",
                    "elevated.no-particles.scene.json",
                    "elevated.no-water.scene.json",
                    "elevated.terrain-only.scene.json",
                ]
            ),
        )
        self.assertTrue((self.out / "commands.txt").is_file())

    def test_elevated_config_contents(self):
        self._emit_real()
        config = json.loads((self.out / "elevated.scene.json").read_text(encoding="utf-8"))
        self.assertEqual(
            config["camera"], {"pos": [8, 56, 8], "yaw": 35, "pitch": -6, "fov": 110}
        )
        self.assertEqual(config["time_of_day"], 0.04)
        self.assertNotIn("weather", config)
        self.assertTrue(Path(config["screenshot"]).is_absolute())
        self.assertEqual(Path(config["screenshot"]).name, "elevated.ppm")

    def test_storm_variant_and_layer_variants(self):
        self._emit_real()
        storm = json.loads((self.out / "elevated.storm.scene.json").read_text(encoding="utf-8"))
        self.assertEqual(storm["weather"], {"type": "storm", "intensity": 1.0})
        terrain = json.loads((self.out / "elevated.terrain-only.scene.json").read_text(encoding="utf-8"))
        self.assertNotIn("weather", terrain)
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
        self.assertEqual(len(marks), 2 * 7)
        for line in marks:
            self.assertTrue(line.startswith("#"), line)

    def test_each_scene_emits_benchmark_and_screenshot_forms(self):
        self._emit_real()
        commands = [
            line
            for line in (self.out / "commands.txt").read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
        self.assertEqual(len(commands), 3 * 7)
        benchmark = [c for c in commands if "--render-benchmark" in c]
        self.assertEqual(len(benchmark), 7)
        validators = [c for c in commands if c.startswith("python3 tools/perf/validate_render_capture.py")]
        self.assertEqual(len(validators), 7)
        for line in validators:
            self.assertIn("--require-settled", line)
            self.assertIn("--position 8 56 8", line)

    def test_no_world_seed_flag_emitted(self):
        self._emit_real()
        self.assertNotIn("--world-seed", (self.out / "commands.txt").read_text(encoding="utf-8"))

    def test_unresolved_without_flag_exits_2(self):
        code, err = _run(["--matrix", str(MATRIX_PATH), "--out", str(self.out)])
        self.assertEqual(code, 2)
        for scene_id in ("ground", "high", "cave_air"):
            self.assertIn(scene_id, err)
        self.assertFalse((self.out / "commands.txt").exists())

    def test_unknown_layer_token_is_rejected_by_generator(self):
        data = _load_matrix()
        data["scenes"][0]["variants"][0]["layers"] = ["terrain", "nope"]
        matrix = Path(self._tmp.name) / "bad.json"
        matrix.write_text(json.dumps(data), encoding="utf-8")
        code, err = _run(["--matrix", str(matrix), "--out", str(self.out), "--allow-unresolved"])
        self.assertEqual(code, 1)
        self.assertIn("nope", err)
        self.assertFalse((self.out / "commands.txt").exists())

    def test_build_dir_is_substituted(self):
        code, _ = _run(
            [
                "--matrix",
                str(MATRIX_PATH),
                "--out",
                str(self.out),
                "--allow-unresolved",
                "--build-dir",
                "/opt/lum build",
            ]
        )
        self.assertEqual(code, 0)
        text = (self.out / "commands.txt").read_text(encoding="utf-8")
        self.assertIn("'/opt/lum build/bin/luminumbra_client_app'", text)


class EngineContractTests(unittest.TestCase):
    """Every emitted token must be read by the engine or validator sources."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.out = Path(self._tmp.name) / "out"
        code, err = _run(["--matrix", str(MATRIX_PATH), "--out", str(self.out), "--allow-unresolved"])
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
            elif argv[0] == "python3":
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

    def test_isolation_tokens_are_accepted_by_isolation_config(self):
        accepted = _isolation_literals()
        for argv in self._command_lines():
            for index, token in enumerate(argv):
                if token == "--isolation-layers":
                    for layer in argv[index + 1].split(","):
                        self.assertIn(layer, accepted)


if __name__ == "__main__":
    unittest.main()

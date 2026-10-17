#!/usr/bin/env python3
"""Stdlib discovery contract tests; --cli adds the CPU executable smoke tests."""

import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("station_contract", HERE / "station_discovery_contract.py")
contract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(contract)


def document(kind="mouth"):
    return {"schema": contract.SCHEMA_ID, "engine_revision": "a" * 40,
            "inputs_sha256": "b" * 64, "algorithm": "sampled-portals.v1",
            "numeric_profile": "canonical-worldgen.v1", "certified": False, "seed": 17,
            "query": {"kind": kind, "origin_mm": [0, 0], "radius_m": 16,
                      "y_range_m": [0, 100], "view_distance_m": 12, "limit": 64},
            "candidates": []}


def candidate(x=0, z=0, cz=0, cx=0, index=0):
    def signed(n):
        return ("n" if n < 0 else "p") + str(abs(n))
    return {"id": "mouth-z-%s-x-%s-i-%d" % (signed(cz), signed(cx), index),
            "anchor_mm": [x, 0, z],
            "pose": {"x": x / 1000 - 12, "y": 1.8, "z": z / 1000,
                     "yaw": 0, "pitch": math.degrees(math.atan2(-1.8, 12))}}


class ContractTests(unittest.TestCase):
    def test_capture_bounds_names_and_entries_before_retaining_validated_json(self):
        source = (HERE.parent / "station_discovery.cpp").read_text(encoding="utf-8")
        capture = source[source.index("Snapshot CaptureInputs("):
                         source.index("std::string InputIdentity(")]
        reserve = source[source.index("std::string ReserveInputPath("):
                         source.index("using Snapshot =")]
        self.assertIn("kInputEntryLimit = 4096", source)
        self.assertIn("kInputPathByteLimit =", source)
        self.assertIn("budget.remaining_entries == 0", reserve)
        self.assertIn("budget.remaining_path_bytes", reserve)
        self.assertIn("IsValidDependencyPathUtf8(name)", reserve)
        self.assertIn("dependency pathname must be portable UTF-8", reserve)
        self.assertLess(capture.index("ReserveInputPath("), capture.index("ReadBytes("))
        self.assertLess(capture.index("StrictJson(bytes)"), capture.index("files.emplace("))
        self.assertLess(capture.index("CheckStructureInput(data, true)"),
                        capture.index("files.emplace("))
        self.assertIn("ReserveInputPath(relative_type, budget)", capture)
        self.assertNotIn("for (const auto& [name, bytes] : files)", capture)

    def test_all_loader_dependencies_use_the_validated_snapshot_reader(self):
        root = HERE.parents[1]
        source = (root / "tools/station_discovery.cpp").read_text(encoding="utf-8")
        loader = (root / "src/luminumbra_common/world/TerrainPresetLoader.cpp").read_text(
            encoding="utf-8")
        capture = source[source.index("Snapshot CaptureInputs("):
                         source.index("std::string InputIdentity(")]
        for dependency in ('capture(PathFromUtf8(table))', '"placement.json"',
                           'piece.path()', '"common/creatures/species"', 'entry.path()'):
            self.assertIn(dependency, capture)
        self.assertIn("SnapshotReader(options, files)", source)
        reader = source[source.index("Luminumbra::World::WorldgenFileReader SnapshotReader("):
                        source.index("std::string InputIdentity(")]
        self.assertIn("files.find(", reader)
        self.assertIn("files.end()", reader)
        self.assertNotIn("ReadBytes(", reader)
        self.assertNotIn("ReadWorldgenFile(", reader)
        self.assertIn("dependency_reader(path, bytes)", loader)
        manifest = loader[loader.index("bool ResolveDependencyManifest("):
                          loader.index("void WarnUnknownKeys(")]
        self.assertNotIn("World::ReadWorldgenFile(path, bytes)", manifest)
        validation = source[source.index("void ValidateDependencies("):
                            source.index("void CheckNumerics()")]
        self.assertNotIn("BiomeTable::Load(", validation)
        self.assertNotIn("LoadStructureTemplatePool(", validation)
        self.assertIn("worldgen_dependencies", validation)

    def test_both_dependency_path_guards_reject_root_directory(self):
        source = (HERE.parent / "station_discovery.cpp").read_text(encoding="utf-8")
        for variable, start, end in (("table", "void CheckPresetShape(", "void CheckHydroDomain("),
                                     ("relative", "fs::path DependencyPath(",
                                      "std::string ReserveInputPath(")):
            guard = source[source.index(start):source.index(end)]
            self.assertIn(variable + ".has_root_name()", guard)
            self.assertIn(variable + ".has_root_directory()", guard)

    def refused(self, value):
        with self.assertRaises(contract.ContractError):
            contract.validate(value)

    def test_numeric_bootstrap_precedes_common_library_calls_on_both_platforms(self):
        source = (HERE.parents[1] / "tools/station_discovery.cpp").read_text(encoding="utf-8")
        # Both signatures enter the same body. Its first statement must check
        # capabilities, including before the POSIX common-library signal helper.
        entry = source[source.index("int wmain("):]
        self.assertRegex(entry, r"int wmain\(.*?\{\s*#else\s*int main\(.*?\{\s*#endif"
                               r"\s*try\s*\{\s*(?://[^\n]*\n\s*)*CheckNumerics\(\);")
        for call in ("IgnoreDefaultSigpipe()", "std::make_shared<", "ParseOptions(argc, argv)"):
            self.assertLess(entry.index("CheckNumerics();"), entry.index(call))
        root = HERE.parents[1]
        numeric_check = source[source.index("void CheckNumerics()"):
                               source.index("json Document(")]
        self.assertIn("Luminumbra::Startup::SupportsCommonLibrary()", numeric_check)
        self.assertNotIn("CPUMaxSIMDLevel", numeric_check)
        cmake = (root / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('luminumbra_baseline_startup("${CMAKE_CURRENT_SOURCE_DIR}/station_discovery.cpp")',
                      cmake)
        baseline = (root / "cmake/runtime_isa.cmake").read_text(encoding="utf-8")
        self.assertIn('if(MSVC)', baseline)
        self.assertIn('set(_startup_arch "/arch:SSE2")', baseline)
        self.assertIn('set(_startup_arch "/arch:IA32")', baseline)
        self.assertIn('COMPILE_OPTIONS "${_startup_arch};/GL-"', baseline)
        self.assertIn('elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")', baseline)
        for option in ("-march=x86-64", "-march=i686", "-mno-sse", "-mno-sse2",
                       "-mno-avx", "-mno-avx2", "-mno-fma",
                       "-mno-bmi", "-mno-bmi2", "-mno-popcnt", "-mno-lzcnt", "-mno-f16c",
                       "-mno-movbe", "-fno-lto"):
            self.assertIn(option, baseline)
        wrapper = (root / "src/startup/RuntimeEntry.cpp").read_text(encoding="utf-8")
        self.assertRegex(wrapper, r"int main\(.*?\{\s*if \(!Luminumbra::Startup::"
                                 r"SupportsCommonLibrary\(\)\) \{")
        self.assertLess(wrapper.index("SupportsCommonLibrary()"),
                        wrapper.index("return LuminumbraApplicationMain(argc, argv)"))
        self.assertIn("return 1;", wrapper)
        for application in ("client", "server"):
            app = root / ("src/luminumbra_" + application)
            implementation = (app / ("main_" + application + ".cpp")).read_text(encoding="utf-8")
            self.assertIn("int main(int argc, char* argv[])", implementation)
            manifest = (app / "sources.cmake").read_text(encoding="utf-8")
            self.assertIn("/../startup/RuntimeEntry.cpp", manifest)
            self.assertIn('COMPILE_DEFINITIONS "main=LuminumbraApplicationMain"', manifest)
            self.assertRegex(manifest, r"set_source_files_properties\([^\n]*main_" + application +
                             r"\.cpp\"? PROPERTIES\s*COMPILE_DEFINITIONS")
        self.assertIn('luminumbra_baseline_startup("${CMAKE_CURRENT_SOURCE_DIR}/startup/RuntimeEntry.cpp")',
                      (root / "src/CMakeLists.txt").read_text(encoding="utf-8"))
        self.assertIn("include(cmake/runtime_isa.cmake)",
                      (root / "CMakeLists.txt").read_text(encoding="utf-8"))

    def test_numeric_negative_controls_use_the_shipping_entry_on_both_platforms(self):
        root = HERE.parents[1]
        harness = (root / "test/common/StationDiscoveryNumericEnvironment.cpp").read_text(
            encoding="utf-8")
        self.assertIn("int StationDiscoveryEntry(int argc, wchar_t** argv);", harness)
        self.assertIn("int StationDiscoveryEntry(int argc, char** argv);", harness)
        self.assertIn("StationDiscoveryEntry(argc - 1, argv + 1)", harness)
        for control in ("FE_DOWNWARD", "FE_UPWARD", "FE_TOWARDZERO", "_mm_setcsr(baseline |"):
            self.assertIn(control, harness)
        cmake = (root / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_library(station_discovery_numeric_entry OBJECT station_discovery.cpp)", cmake)
        self.assertIn("main=StationDiscoveryEntry wmain=StationDiscoveryEntry", cmake)
        self.assertIn("$<TARGET_OBJECTS:station_discovery_numeric_entry>", cmake)
        self.assertIn("luminumbra_baseline_startup(\n        ", cmake)
        self.assertIn("station_discovery_numeric_environment PRIVATE -municode", cmake)
        registration = (root / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("add_test(NAME StationDiscoveryNumericRefusals", registration)
        self.assertIn('--numeric-cli "$<TARGET_FILE:station_discovery_numeric_environment>"',
                      registration)

    def test_empty_completed_document(self):
        self.assertEqual(contract.validate(document())["candidates"], [])

    def test_schema_is_2020_12_and_closed(self):
        self.assertEqual(contract.SCHEMA["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertFalse(contract.SCHEMA["additionalProperties"])
        self.assertEqual(set(contract.SCHEMA["required"]), set(document()))
        self.assertEqual(set(contract.POSE_SCHEMA["properties"]), contract.POSE_KEYS)

    def test_each_missing_and_extra_field(self):
        for key in document():
            value = document()
            del value[key]
            with self.subTest(key=key):
                self.refused(value)
        for level in ("root", "query", "candidate", "pose"):
            value = document()
            value["candidates"] = [candidate()]
            node = {"root": value, "query": value["query"],
                    "candidate": value["candidates"][0], "pose": value["candidates"][0]["pose"]}[level]
            node["unexpected"] = True
            self.refused(value)

    def test_no_settled_or_qualification_claims(self):
        for key in ("settled", "qualification", "status", "content_identity"):
            value = document()
            value[key] = True
            self.refused(value)

    def test_bool_is_not_number_or_integer(self):
        for field in ("seed",):
            value = document()
            value[field] = True
            self.refused(value)
        for field in ("radius_m", "view_distance_m", "limit"):
            value = document()
            value["query"][field] = False
            self.refused(value)
        value = document()
        value["candidates"] = [candidate()]
        value["candidates"][0]["pose"]["x"] = True
        self.refused(value)

    def test_version_and_numeric_profile(self):
        for key in ("schema", "algorithm", "numeric_profile"):
            value = document()
            value[key] += ".unsupported"
            self.refused(value)
        for value in (True, 0, "false", None):
            data = document()
            data["certified"] = value
            self.refused(data)

    def test_certification_cannot_be_claimed(self):
        with self.assertRaisesRegex(contract.ContractError, "certification"):
            contract.validate(document(), require_certified=True)

    def test_duplicate_fields_at_every_depth(self):
        for text in ('{"seed":1,"seed":2}', '{"query":{"kind":"arch","kind":"mouth"}}',
                     '{"candidates":[{"pose":{"x":0,"x":1}}]}'):
            with self.assertRaisesRegex(contract.ContractError, "duplicate"):
                contract.loads(text)

    def test_nonfinite_json_and_overflow(self):
        for text in ('{"x":NaN}', '{"x":Infinity}', '{"x":-Infinity}'):
            with self.assertRaises(contract.ContractError):
                contract.loads(text)
        for number in (float("nan"), float("inf"), -float("inf"), 10 ** 1000):
            value = document()
            value["candidates"] = [candidate()]
            value["candidates"][0]["pose"]["yaw"] = number
            self.refused(value)
        value = document()
        value["candidates"] = [candidate()]
        value["candidates"][0]["pose"]["yaw"] = contract.loads('{"x":1e999}')["x"]
        self.refused(value)

    def test_bad_json_and_invalid_encoding(self):
        for raw in (b'\xff', b'{', b'{"x": 1,}', b'{} {}'):
            with self.assertRaises(contract.ContractError):
                contract.loads(raw)

    def test_seed_and_query_scalar_bounds(self):
        for seed in (-1, 2147483616, 2.5, "17"):
            value = document()
            value["seed"] = seed
            self.refused(value)
        for key, numbers in (("radius_m", (0, 16385)), ("view_distance_m", (-1, 16385)),
                             ("limit", (0, 65))):
            for number in numbers:
                value = document()
                value["query"][key] = number
                self.refused(value)

    def test_vertical_and_horizontal_spans(self):
        for span in ([1, 0], [0, 513], [0], [0, True], [-2147483649, -2147483648]):
            value = document()
            value["query"]["y_range_m"] = span
            self.refused(value)
        value = document()
        value["query"]["y_range_m"] = [-256, 256]
        contract.validate(value)
        for origin in ([contract.HORIZONTAL_MM, 0], [-contract.HORIZONTAL_MM, 0], [0, 0, 0]):
            value = document()
            value["query"]["origin_mm"] = origin
            self.refused(value)

    def test_work_budgets_are_inclusive(self):
        contract.validate_work(contract.CELL_BUDGET, contract.SAMPLE_BUDGET)
        contract.validate_work(contract.CELL_BUDGET, contract.SAMPLE_BUDGET,
                               contract.LATTICE_VISIT_BUDGET)
        for cells, samples in ((contract.CELL_BUDGET + 1, 0), (0, contract.SAMPLE_BUDGET + 1),
                               (-1, 0), (True, 0), (0, float("inf"))):
            with self.assertRaises(contract.ContractError):
                contract.validate_work(cells, samples)
        for visits in (contract.LATTICE_VISIT_BUDGET + 1, -1, True, float("inf")):
            with self.assertRaises(contract.ContractError):
                contract.validate_work(0, 0, visits)

    def test_identity_pins_and_mismatch(self):
        contract.validate(document(), expected_engine_revision="a" * 40, expected_inputs_sha256="b" * 64)
        for kwargs in ({"expected_engine_revision": "c" * 40}, {"expected_inputs_sha256": "d" * 64}):
            with self.assertRaisesRegex(contract.ContractError, "identity mismatch"):
                contract.validate(document(), **kwargs)
        for key, value in (("engine_revision", "A" * 40), ("inputs_sha256", "b" * 63)):
            data = document()
            data[key] = value
            self.refused(data)

    def test_manifest_exact_bytes_relative_names_and_order(self):
        files = {"preset.json": b'{"a":1}\n', "data/common/table.json": b'{}\r\n'}
        manifest = {name: hashlib.sha256(raw).hexdigest() for name, raw in files.items()}
        expected = hashlib.sha256(json.dumps(manifest, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        self.assertEqual(contract.input_identity(files), expected)
        self.assertEqual(contract.input_identity(dict(reversed(list(files.items())))), expected)
        for name in files:
            changed = dict(files)
            changed[name] += b" "
            self.assertNotEqual(contract.input_identity(changed), expected)
        renamed = dict(files)
        renamed["data/renamed.json"] = renamed.pop("data/common/table.json")
        self.assertNotEqual(contract.input_identity(renamed), expected)
        for name in ("/absolute", "../escape", "data/../escape", "C:/path", "data\\table.json",
                     "data/file\0suffix.json"):
            with self.assertRaises(contract.ContractError):
                contract.input_identity({"preset.json": b"{}", name: b"{}"})

    def test_manifest_non_ascii_names_use_unescaped_utf8(self):
        files = {"preset.json": b"{}", "data/common/pi\u00e8ce-\u96ea-\U0001f30d.json": b"[]"}
        digest = hashlib.sha256(b"[]").hexdigest()
        preset_digest = hashlib.sha256(b"{}").hexdigest()
        expected_bytes = ('{"data/common/pi\u00e8ce-\u96ea-\U0001f30d.json":"' + digest +
                          '","preset.json":"' + preset_digest + '"}').encode("utf-8")
        self.assertEqual(contract.input_identity(files), hashlib.sha256(expected_bytes).hexdigest())
        manifest = {name: hashlib.sha256(raw).hexdigest() for name, raw in files.items()}
        self.assertEqual(contract.canonical(manifest), expected_bytes + b"\n")
        escaped = json.dumps(manifest, sort_keys=True, separators=(",", ":"),
                             ensure_ascii=True).encode("ascii")
        self.assertNotEqual(contract.input_identity(files), hashlib.sha256(escaped).hexdigest())

    def test_distance_order_negative_coordinates_and_signed_ties(self):
        value = document()
        value["candidates"] = [candidate(-4000, cz=-1, cx=-2, index=0),
                               candidate(4000, cz=-1, cx=-2, index=1),
                               candidate(-4000, cz=-1, cx=1, index=0),
                               candidate(4000, cz=1, index=0), candidate(8000, index=0)]
        contract.validate(value)
        for a, b in ((0, 1), (1, 2), (2, 3), (3, 4)):
            changed = copy.deepcopy(value)
            changed["candidates"][a], changed["candidates"][b] = changed["candidates"][b], changed["candidates"][a]
            self.refused(changed)

    def test_unique_ids_and_correct_kind(self):
        value = document()
        value["candidates"] = [candidate(), candidate()]
        self.refused(value)
        for token in ("mouth-z-n0-x-p0-i-0", "mouth-z-p01-x-p0-i-0", "arch-z-p0-x-p0-i-0",
                      "horizon-control-z-p0-x-p0-i-0", "mouth-z-p2147483648-x-p0-i-0"):
            value = document()
            value["candidates"] = [candidate()]
            value["candidates"][0]["id"] = token
            self.refused(value)

    def test_anchor_domain_lattice_radius_and_limit(self):
        for anchor in ([1000, 0, 0], [20000, 0, 0], [0, -4000, 0], [0, 0, contract.HORIZONTAL_MM + 1]):
            value = document()
            value["candidates"] = [candidate()]
            value["candidates"][0]["anchor_mm"] = anchor
            self.refused(value)
        value = document()
        value["query"]["limit"] = 1
        value["candidates"] = [candidate(), candidate(4000, index=1)]
        self.refused(value)

    def test_game_pose_fields_and_direction_limits(self):
        value = document()
        value["candidates"] = [candidate()]
        pose = value["candidates"][0]["pose"]
        self.assertEqual(set(pose), {"x", "y", "z", "yaw", "pitch"})
        # These are exactly the scene-set's numeric, finite five fields.
        self.assertTrue(all(type(v) in (int, float) and math.isfinite(v) for v in pose.values()))
        contract.validate(value)
        for field, number in (("yaw", 180), ("yaw", -181), ("pitch", -90), ("pitch", 90),
                              ("x", contract.HORIZONTAL_MM / 1000), ("y", 100)):
            changed = copy.deepcopy(value)
            changed["candidates"][0]["pose"][field] = number
            self.refused(changed)

    def test_search_radius_is_separate_from_view_distance(self):
        value = document()
        value["query"]["radius_m"] = 1
        value["query"]["view_distance_m"] = 16000
        value["candidates"] = [candidate()]
        value["candidates"][0]["pose"]["x"] = -16000
        value["candidates"][0]["pose"]["pitch"] = math.degrees(math.atan2(-1.8, 16000))
        contract.validate(value)
        value["candidates"][0]["pose"]["x"] = -8000
        self.refused(value)

    def test_feature_views_must_be_cardinal(self):
        value = document()
        value["candidates"] = [candidate()]
        pose = value["candidates"][0]["pose"]
        # Same radius and matching orientation, but the producer only uses cardinals.
        pose.update(x=-7.2, z=-9.6, yaw=math.degrees(math.atan2(9.6, 7.2)))
        self.refused(value)

    def test_feature_orientation_must_follow_displacement(self):
        for yaw, pitch in ((-180, None), (90, None), (None, 8.5), (None, -20)):
            value = document()
            value["candidates"] = [candidate()]
            pose = value["candidates"][0]["pose"]
            if yaw is not None:
                pose["yaw"] = yaw
            if pitch is not None:
                pose["pitch"] = pitch
            self.refused(value)

    def test_feature_cardinal_and_zero_distance_orientation_controls(self):
        for dx, dz, yaw in ((12, 0, 0), (0, 12, 90), (-12, 0, -180), (0, -12, -90), (0, 0, 0)):
            value = document()
            value["query"]["view_distance_m"] = abs(dx) + abs(dz)
            value["candidates"] = [candidate()]
            value["candidates"][0]["pose"].update(
                x=-dx, z=-dz, yaw=yaw,
                pitch=max(-89, math.degrees(math.atan2(-1.8, abs(dx) + abs(dz)))))
            contract.validate(value)

    def test_feature_eye_must_clear_anchor(self):
        value = document()
        value["candidates"] = [candidate()]
        value["candidates"][0]["pose"].update(
            y=1.0, pitch=math.degrees(math.atan2(-1.0, 12)))
        self.refused(value)

    def test_feature_portal_stencil_must_fit_vertical_span(self):
        value = document()
        value["candidates"] = [candidate()]
        anchor = value["candidates"][0]
        anchor["anchor_mm"][1] = 88000
        anchor["pose"]["y"] = 89.8
        contract.validate(value)
        anchor["anchor_mm"][1] = 96000
        anchor["pose"]["y"] = 97.8
        self.refused(value)

    def test_local_index_must_fit_cpp_uint64(self):
        value = document()
        value["candidates"] = [candidate(index=2 ** 64 - 1)]
        contract.validate(value)
        for index in (2 ** 64, 10 ** 100):
            value["candidates"] = [candidate(index=index)]
            self.refused(value)

    def test_control_height_offsets_are_fixed(self):
        for kind, index, height in (("horizon", 0, 50), ("coverage", 0, 50),
                                    ("coverage", 4, 1.8), ("coverage", 4, 79.999)):
            value = document(kind)
            value["candidates"] = [{"id": "%s-control-z-p0-x-p0-i-%d" % (kind, index),
                "anchor_mm": [0, 0, 0], "pose": {"x": 0, "y": height, "z": 0,
                "yaw": 45 if index == 4 else 0, "pitch": -20 if index == 4 else 0}}]
            self.refused(value)

    def test_controls_are_distinct_and_follow_fixed_conventions(self):
        for kind in ("horizon", "coverage"):
            value = document(kind)
            for index in range(4 if kind == "horizon" else 5):
                value["candidates"].append({"id": "%s-control-z-p0-x-p0-i-%d" % (kind, index),
                    "anchor_mm": [0, 0, 0], "pose": {"x": 0, "y": 80 if index == 4 else 1.8,
                    "z": 0, "yaw": (0, 90, -180, -90, 45)[index], "pitch": -20 if index == 4 else 0}})
            contract.validate(value)
            value["candidates"][0]["pose"]["yaw"] = 45
            self.refused(value)

    def test_controls_must_share_terrain_height_even_with_correct_eye_offsets(self):
        for kind in ("horizon", "coverage"):
            count = 4 if kind == "horizon" else 5
            for changed_index in range(count):
                with self.subTest(kind=kind, changed_index=changed_index):
                    value = document(kind)
                    for index in range(count):
                        value["candidates"].append({
                            "id": "%s-control-z-p0-x-p0-i-%d" % (kind, index),
                            "anchor_mm": [0, 0, 0],
                            "pose": {"x": 0, "y": 80 if index == 4 else 1.8, "z": 0,
                                     "yaw": (0, 90, -180, -90, 45)[index],
                                     "pitch": -20 if index == 4 else 0}})
                    contract.validate(value)
                    changed = value["candidates"][changed_index]
                    changed["anchor_mm"][1] = 4000
                    changed["pose"]["y"] = 84 if changed_index == 4 else 5.8
                    with self.assertRaisesRegex(contract.ContractError, "share one terrain"):
                        contract.validate(value)
                    # A common nonzero terrain height is valid for every control.
                    for control in value["candidates"]:
                        control["anchor_mm"][1] = 4000
                        control["pose"]["y"] = 84 if control["pose"]["pitch"] == -20 else 5.8
                    contract.validate(value)

    def test_control_distance_does_not_expand_pose_domain(self):
        value = document("horizon")
        value["query"]["origin_mm"] = [contract.HORIZONTAL_MM - 10000, 0]
        value["query"]["radius_m"] = 1
        value["query"]["view_distance_m"] = 16000
        contract.validate(value)
        value["query"]["kind"] = "mouth"
        self.refused(value)

    def test_validator_subprocess_repeat_and_no_partial_refusal(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "result.json"
            path.write_bytes(contract.canonical(document()))
            command = [sys.executable, "-I", "-B", str(HERE / "station_discovery_contract.py"),
                       "--input", str(path)]
            first = subprocess.run(command, capture_output=True, check=False)
            second = subprocess.run(command, capture_output=True, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(first.stdout, second.stdout)
            self.assertTrue(first.stdout.endswith(b"\n"))
            self.assertFalse(first.stdout.endswith(b"\n\n"))
            self.assertNotIn(b"\r", first.stdout)
            self.assertEqual(contract.loads(first.stdout), document())
            for raw in (b'{"seed":1,"seed":2}', b'{"x":NaN}', b'{}'):
                path.write_bytes(raw)
                result = subprocess.run(command, capture_output=True, check=False)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, b"")
                self.assertTrue(result.stderr)
            usage = subprocess.run(command + ["--expected-inputs-sha256", "invalid"], capture_output=True)
            self.assertEqual(usage.returncode, 2)
            self.assertEqual(usage.stdout, b"")

    def test_stdout_write_failures_exit_one_for_schema_and_document(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "result.json"
            original = contract.canonical(document())
            path.write_bytes(original)
            command = [sys.executable, "-I", "-B", str(HERE / "station_discovery_contract.py")]
            for arguments in (["--schema"], ["--input", str(path)]):
                # A read-only regular handle fails writes on Windows and POSIX,
                # without pipe capacity, signals, or scheduling dependencies.
                with path.open("rb") as output:
                    result = subprocess.run(command + arguments, stdout=output,
                                            stderr=subprocess.PIPE, check=False)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"station discovery refused:", result.stderr)
                self.assertEqual(path.read_bytes(), original)

    def test_schema_subprocess_is_byte_identical(self):
        command = [sys.executable, "-I", "-B", str(HERE / "station_discovery_contract.py"), "--schema"]
        first = subprocess.run(command, capture_output=True, check=True)
        second = subprocess.run(command, capture_output=True, check=True)
        self.assertEqual(first.stdout, second.stdout)
        self.assertEqual(contract.loads(first.stdout), contract.SCHEMA)


class NativeSmoke(unittest.TestCase):
    cli = None

    @staticmethod
    def flat_preset():
        return {"schema_rev": 6, "generation_params": {
            "terrain": {"base_frequency": 0.004, "base_amplitude": 0, "octaves": 1,
                        "persistence": 0.5, "lacunarity": 2, "height_offset": 0},
            "features": {"caves_enabled": False, "cave_frequency": 0.02}}}

    def test_snapshot_entry_and_pathname_limits_are_inclusive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            species = data / "common/creatures/species"
            species.mkdir(parents=True)
            preset = root / "flat.json"
            preset.write_bytes(contract.canonical(self.flat_preset()))
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                       "--seed", "17", "--kind", "mouth", "--origin-mm", "0", "0",
                       "--radius-m", "1", "--y-range-m", "0", "100",
                       "--view-distance-m", "0", "--limit", "64"]

            def check(code, message=b""):
                result = subprocess.run(command, capture_output=True, check=False)
                self.assertEqual(result.returncode, code, result.stderr)
                if code:
                    self.assertEqual(result.stdout, b"")
                    self.assertIn(message, result.stderr)
                else:
                    contract.validate(contract.loads(result.stdout))

            for index in range(4095):  # Plus the preset: exactly 4096 entries.
                (species / ("%04d.json" % index)).write_bytes(b'{"id":"fixture"}')
            check(0)
            extra = species / "extra.json"
            extra.write_bytes(b'{"id":"extra"}')
            check(1, b"input-limit: entry limit exceeded")
            for path in species.iterdir():
                path.unlink()

            prefix_bytes = len("data/common/creatures/species/")
            remaining = 1024 * 1024 - len("preset.json")
            width = prefix_bytes + 250
            lengths = [250] * (remaining // width)
            tail = remaining % width
            if tail < prefix_bytes + len("0000.json"):
                lengths.pop()
                tail += width
                lengths.append(tail // 2 - prefix_bytes)
                tail -= tail // 2
            lengths.append(tail - prefix_bytes)
            for index, length in enumerate(lengths):
                stem_bytes = length - len("0000.json")
                # Short native names with long UTF-8 spellings also work on Windows.
                name = "\u96ea" * (stem_bytes // 3) + "x" * (stem_bytes % 3) + "%04d.json" % index
                self.assertEqual(len(name.encode("utf-8")), length)
                (species / name).write_bytes(b'{"id":"fixture"}')
            check(0)  # Exactly 1 MiB of keys; content bytes are far below 8 MiB.
            extra.write_bytes(b'{"id":"extra"}')
            check(1, b"input-limit: pathname byte limit exceeded")

    def test_empty_structure_directories_cannot_evade_entry_limit(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            structures = data / "common/structures"
            structures.mkdir(parents=True)
            for index in range(4096):
                (structures / str(index)).mkdir()
            fields = self.flat_preset()
            fields["generation_params"]["features"]["structures_enabled"] = True
            preset = root / "flat.json"
            preset.write_bytes(contract.canonical(fields))
            result = subprocess.run([self.cli, "--preset-json", str(preset), "--data-root", str(data),
                "--seed", "17", "--kind", "mouth", "--origin-mm", "0", "0", "--radius-m", "1",
                "--y-range-m", "0", "100", "--view-distance-m", "0", "--limit", "64"],
                capture_output=True, check=False)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertEqual(result.stdout, b"")
            self.assertIn(b"input-limit: entry limit exceeded", result.stderr)

    def test_rooted_biome_paths_are_refused_before_capture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            data.mkdir()
            outside = root / "outside.json"
            outside.write_bytes(b"{")
            preset = root / "flat.json"
            # Windows root-directory-only and drive-relative names both redirect
            # path append. POSIX has a single rooted form.
            paths = ([str(outside), "\\" + str(outside.relative_to(outside.anchor)),
                      outside.drive + "outside.json"] if os.name == "nt" else [str(outside)])
            for path in paths:
                fields = self.flat_preset()
                fields["generation_params"]["biomes"] = {"table": path}
                preset.write_bytes(contract.canonical(fields))
                result = subprocess.run([self.cli, "--preset-json", str(preset), "--data-root", str(data),
                    "--seed", "17", "--kind", "mouth", "--origin-mm", "0", "0", "--radius-m", "1",
                    "--y-range-m", "0", "100", "--view-distance-m", "0", "--limit", "64"],
                    capture_output=True, check=False)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"biome table must name a relative dependency", result.stderr)

    def test_dependency_logical_names_match_the_portable_manifest_contract(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            data.mkdir()
            preset = root / "flat.json"
            for name in ("file:stream.json", "file\u0000suffix.json"):
                fields = self.flat_preset()
                fields["generation_params"]["biomes"] = {"table": name}
                preset.write_bytes(contract.canonical(fields))
                result = subprocess.run([self.cli, "--preset-json", str(preset), "--data-root", str(data),
                    "--seed", "17", "--kind", "mouth", "--origin-mm", "0", "0", "--radius-m", "1",
                    "--y-range-m", "0", "100", "--view-distance-m", "0", "--limit", "64"],
                    capture_output=True, check=False)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"dependency pathname must be portable UTF-8", result.stderr)

    def test_input_file_byte_limit_includes_preset_and_every_dependency_kind(self):
        file_limit = 1024 * 1024
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            pool = data / "common" / "structures" / "fixture"
            pool.mkdir(parents=True)
            table = data / "common" / "biomes.json"
            piece = pool / "piece.json"
            placement = pool / "placement.json"
            species = data / "common" / "creatures" / "species" / "fixture.json"
            species.parent.mkdir(parents=True)
            preset = root / "flat.json"
            fields = self.flat_preset()
            fields["generation_params"]["biomes"] = {"table": "common/biomes.json"}
            fields["generation_params"]["features"]["structures_enabled"] = True
            inputs = {
                preset: contract.canonical(fields),
                table: contract.canonical({"schema": "luminumbra.biome_table.v1", "biomes": [
                    {"id": 0, "name": "fixture", "surface": {
                        "top": "Grass", "filler": "Soil", "depth": "Stone", "underwater": "Sand"}}]}),
                piece: contract.canonical({"name": "fixture", "boxes": [
                    {"min": [0, 0, 0], "size": [1, 1, 1], "material": 1}]}),
                placement: contract.canonical({"spacing": 64, "separation": 16, "density": 1}),
                species: contract.canonical({"id": "fixture"}),
            }
            for path, raw in inputs.items():
                path.write_bytes(raw)
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                       "--seed", "17", "--kind", "mouth", "--origin-mm", "0", "0",
                       "--radius-m", "1", "--y-range-m", "0", "100",
                       "--view-distance-m", "0", "--limit", "64"]
            for path, raw in inputs.items():
                with self.subTest(input=path.name):
                    padded = raw + b" " * (file_limit - len(raw))
                    path.write_bytes(padded)
                    accepted = subprocess.run(command, capture_output=True, check=False)
                    self.assertEqual(accepted.returncode, 0, accepted.stderr)
                    manifest = {"preset.json" if item == preset else
                                "data/" + item.relative_to(data).as_posix(): item.read_bytes()
                                for item in inputs}
                    contract.validate(contract.loads(accepted.stdout),
                                      expected_inputs_sha256=contract.input_identity(manifest))
                    # A sparse oversized input is refused by its size, before JSON parsing.
                    with path.open("wb") as output:
                        output.truncate(file_limit + 1)
                    refused = subprocess.run(command, capture_output=True, check=False)
                    self.assertEqual(refused.returncode, 1, refused.stderr)
                    self.assertEqual(refused.stdout, b"")
                    self.assertIn(b"input-limit: per-file byte limit exceeded", refused.stderr)
                    path.write_bytes(raw)

    def test_species_capture_without_biomes_or_structures(self):
        file_limit = 1024 * 1024
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            species = data / "common" / "creatures" / "species"
            species.parent.mkdir(parents=True)
            preset = root / "flat.json"
            raw_preset = contract.canonical(self.flat_preset())
            preset.write_bytes(raw_preset)
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                       "--seed", "17", "--kind", "mouth", "--origin-mm", "0", "0",
                       "--radius-m", "1", "--y-range-m", "0", "100",
                       "--view-distance-m", "0", "--limit", "64"]

            def accepted(files):
                result = subprocess.run(command, capture_output=True, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                return contract.validate(contract.loads(result.stdout),
                                         expected_inputs_sha256=contract.input_identity(files))

            def refused(message):
                result = subprocess.run(command, capture_output=True, check=False)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, b"")
                self.assertIn(message, result.stderr)

            manifest = {"preset.json": raw_preset}
            accepted(manifest)  # An absent catalogue is valid.
            species.write_bytes(b"{}")
            refused(b"species path must be a directory")
            species.unlink()
            species.mkdir()
            accepted(manifest)  # An empty catalogue is valid too.
            (species / "nested.json").mkdir()
            (species / "nested.json" / "ignored.json").write_bytes(b"{")
            with (species / "ignored.txt").open("wb") as output:
                output.truncate(file_limit + 1)
            accepted(manifest)  # Match the immediate regular *.json scan.
            path = species / "fixture.json"
            for raw, message in ((b'{"id":', b"station discovery refused:"),
                                 (b'{"id":"a","id":"b"}', b"duplicate JSON field"),
                                 (b'{"id":"a","rarity":1e39}', b"unsupported input number")):
                path.write_bytes(raw)
                refused(message)
            with path.open("wb") as output:
                output.truncate(file_limit + 1)
            refused(b"input-limit: per-file byte limit exceeded")
            path.write_bytes(contract.canonical({"id": "fixture"}))
            name = "data/common/creatures/species/fixture.json"
            manifest[name] = path.read_bytes()
            first = accepted(manifest)
            path.write_bytes(contract.canonical({"id": "changed"}))
            manifest[name] = path.read_bytes()
            self.assertNotEqual(accepted(manifest)["inputs_sha256"], first["inputs_sha256"])
            # Species alone must also consume the aggregate budget.
            for index in range(8):
                raw = contract.canonical({"id": "fixture-%d" % index})
                (species / ("species-%d.json" % index)).write_bytes(
                    raw + b" " * (file_limit - len(raw)))
            for entry in species.iterdir():
                if entry.is_file() and entry.suffix == ".json":
                    contract.loads(entry.read_bytes())
            refused(b"input-limit: aggregate byte limit exceeded")

    def test_aggregate_input_byte_limit_is_inclusive(self):
        file_limit = 1024 * 1024
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            pool = data / "common" / "structures" / "fixture"
            pool.mkdir(parents=True)
            preset = root / "flat.json"
            fields = self.flat_preset()
            fields["generation_params"]["features"]["structures_enabled"] = True
            inputs = {preset: contract.canonical(fields)}
            for index in range(7):
                inputs[pool / ("piece-%d.json" % index)] = contract.canonical({
                    "name": "piece-%d" % index, "boxes": [
                        {"min": [0, 0, 0], "size": [1, 1, 1], "material": 1}]})
            for path, raw in inputs.items():
                path.write_bytes(raw + b" " * (file_limit - len(raw)))
            self.assertEqual(sum(path.stat().st_size for path in inputs), 8 * file_limit)
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                       "--seed", "17", "--kind", "mouth", "--origin-mm", "0", "0",
                       "--radius-m", "1", "--y-range-m", "0", "100",
                       "--view-distance-m", "0", "--limit", "64"]
            accepted = subprocess.run(command, capture_output=True, check=False)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)
            # Exactly 8 MiB succeeds. Every file remains valid JSON, independent of
            # directory enumeration order, so the aggregate limit must win.
            # Trim one padding byte so the two-byte object exceeds 8 MiB by
            # exactly one byte, still with every input individually valid.
            preset.write_bytes(preset.read_bytes()[:-1])
            (pool / "extra.json").write_bytes(b"{}")
            captured = (preset, *pool.glob("*.json"))
            self.assertEqual(sum(path.stat().st_size for path in captured), 8 * file_limit + 1)
            for path in captured:
                contract.loads(path.read_bytes())
            refused = subprocess.run(command, capture_output=True, check=False)
            self.assertEqual(refused.returncode, 1, refused.stderr)
            self.assertEqual(refused.stdout, b"")
            self.assertIn(b"input-limit: aggregate byte limit exceeded", refused.stderr)
            # A malformed explicitly selected biome is captured before the
            # structure scan. Its parse refusal wins over later aggregate work.
            (pool / "extra.json").unlink()
            fields["generation_params"]["biomes"] = {"table": "common/invalid.json"}
            raw = contract.canonical(fields)
            preset.write_bytes(raw + b" " * (file_limit - len(raw)))
            (data / "common/invalid.json").write_bytes(b" ")
            refused = subprocess.run(command, capture_output=True, check=False)
            self.assertEqual(refused.returncode, 1, refused.stderr)
            self.assertEqual(refused.stdout, b"")
            self.assertIn(b"parse_error.101", refused.stderr)
            self.assertNotIn(b"input-limit:", refused.stderr)

    def test_input_number_range_is_checked_at_every_json_depth(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            data.mkdir()
            preset = root / "input.json"
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                       "--seed", "17", "--kind", "mouth", "--origin-mm", "0", "0",
                       "--radius-m", "1", "--y-range-m", "0", "100",
                       "--view-distance-m", "0", "--limit", "64"]
            for raw in (b'1e39', b'{"ignored":1e39}', b'{"ignored":[{"value":-1e39}]}',
                        b'[{"value":1e39}]'):
                with self.subTest(raw=raw):
                    preset.write_bytes(raw)
                    refused = subprocess.run(command, capture_output=True, check=False)
                    self.assertEqual(refused.returncode, 1, refused.stderr)
                    self.assertEqual(refused.stdout, b"")
                    self.assertIn(b"nonfinite or unsupported input number", refused.stderr)

    def test_native_unicode_input_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "\u96ea station \U0001f30d"
            data = root / "data \u96ea"
            data.mkdir(parents=True)
            preset = root / "flat \u96ea.json"
            preset.write_bytes(contract.canonical(self.flat_preset()))
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                       "--seed", "17", "--kind", "coverage", "--origin-mm", "0", "0",
                       "--radius-m", "1", "--y-range-m", "0", "100",
                       "--view-distance-m", "0", "--limit", "64"]
            result = subprocess.run(command, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)
            value = contract.validate(contract.loads(result.stdout))
            self.assertEqual(len(value["candidates"]), 5)

    if os.name == "nt":
        def test_data_root_preserves_windows_device_prefix(self):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                data = root / "data with spaces"
                data.mkdir()
                preset = root / "flat.json"
                preset.write_bytes(contract.canonical(self.flat_preset()))
                command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                           "--seed", "17", "--kind", "coverage", "--origin-mm", "0", "0",
                           "--radius-m", "1", "--y-range-m", "0", "100",
                           "--view-distance-m", "0", "--limit", "64"]
                baseline = subprocess.run(command, capture_output=True, check=False)
                self.assertEqual(baseline.returncode, 0, baseline.stderr)
                contract.validate(contract.loads(baseline.stdout))
                # Device prefixes take the same UNC-safe helper branch, without
                # requiring an available network share or administrative rights.
                native = str(data.resolve())
                device = ("\\\\?\\UNC\\" + native[2:] if native.startswith("\\\\")
                          else "\\\\?\\" + native)
                command[command.index("--data-root") + 1] = device
                result = subprocess.run(command, capture_output=True, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, baseline.stdout)

    def test_integer_overflow_refuses_and_invalid_syntax_is_usage(self):
        command = [self.cli, "--preset-json", "unused.json", "--data-root", ".",
                   "--seed", "17", "--kind", "coverage", "--origin-mm", "0", "0",
                   "--radius-m", "1", "--y-range-m", "0", "100",
                   "--view-distance-m", "0", "--limit", "64"]
        for option, arity, bits in (("--seed", 1, 32), ("--origin-mm", 2, 64),
                                    ("--radius-m", 1, 32), ("--y-range-m", 2, 32),
                                    ("--view-distance-m", 1, 32), ("--limit", 1, 32)):
            overflow = str(2 ** (bits - 1))
            underflow = str(-2 ** (bits - 1) - 1)
            for axis in range(arity):
                for text, code, diagnostic in (
                        (overflow, 1, b"station discovery refused:"),
                        (underflow, 1, b"station discovery refused:"),
                        ("9" * 100, 1, b"station discovery refused:"),
                        ("", 2, b"station discovery usage:"),
                        ("abc", 2, b"station discovery usage:"),
                        ("1.5", 2, b"station discovery usage:"),
                        ("+1", 2, b"station discovery usage:"),
                        (overflow + "x", 2, b"station discovery usage:")):
                    with self.subTest(option=option, axis=axis, text=text):
                        args = list(command)
                        args[args.index(option) + 1 + axis] = text
                        result = subprocess.run(args, capture_output=True, check=False)
                        self.assertEqual(result.returncode, code, result.stderr)
                        self.assertEqual(result.stdout, b"")
                        self.assertIn(diagnostic, result.stderr)
                        self.assertIn(b"integer", result.stderr)

    def test_structure_integers_are_checked_before_loading(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            pool = data / "common" / "structures" / "fixture"
            pool.mkdir(parents=True)
            piece = pool / "piece.json"
            placement = pool / "placement.json"
            valid_piece = {"name": "fixture", "boxes": [
                {"min": [0, 0, 0], "size": [1, 1, 1], "material": 1}],
                "sockets": [{"name": "join", "position": [0, 0, 0]}]}
            valid_placement = {"spacing": 64, "separation": 16, "salt": 0, "density": 1}
            piece.write_bytes(contract.canonical(valid_piece))
            placement.write_bytes(contract.canonical(valid_placement))
            fields = self.flat_preset()
            fields["generation_params"]["features"]["structures_enabled"] = True
            preset = root / "flat.json"
            preset.write_bytes(contract.canonical(fields))
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data), "--seed", "17",
                       "--kind", "mouth", "--origin-mm", "0", "0", "--radius-m", "16",
                       "--y-range-m", "0", "100", "--view-distance-m", "12", "--limit", "64"]
            baseline = subprocess.run(command, capture_output=True)
            self.assertEqual(baseline.returncode, 0, baseline.stderr)
            # An invalid later field would throw inside the pool loader. The
            # integer-domain error must win before that loader can read the box.
            invalid = copy.deepcopy(valid_piece)
            invalid["boxes"][0].update(min=[1e38, 0, 0], material="invalid")
            piece.write_bytes(contract.canonical(invalid))
            refusal = subprocess.run(command, capture_output=True)
            self.assertEqual(refusal.returncode, 1, refusal.stderr)
            self.assertEqual(refusal.stdout, b"")
            self.assertIn(b"structure integer vector outside bounds", refusal.stderr)
            piece.write_bytes(b'{"name":"fixture","name":"duplicate",'
                              b'"boxes":[{"min":[1e38,0,0],"material":"invalid"}]}')
            refusal = subprocess.run(command, capture_output=True)
            self.assertEqual(refusal.returncode, 1, refusal.stderr)
            self.assertEqual(refusal.stdout, b"")
            self.assertIn(b"duplicate JSON field", refusal.stderr)
            piece.write_bytes(contract.canonical(valid_piece))
            for key in ("spacing", "separation", "salt"):
                for number in (1e38, 1.5, -1, 2 ** (32 if key == "salt" else 31)):
                    with self.subTest(key=key, number=number):
                        invalid = dict(valid_placement, **{key: number})
                        placement.write_bytes(contract.canonical(invalid))
                        refusal = subprocess.run(command, capture_output=True)
                        self.assertEqual(refusal.returncode, 1, refusal.stderr)
                        self.assertEqual(refusal.stdout, b"")
            placement.write_bytes(contract.canonical(valid_placement))
            for key in ("min", "size", "position", "material"):
                for axis in range(1 if key == "material" else 3):
                    numbers = (1e38, 1.5, -2147483649, 2147483648)
                    if key == "material":
                        numbers += (-1, 256)
                    elif key == "size":
                        numbers += (0,)
                    for number in numbers:
                        with self.subTest(key=key, axis=axis, number=number):
                            invalid = copy.deepcopy(valid_piece)
                            target = invalid["sockets" if key == "position" else "boxes"][0]
                            if key == "material":
                                target[key] = number
                            else:
                                target[key][axis] = number
                            piece.write_bytes(contract.canonical(invalid))
                            refusal = subprocess.run(command, capture_output=True)
                            self.assertEqual(refusal.returncode, 1, refusal.stderr)
                            self.assertEqual(refusal.stdout, b"")
            for axis in range(3):
                invalid = copy.deepcopy(valid_piece)
                invalid["boxes"][0]["min"][axis] = 2147483647
                piece.write_bytes(contract.canonical(invalid))
                refusal = subprocess.run(command, capture_output=True)
                self.assertEqual(refusal.returncode, 1, refusal.stderr)
                self.assertEqual(refusal.stdout, b"")

            piece.write_bytes(contract.canonical(valid_piece))
            placement.unlink()
            alias = pool / "PLACEMENT.JSON"
            alias.write_bytes(contract.canonical(dict(valid_placement, spacing=1e38,
                                                      density="invalid")))
            result = subprocess.run(command, capture_output=True)
            if placement.exists():
                # The loader consumes the fixed lowercase name on this filesystem.
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"integer input outside bounds: spacing", result.stderr)
            else:
                # Uppercase extensions are outside the loader's piece catalogue.
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_hydro_coordinate_and_parameter_refusals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            data.mkdir()
            preset = root / "flat.json"
            fields = self.flat_preset()
            fields["generation_params"]["terrain"]["hydro"] = {
                "enabled": True, "iterations": 0, "cell_size_m": 8}
            preset.write_bytes(contract.canonical(fields))
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data), "--seed", "17",
                       "--kind", "horizon", "--origin-mm", "1000", "-1000", "--radius-m", "1",
                       "--y-range-m", "0", "100", "--view-distance-m", "0", "--limit", "64"]
            baseline = subprocess.run(command, capture_output=True)
            self.assertEqual(baseline.returncode, 0, baseline.stderr)
            self.assertEqual(len(contract.validate(contract.loads(baseline.stdout))["candidates"]), 4)
            for key, numbers in (("cell_size_m", (1e-30, 2 ** -40, 0, -1, 1e38)),
                                 ("iterations", (513, 1e38, 0.5)),
                                 ("thermal_rate", (-0.1, 1.1)),
                                 ("deposition", (-0.1, 1.1)),
                                 ("evaporation", (-0.1, 1.1)),
                                 ("talus_height", (-1,)), ("rain_per_sweep", (-1,)),
                                 ("solubility", (-1,)), ("sediment_capacity", (-1,)),
                                 ("max_offset", (-1,))):
                for number in numbers:
                    with self.subTest(key=key, number=number):
                        invalid = copy.deepcopy(fields)
                        invalid["generation_params"]["terrain"]["hydro"][key] = number
                        preset.write_bytes(contract.canonical(invalid))
                        refusal = subprocess.run(command, capture_output=True)
                        self.assertEqual(refusal.returncode, 1, refusal.stderr)
                        self.assertEqual(refusal.stdout, b"")
            # The query footprint fits, but the erosion region/halo does not.
            preset.write_bytes(contract.canonical(fields))
            for origin in (contract.HORIZONTAL_MM - 1500, -contract.HORIZONTAL_MM + 1500):
                args = list(command)
                args[args.index("--origin-mm") + 1] = str(origin)
                refusal = subprocess.run(args, capture_output=True)
                self.assertEqual(refusal.returncode, 1, refusal.stderr)
                self.assertEqual(refusal.stdout, b"")

    def test_dependency_identity_and_missing_or_clamped_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            common = data / "common"
            common.mkdir(parents=True)
            pool = common / "structures" / "pi\u00e8ces-\u96ea"
            pool.mkdir(parents=True)
            table = common / "biomes-\u00e9.json"
            table.write_bytes(contract.canonical({"schema": "luminumbra.biome_table.v1",
                "biomes": [{"id": 0, "name": "fixture", "surface": {
                    "top": "Grass", "filler": "Soil", "depth": "Stone", "underwater": "Sand"}}]}))
            piece = pool / "pi\u00e8ce-\U0001f30d.json"
            piece.write_bytes(contract.canonical({"name": "fixture", "boxes": [
                {"min": [0, 0, 0], "size": [1, 1, 1], "material": 1}]}))
            placement = pool / "placement.json"
            placement.write_bytes(contract.canonical({"spacing": 64, "separation": 16, "density": 1}))
            fields = self.flat_preset()
            fields["generation_params"]["biomes"] = {"table": table.relative_to(data).as_posix()}
            fields["generation_params"]["features"]["structures_enabled"] = True
            preset = root / "flat.json"
            preset.write_bytes(contract.canonical(fields))
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data), "--seed", "17",
                       "--kind", "mouth", "--origin-mm", "0", "0", "--radius-m", "16",
                       "--y-range-m", "0", "100", "--view-distance-m", "12", "--limit", "64"]

            def expected():
                return contract.input_identity({"preset.json": preset.read_bytes(),
                    "data/" + table.relative_to(data).as_posix(): table.read_bytes(),
                    "data/" + piece.relative_to(data).as_posix(): piece.read_bytes(),
                    "data/" + placement.relative_to(data).as_posix(): placement.read_bytes()})

            first = subprocess.run(command, capture_output=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            first_document = contract.validate(contract.loads(first.stdout), expected_inputs_sha256=expected())
            for dependency in (table, piece, placement):
                dependency.write_bytes(dependency.read_bytes() + b" ")
                changed = subprocess.run(command, capture_output=True)
                self.assertEqual(changed.returncode, 0, changed.stderr)
                value = contract.validate(contract.loads(changed.stdout), expected_inputs_sha256=expected())
                self.assertNotEqual(value["inputs_sha256"], first_document["inputs_sha256"])
                first_document = value
            saved = table.read_bytes()
            table.unlink()
            missing = subprocess.run(command, capture_output=True)
            self.assertEqual(missing.returncode, 1)
            self.assertEqual(missing.stdout, b"")
            table.write_bytes(saved)
            placement.write_bytes(contract.canonical({"spacing": 0}))
            clamped = subprocess.run(command, capture_output=True)
            self.assertEqual(clamped.returncode, 1)
            self.assertEqual(clamped.stdout, b"")

    def test_stdout_failure_refuses_after_flushing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            data.mkdir()
            preset = root / "flat.json"
            original = contract.canonical(self.flat_preset())
            preset.write_bytes(original)
            for kind in ("mouth", "horizon", "coverage"):
                command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                           "--seed", "17", "--kind", kind, "--origin-mm", "0", "0",
                           "--radius-m", "1", "--y-range-m", "0", "100",
                           "--view-distance-m", "0", "--limit", "64"]
                with preset.open("rb") as output:
                    result = subprocess.run(command, stdout=output, stderr=subprocess.PIPE,
                                            check=False)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"cannot write station discovery stdout", result.stderr)
                self.assertEqual(preset.read_bytes(), original)

    if os.name == "posix":
        def test_closed_stdout_pipe_refuses(self):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                data = root / "data"
                data.mkdir()
                preset = root / "flat.json"
                preset.write_bytes(contract.canonical(self.flat_preset()))
                for kind in ("mouth", "arch", "horizon", "coverage"):
                    with self.subTest(kind=kind):
                        command = [self.cli, "--preset-json", str(preset), "--data-root", str(data),
                                   "--seed", "17", "--kind", kind, "--origin-mm", "0", "0",
                                   "--radius-m", "1", "--y-range-m", "0", "100",
                                   "--view-distance-m", "0", "--limit", "64"]
                        read_fd, write_fd = os.pipe()
                        # Close before launch: no pipe capacity or reader scheduling race.
                        os.close(read_fd)
                        with os.fdopen(write_fd, "wb") as output:
                            # Restore SIGPIPE to default after Python's inherited ignore.
                            result = subprocess.run(command, stdout=output, stderr=subprocess.PIPE,
                                                    restore_signals=True, check=False)
                        self.assertEqual(result.returncode, 1, result.stderr)
                        self.assertIn(b"station discovery refused: cannot write station discovery stdout",
                                      result.stderr)

    def test_repeat_empty_disabled_world_and_refusals(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            preset = root / "flat.json"
            data = root / "data"
            data.mkdir()
            fields = self.flat_preset()
            preset.write_bytes(contract.canonical(fields))
            command = [self.cli, "--preset-json", str(preset), "--data-root", str(data), "--seed", "17",
                       "--kind", "mouth", "--origin-mm", "-4000", "-4000", "--radius-m", "16",
                       "--y-range-m", "0", "100", "--view-distance-m", "12", "--limit", "64"]
            first = subprocess.run(command, capture_output=True)
            second = subprocess.run(command, capture_output=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(first.stdout, second.stdout)
            self.assertEqual(first.stdout[-2:], b"}\n")
            self.assertEqual(first.stdout.count(b"\n"), 1)
            self.assertNotIn(b"\r", first.stdout)
            result = contract.validate(contract.loads(first.stdout),
                expected_inputs_sha256=contract.input_identity({"preset.json": preset.read_bytes()}))
            self.assertFalse(result["certified"])
            self.assertEqual(result["candidates"], [])
            for kind, count in (("horizon", 4), ("coverage", 5)):
                control_command = list(command)
                control_command[control_command.index("--kind") + 1] = kind
                control = subprocess.run(control_command, capture_output=True)
                self.assertEqual(control.returncode, 0, control.stderr)
                self.assertEqual(len(contract.validate(contract.loads(control.stdout))["candidates"]), count)
            for args, code in ((command + ["--seed", "17"], 2), (command[:-1], 2),
                               (command + ["--unsupported"], 2)):
                refusal = subprocess.run(args, capture_output=True)
                self.assertEqual(refusal.returncode, code)
                self.assertEqual(refusal.stdout, b"")
            for raw in (b'{"x":1,"x":2}', b'{"x":NaN}', b'{}'):
                preset.write_bytes(raw)
                refusal = subprocess.run(command, capture_output=True)
                self.assertEqual(refusal.returncode, 1)
                self.assertEqual(refusal.stdout, b"")
            for field, values in (("--seed", ("-1", "2147483616")),
                                  ("--radius-m", ("0", "16385")),
                                  ("--view-distance-m", ("-1", "16385"))):
                preset.write_bytes(contract.canonical(fields))
                for value in values:
                    args = list(command)
                    args[args.index(field) + 1] = value
                    refusal = subprocess.run(args, capture_output=True)
                    self.assertEqual(refusal.returncode, 1)
                    self.assertEqual(refusal.stdout, b"")
            for mutation in ("warning", "clamping", "integer", "block", "budget"):
                invalid = copy.deepcopy(fields)
                features = invalid["generation_params"]["features"]
                if mutation == "warning":
                    features["unknown_parameter"] = 1
                elif mutation == "clamping":
                    features.update(surface_breaks_enabled=True, feature_cell_size=16, max_feature_radius=16)
                elif mutation == "integer":
                    invalid["generation_params"]["terrain"]["octaves"] = 1.5
                elif mutation == "block":
                    invalid["generation_params"]["terrain"]["shaping"] = []
                else:
                    features.update(surface_breaks_enabled=True, feature_cell_size=16, max_feature_radius=3)
                preset.write_bytes(contract.canonical(invalid))
                args = list(command)
                if mutation == "budget":
                    args[args.index("--radius-m") + 1] = "16384"
                refusal = subprocess.run(args, capture_output=True)
                self.assertEqual(refusal.returncode, 1, (mutation, refusal.stderr))
                self.assertEqual(refusal.stdout, b"")
            fields["generation_params"]["features"]["unknown_parameter"] = 1
            preset.write_bytes(contract.canonical(fields))
            refusal = subprocess.run(command, capture_output=True)
            self.assertEqual(refusal.returncode, 1)
            self.assertEqual(refusal.stdout, b"")


class NumericSmoke(unittest.TestCase):
    cli = None

    def test_unsupported_floating_point_states_refuse_before_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            data = root / "data"
            data.mkdir()
            preset = root / "flat.json"
            preset.write_bytes(contract.canonical(NativeSmoke.flat_preset()))
            arguments = ["--preset-json", str(preset), "--data-root", str(data),
                         "--seed", "17", "--kind", "coverage", "--origin-mm", "0", "0",
                         "--radius-m", "1", "--y-range-m", "0", "100",
                         "--view-distance-m", "0", "--limit", "64"]
            # The same entry and inputs must produce a complete document when
            # the controls are supported, ruling out unrelated startup refusal.
            supported = subprocess.run([self.cli, "supported", *arguments],
                                       capture_output=True, check=False)
            self.assertEqual(supported.returncode, 0, supported.stderr)
            self.assertEqual(len(contract.validate(contract.loads(supported.stdout))["candidates"]), 5)
            states = ("downward", "upward", "toward-zero", "ftz", "daz",
                      "simd-downward", "simd-upward", "simd-toward-zero")
            for state in states:
                for inputs in (arguments, []):
                    with self.subTest(state=state, valid_arguments=bool(inputs)):
                        refused = subprocess.run([self.cli, state, *inputs],
                                                 capture_output=True, check=False)
                        self.assertEqual(refused.returncode, 1, refused.stderr)
                        self.assertEqual(refused.stdout, b"")
                        # Some CRTs expose SIMD rounding through fegetround too.
                        self.assertRegex(refused.stderr,
                                         br"station discovery refused: unsupported "
                                         br"(?:floating-point profile|SIMD floating-point environment)")
                        self.assertNotIn(b"station discovery usage:", refused.stderr)


def load_tests(loader, tests, pattern):
    # Generic unittest discovery remains stdlib-only. The ordinary CLI CTest
    # entry supplies its built executable explicitly to run the native cases.
    return loader.loadTestsFromTestCase(ContractTests)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli")
    parser.add_argument("--numeric-cli")
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ContractTests)
    if args.cli is not None:
        NativeSmoke.cli = args.cli
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(NativeSmoke))
    if args.numeric_cli is not None:
        NumericSmoke.cli = args.numeric_cli
        suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(NumericSmoke))
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())

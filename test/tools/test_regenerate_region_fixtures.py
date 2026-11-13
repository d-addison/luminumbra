#!/usr/bin/env python3
"""Isolated stdlib regressions for region fixture capture admission/publication."""

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/regenerate_region_fixtures.py"
SPEC = importlib.util.spec_from_file_location("region_regenerator", TOOL)
REGEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REGEN)


def capture(indexed=False):
    region = (ROOT / "test/fixtures/active-regions/devel-off/r.0.0.lmr").read_bytes()
    manifest = {"container": "LMR1", "container_version": 2,
                "schema": "luminumbra.persistence.world_manifest.v1"}
    scenarios = {}
    for scenario in REGEN.SCENARIOS:
        payloads = {REGEN.REGION: region, REGEN.MANIFEST: REGEN.canonical_json(manifest)}
        if indexed or scenario.endswith("plant-edit"):
            payloads[REGEN.PLANTS] = REGEN.canonical_json({
                "schema": "luminumbra.plants.v2" if indexed
                else "luminumbra.ecs.entity_snapshot.v1",
                "entities": [{}] if scenario.endswith("plant-edit") else [], "next_id": 2})
        if indexed:
            payloads[REGEN.INDEX] = REGEN.canonical_json({
                "schema": "luminumbra.snapshot.v1", "generation": 1, "tick": 7,
                "txn_id": 1, "anchors": [[0.0, 0.0, 0.0]], "members": [
                    {"path": name, "state": "present", "class": "authoritative",
                     "bytes": len(payloads[name]), "fnv64": REGEN.fnv64(payloads[name]),
                     "generation": 1} for name in sorted(payloads)]})
        members = {name: {"bytes": len(data), "hex": data.hex(), "fnv64": REGEN.fnv64(data)}
                   for name, data in payloads.items()}
        parts = {key: "123456789abcdef0" for key in REGEN.SUBHASHES}
        parts["scents"] = ""
        scenarios[scenario] = {"members": members, "hashes": {
            "live_world_hash": REGEN.world_hash(parts), "loaded_world_hash": REGEN.world_hash(parts),
            "live_subhashes": parts, "loaded_subhashes": dict(parts)}}
    return {"schema": REGEN.SCHEMA, "scenarios": scenarios}


class RegionFixtureRegeneratorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="region-regenerator-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "capture.json"
        self.output = self.root / "fixtures.zip"

    def write(self, value):
        self.source.write_bytes(REGEN.canonical_json(value))

    def cli(self):
        return subprocess.run([sys.executable, "-I", "-B", str(TOOL), "--capture",
                               str(self.source), "--output", str(self.output)],
                              capture_output=True, text=True, check=False)

    def assert_refused(self, value):
        self.write(value)
        self.output.write_bytes(b"previous output")
        with self.assertRaises((ValueError, OSError)):
            REGEN.regenerate(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous output")
        self.assertEqual(sorted(item.name for item in self.root.iterdir()),
                         ["capture.json", "fixtures.zip"])

    @staticmethod
    def replace_index(value, document):
        payload = REGEN.canonical_json(document)
        value["scenarios"]["devel-off"]["members"][REGEN.INDEX] = {
            "bytes": len(payload), "hex": payload.hex(), "fnv64": REGEN.fnv64(payload)}

    @staticmethod
    def index(value):
        return json.loads(bytes.fromhex(
            value["scenarios"]["devel-off"]["members"][REGEN.INDEX]["hex"]))

    def test_same_path_refused_without_modification(self):
        self.write(capture())
        original = self.source.read_bytes()
        for output in (self.source, self.root / "." / self.source.name):
            with self.subTest(output=output):
                with self.assertRaisesRegex(ValueError, "same file"):
                    REGEN.regenerate(self.source, output)
                self.assertEqual(self.source.read_bytes(), original)
                self.assertEqual(list(self.root.iterdir()), [self.source])
        self.output = self.source
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("same file", result.stderr)
        self.assertEqual(self.source.read_bytes(), original)

    def test_hard_link_alias_refused_without_modification(self):
        self.write(capture())
        original = self.source.read_bytes()
        os.link(self.source, self.output)
        with self.assertRaisesRegex(ValueError, "same file"):
            REGEN.regenerate(self.source, self.output)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(self.output.read_bytes(), original)
        self.assertTrue(os.path.samefile(self.source, self.output))
        self.assertEqual(set(self.root.iterdir()), {self.source, self.output})

    def test_resolved_alias_and_identity_errors_precede_writes(self):
        self.write(capture())
        original = self.source.read_bytes()
        # Exercise a resolved alias without requiring Windows symlink privileges.
        with mock.patch.object(REGEN.Path, "resolve", return_value=self.source):
            with self.assertRaisesRegex(ValueError, "same file"):
                REGEN.regenerate(self.source, self.output)
        with mock.patch.object(REGEN.os.path, "samefile", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                REGEN.regenerate(self.source, self.output)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(list(self.root.iterdir()), [self.source])

    def test_each_required_index_field_is_required(self):
        for field in ("schema", "generation", "tick", "txn_id", "anchors", "members"):
            with self.subTest(field=field):
                value = capture(indexed=True)
                document = self.index(value)
                del document[field]
                self.replace_index(value, document)
                self.assert_refused(value)
        for state in ("present", "absent"):
            for field in ("path", "class", "state", "bytes", "fnv64", "generation"):
                with self.subTest(member_field=field, state=state):
                    value = capture(indexed=True)
                    document = self.index(value)
                    if state == "absent":
                        document["members"].append({
                            "path": "world_info.json", "state": "absent",
                            "class": "authoritative", "bytes": 0,
                            "fnv64": "0" * 16, "generation": 1})
                    target = 0 if state == "present" else -1
                    del document["members"][target][field]
                    self.replace_index(value, document)
                    self.assert_refused(value)

    def test_index_numeric_domains_and_anchors(self):
        for field in ("generation", "tick", "txn_id"):
            for invalid in (None, True, -1, 1.0, 1e38, 1 << 64, "1"):
                with self.subTest(field=field, invalid=invalid):
                    value = capture(indexed=True)
                    document = self.index(value)
                    document[field] = invalid
                    self.replace_index(value, document)
                    self.assert_refused(value)
        for anchors in (None, {}, [None], [[]], [[0, 0]], [[0, 0, 0, 0]],
                        [[True, 0, 0]], [["0", 0, 0]], [[None, 0, 0]], [[3.5e38, 0, 0]]):
            with self.subTest(anchors=anchors):
                value = capture(indexed=True)
                document = self.index(value)
                document["anchors"] = anchors
                self.replace_index(value, document)
                self.assert_refused(value)
        value = capture(indexed=True)
        document = self.index(value)
        document["generation"] = 0
        self.replace_index(value, document)
        self.assert_refused(value)

    def test_index_member_metadata_and_order(self):
        for field, invalids in (
                ("class", (None, [], "unknown")), ("state", (None, [], "unknown")),
                ("path", (None, [], "../escape", "C:/capture")),
                ("bytes", (None, True, -1, 1.0, 1e38, 1 << 64, "1", 0)),
                ("generation", (None, True, -1, 1.0, 1e38, 1 << 64, "1")),
                ("fnv64", (None, [], "", "f" * 17, "g" * 16, "0" * 16))):
            for invalid in invalids:
                with self.subTest(field=field, invalid=invalid):
                    value = capture(indexed=True)
                    document = self.index(value)
                    document["members"][0][field] = invalid
                    self.replace_index(value, document)
                    self.assert_refused(value)
        value = capture(indexed=True)
        document = self.index(value)
        document["members"].reverse()
        self.replace_index(value, document)
        self.assert_refused(value)

    def test_index_inventory_and_unvalidated_capture_metadata_refused(self):
        for state in ("present", "absent"):
            value = capture(indexed=True)
            document = self.index(value)
            document["members"].append({"path": "world_info.json", "state": state,
                                        "class": "authoritative", "bytes": 0,
                                        "fnv64": "0" * 16, "generation": 1})
            if state == "absent":
                document["members"][0]["state"] = "absent"
            self.replace_index(value, document)
            self.assert_refused(value)
        for invalid in (None, [], {}, {"bytes": 1}, {"bytes": "1", "fnv64": "0" * 16}):
            value = capture(indexed=True)
            value["scenarios"]["devel-off"]["members"][REGEN.MANIFEST] = invalid
            self.assert_refused(value)

    def test_index_decoder_boundaries_and_extensions(self):
        value = capture(indexed=True)
        document = self.index(value)
        document.update(generation=(1 << 64) - 1, tick=0, txn_id=0, anchors=[], future=True)
        document["members"][0].update(generation=0, future=True, **{"class": "derived"})
        document["members"][0]["fnv64"] = document["members"][0]["fnv64"].upper()
        self.replace_index(value, document)
        self.write(value)
        self.assertEqual(self.cli().returncode, 0)
        document.update(tick=(1 << 64) - 1, txn_id=(1 << 64) - 1,
                        anchors=[[-3.4028234663852886e38, 0, 3.4028234663852886e38]])
        self.replace_index(value, document)
        self.write(value)
        self.assertEqual(self.cli().returncode, 0)

    def test_reproducible_sorted_archive_and_computed_pins(self):
        for indexed in (False, True):
            with self.subTest(indexed=indexed):
                original = capture(indexed)
                self.write(original)
                self.assertEqual(self.cli().returncode, 0)
                first = self.output.read_bytes()
                # Reverse every JSON object's insertion order; archive stays identical.
                def reverse(value):
                    if isinstance(value, dict):
                        return {key: reverse(item) for key, item in reversed(list(value.items()))}
                    return value
                self.source.write_text(json.dumps(reverse(original)), encoding="utf-8")
                with mock.patch.object(REGEN.Path, "glob", side_effect=AssertionError("glob")), \
                     mock.patch.object(REGEN.os, "scandir", side_effect=AssertionError("scan")):
                    REGEN.regenerate(self.source, self.output)
                self.assertEqual(self.output.read_bytes(), first)
                with zipfile.ZipFile(self.output) as archive:
                    names = archive.namelist()
                    self.assertEqual(names, sorted(names))
                    expected = {"capture-manifest.json"}
                    for scenario, entry in original["scenarios"].items():
                        expected.add(scenario + "/hashes.json")
                        for path, member in entry["members"].items():
                            name = scenario + "/" + path.rsplit("/", 1)[-1]
                            expected.add(name)
                            self.assertEqual(archive.read(name), bytes.fromhex(member["hex"]))
                    self.assertEqual(set(names), expected)
                    pins = json.loads(archive.read("capture-manifest.json"))["members"]
                    self.assertEqual(set(pins), expected - {"capture-manifest.json"})
                    for name, pin in pins.items():
                        payload = archive.read(name)
                        self.assertEqual(pin, {"bytes": len(payload),
                                              "sha256": hashlib.sha256(payload).hexdigest()})
                    for info in archive.infolist():
                        self.assertEqual(info.date_time, (1980, 1, 1, 0, 0, 0))
                        self.assertEqual(info.compress_type, zipfile.ZIP_STORED)

    def test_missing_unknown_and_escaping_members(self):
        for scenario in REGEN.SCENARIOS:
            baseline = capture()
            for path in baseline["scenarios"][scenario]["members"]:
                value = copy.deepcopy(baseline)
                del value["scenarios"][scenario]["members"][path]
                with self.subTest(scenario=scenario, missing=path):
                    self.assert_refused(value)
            for path in ("", "../escape", "/absolute", "C:/capture", "chunks\\region\\x",
                         "chunks/region/r.1.0.lmr", "chunks/region/plant-identities.json"):
                value = copy.deepcopy(baseline)
                value["scenarios"][scenario]["members"][path] = {"bytes": 1, "hex": "00"}
                with self.subTest(scenario=scenario, unknown=path):
                    self.assert_refused(value)

    def test_crlf_capture_and_unicode_paths_preserve_bytes(self):
        self.source = self.root / "captur\u00e9.json"
        self.output = self.root / "fixtures-\u00e9.zip"
        original = REGEN.canonical_json(capture())
        self.source.write_bytes(original)
        self.assertEqual(self.cli().returncode, 0)
        first = self.output.read_bytes()
        self.source.write_bytes(original.replace(b"\n", b"\r\n"))
        self.assertEqual(self.cli().returncode, 0)
        self.assertEqual(self.output.read_bytes(), first)

    def test_index_absent_metadata_and_cursor_boundary(self):
        value = capture(indexed=True)
        entry = value["scenarios"]["devel-off"]
        document = json.loads(bytes.fromhex(entry["members"][REGEN.INDEX]["hex"]))
        document["members"].append({"path": "world_info.json", "state": "absent",
                                    "class": "authoritative", "bytes": 0,
                                    "fnv64": "0" * 16, "generation": 1})
        for cursor in (1, 1 << 63):
            payload = REGEN.canonical_json({"schema": "luminumbra.plants.v2",
                                            "entities": [], "next_id": cursor})
            entry["members"][REGEN.PLANTS] = {"bytes": len(payload), "hex": payload.hex(),
                                              "fnv64": REGEN.fnv64(payload)}
            document["members"][0].update(bytes=len(payload), fnv64=REGEN.fnv64(payload))
            self.replace_index(value, document)
            self.write(value)
            self.assertEqual(self.cli().returncode, 0)

    def test_missing_scenarios_and_unknown_metadata(self):
        for field in ("schema", "scenarios"):
            value = capture()
            del value[field]
            self.assert_refused(value)
        for scenario in REGEN.SCENARIOS:
            value = capture()
            del value["scenarios"][scenario]
            self.assert_refused(value)
        for container in ("root", "scenario", "member", "hashes"):
            value = capture()
            entry = value["scenarios"]["devel-off"]
            target = {"root": value, "scenario": entry, "member": entry["members"][REGEN.REGION],
                      "hashes": entry["hashes"]}[container]
            target["unknown"] = 1
            self.assert_refused(value)
        for wrong in ([], None, "future"):
            value = capture()
            value["schema"] = wrong
            self.assert_refused(value)

    def test_size_hex_and_type_refusals(self):
        for size in (True, -1, 0, REGEN.MAX_MEMBER_BYTES + 1, 1e38, "1", None):
            value = capture()
            value["scenarios"]["devel-off"]["members"][REGEN.REGION]["bytes"] = size
            self.assert_refused(value)
        for encoded in ("", "00", "0g", "AA", "00 00", [], None):
            value = capture()
            value["scenarios"]["devel-off"]["members"][REGEN.REGION]["hex"] = encoded
            self.assert_refused(value)
        for field in ("members", "hashes"):
            value = capture()
            value["scenarios"]["devel-off"][field] = []
            self.assert_refused(value)

    def test_incomplete_or_invalid_hashes(self):
        for phase in ("live", "loaded"):
            for suffix in ("_world_hash", "_subhashes"):
                value = capture()
                del value["scenarios"]["devel-off"]["hashes"][phase + suffix]
                self.assert_refused(value)
            for bad in ("", "f" * 17, "F" * 16, 0, None):
                value = capture()
                value["scenarios"]["devel-off"]["hashes"][phase + "_world_hash"] = bad
                self.assert_refused(value)
            for key in REGEN.SUBHASHES:
                value = capture()
                del value["scenarios"]["devel-off"]["hashes"][phase + "_subhashes"][key]
                self.assert_refused(value)
            value = capture()
            value["scenarios"]["devel-off"]["hashes"][phase + "_subhashes"]["wind"] = "bad"
            self.assert_refused(value)

    def test_malformed_payload_and_format_mismatch(self):
        for name, payload in ((REGEN.REGION, b"LMR1"), (REGEN.REGION, b"x" * 24),
                              (REGEN.MANIFEST, b"{}"), (REGEN.PLANTS, b"{}"),
                              (REGEN.MANIFEST, b"{\"x\":1,\"x\":2}"),
                              (REGEN.MANIFEST, b"{\"x\":1e999}"),
                              (REGEN.MANIFEST, b"{\"x\":NaN}"),
                              (REGEN.INDEX, b"{}")):
            value = capture(indexed=True)
            value["scenarios"]["devel-off"]["members"][name] = {
                "bytes": len(payload), "hex": payload.hex(), "fnv64": REGEN.fnv64(payload)}
            self.assert_refused(value)
        value = capture(indexed=True)
        del value["scenarios"]["devel-off-plant-edit"]["members"][REGEN.INDEX]
        self.assert_refused(value)

    def test_changed_bytes_and_mismatched_composed_hash_refused(self):
        value = capture()
        member = value["scenarios"]["devel-off"]["members"][REGEN.REGION]
        member["hex"] = "00" + member["hex"][2:]
        self.assert_refused(value)
        for phase in ("live", "loaded"):
            value = capture()
            value["scenarios"]["devel-off"]["hashes"][phase + "_world_hash"] = "0" * 16
            self.assert_refused(value)

    def test_incomplete_roster_cursor_and_index_refusals(self):
        def replace_document(value, name, document):
            payload = REGEN.canonical_json(document)
            value["scenarios"]["devel-off-plant-edit"]["members"][name] = {
                "bytes": len(payload), "hex": payload.hex(), "fnv64": REGEN.fnv64(payload)}

        for cursor in (None, True, 0, -1, (1 << 63) + 1, 1e38):
            value = capture(indexed=True)
            replace_document(value, REGEN.PLANTS, {"schema": "luminumbra.plants.v2",
                                                  "entities": [{}], "next_id": cursor})
            self.assert_refused(value)
        value = capture(indexed=True)
        replace_document(value, REGEN.PLANTS, {"schema": "luminumbra.plants.v2",
                                              "entities": [], "next_id": 2})
        self.assert_refused(value)
        for records in ([], [None], [{"path": "../x", "state": "present"}],
                        [{"path": REGEN.REGION, "state": "unknown"}],
                        [{"path": REGEN.REGION, "state": "absent"}],
                        [{"path": REGEN.REGION, "state": "present"}] * 2,
                        [{"path": REGEN.REGION, "state": "present"}] * 17):
            value = capture(indexed=True)
            document = self.index(value)
            document["members"] = records
            replace_document(value, REGEN.INDEX, document)
            self.assert_refused(value)

    def test_malformed_json_cli_preserves_existing_and_absent_output(self):
        for payload in (b"", b"{", b"[]", b"\xff", b'{"schema":1,"schema":2}',
                        b"[" * 65 + b"]" * 65, b"{\"x\":Infinity}"):
            for existing in (False, True):
                if existing:
                    self.output.write_bytes(b"previous")
                else:
                    self.output.unlink(missing_ok=True)
                self.source.write_bytes(payload)
                result = self.cli()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("capture refused:", result.stderr)
                self.assertEqual(self.output.exists(), existing)
                if existing:
                    self.assertEqual(self.output.read_bytes(), b"previous")

    def test_bounded_regular_handle_input(self):
        self.write(capture())
        for size in (0, REGEN.MAX_CAPTURE_BYTES + 1, self.source.stat().st_size - 1):
            info = mock.Mock(st_mode=stat.S_IFREG, st_size=size)
            with mock.patch.object(REGEN.os, "fstat", return_value=info):
                with self.assertRaises(ValueError):
                    REGEN.read_capture(self.source)
        info = mock.Mock(st_mode=stat.S_IFIFO, st_size=1)
        with mock.patch.object(REGEN.os, "fstat", return_value=info):
            with self.assertRaises(ValueError):
                REGEN.read_capture(self.source)
        with mock.patch.object(REGEN.os, "open", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                REGEN.read_capture(self.source)
        self.source.unlink()
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(self.output.exists())

    def test_publication_failures_are_atomic_and_clean(self):
        self.write(capture())
        for existing in (False, True):
            for owner, method in ((REGEN.zipfile.ZipFile, "writestr"),
                                  (REGEN.os, "fsync"), (REGEN.os, "replace")):
                self.output.unlink(missing_ok=True)
                if existing:
                    self.output.write_bytes(b"previous")
                with mock.patch.object(owner, method, side_effect=OSError("injected failure")):
                    with self.assertRaises(OSError):
                        REGEN.regenerate(self.source, self.output)
                self.assertEqual(self.output.exists(), existing)
                if existing:
                    self.assertEqual(self.output.read_bytes(), b"previous")
                self.assertEqual({item.name for item in self.root.iterdir()},
                                 {"capture.json", "fixtures.zip"} if existing else {"capture.json"})

    def test_staging_creation_failure_preserves_destination(self):
        self.write(capture())
        self.output.write_bytes(b"previous")
        with mock.patch.object(REGEN.tempfile, "mkstemp", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                REGEN.regenerate(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous")
        self.source = self.root
        self.assertNotEqual(self.cli().returncode, 0)
        self.assertEqual(self.output.read_bytes(), b"previous")

    def test_registration_and_dormant_native_capture(self):
        cmake = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        paths = (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8")
        self.assertIn("common/RegionFixtureCapture_test.cpp", cmake)
        self.assertIn("RegionFixtureRegeneratorContract", cmake)
        self.assertIn("test/tools/test_regenerate_region_fixtures.py", cmake)
        self.assertIn('"test/tools/test_regenerate_region_fixtures.py"', paths)
        self.assertIn('"tools/regenerate_region_fixtures.py"', paths)
        native = (ROOT / "test/common/RegionFixtureCapture_test.cpp").read_text(encoding="utf-8")
        self.assertLess(native.index('std::getenv("LUMINUMBRA_REGION_FIXTURE_CAPTURE")'),
                        native.index("Capture(false)"))
        self.assertIn("if (!RequestedCapture(destination)) {\n        GTEST_SKIP()", native)
        self.assertIn('_wdupenv_s(&value, &size, L"LUMINUMBRA_REGION_FIXTURE_CAPTURE")', native)
        self.assertIn('_wgetenv(L"LUMINUMBRA_REGION_FIXTURE_CAPTURE")', native)
        for name in (REGEN.REGION, REGEN.MANIFEST, REGEN.PLANTS, REGEN.INDEX):
            self.assertIn('"' + name + '"', native)


if __name__ == "__main__":
    unittest.main()

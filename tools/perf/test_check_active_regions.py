#!/usr/bin/env python3
"""Evidence refusal regressions; synthetic artifacts only, no engine launches."""
from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_active_regions as check
from test_capture_active_regions import encoded, fixture, run_capture, trace


class ActiveRegionEvidenceContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.args, runner = fixture(Path(self.temporary.name))
        self.expected = run_capture(self.args, runner)
        self.root = self.args.evidence_dir
        self.original = check.parse((self.root / "capture.json").read_bytes())
        self.manifest = copy.deepcopy(self.original)

    def update(self, name, data):
        raw = data if isinstance(data, bytes) else encoded(data)
        (self.root / name).write_bytes(raw)
        self.manifest["files"][name] = {"size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}

    def artifact(self, name):
        return check.parse((self.root / name).read_bytes())

    def validate(self):
        return check.validate(self.root, self.manifest)

    def test_valid_deterministic_output_and_convergent_availability(self):
        self.assertEqual(self.validate(), self.expected)
        first = io.StringIO()
        with contextlib.redirect_stdout(first):
            self.assertEqual(check.main([str(self.root)]), 0)
        second = io.StringIO()
        with contextlib.redirect_stdout(second):
            self.assertEqual(check.main([str(self.root)]), 0)
        self.assertEqual(first.getvalue(), second.getvalue())
        self.assertEqual(first.getvalue(), check.canonical(self.expected) + "\n")
        # The fixture has different availability digests for each worker and a
        # false per-tick replay match. Final authority equality still qualifies.
        self.assertEqual(self.expected["availability_timing"], "observational")

    def test_save_members_may_evolve_while_verified_edit_stays_identical(self):
        tour = self.artifact("tour.json")
        for index, stage in enumerate(tour["stages"][2:]):
            saved = self.artifact(stage["receipt"])
            saved["files"][0]["sha256"] = hashlib.sha256(str(index).encode()).hexdigest()
            stage["saved_member_sha256"] = saved["files"][0]["sha256"]
            lines = "".join(f"{row['path']}\0{row['size']}\0{row['sha256']}\n" for row in saved["files"])
            saved["tree_digest"] = hashlib.sha256(lines.encode()).hexdigest()
            self.update(stage["receipt"], saved)
        self.update("tour.json", tour)
        self.assertTrue(self.validate()["passed"])

    def test_reconciled_header_tick_remains_valid(self):
        for name in ("tour/dump.json", "verified-ledger.json"):
            value = self.artifact(name)
            value["header_tick"] += 1
            self.update(name, value)
        self.assertTrue(self.validate()["passed"])

    def test_boolean_and_integer_metadata_cannot_be_replaced_by_equal_numbers(self):
        for name, block, field, value in (("tour.json", "configuration", "surface_radius", True),
                                          ("tour.json", "effective_simulation", "active_regions", 1),
                                          ("w2/moving.json", "effective_simulation", "active_regions", 1.0),
                                          ("tour/dump.json", "config", "reduced_shift", True)):
            with self.subTest(name=name, field=field):
                self.manifest = copy.deepcopy(self.original)
                raw = (self.root / name).read_bytes()
                data = check.parse(raw)
                data[block][field] = value
                self.update(name, data)
                try:
                    with self.assertRaises(ValueError):
                        self.validate()
                finally:
                    (self.root / name).write_bytes(raw)

    def test_cross_worker_authority_and_schedule_mismatch(self):
        for failure in ("record", "schedule", "moving", "subhash"):
            with self.subTest(failure=failure):
                self.manifest = copy.deepcopy(self.original)
                names = ("w4/replay.json", "w4/record.log", "w4/session.lrec.regions.json", "w4/moving.json")
                originals = {name: (self.root / name).read_bytes() for name in names}
                if failure == "record":
                    replay = self.artifact("w4/replay.json")
                    replay["end_world_hash"] = "f" * 16
                    self.update("w4/replay.json", replay)
                    self.update("w4/record.log", originals["w4/record.log"].replace(b"0123456789abcdef", b"ffffffffffffffff"))
                elif failure == "schedule":
                    value = trace()
                    # Independently valid FNV checksum for a different complete trace.
                    value["ticks"][1]["digest"] = "f" * 16
                    bare = {"schema": value["schema"], "ticks": value["ticks"]}
                    checksum = 14695981039346656037
                    for byte in check.canonical(bare).encode():
                        checksum = ((checksum ^ byte) * 1099511628211) % 2**64
                    value["checksum"] = f"{checksum:016x}"
                    self.update("w4/session.lrec.regions.json", value)
                else:
                    moving = self.artifact("w4/moving.json")
                    if failure == "moving":
                        moving["world_hash"] = moving["world_hash_replay"] = "f" * 16
                        for run in moving["runs"]:
                            run["world_hash"] = "f" * 16
                    else:
                        for run in moving["runs"]:
                            run["sub_hashes"]["plants"] = "f" * 16
                    self.update("w4/moving.json", moving)
                try:
                    with self.assertRaisesRegex(ValueError, "cross-worker"):
                        self.validate()
                finally:
                    for name, raw in originals.items():
                        (self.root / name).write_bytes(raw)

    def test_missing_trace_dump_receipt_logs_and_inventory(self):
        for name in ("w1/session.lrec.regions.json", "w2/session.lrec", "w4/replay.json", "w1/moving.json",
                     "tour/dump.json", "verified-ledger.json", "tour/saved.json", "tour/reopened.json", "w4/moving.log"):
            with self.subTest(name=name):
                raw = (self.root / name).read_bytes()
                (self.root / name).unlink()
                with self.assertRaises(OSError):
                    self.validate()
                (self.root / name).write_bytes(raw)
                removed = self.manifest["files"].pop(name)
                with self.assertRaises(ValueError):
                    self.validate()
                self.manifest["files"][name] = removed

    def test_trace_refuses_partial_reordered_duplicate_and_corrupt_samples(self):
        valid = trace()
        variants = [None, [], {}, dict(valid, schema="luminumbra.region_schedule_trace.v2"),
                    dict(valid, checksum="0" * 16), dict(valid, ticks=valid["ticks"][:-1]),
                    dict(valid, ticks=list(reversed(valid["ticks"]))),
                    dict(valid, ticks=[valid["ticks"][0]] * 2),
                    dict(valid, ticks=[{"tick": True, "digest": "f" * 16}] * 2),
                    dict(valid, ticks=[{"tick": 1, "digest": "invalid"}] * 2)]
        for value in variants:
            with self.subTest(value=value), self.assertRaises(ValueError):
                check.schedule(value, 2)
        check.schedule(valid, 2)
        self.assertEqual(valid["checksum"], "4bd26299d5434b64")

    def test_process_commands_and_worker_receipts_are_bound(self):
        for mode in ("record", "replay", "moving"):
            for field in ("returncode", "command", "environment", "log"):
                with self.subTest(mode=mode, field=field):
                    self.manifest = copy.deepcopy(self.original)
                    value = self.manifest["cells"][1][mode]
                    if field == "returncode":
                        value[field] = True
                    elif field == "command":
                        value[field].append("--sim-budget-profile")
                    elif field == "environment":
                        value[field]["LUMINUMBRA_JOB_WORKERS"] = "4"
                    else:
                        value[field] = "w1/record.log"
                    with self.assertRaises(ValueError):
                        self.validate()

    def test_missing_disabled_or_mixed_effective_flags_refused(self):
        for flag in ("active_regions", "plant_growth", "future_gate", "finite_hydrology", "weather_rain"):
            for value in (False, None, 1):
                with self.subTest(flag=flag, value=value):
                    self.manifest = copy.deepcopy(self.original)
                    self.manifest["effective_simulation"][flag] = value
                    with self.assertRaises(ValueError):
                        self.validate()
        self.manifest = copy.deepcopy(self.original)
        moving = self.artifact("w2/moving.json")
        moving["effective_simulation"]["active_regions"] = False
        self.update("w2/moving.json", moving)
        with self.assertRaises(ValueError):
            self.validate()

    def test_malformed_evidence_and_unsupported_metadata(self):
        variants = [None, [], {}, dict(self.original, schema="future"),
                    dict(self.original, identity={}), dict(self.original, cells=[]),
                    dict(self.original, files={}), dict(self.original, configuration={})]
        for value in variants:
            with self.subTest(value=value), self.assertRaises(ValueError):
                check.validate(self.root, value)
        for raw in (b'{', b'[]', b'{"schema":1,"schema":2}', b'{"v":NaN}', b'{"v":1e999}', b'\xff'):
            with self.subTest(raw=raw), contextlib.redirect_stderr(io.StringIO()):
                (self.root / "capture.json").write_bytes(raw)
                self.assertEqual(check.main([str(self.root)]), 1)

    def test_configuration_and_identity_bounds(self):
        valid = self.original["configuration"]
        cases = {"ticks": (0, -1, True, check.MAX_TICKS + 1, 1e38),
                 "seed": ("", "a" * 4097, "a\0b", 1337), "preset": ("unknown", None),
                 "surface_radius": (0, 2, True), "collision_radius": (0, 2, True),
                 "autosave_ticks": (0, 9000, True)}
        for key, values in cases.items():
            for value in values:
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    check.configuration(dict(valid, **{key: value}))
        for key in self.original["identity"]:
            for value in (None, "", "g" * 64, 1):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    check.validate_identity(dict(self.original["identity"], **{key: value}))
        for ticks in (1, 30, 31, check.MAX_TICKS):
            check.configuration(dict(valid, ticks=ticks))
        for value in ("C:/long path/server.exe", "//host/share/runtime", "/runtime"):
            check.location(value)
        for value in (None, "", "relative", "C:relative", "/root/../escape", "a\0b"):
            with self.subTest(location=value), self.assertRaises(ValueError):
                check.location(value)

    def test_tampering_traversal_special_files_and_bounds_refused(self):
        (self.root / "w1/replay.json").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.validate()
        for name in ("", "../escape", "/absolute", "C:/escape", "C:escape", "folder\\escape", "a/../b", "a//b", "a\0b"):
            with self.subTest(name=name), self.assertRaises((OSError, ValueError)):
                check.read(self.root, name)
        with self.assertRaises(ValueError):
            check.Evidence(self.root, {"big": {"size": check.MAX_FILE + 1, "sha256": "a" * 64}})
        with self.assertRaises(ValueError):
            check.Evidence(self.root, {str(i): {"size": 0, "sha256": "a" * 64} for i in range(check.MAX_FILES + 1)})
        with self.assertRaises(ValueError):
            check.read(self.root, "w1")
        with self.assertRaises(ValueError):
            check.read(self.root, "systems.json", limit=1)
        with mock.patch.object(check, "MAX_TOTAL", 1), self.assertRaises(ValueError):
            check.digest_file(self.args.binary)
        with mock.patch.object(check, "MAX_TOTAL", 1), self.assertRaises(ValueError):
            check.Evidence(self.root, {"tiny": {"size": 2, "sha256": "a" * 64}})
        # mkfifo is unavailable on Windows; directory refusal above exercises the
        # common non-regular contract there, without requesting symlink privilege.
        if hasattr(os, "mkfifo"):
            os.mkfifo(self.root / "pipe")
            with self.assertRaises(ValueError):
                check.read(self.root, "pipe")

    def test_authority_digest_shapes_match_shipping_artifacts(self):
        for name in ("terrain", "water", "entities", "wind", "weather", "aether", "aether_state",
                     "scents", "ecology", "plants"):
            self.assertTrue(check.authority_digest(name, "a" * 16))
            self.assertFalse(check.authority_digest(name, "a" * 17))
            self.assertFalse(check.authority_digest(name, None))
            self.assertEqual(check.authority_digest(name, ""), name in ("scents", "plants", "aether_state"))
        self.assertTrue(check.authority_digest("water", "a" * 16 + "|waterSimCursor:v1:" + "b" * 16))
        for cursor in ("", "b" * 15, "b" * 17, "g" * 16):
            self.assertFalse(check.authority_digest("water", "a" * 16 + "|waterSimCursor:v1:" + cursor))

    def test_tour_stage_identity_receipts_and_dump_are_required(self):
        original = (self.root / "tour.json").read_bytes()
        for failure in ("stage", "identity", "worker", "failed", "payload", "exporter", "dump", "ledger"):
            with self.subTest(failure=failure):
                self.manifest = copy.deepcopy(self.original)
                tour = check.parse(original)
                if failure == "stage":
                    tour["stages"] = tour["stages"][:-1]
                elif failure == "identity":
                    tour["stages"][3]["identity"]["revision"] = "f" * 40
                elif failure == "worker":
                    tour["stages"][2]["worker_count"] = 2
                elif failure == "failed":
                    tour["stages"][0]["returncode"] = 1
                elif failure == "payload":
                    tour["edited_payload"]["sha256"] = "f" * 64
                elif failure == "exporter":
                    tour["exporter"]["command"] = ["other", "ledger"]
                elif failure == "dump":
                    tour["ledger_dump"] = "tour/saved.json"
                else:
                    tour["ledger"] = "w1/session.lrec"
                self.update("tour.json", tour)
                with self.assertRaises(ValueError):
                    self.validate()
        (self.root / "tour.json").write_bytes(original)

    def test_save_receipt_failure_inputs(self):
        valid = self.artifact("tour/saved.json")
        variants = [None, [], {}, dict(valid, files=[]), dict(valid, file_count=True),
                    dict(valid, total_bytes=-1), dict(valid, tree_digest="f" * 64),
                    dict(valid, files=list(reversed(valid["files"]))),
                    dict(valid, files=[valid["files"][0]] * 2)]
        for field, value in (("path", "../escape"), ("path", "C:/save"), ("size", True),
                             ("size", 2**64), ("sha256", "bad")):
            changed = copy.deepcopy(valid)
            changed["files"][0][field] = value
            variants.append(changed)
        for value in variants:
            with self.subTest(value=value), self.assertRaises(ValueError):
                check.save_receipt(value)
        check.save_receipt(valid)

    def test_replay_and_moving_refuse_partial_success_claims(self):
        for name, fields in (("w2/replay.json", ("passed", "diverged", "tick_count", "end_hash_match",
                                                  "input_records", "checkpoints_verified", "end_world_hash")),
                             ("w2/moving.json", ("passed", "runs", "availability_trace", "world_hash_replay",
                                                  "sub_hashes_match", "effective_simulation"))):
            raw = (self.root / name).read_bytes()
            for field in fields:
                with self.subTest(name=name, field=field):
                    self.manifest = copy.deepcopy(self.original)
                    value = check.parse(raw)
                    del value[field]
                    self.update(name, value)
                    with self.assertRaises(ValueError):
                        self.validate()
            (self.root / name).write_bytes(raw)


if __name__ == "__main__":
    unittest.main()

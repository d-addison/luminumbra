#!/usr/bin/env python3
"""Fast capture regressions using only synthetic files and an in-process runner."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_active_regions as capture
import check_active_regions as check

HASH = "0123456789abcdef"
EFFECTIVE = {"active_regions": True, "plant_growth": True, "future_gate": True,
             "finite_hydrology": True, "weather_rain": True}


def encoded(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def trace(ticks=2):
    value = {"schema": "luminumbra.region_schedule_trace.v1",
             "ticks": [{"tick": tick, "digest": HASH} for tick in range(1, ticks + 1)]}
    checksum = 14695981039346656037
    for byte in json.dumps(value, sort_keys=True, separators=(",", ":")).encode():
        checksum = ((checksum ^ byte) * 1099511628211) % 2**64
    return dict(value, checksum=f"{checksum:016x}")


def receipt(ledger):
    files = [{"path": "chunks/edited.bin", "size": 4, "sha256": hashlib.sha256(b"edit").hexdigest()},
             {"path": "chunks/region/active-regions.arl", "size": len(ledger),
              "sha256": hashlib.sha256(ledger).hexdigest()}]
    lines = "".join(f"{row['path']}\0{row['size']}\0{row['sha256']}\n" for row in files)
    return {"schema": "luminumbra.save_receipt.v1", "files": files, "file_count": len(files),
            "total_bytes": sum(row["size"] for row in files),
            "tree_digest": hashlib.sha256(lines.encode()).hexdigest()}


class FakeRunner:
    def __init__(self, dump):
        self.dump = dump
        self.calls = []
        self.fail = None
        self.omit = None
        self.dirty = False
        self.hook = None

    def __call__(self, argv, **kwargs):
        self.calls.append((argv, kwargs))
        if argv[0] == "git":
            stdout = ("a" * 40 + "\n") if argv[1] == "rev-parse" else (" M source\n" if self.dirty else "")
            return subprocess.CompletedProcess(argv, 0, stdout=stdout)
        if self.hook:
            self.hook(argv, kwargs)
        if len(argv) == 2:
            kwargs["stdout"].write(encoded(self.dump))
            return subprocess.CompletedProcess(argv, 0)
        mode = next(name for name, flag in (("record", "--record"), ("replay", "--replay"),
                                            ("moving", "--smoke-moving")) if flag in argv)
        if self.fail == mode:
            return subprocess.CompletedProcess(argv, 3)
        def option(name):
            return argv[argv.index(name) + 1]
        worker = kwargs["env"]["LUMINUMBRA_JOB_WORKERS"]
        kwargs["stdout"].write(f"JobSystem started with {worker} threads.\r\n".encode())
        ticks = int(option("--ticks")) if mode != "replay" else self.ticks
        if mode == "record":
            self.ticks = ticks
            replay = Path(option("--record"))
            if self.omit != "recording":
                replay.write_bytes(b"LREC1synthetic")
            if self.omit != "trace":
                Path(str(replay) + ".regions.json").write_bytes(encoded(trace(ticks)))
            if self.omit != "completion":
                kwargs["stdout"].write(f"record: wrote stream ({ticks} ticks, {ticks} input records, "
                                       f"{(ticks + 29) // 30} checkpoints, end_hash={HASH})\r\n".encode())
        elif mode == "replay":
            artifact = {"schema": "luminumbra.replay_roundtrip.v1", "passed": True, "diverged": False,
                        "engine_version": "1", "replayed_engine_version": "1",
                        "start_world_hash_match": True, "end_hash_match": True,
                        "preset": "default", "seed": "1337", "tick_count": ticks,
                        "ticks_replayed": ticks, "input_records": ticks,
                        "checkpoints_verified": (ticks + 29) // 30,
                        "start_world_hash": HASH, "end_world_hash": HASH}
            if self.omit != "replay":
                Path(option("--artifact")).write_bytes(encoded(artifact))
        else:
            sub = dict.fromkeys(("terrain", "water", "entities", "wind", "weather", "aether",
                                 "aether_state", "scents", "ecology", "plants"), HASH)
            sub["water"] = HASH + "|waterSimCursor:v1:" + HASH
            for key in ("aether_state", "scents", "plants"):
                sub[key] = ""
            runs = [{"ok": True, "ticks_executed": ticks, "world_hash": HASH, "sub_hashes": sub}] * 2
            artifact = {"schema": "luminumbra.server_tick.v1", "passed": True, "deterministic": True,
                        "sub_hashes_match": True, "world_hash": HASH, "world_hash_replay": HASH,
                        "effective_simulation": EFFECTIVE, "preset": "default", "seed": "1337",
                        "surface_radius": 1, "collision_radius": 1, "ticks_requested": ticks, "runs": runs,
                        "availability_trace_match": False,
                        "availability_trace": [{"tick": tick, "avail": f"{int(worker):016x}"}
                                               for tick in range(1, ticks + 1)]}
            if self.omit != "moving":
                Path(option("--artifact")).write_bytes(encoded(artifact))
        return subprocess.CompletedProcess(argv, 0)


def fixture(base):
    repository, root = base / "repository", base / "runtime root"
    repository.mkdir()
    (root / "data/common").mkdir(parents=True)
    (root / "worlds/atlas/presets").mkdir(parents=True)
    systems = {"sim": {key: {"enabled": True} for key in ("active_regions", "plant_growth", "future_gate")}}
    (root / "data/common/systems.json").write_bytes(encoded(systems))
    (root / "worlds/atlas/presets/default.json").write_bytes(b"{}\n")
    binary, dump_binary = base / "server", base / "exporter"
    binary.write_bytes(b"synthetic server")
    dump_binary.write_bytes(b"synthetic exporter")
    ledger = b"synthetic ledger"
    dump = {"schema": "luminumbra.active_regions_dump.v1", "source_version": 3,
            "source_bytes": len(ledger), "tick": 2, "header_tick": 2,
            "config": {"work_limit": 2**64 - 1, "hold_ticks": 30, "reduced_shift": 1},
            "records": [{"edited": True}]}
    runner = FakeRunner(dump)
    args = argparse.Namespace(repository=repository, root=root, binary=binary, dump_binary=dump_binary,
                              evidence_dir=base / "evidence", tour=base / "tour/tour.json",
                              preset="default", seed="1337", ticks=2)
    with mock.patch.object(capture, "resolved_fixture_hash", return_value="b" * 64):
        identity = capture.identities(repository, root, binary, dump_binary, "default", runner)
    args.tour.parent.mkdir()
    tour = {"schema": "luminumbra.active_region_tour.v1", "identity": identity,
            "save_root": (base / "saved-world").as_posix(),
            "effective_simulation": EFFECTIVE, "worker_count": 1,
            "configuration": {"preset": "default", "seed": "1337", "ticks": 2,
                              "surface_radius": 1, "collision_radius": 1, "autosave_ticks": 30},
            "edited_payload": {"path": "chunks/edited.bin", "sha256": hashlib.sha256(b"edit").hexdigest()},
            "stages": [], "ledger": "ledger.arl", "ledger_dump": "dump.json"}
    for name in check.TOUR_STAGES:
        stage = {"name": name, "identity": identity, "worker_count": 1, "verified": True,
                 "payload_sha256": tour["edited_payload"]["sha256"], "command": ["tour-driver", name],
                 "returncode": 0, "environment": {"LUMINUMBRA_JOB_WORKERS": "1", "LUMINUMBRA_JOB_THROTTLE": "0"},
                 "log": name + ".log"}
        (args.tour.parent / stage["log"]).write_bytes(b"verified boundary\n")
        if name in check.TOUR_STAGES[2:]:
            stage["receipt"] = name + ".json"
            stage["saved_member_sha256"] = hashlib.sha256(b"edit").hexdigest()
            (args.tour.parent / stage["receipt"]).write_bytes(encoded(receipt(ledger)))
        tour["stages"].append(stage)
    (args.tour.parent / "ledger.arl").write_bytes(ledger)
    (args.tour.parent / "dump.json").write_bytes(encoded(dump))
    args.tour.write_bytes(encoded(tour))
    runner.calls.clear()
    return args, runner


def run_capture(args, runner):
    with mock.patch.object(capture, "resolved_fixture_hash", return_value="b" * 64):
        return capture.capture(args, runner)


class ActiveRegionCaptureContract(unittest.TestCase):
    def test_commands_cover_enabled_record_replay_and_moving_flags(self):
        config = {"preset": "mountains", "seed": "seed with spaces", "ticks": 31}
        commands = capture.commands("C:/Program Files/server.exe", "D:/runtime", "D:/evidence/w2", config)
        for argv in commands.values():
            self.assertEqual(argv[0], "C:/Program Files/server.exe")
            self.assertIn("--systems-config", argv)
            self.assertIn("D:/runtime/worlds/atlas/presets/mountains.json", argv)
            self.assertIn("--no-audio", argv)
            self.assertNotIn("--sim-budget-profile", argv)
        self.assertEqual(commands["record"][-2:], ["--record", "D:/evidence/w2/session.lrec"])
        self.assertIn("seed with spaces", commands["record"])
        self.assertNotIn("--ticks", commands["replay"])
        self.assertIn("--smoke-moving", commands["moving"])
        self.assertIn("--avail-trace", commands["moving"])
        self.assertEqual(commands["moving"][commands["moving"].index("--autosave-ticks") + 1], "30")

    def test_capture_complete_and_environment_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            args, runner = fixture(Path(temp))
            with mock.patch.dict(os.environ, {"LUMINUMBRA_JOB_WORKERS": "7", "LUMINUMBRA_JOB_THROTTLE": "9",
                                              "LUMINUMBRA_UNKNOWN_OVERRIDE": "1"}):
                before = dict(os.environ)
                result = run_capture(args, runner)
                self.assertTrue(before == dict(os.environ), "capture mutated caller environment")
            self.assertTrue(result["passed"])
            native = [(argv, kw) for argv, kw in runner.calls if argv[0] != "git"]
            self.assertEqual(len(native), 10)
            for argv, kw in native:
                self.assertNotIn("LUMINUMBRA_UNKNOWN_OVERRIDE", kw["env"])
            manifest = check.parse((args.evidence_dir / "capture.json").read_bytes())
            self.assertEqual(check.validate(args.evidence_dir, manifest), result)
            self.assertEqual((args.tour.parent / "ledger.arl").read_bytes(), b"synthetic ledger")
            for _, kw in native:
                self.assertTrue(kw["stdout"].closed)

    def test_failed_and_incomplete_captures_never_publish_manifest(self):
        for failure in ("record", "replay", "moving", "recording", "trace", "completion", "replay-output", "moving-output"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                args, runner = fixture(Path(temp))
                if failure in ("record", "replay", "moving"):
                    runner.fail = failure
                else:
                    runner.omit = failure.replace("-output", "")
                before = dict(os.environ)
                with self.assertRaises((OSError, ValueError)):
                    run_capture(args, runner)
                self.assertTrue(before == dict(os.environ), "failed capture mutated caller environment")
                self.assertFalse((args.evidence_dir / "capture.json").exists())

    def test_refuses_timeout_dirty_source_changed_identity_and_export_mismatch(self):
        for failure in ("timeout", "dirty", "identity", "dump"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                args, runner = fixture(Path(temp))
                if failure == "dirty":
                    runner.dirty = True
                elif failure == "dump":
                    runner.dump = dict(runner.dump, tick=3)
                else:
                    def hook(argv, kwargs):
                        if len(argv) == 2:
                            return
                        if failure == "timeout":
                            raise subprocess.TimeoutExpired(argv, 600)
                        args.binary.write_bytes(b"changed executable")
                    runner.hook = hook
                with self.assertRaises((ValueError, subprocess.TimeoutExpired)):
                    run_capture(args, runner)
                self.assertFalse((args.evidence_dir / "capture.json").exists())

    def test_destinations_and_existing_evidence_refused(self):
        for destination in ("repository", "runtime", "existing", "runtime-parent", "tour-source", "tour-save"):
            with self.subTest(destination=destination), tempfile.TemporaryDirectory() as temp:
                args, runner = fixture(Path(temp))
                if destination == "repository":
                    args.evidence_dir = args.repository / "evidence"
                elif destination == "runtime":
                    args.evidence_dir = args.root / "evidence"
                elif destination == "runtime-parent":
                    args.evidence_dir = args.root.parent
                elif destination == "tour-source":
                    args.evidence_dir = args.tour.parent / "evidence"
                elif destination == "tour-save":
                    args.evidence_dir = Path(temp) / "saved-world/evidence"
                else:
                    args.evidence_dir.mkdir()
                    (args.evidence_dir / "keep").write_bytes(b"keep")
                with self.assertRaises((OSError, ValueError)):
                    run_capture(args, runner)
                self.assertFalse(any(argv[0] != "git" for argv, _ in runner.calls))

    def test_missing_and_mixed_tour_stages_refused(self):
        for failure in ("missing", "mixed", "unverified", "payload", "receipt", "ledger"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                args, runner = fixture(Path(temp))
                tour = check.parse(args.tour.read_bytes())
                if failure == "missing":
                    tour["stages"].pop()
                elif failure == "mixed":
                    tour["stages"][1]["identity"] = dict(tour["identity"], revision="c" * 40)
                elif failure == "unverified":
                    tour["stages"][0]["verified"] = False
                elif failure == "payload":
                    tour["stages"][2]["payload_sha256"] = "c" * 64
                elif failure == "receipt":
                    (args.tour.parent / "saved.json").unlink()
                else:
                    (args.tour.parent / "ledger.arl").write_bytes(b"different")
                args.tour.write_bytes(encoded(tour))
                with self.assertRaises((OSError, ValueError)):
                    run_capture(args, runner)
                self.assertFalse((args.evidence_dir / "capture.json").exists())
                self.assertFalse(any("--record" in argv for argv, _ in runner.calls))

    def test_final_publication_failure_keeps_only_diagnostic_pending_file(self):
        with tempfile.TemporaryDirectory() as temp:
            args, runner = fixture(Path(temp))
            with mock.patch.object(Path, "replace", side_effect=PermissionError("publication refused")):
                with self.assertRaises(PermissionError):
                    run_capture(args, runner)
            self.assertFalse((args.evidence_dir / "capture.json").exists())
            self.assertTrue((args.evidence_dir / "capture.pending").is_file())

    def test_write_and_flush_failure_do_not_publish_manifest(self):
        for failure in ("short", "flush"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                stream = mock.MagicMock()
                stream.__enter__.return_value = stream
                stream.write.return_value = 1 if failure == "short" else 4
                if failure == "flush":
                    stream.flush.side_effect = OSError("flush refused")
                with mock.patch.object(Path, "open", return_value=stream):
                    with self.assertRaises((OSError, ValueError)):
                        capture.write_new(root, "capture.pending", b"data")
                self.assertFalse((root / "capture.json").exists())

    def test_suite_is_registered_for_ctest_and_standalone_ci(self):
        root = Path(__file__).resolve().parents[2]
        cmake = (root / "test/CMakeLists.txt").read_text()
        standalone = (root / "tools/ci/ci_paths.py").read_text()
        for name, filename in (("ActiveRegionCaptureContract", "test_capture_active_regions.py"),
                               ("ActiveRegionEvidenceContract", "test_check_active_regions.py")):
            self.assertIn(f'add_test(NAME {name}\n    COMMAND "${{Python3_EXECUTABLE}}" -I -B '
                          f'"${{CMAKE_SOURCE_DIR}}/tools/perf/{filename}")', cmake)
            self.assertIn(f'"tools/perf/{filename}",', standalone.split("PYTHON_MODULES =", 1)[0])


if __name__ == "__main__":
    unittest.main()

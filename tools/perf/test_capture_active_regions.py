#!/usr/bin/env python3
"""Fast capture regressions using only synthetic files and an in-process runner."""
from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_active_regions as capture
import check_active_regions as check
from test_fixture_hash import directory_link, remove_directory_link

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


def region(payload=b"edit", *, block=None, others=()):
    # Literal-only LZ4 blocks keep fixtures independent of external codecs.
    def literal(raw):
        size = len(raw)
        extra = size - 15
        prefix = bytes([min(size, 15) << 4])
        if size >= 15:
            prefix += b"\xff" * (extra // 255) + bytes([extra % 255])
        return prefix + raw
    records = [(7, payload, literal(payload) if block is None else block)]
    records.extend((identity, raw, literal(raw)) for identity, raw in others)
    headers = b"".join(struct.pack("<QBBII", identity, 0, 0, len(raw), len(compressed))
                       for identity, raw, compressed in records)
    return (b"LMR1" + struct.pack("<HH", 2, len(records)) + headers
            + b"".join(compressed for _, _, compressed in records))


def receipt(ledger, member=None):
    member = region() if member is None else member
    files = [{"path": "chunks/edited.bin", "size": len(member), "sha256": hashlib.sha256(member).hexdigest()},
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
    for preset in ("default", "mountains", "archipelago"):
        (root / "worlds/atlas/presets" / (preset + ".json")).write_bytes(b"{}\n")
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
    tour = {"schema": "luminumbra.active_region_tour.v2", "identity": identity,
            "save_root": (base / "saved-world").as_posix(),
            "effective_simulation": EFFECTIVE, "worker_count": 1,
            "configuration": {"preset": "default", "seed": "1337", "ticks": 2,
                              "surface_radius": 1, "collision_radius": 1, "autosave_ticks": 30},
            "edited_payload": {"path": "chunks/edited.bin", "record_id": 7, "size": 4,
                               "sha256": hashlib.sha256(b"edit").hexdigest()},
            "stages": [], "ledger": "ledger.arl", "ledger_dump": "dump.json"}
    for name in check.TOUR_STAGES:
        stage = {"name": name, "identity": identity, "worker_count": 1, "verified": True,
                 "payload_sha256": tour["edited_payload"]["sha256"], "command": ["tour-driver", name],
                 "returncode": 0, "environment": {"LUMINUMBRA_JOB_WORKERS": "1", "LUMINUMBRA_JOB_THROTTLE": "0"},
                 "log": name + ".log"}
        (args.tour.parent / stage["log"]).write_bytes(b"verified boundary\n")
        if name in check.TOUR_STAGES[2:]:
            stage["receipt"] = name + ".json"
            stage["saved_member"] = name + ".bin"
            stage["payload_offset"] = 0
            stage["saved_member_sha256"] = hashlib.sha256(region()).hexdigest()
            (args.tour.parent / stage["saved_member"]).write_bytes(region())
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
    def test_staged_libraries_are_copied_and_bound_for_both_executables(self):
        for filename in ("network.DLL", "libnetwork.so", "libnetwork.so.1", "libnetwork.dylib"):
            with self.subTest(filename=filename), tempfile.TemporaryDirectory() as temp:
                args, _ = fixture(Path(temp))
                library = args.binary.parent / filename
                library.write_bytes(b"original library")
                (args.binary.parent / "ignored.txt").write_bytes(b"not a library")
                target = Path(temp) / "snapshot"
                target.mkdir()
                _, binary, exporter = capture.snapshot_inputs(args.root, args.binary, args.dump_binary, target)
                for executable in (binary, exporter):
                    staged = executable.parent / filename
                    self.assertEqual(staged.read_bytes(), b"original library")
                    self.assertNotEqual(staged, library)
                    self.assertFalse((staged.parent / "ignored.txt").exists())
                    self.assertEqual(capture.library_identity(executable), capture.library_identity(args.binary))

    def test_changed_source_or_snapshot_libraries_refuse_publication(self):
        for changed in ("source", "snapshot"):
            with self.subTest(changed=changed), tempfile.TemporaryDirectory() as temp:
                args, runner = fixture(Path(temp))
                library = args.binary.parent / "network.dll"
                library.write_bytes(b"library")
                tour = check.parse(args.tour.read_bytes())
                with mock.patch.object(capture, "resolved_fixture_hash", return_value="b" * 64):
                    identity = capture.identities(args.repository, args.root, args.binary, args.dump_binary,
                                                  "default", runner)
                tour["identity"] = identity
                for stage in tour["stages"]:
                    stage["identity"] = identity
                args.tour.write_bytes(encoded(tour))

                def change_library(argv, kwargs):
                    if len(argv) != 2:
                        return
                    target = library if changed != "snapshot" else Path(argv[0]).parent / library.name
                    target.chmod(0o600)
                    target.write_bytes(b"replaced")
                runner.hook = change_library
                with self.assertRaisesRegex(ValueError, "identity changed"):
                    run_capture(args, runner)
                self.assertFalse((args.evidence_dir / "capture.json").exists())

    def test_library_identity_binds_names_empty_sets_and_additions(self):
        with tempfile.TemporaryDirectory() as temp:
            args, _ = fixture(Path(temp))
            empty = capture.library_identity(args.binary)
            library = args.binary.parent / "network.dll"
            library.write_bytes(b"library")
            added = capture.library_identity(args.binary)
            self.assertNotEqual(added, empty)
            library.rename(library.with_name("renamed.dll"))
            self.assertNotEqual(capture.library_identity(args.binary), added)
            library.with_name("renamed.dll").unlink()
            self.assertEqual(capture.library_identity(args.binary), empty)
            library.write_bytes(b"1234")
            library.with_name("second.dll").write_bytes(b"5678")
            with mock.patch.object(check, "MAX_TOTAL", 7), self.assertRaisesRegex(ValueError, "oversized"):
                capture.library_identity(args.binary)

    def test_optional_placement_defaults_and_escape_in_runtime_snapshot(self):
        for state in ("missing", "escape", "required"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as temp:
                args, runner = fixture(Path(temp))
                name = "pieces.json" if state == "required" else "placement.json"
                placement = args.root / "data/common/structures/tree" / name
                placement.parent.mkdir(parents=True)
                destination = (Path(temp) / "missing" if state == "escape"
                               else placement.parent / "missing")
                directory_link(placement, destination)
                try:
                    if state == "missing":
                        self.assertTrue(run_capture(args, runner)["passed"])
                    else:
                        with self.assertRaises((OSError, ValueError)):
                            run_capture(args, runner)
                        self.assertFalse((args.evidence_dir / "capture.json").exists())
                finally:
                    remove_directory_link(placement)

    def test_library_and_content_scans_share_entry_and_byte_bounds(self):
        for failure in ("entries", "depth", "bytes", "directory", "unreadable", "partial"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                args, _ = fixture(Path(temp))
                library = args.binary.parent / "network.dll"
                library.mkdir() if failure == "directory" else library.write_bytes(b"library")
                target = Path(temp) / "snapshot"
                target.mkdir()
                if failure == "entries":
                    # Runtime alone fits; both sibling scans must share its budget.
                    runtime_work = 2 + len(list(capture.fixture_entries(args.root)))
                    patch = mock.patch("fixture_hash.MAX_FIXTURE_ENTRIES", runtime_work)
                elif failure == "depth":
                    deep = args.root / "data/a/b/c/d"
                    deep.mkdir(parents=True)
                    patch = mock.patch("fixture_hash.MAX_FIXTURE_DEPTH", 4)
                elif failure == "bytes":
                    # Content and executables fit, but two library copies do not.
                    size = sum(path.stat().st_size for path in args.root.rglob("*") if path.is_file())
                    size += args.binary.stat().st_size + args.dump_binary.stat().st_size
                    patch = mock.patch.object(check, "MAX_TOTAL", size)
                else:
                    reader = capture.open_regular
                    @contextlib.contextmanager
                    def read_library(root, path):
                        if path != library:
                            with reader(root, path) as stream:
                                yield stream
                        elif failure == "unreadable":
                            raise PermissionError("denied")
                        else:
                            with reader(root, path) as stream:
                                wrapped = mock.Mock(wraps=stream)
                                wrapped.read.return_value = b""
                                yield wrapped
                    patch = mock.patch.object(capture, "open_regular", side_effect=read_library)
                with patch, self.assertRaises((OSError, ValueError)):
                    capture.snapshot_inputs(args.root, args.binary, args.dump_binary, target)

    def test_replaced_and_restored_sources_never_change_executed_snapshots(self):
        with tempfile.TemporaryDirectory() as temp:
            args, runner = fixture(Path(temp))
            # Include a referenced data file outside data/common in the identity.
            dependency = args.root / "data/biomes.json"
            dependency.write_bytes(b"{}\n")
            preset = args.root / "worlds/atlas/presets/default.json"
            preset.write_bytes(encoded({"generation_params": {"biomes": {"table": "biomes.json"}}}))
            tour = check.parse(args.tour.read_bytes())
            identity = capture.identities(args.repository, args.root, args.binary, args.dump_binary,
                                          "default", runner)
            tour["identity"] = identity
            for stage in tour["stages"]:
                stage["identity"] = identity
            args.tour.write_bytes(encoded(tour))
            sources = [args.binary, args.dump_binary, dependency, preset,
                       args.root / "data/common/systems.json"]
            originals = {path: path.read_bytes() for path in sources}
            snapshots = set()

            def replace_sources(argv, kwargs):
                for path in sources:
                    path.write_bytes(b"concurrent replacement")
                try:
                    executable = Path(argv[0])
                    source = args.dump_binary if len(argv) == 2 else args.binary
                    self.assertNotEqual(executable, source)
                    self.assertEqual(executable.read_bytes(), originals[source])
                    snapshots.add(executable.parent.parent)
                    if len(argv) != 2:
                        runtime = Path(argv[argv.index("--root") + 1])
                        self.assertNotEqual(runtime, args.root)
                        for path in sources[2:]:
                            self.assertEqual((runtime / path.relative_to(args.root)).read_bytes(), originals[path])
                finally:
                    for path, raw in originals.items():
                        path.write_bytes(raw)

            runner.hook = replace_sources
            # This race regression uses real resolved content hashes throughout.
            self.assertTrue(capture.capture(args, runner)["passed"])
            self.assertEqual(len(snapshots), 1)
            self.assertTrue(all(not path.exists() for path in snapshots))

    def test_snapshot_refuses_partial_unreadable_oversized_and_changed_inputs(self):
        for failure in ("partial", "growing", "unreadable", "oversized", "changed", "short", "flush", "entries"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                args, runner = fixture(Path(temp))
                if failure == "unreadable":
                    patch = mock.patch.object(capture, "open_regular", side_effect=PermissionError("denied"))
                elif failure in ("partial", "growing", "oversized", "short", "flush"):
                    if failure in ("partial", "growing"):
                        reader = capture.open_regular
                        @contextlib.contextmanager
                        def changed_stream(root, path):
                            with reader(root, path) as stream:
                                wrapped = mock.Mock(wraps=stream)
                                wrapped.read.return_value = b"" if failure == "partial" else b"x" * 1024
                                yield wrapped
                        patch = mock.patch.object(capture, "open_regular", side_effect=changed_stream)
                    elif failure == "oversized":
                        patch = mock.patch.object(check, "MAX_TOTAL", 1)
                    else:
                        output = mock.MagicMock()
                        output.__enter__.return_value = output
                        output.write.side_effect = lambda block: len(block) - (failure == "short")
                        if failure == "flush":
                            output.flush.side_effect = OSError("flush refused")
                        patch = mock.patch.object(Path, "open", return_value=output)
                elif failure == "entries":
                    patch = mock.patch.object(capture, "fixture_entries", side_effect=ValueError("entry budget"))
                else:
                    original = capture.snapshot_inputs
                    def change_snapshot(*arguments):
                        runtime, binary, exporter = original(*arguments)
                        binary.chmod(0o700)
                        binary.write_bytes(b"changed copy")
                        return runtime, binary, exporter
                    patch = mock.patch.object(capture, "snapshot_inputs", side_effect=change_snapshot)
                with patch:
                    if failure in ("short", "flush", "partial", "growing", "oversized", "unreadable", "entries"):
                        target = Path(temp) / "snapshot"
                        target.mkdir()
                        # Exercise the copy itself so writer mocks do not intercept
                        # source identity reads or cross a subprocess boundary.
                        with mock.patch.object(check, "MAX_TOTAL", 4096 if failure == "growing" else check.MAX_TOTAL):
                            with self.assertRaises((OSError, ValueError)):
                                capture.snapshot_inputs(args.root, args.binary, args.dump_binary, target)
                    else:
                        with self.assertRaisesRegex(ValueError, "snapshot identity mismatch"):
                            run_capture(args, runner)
                self.assertFalse((args.evidence_dir / "capture.json").exists())
                self.assertFalse(any(argv[0] != "git" for argv, _ in runner.calls))

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
        for failure in ("schema", "missing", "mixed", "unverified", "payload", "receipt", "ledger"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                args, runner = fixture(Path(temp))
                tour = check.parse(args.tour.read_bytes())
                if failure == "schema":
                    tour["schema"] = "luminumbra.active_region_tour.v1"
                elif failure == "missing":
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
                if failure == "schema":
                    self.assertFalse(any(argv[0] != "git" for argv, _ in runner.calls))

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
                               ("ActiveRegionEvidenceContract", "test_check_active_regions.py"),
                               ("ActiveRegionRecordEvidenceContract", "test_check_active_region_records.py")):
            self.assertIn(f'add_test(NAME {name}\n    COMMAND "${{Python3_EXECUTABLE}}" -I -B '
                          f'"${{CMAKE_SOURCE_DIR}}/tools/perf/{filename}")', cmake)
            self.assertIn(f'"tools/perf/{filename}",', standalone.split("PYTHON_MODULES =", 1)[0])


if __name__ == "__main__":
    unittest.main()

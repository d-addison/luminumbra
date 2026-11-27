#!/usr/bin/env python3
"""Fast regional capture contracts with a synthetic executable and runner."""
from __future__ import annotations

import argparse
import contextlib
import copy
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import capture_regional_cadence as capture
import render_runner_process as process


HASH = "0123456789abcdef"


def artifact(preset="default", seed="1337"):
    ticks = 2
    owners = []
    for index in range(9):
        owner = dict.fromkeys(capture.OWNER_COUNTERS, 0)
        owner.update(x=-4 + (index % 3) * 4, z=-4 + (index // 3) * 4)
        if index < 8:
            owner.update(plant_services=2, elapsed_steps=2, last_plant_tick=2,
                         max_service_gap=1, water_services=2, water_interior_windows=2,
                         max_water_service_gap=1)
        else:
            owner.update(frozen_samples=2, service_gap=2, max_service_gap=2,
                         water_service_gap=2, max_water_service_gap=2)
        owners.append(owner)
    diagnostic = dict.fromkeys(capture.COUNTERS, 0)
    diagnostic.update(tick=2, observations=2, owner_visits=18, owners=owners,
                      water_publications=2, water_window_visits=144, water_granted_windows=16,
                      water_completed_cells=1024, water_total_cells_max=512)
    stages = []
    for name in capture.budget.STAGES:
        inactive = name == "wind_weather"
        stages.append({"name": name, "samples": 0 if inactive else ticks,
                       "work_total": 0 if inactive else 3,
                       "work_trace": [] if inactive else [{"tick": 1, "work": 1}, {"tick": 2, "work": 2}],
                       "duration_ms": None if inactive else
                       {"p50": 0.1, "p95": 0.2, "p99": 0.3, "maximum": 0.4}})
    return {"schema": "luminumbra.server_tick.v1", "preset": preset, "seed": seed,
            "ticks_requested": ticks, "surface_radius": 1, "collision_radius": 1,
            "passed": True, "deterministic": True, "sub_hashes_match": True,
            "world_hash": HASH, "world_hash_replay": HASH,
            "effective_simulation": dict.fromkeys(
                capture.budget.shipping_simulation_gates() + ("finite_hydrology", "weather_rain"), True),
            "runs": [{"ok": True, "ticks_executed": ticks, "world_hash": HASH,
                      "sub_hashes": dict.fromkeys(capture.SUB_HASHES, HASH)} for _ in range(2)],
            "sim_budget": {"schema": "luminumbra.sim_budget.v2", "work_replay_match": True,
                           "stages": stages, "work_items": {"schema": "luminumbra.sim_work.v1",
                           "counters": [{"name": name, "work_trace": [
                               {"tick": 1, "work": 1}, {"tick": 2, "work": 2}]}
                               for name in capture.budget.WORK_ITEMS]}},
            "regional_cadence": {"schema": "luminumbra.regional_cadence.v1", "workload": capture.WORKLOAD,
                                 "anchors": 8, "participants": 9, "observations_replay_match": True,
                                 "run": diagnostic, "replay": copy.deepcopy(diagnostic)}}


def write(path, value):
    path.write_text(capture.canonical(value), encoding="utf-8")


class FakeRunner:
    def __init__(self):
        self.calls = []
        self.hook = None
        self.fail = False
        self.omit = False

    def __call__(self, argv, **kwargs):
        if argv[0] == "git":
            return subprocess.CompletedProcess(argv, 0, stdout="a" * 40 + "\r\n" if argv[1] == "rev-parse" else "")
        self.calls.append((argv, kwargs))
        if self.fail:
            return subprocess.CompletedProcess(argv, 2)
        def option(name):
            return argv[argv.index(name) + 1]
        value = artifact(option("--preset"), option("--seed"))
        if self.hook:
            self.hook(value, len(self.calls))
        kwargs["stdout"].write(b"synthetic smoke complete\r\n")
        if not self.omit:
            write(Path(option("--artifact")), value)
        return subprocess.CompletedProcess(argv, 0)


class RegionalCadenceCaptureContract(unittest.TestCase):
    def setUp(self):
        gates = mock.patch.object(capture.budget, "shipping_simulation_gates",
                                  return_value=capture.budget.shipping_simulation_gates())
        gates.start()
        self.addCleanup(gates.stop)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.repository, self.root = self.base / "source", self.base / "runtime root"
        self.repository.mkdir()
        (self.root / "data/common").mkdir(parents=True)
        presets = self.root / "worlds/atlas/presets"
        presets.mkdir(parents=True)
        for name in capture.PRESETS:
            write(presets / f"{name}.json", {})
        write(self.root / "data/common/systems.json", {"sim": {}})
        self.binary = self.base / "fake server.exe"
        self.binary.write_bytes(b"synthetic executable fixture")
        self.args = argparse.Namespace(repository=self.repository, root=self.root, binary=self.binary,
            evidence_dir=self.base / "evidence", presets=list(capture.PRESETS), seeds=["1337"],
            repeats=2, ticks=2, radius=1, collision_radius=1)
        self.runner = FakeRunner()
        # Artifact-validation cases need no pipe worker. Log-specific cases below
        # restore the real drainer, including coverage with a real Python child.
        @contextlib.contextmanager
        def artifact_log(path, status, *, exclusive=False):
            status.update(log_truncated=False, log_limit_bytes=process.MAX_LOG_BYTES, log_bytes_retained=0)
            with path.open("xb" if exclusive else "wb") as log:
                try:
                    yield log
                finally:
                    status["log_bytes_retained"] = log.tell()
        self.log_stub = mock.patch.object(process, "bounded_log", artifact_log)
        self.log_stub.start()
        self.addCleanup(self.log_stub.stop)

    def collect(self):
        return capture.capture(self.args, self.runner)

    def validate(self, value):
        return capture.validate_artifact(value, capture.matrix(["default"], ["1337"], 2, 2, 1, 1),
                                         "default", "1337")

    def mutate_diagnostic(self, field, value, owner=None):
        data = artifact()
        for mode in ("run", "replay"):
            target = data["regional_cadence"][mode]
            if owner is not None:
                target = target["owners"][owner]
            target[field] = value
        return data

    def test_stub_capture_smoke(self):
        with mock.patch.dict(os.environ, {"LUMINUMBRA_JOB_WORKERS": "99"}):
            manifest = self.collect()
        loaded = capture.load_capture(self.args.evidence_dir)
        self.assertEqual(len(loaded["cells"]), 6)
        self.assertEqual(manifest["identity"]["binary_sha256"], capture.file_digest(self.binary))
        self.assertEqual(manifest["workload_sha256"], capture.digest(manifest["workload"]))
        self.assertEqual(manifest["identity"]["content_sha256"], capture.resolved_fixture_hash(self.root))
        self.assertEqual(manifest["identity"]["revision"], "a" * 40)
        for argv, kwargs in self.runner.calls:
            self.assertEqual(argv[0], self.binary.as_posix())
            for flag in ("--smoke", "--no-audio", "--sim-budget", "--regional-cadence-workload"):
                self.assertEqual(argv.count(flag), 1)
            for flag in ("--ecology-roster", "--planted-roster", "--water-smoke", "--smoke-moving",
                         "--record", "--replay"):
                self.assertNotIn(flag, argv)
            for flag, value in (("--root", self.root.as_posix()), ("--seed", "1337"), ("--ticks", "2"),
                                ("--autosave-ticks", "30"), ("--radius", "1"), ("--collision-radius", "1")):
                self.assertEqual(argv[argv.index(flag) + 1], value)
            self.assertIn(argv[argv.index("--preset") + 1], capture.PRESETS)
            self.assertEqual(kwargs["timeout"], 600)
            self.assertFalse(any(key.upper().startswith("LUMINUMBRA_") for key in kwargs["env"]))
        self.assertTrue(all(row["same"] for row in capture.compare(
            self.args.evidence_dir, self.args.evidence_dir)["integer_work_and_service"]))

    def test_capture_and_comparison_publish_only_safe_paths(self):
        manifest = self.collect()
        self.assertEqual(manifest["schema"], "luminumbra.regional_cadence_capture.v2")
        encoded = (self.args.evidence_dir / "capture.json").read_text(encoding="utf-8")
        self.assertEqual(json.loads(encoded), manifest)
        self.assertNotIn(self.base.as_posix(), encoded)
        self.assertEqual(manifest["binary"], "binary-sha256-" + capture.file_digest(self.binary))
        self.assertEqual(manifest["runtime_root"],
                         "runtime-sha256-" + capture.resolved_fixture_hash(self.root))
        self.assertEqual(manifest["capture_root"], ".")
        for row in manifest["cells"]:
            self.assertEqual(row["command"], capture.command(
                manifest["binary"], manifest["runtime_root"], row["artifact"],
                manifest["workload"], row["preset"], row["seed"]))
        relocated = self.base / "relocated"
        shutil.move(self.args.evidence_dir, relocated)
        report = capture.compare(relocated, relocated)
        self.assertEqual(report["schema"], "luminumbra.regional_cadence_compare.v2")
        self.assertNotIn(self.base.as_posix(), capture.canonical(report))
        self.assertEqual(report["before"], manifest)
        self.assertEqual(report["after"], manifest)

    def test_legacy_comparison_redacts_bindings_commands_and_unknown_metadata(self):
        manifest = self.collect()
        portable = self.base / "portable"
        shutil.copytree(self.args.evidence_dir, portable)
        legacy = copy.deepcopy(manifest)
        legacy["schema"] = "luminumbra.regional_cadence_capture.v1"
        legacy.update(binary=self.binary.as_posix(), runtime_root=self.root.as_posix(),
                      capture_root=self.args.evidence_dir.as_posix(), unknown=self.base.as_posix())
        legacy["identity"]["unknown"] = self.base.as_posix()
        for row in legacy["cells"]:
            row["unknown"] = self.base.as_posix()
            row["command"] = capture.command(legacy["binary"], legacy["runtime_root"],
                self.args.evidence_dir / row["artifact"], legacy["workload"], row["preset"], row["seed"])
        write(self.args.evidence_dir / "capture.json", legacy)
        for before, after in ((self.args.evidence_dir, self.args.evidence_dir),
                              (self.args.evidence_dir, portable), (portable, self.args.evidence_dir)):
            with self.subTest(before=before.name, after=after.name):
                report = capture.compare(before, after)
                self.assertEqual(report["before"], manifest)
                self.assertEqual(report["after"], manifest)
                self.assertNotIn(self.base.as_posix(), capture.canonical(report))

    def test_capture_bounds_logs_and_records_truncation_on_all_outcomes(self):
        self.log_stub.stop()
        self.assertEqual(process.MAX_LOG_BYTES, 16 * 1024**2)
        prefix = b"synthetic smoke complete\r\n"
        limit = len(prefix)
        self.args.presets = ["default"]
        for failure in (None, "failed", "timeout", "spawn"):
            for count in (0, limit - 1, limit, limit + 1, 200000):
                self.args.evidence_dir = self.base / f"bounded-{failure}-{count}"
                runner = FakeRunner()
                def noisy(argv, **kwargs):
                    if argv[0] != "git":
                        kwargs["stdout"].write((prefix + b"x" * count)[:count])
                        if failure == "timeout":
                            raise subprocess.TimeoutExpired(argv, 600)
                        if failure == "spawn":
                            raise OSError("spawn failed")
                        if failure == "failed":
                            return subprocess.CompletedProcess(argv, 2)
                        # FakeRunner's own message must not alter this test's byte count.
                        kwargs = dict(kwargs, stdout=io.BytesIO())
                    return runner(argv, **kwargs)
                with self.subTest(failure=failure, count=count), \
                        mock.patch.object(process, "MAX_LOG_BYTES", limit):
                    if failure or count > limit:
                        with self.assertRaises((ValueError, OSError, subprocess.SubprocessError)):
                            capture.capture(self.args, noisy)
                        self.assertFalse((self.args.evidence_dir / "capture.json").exists())
                    else:
                        capture.capture(self.args, noisy)
                    status = capture.read_json(self.args.evidence_dir / "cell-0.log.json")
                    self.assertEqual((self.args.evidence_dir / "cell-0.log").read_bytes(),
                                     (prefix + b"x" * count)[:min(count, limit)])
                    self.assertEqual(status, {"log_truncated": count > limit,
                        "log_limit_bytes": limit, "log_bytes_retained": min(count, limit),
                        "log_complete": count <= limit})

    def test_log_io_failures_refuse_publication_and_record_incomplete_log(self):
        self.log_stub.stop()
        for operation in ("write", "flush"):
            self.args.evidence_dir = self.base / operation
            sink = io.BytesIO()
            original_open = Path.open
            def open_output(path, *args, **kwargs):
                if path.suffix == ".log":
                    return contextlib.nullcontext(sink)
                return original_open(path, *args, **kwargs)
            with self.subTest(operation=operation), \
                    mock.patch.object(Path, "open", open_output), \
                    mock.patch.object(sink, operation, side_effect=OSError("log failed")):
                with self.assertRaisesRegex(ValueError, "incomplete smoke log"):
                    self.collect()
            self.assertFalse((self.args.evidence_dir / "capture.json").exists())
            self.assertFalse(capture.read_json(self.args.evidence_dir / "cell-0.log.json")["log_complete"])

    def test_log_setup_failure_records_unknown_status_without_launch(self):
        self.log_stub.stop()
        with mock.patch.object(process, "bounded_log", side_effect=OSError("pipe failed")):
            with self.assertRaisesRegex(OSError, "pipe failed"):
                self.collect()
        self.assertEqual(self.runner.calls, [])
        self.assertFalse((self.args.evidence_dir / "capture.json").exists())
        self.assertEqual(capture.read_json(self.args.evidence_dir / "cell-0.log.json"), {
            "log_truncated": None, "log_limit_bytes": None, "log_bytes_retained": None,
            "log_complete": False})
        self.args.evidence_dir = self.base / "occupied-log"
        occupied = self.args.evidence_dir / "cell-1.log"
        self.runner.hook = lambda data, number: occupied.write_bytes(b"retained")
        with self.assertRaises(FileExistsError):
            self.collect()
        self.assertEqual(len(self.runner.calls), 1)
        self.assertEqual(occupied.read_bytes(), b"retained")
        self.assertFalse((self.args.evidence_dir / "capture.json").exists())
        self.assertEqual(capture.read_json(self.args.evidence_dir / "cell-1.log.json"), {
            "log_truncated": None, "log_limit_bytes": None, "log_bytes_retained": None,
            "log_complete": False})

    def test_log_status_write_failure_refuses_publication(self):
        self.log_stub.stop()
        with mock.patch.object(capture, "write_new", side_effect=OSError("status failed")):
            with self.assertRaisesRegex(OSError, "status failed"):
                self.collect()
        self.assertEqual(len(self.runner.calls), 1)
        self.assertFalse((self.args.evidence_dir / "capture.json").exists())

    def test_real_python_stdout_and_stderr_use_bounded_pipe(self):
        self.log_stub.stop()
        self.args.presets = ["default"]
        for count in (0, 128, 100000):
            self.args.evidence_dir = self.base / f"python-output-{count}"
            def invoke(argv, **kwargs):
                result = self.runner(argv, **dict(kwargs, stdout=io.BytesIO()))
                if argv[0] == "git":
                    return result
                return subprocess.run([sys.executable, "-I", "-B", "-c",
                    "import sys; count = int(sys.argv[1]); "
                    "sys.stdout.buffer.write(b'x' * count); sys.stdout.buffer.flush(); "
                    "sys.stderr.buffer.write(b'y' * count)", str(count)], **kwargs)
            with self.subTest(count=count), mock.patch.object(process, "MAX_LOG_BYTES", 256):
                if count > 128:
                    with self.assertRaisesRegex(ValueError, "incomplete smoke log"):
                        capture.capture(self.args, invoke)
                else:
                    capture.capture(self.args, invoke)
            self.assertEqual((self.args.evidence_dir / "cell-0.log").read_bytes(),
                             (b"x" * count + b"y" * count)[:256])
            self.assertEqual(capture.read_json(self.args.evidence_dir / "cell-0.log.json"), {
                "log_truncated": count > 128, "log_limit_bytes": 256,
                "log_bytes_retained": min(2 * count, 256), "log_complete": count <= 128})

    def test_refuses_vacuous_remote_work(self):
        for index in range(8):
            for field in ("plant_services", "elapsed_steps", "water_services", "water_interior_windows"):
                with self.subTest(owner=index, field=field), self.assertRaisesRegex(ValueError, "remote"):
                    self.validate(self.mutate_diagnostic(field, 0, index))

    def test_replay_comparison_ignores_durations(self):
        first = artifact()
        changed = copy.deepcopy(first)
        changed["runs"][0].update(wall_seconds=1234, frames_executed=80)
        changed["runs"][0]["sub_hashes"]["mesh"] = "different"
        for stage in changed["sim_budget"]["stages"]:
            if stage["duration_ms"]:
                stage["duration_ms"] = dict.fromkeys(("p50", "p95", "p99", "maximum"), 999)
        self.assertEqual(self.validate(first)["integer"], self.validate(changed)["integer"])
        self.assertNotEqual(self.validate(first)["duration_ms"], self.validate(changed)["duration_ms"])
        self.collect()
        before = self.args.evidence_dir
        self.args.evidence_dir = self.base / "after"
        self.runner.hook = lambda data, _: data.update(sim_budget=copy.deepcopy(changed["sim_budget"]))
        self.collect()
        report = capture.compare(before, self.args.evidence_dir)
        self.assertTrue(all(row["same"] for row in report["integer_work_and_service"]))
        self.assertTrue(all(row["before"] != row["after"] for row in report["duration_ms"]))

    def test_refuses_mismatched_workload_identity(self):
        for field, value in (("workload", "unknown"), ("anchors", 1), ("participants", True), ("schema", "future")):
            data = artifact()
            data["regional_cadence"][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "workload"):
                self.validate(data)
        self.collect()
        manifest_path = self.args.evidence_dir / "capture.json"
        manifest = capture.read_json(manifest_path)
        manifest["workload_sha256"] = "0" * 64
        write(manifest_path, manifest)
        with self.assertRaisesRegex(ValueError, "workload"):
            capture.load_capture(self.args.evidence_dir)

    def test_refuses_frozen_advancement_missing_and_incomplete_observations(self):
        cases = (("frozen_services", 1, 0), ("frozen_water_windows", 1, 7),
                 ("elapsed_steps", 1, 8), ("unadmitted_samples", 1, 8),
                 ("missing_samples", 1, 2), ("observations", 1, None),
                 ("owner_visits", 17, None), ("water_deferred_samples", 1, None),
                 ("service_gap", 3, 0), ("plant_services", True, 0), ("tick", -1, None),
                 ("frozen_samples", 2, 0), ("unadmitted_samples", 2, 0))
        for field, value, owner in cases:
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(self.mutate_diagnostic(field, value, owner))
        for mode in ("run", "replay"):
            data = artifact()
            data["regional_cadence"][mode]["owners"].pop()
            with self.assertRaisesRegex(ValueError, "owners"):
                self.validate(data)

    def test_refuses_exceeded_cell_caps(self):
        for field, value in (("water_total_cells_max", 4097), ("water_completed_cells", 8193)):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "cap"):
                self.validate(self.mutate_diagnostic(field, value))
        data = artifact()
        data["sim_budget"]["work_items"]["counters"][0]["work_trace"][0]["work"] = 4097
        with self.assertRaisesRegex(ValueError, "water cell trace"):
            self.validate(data)
        data = artifact()
        stage = data["sim_budget"]["stages"][-1]
        stage["work_trace"][0]["work"] = 4097
        stage["work_total"] = 4099
        with self.assertRaisesRegex(ValueError, "cap"):
            self.validate(data)
        self.validate(self.mutate_diagnostic("water_total_cells_max", 4096))

    def test_refuses_divergent_deterministic_traces_even_with_passing_flags(self):
        data = artifact()
        data["regional_cadence"]["replay"]["owners"][0]["elapsed_steps"] += 1
        with self.assertRaisesRegex(ValueError, "divergent"):
            self.validate(data)
        data = artifact()
        data["runs"][1]["sub_hashes"]["plants"] = "f" * 16
        with self.assertRaisesRegex(ValueError, "divergent"):
            self.validate(data)
        def change_second(value, number):
            if number == 2:
                for mode in ("run", "replay"):
                    value["regional_cadence"][mode]["owners"][0]["elapsed_steps"] += 1
        self.runner.hook = change_second
        with self.assertRaisesRegex(ValueError, "repeated traces"):
            self.collect()
        self.assertFalse((self.args.evidence_dir / "capture.json").exists())

    def test_process_failure_timeout_missing_partial_and_unreadable_packets(self):
        for failure in ("failed", "timeout", "missing", "partial", "directory"):
            self.args.evidence_dir = self.base / failure
            runner = FakeRunner()
            runner.fail = failure == "failed"
            runner.omit = failure == "missing"
            def invoke(argv, **kwargs):
                result = runner(argv, **kwargs)
                if argv[0] != "git":
                    path = Path(argv[-1])
                    if failure == "timeout":
                        raise subprocess.TimeoutExpired(argv, 600)
                    if failure == "partial":
                        path.write_bytes(b'{"schema":')
                    if failure == "directory":
                        path.unlink()
                        path.mkdir()
                return result
            with self.subTest(failure=failure), self.assertRaises((ValueError, OSError, subprocess.SubprocessError)):
                capture.capture(self.args, invoke)
            self.assertFalse((self.args.evidence_dir / "capture.json").exists())

    def test_rechecks_source_binary_and_content_before_publication(self):
        for kind in ("binary", "content", "revision"):
            self.args.evidence_dir = self.base / kind
            runner = FakeRunner()
            def drift(value, number):
                if number == 1:
                    if kind == "binary":
                        self.binary.write_bytes(b"changed executable")
                    elif kind == "content":
                        write(self.root / "data/common/new.json", {"changed": True})
            runner.hook = drift
            def invoke(argv, **kwargs):
                result = runner(argv, **kwargs)
                if kind == "revision" and argv[:2] == ["git", "rev-parse"] and runner.calls:
                    result.stdout = "b" * 40
                return result
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "identity changed"):
                capture.capture(self.args, invoke)
            self.assertFalse((self.args.evidence_dir / "capture.json").exists())

    def test_invalid_matrices_refuse_before_evidence_or_launch(self):
        for field, value in (("presets", []), ("presets", ["unknown"]), ("seeds", []),
                             ("seeds", ["1", "01"]), ("seeds", [str(2**64)]),
                             ("repeats", 1), ("repeats", 33), ("ticks", 0),
                             ("ticks", 36001), ("radius", 13), ("collision_radius", -1)):
            args = copy.copy(self.args)
            setattr(args, field, value)
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                capture.capture(args, self.runner)
            self.assertFalse(self.args.evidence_dir.exists())
            self.assertEqual(self.runner.calls, [])
        maximum = capture.matrix(["default"], [str(seed) for seed in range(16)], 2, 36000, 12, 12)
        self.assertEqual(len(list(capture.cells(maximum))), 32)

    def test_every_required_diagnostic_counter_is_checked(self):
        for field in capture.COUNTERS:
            data = artifact()
            del data["regional_cadence"]["replay"][field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(data)
        for field in ("x", "z", *capture.OWNER_COUNTERS):
            data = artifact()
            del data["regional_cadence"]["run"]["owners"][0][field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(data)

    def test_total_artifact_bound_refuses_capture_and_load(self):
        self.collect()
        with mock.patch.object(capture, "MAX_TOTAL", 16), self.assertRaisesRegex(ValueError, "oversized"):
            capture.load_capture(self.args.evidence_dir)
        self.args.evidence_dir = self.base / "bounded"
        with mock.patch.object(capture, "MAX_TOTAL", 16), self.assertRaisesRegex(ValueError, "oversized"):
            self.collect()
        self.assertFalse((self.args.evidence_dir / "capture.json").exists())

    def test_refuses_existing_evidence_and_root_overlap(self):
        for destination in (self.repository / "evidence", self.root / "evidence", self.base):
            self.args.evidence_dir = destination
            with self.assertRaises(ValueError):
                self.collect()
        self.args.evidence_dir = self.base / "existing"
        self.args.evidence_dir.mkdir()
        sentinel = self.args.evidence_dir / "keep"
        sentinel.write_bytes(b"retained")
        with self.assertRaises(FileExistsError):
            self.collect()
        self.assertEqual(sentinel.read_bytes(), b"retained")

    def test_packet_matrix_commands_and_digests_are_revalidated(self):
        self.collect()
        path = self.args.evidence_dir / "capture.json"
        original = capture.read_json(path)
        for mutation in ("missing", "duplicate", "command", "escape", "digest", "identity", "schema"):
            manifest = copy.deepcopy(original)
            if mutation == "missing":
                manifest["cells"].pop()
            elif mutation == "duplicate":
                manifest["cells"][1] = manifest["cells"][0]
            elif mutation == "command":
                manifest["cells"][0]["command"].remove("--sim-budget")
            elif mutation == "escape":
                manifest["cells"][0]["artifact"] = "../outside.json"
            elif mutation == "digest":
                manifest["cells"][0]["artifact_sha256"] = "0" * 64
            elif mutation == "identity":
                manifest["identity"]["binary_sha256"] = None
            else:
                manifest["schema"] = "future"
            write(path, manifest)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                capture.load_capture(self.args.evidence_dir)
        write(path, original)
        packet = self.args.evidence_dir / "cell-0.json"
        data = capture.read_json(packet)
        data["world_hash"] = "0" * 16
        write(packet, data)
        with self.assertRaisesRegex(ValueError, "digest"):
            capture.load_capture(self.args.evidence_dir)

    def test_parser_size_duplicate_keys_and_special_files(self):
        path = self.base / "input.json"
        for raw in (b'{"x":1,"x":2}', b'{', b'\xff'):
            path.write_bytes(raw)
            with self.assertRaises(ValueError):
                capture.read_json(path)
        path.write_bytes(b'{} ')
        with self.assertRaisesRegex(ValueError, "oversized"):
            capture.read_json(path, 2)
        path.write_bytes(b'\xef\xbb\xbf{\r\n"x":1\r\n}\r\n')
        self.assertEqual(capture.read_json(path), {"x": 1})
        if hasattr(os, "mkfifo"):
            fifo = self.base / "fifo"
            os.mkfifo(fifo)
            with self.assertRaisesRegex(ValueError, "regular"):
                capture.read_json(fifo)
        with mock.patch.object(capture, "MAX_BINARY", 1), self.assertRaisesRegex(ValueError, "bound"):
            capture.file_digest(self.binary)

    def test_archive_paths_require_safe_labels_or_legacy_absolute_bindings(self):
        self.collect()
        path = self.args.evidence_dir / "capture.json"
        original = capture.read_json(path)
        for key in ("binary", "runtime_root", "capture_root"):
            for value in ("", "relative", "C:relative", "../escape", "\\root", "bad\0name",
                          "binary-sha256-" + "0" * 64, None, [], {}):
                manifest = copy.deepcopy(original)
                manifest[key] = value
                write(path, manifest)
                with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, "capture paths"):
                    capture.load_capture(self.args.evidence_dir)
        manifest = copy.deepcopy(original)
        manifest.update(binary="C:/Program Files/server.exe", runtime_root="D:/runtime root",
                        capture_root="E:/evidence")
        for row in manifest["cells"]:
            row["command"] = capture.command(manifest["binary"], manifest["runtime_root"],
                Path(manifest["capture_root"]) / row["artifact"], manifest["workload"], row["preset"], row["seed"])
        write(path, manifest)
        with self.assertRaisesRegex(ValueError, "capture paths"):
            capture.load_capture(self.args.evidence_dir)
        manifest["schema"] = "luminumbra.regional_cadence_capture.v1"
        write(path, manifest)
        self.assertEqual(len(capture.load_capture(self.args.evidence_dir)["cells"]), 6)
        report = capture.compare(self.args.evidence_dir, self.args.evidence_dir)
        self.assertEqual(report["before"], original)
        self.assertEqual(report["after"], original)
        manifest = copy.deepcopy(original)
        manifest["schema"] = "luminumbra.regional_cadence_capture.v1"
        write(path, manifest)
        with self.assertRaisesRegex(ValueError, "capture paths"):
            capture.load_capture(self.args.evidence_dir)

    def test_complete_traces_gates_durations_and_unknown_metadata(self):
        for mutation in ("runs", "ticks", "gate", "stage", "trace", "total", "duration",
                         "work_items", "regional", "inactive", "layout"):
            data = artifact()
            if mutation == "runs":
                data["runs"].pop()
            elif mutation == "ticks":
                data["runs"][1]["ticks_executed"] = 1
            elif mutation == "gate":
                data["effective_simulation"]["active_regions"] = False
            elif mutation == "stage":
                data["sim_budget"]["stages"].pop()
            elif mutation == "trace":
                data["sim_budget"]["stages"][0]["work_trace"].pop()
            elif mutation == "total":
                data["sim_budget"]["stages"][0]["work_total"] = 0
            elif mutation == "duration":
                data["sim_budget"]["stages"][0]["duration_ms"]["p99"] = float("nan")
            elif mutation == "work_items":
                data["sim_budget"]["work_items"]["counters"][0]["work_trace"].pop()
            elif mutation == "regional":
                data["regional_cadence"]["replay"] = None
            elif mutation == "inactive":
                data["sim_budget"]["stages"][9]["duration_ms"] = {}
            else:
                data["regional_cadence"]["run"]["owners"][1]["x"] = 1
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.validate(data)
        data = artifact()
        data["unknown"] = {"future": True}
        data["regional_cadence"]["run"]["owners"][0]["unknown"] = 3
        data["sim_budget"]["work_items"]["counters"][0]["work_trace"][0]["unknown"] = 3
        self.assertEqual(self.validate(data), self.validate(artifact()))
        for run in data["runs"]:
            run["sub_hashes"].update(aether_state="", scents="", ecology="",
                                     water=HASH + "|waterSimCursor:v1:" + HASH)
        native = self.validate(data)["integer"]["sub_hashes"]
        self.assertEqual(native["aether_state"], "")
        self.assertEqual(native["water"], HASH + "|waterSimCursor:v1:" + HASH)
        for mode in ("run", "replay"):
            data["regional_cadence"][mode]["water_window_visits"] += 9
        self.validate(data)  # Unobserved owners can also receive interior water work.
        for value in (True, -1, float("inf"), 10**400, {}, None):
            data = artifact()
            data["sim_budget"]["stages"][0]["duration_ms"]["p99"] = value
            with self.subTest(duration=value), self.assertRaises(ValueError):
                self.validate(data)
        for key in capture.SUB_HASHES:
            for value in (None, 0, []):
                data = artifact()
                for run in data["runs"]:
                    run["sub_hashes"][key] = value
                with self.subTest(hash=key, value=value), self.assertRaisesRegex(ValueError, "truth hashes"):
                    self.validate(data)
            data = artifact()
            for run in data["runs"]:
                del run["sub_hashes"][key]
            with self.subTest(missing_hash=key), self.assertRaisesRegex(ValueError, "truth hashes"):
                self.validate(data)

    def test_combined_and_separate_wind_weather_coverage(self):
        self.validate(artifact())
        data = artifact()
        for stage in data["sim_budget"]["stages"]:
            if stage["name"] in ("wind", "weather", "wind_weather"):
                active = stage["name"] == "wind_weather"
                stage.update(samples=2 if active else 0, work_total=3 if active else 0,
                             work_trace=[{"tick": 1, "work": 1}, {"tick": 2, "work": 2}] if active else [],
                             duration_ms=dict.fromkeys(("p50", "p95", "p99", "maximum"), 1) if active else None)
        self.validate(data)
        data["sim_budget"]["stages"][7] = copy.deepcopy(artifact()["sim_budget"]["stages"][7])
        with self.assertRaisesRegex(ValueError, "samples"):
            self.validate(data)

    def test_compare_requires_same_workload_content_and_gates(self):
        self.collect()
        before = self.args.evidence_dir
        self.args.evidence_dir = self.base / "after"
        self.collect()
        path = self.args.evidence_dir / "capture.json"
        manifest = capture.read_json(path)
        manifest["identity"]["content_sha256"] = "0" * 64
        manifest.update(capture.path_labels(manifest["identity"]))
        for row in manifest["cells"]:
            row["command"] = capture.command(manifest["binary"], manifest["runtime_root"], row["artifact"],
                                             manifest["workload"], row["preset"], row["seed"])
        write(path, manifest)
        with self.assertRaisesRegex(ValueError, "content"):
            capture.compare(before, self.args.evidence_dir)
        self.args.evidence_dir = self.base / "other-workload"
        self.args.presets = ["default"]
        self.collect()
        with self.assertRaisesRegex(ValueError, "workload"):
            capture.compare(before, self.args.evidence_dir)
        self.args.evidence_dir = self.base / "other-gates"
        self.args.presets = list(capture.PRESETS)
        self.runner.hook = lambda data, _: data["effective_simulation"].update(additive_gate=True)
        self.collect()
        with self.assertRaisesRegex(ValueError, "effective simulation"):
            capture.compare(before, self.args.evidence_dir)

    def test_compare_retains_different_revisions_and_executables(self):
        self.collect()
        before = self.args.evidence_dir
        self.args.evidence_dir = self.base / "after"
        self.collect()
        path = self.args.evidence_dir / "capture.json"
        manifest = capture.read_json(path)
        manifest["identity"].update(revision="b" * 40, binary_sha256="c" * 64)
        manifest.update(capture.path_labels(manifest["identity"]))
        for row in manifest["cells"]:
            row["command"] = capture.command(manifest["binary"], manifest["runtime_root"], row["artifact"],
                                             manifest["workload"], row["preset"], row["seed"])
        write(path, manifest)
        report = capture.compare(before, self.args.evidence_dir)
        self.assertTrue(all(row["same"] for row in report["integer_work_and_service"]))
        self.assertEqual(report["after"]["identity"], manifest["identity"])

    def test_failed_manifest_write_or_rename_never_publishes_capture(self):
        for failure in ("short-write", "flush", "rename"):
            self.args.evidence_dir = self.base / failure
            if failure == "rename":
                patcher = mock.patch.object(Path, "rename", side_effect=OSError("rename failed"))
            else:
                original_write = capture.write_new
                def broken_write(path, value):
                    if path.name != "capture.pending":
                        return original_write(path, value)
                    path.write_bytes(b'{"schema":')
                    raise OSError("write or flush failed")
                patcher = mock.patch.object(capture, "write_new", side_effect=broken_write)
            with self.subTest(failure=failure), patcher, self.assertRaises(OSError):
                self.collect()
            self.assertFalse((self.args.evidence_dir / "capture.json").exists())
            self.assertTrue((self.args.evidence_dir / "capture.pending").exists())
        stream = mock.MagicMock()
        path = mock.MagicMock()
        path.open.return_value.__enter__.return_value = stream
        stream.write.return_value = 0
        with self.assertRaisesRegex(ValueError, "short"):
            capture.write_new(path, {})
        stream.write.return_value = 3
        stream.flush.side_effect = OSError("flush failed")
        with self.assertRaisesRegex(OSError, "flush"):
            capture.write_new(path, {})

    def test_prelaunch_invalid_executable_content_and_source(self):
        self.args.binary = self.root
        with self.assertRaisesRegex(ValueError, "regular"):
            self.collect()
        self.assertFalse(self.args.evidence_dir.exists())
        self.args.binary = self.binary
        preset = self.root / "worlds/atlas/presets/default.json"
        preset.write_bytes(b'{"generation_params":{"biomes":{"table":12}}}')
        with self.assertRaises(ValueError):
            self.collect()
        self.assertFalse(self.args.evidence_dir.exists())
        write(preset, {})
        def invalid_source(argv, **kwargs):
            result = self.runner(argv, **kwargs)
            if argv[:2] == ["git", "rev-parse"]:
                result.stdout = "not a revision"
            return result
        with self.assertRaisesRegex(ValueError, "revision"):
            capture.capture(self.args, invalid_source)
        self.assertFalse(self.args.evidence_dir.exists())

    def test_cli_capture_and_compare_success(self):
        output = io.StringIO()
        with mock.patch.object(capture.subprocess, "run", self.runner), contextlib.redirect_stdout(output):
            code = capture.main(["capture", "--repository", str(self.repository), "--root", str(self.root),
                                 "--binary", str(self.binary), "--evidence-dir", str(self.args.evidence_dir),
                                 "--ticks", "2"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output.getvalue())["schema"], capture.SCHEMA)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = capture.main(["compare", str(self.args.evidence_dir), str(self.args.evidence_dir)])
        self.assertEqual(code, 0)
        self.assertTrue(all(row["same"] for row in json.loads(output.getvalue())["integer_work_and_service"]))

    def test_cli_refusal_writes_no_json(self):
        output, error = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
            code = capture.main(["compare", str(self.base / "absent"), str(self.base / "absent")])
        self.assertEqual((code, output.getvalue()), (1, ""))
        self.assertIn("refused", error.getvalue())

    def test_windows_command_paths_are_single_arguments(self):
        argv = capture.command("C:/Program Files/server.exe", "D:/runtime root", "E:/evidence/cell.json",
                               capture.matrix(["default"], ["1337"], 2, 2, 1, 1), "default", "1337")
        self.assertEqual(argv[0], "C:/Program Files/server.exe")
        self.assertEqual(argv[argv.index("--root") + 1], "D:/runtime root")

    def test_registration_and_isolated_entrypoint(self):
        repository = Path(__file__).resolve().parents[2]
        cmake = (repository / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('add_test(NAME RegionalCadenceCaptureContract\n'
                      '    COMMAND "${Python3_EXECUTABLE}" -I -B '
                      '"${CMAKE_SOURCE_DIR}/tools/perf/test_capture_regional_cadence.py")', cmake)
        self.assertIn('set_tests_properties(RegionalCadenceCaptureContract PROPERTIES '
                      'LABELS "simulation;tools" TIMEOUT 5)', cmake)
        sys.path.insert(0, str(repository / "tools/ci"))
        import ci_paths
        self.assertIn("tools/perf/test_capture_regional_cadence.py", ci_paths.PYTHON_TESTS)
        self.assertIn("tools/perf/capture_regional_cadence.py", ci_paths.PYTHON_MODULES)
        self.assertIn("tools/perf/capture_regional_cadence.py", ci_paths.CI_ENTRYPOINTS)


if __name__ == "__main__":
    unittest.main()

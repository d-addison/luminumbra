"""Portable request ownership, replay, orphan recovery and EOF/cancel races."""
from copy import deepcopy
import io
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
sys.path.insert(0, str(SERVICE.parent / "probes"))
from luminumbra_author.contracts import Refusal, atomic_json, canonical, digest
from luminumbra_author.meshgen_backend import MOCK_CAPABILITY
from luminumbra_author.meshgen_config import PROJECT_CONFIG, load_config
from luminumbra_author.meshgen_jobs import BackendBroker, RequestLedger, process_start, read_notes
from test_meshgen_config import project_fixture
from mock_backend import MockServer, synthetic_glb
from test_meshgen_backend import TOKEN, planning, png
from meshgen_http_fixture import owned_server, client_factory, RuleAssertions


class LedgerTests(RuleAssertions, unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.project = Path(self.temp.name)
        self.plan_digest = digest(b"plan")

    def test_read_only_access_has_no_state_or_build_lock(self):
        ledger = RequestLedger(self.project)
        with self.assertRaises(FileNotFoundError):
            ledger.result("0"*32)
        self.assertEqual(list(self.project.iterdir()), [])

    def test_stage_specific_replay_conflict_active_duplicate_and_quota(self):
        ledger = RequestLedger(self.project, terminal_limit=2)
        first = ledger.begin("generate:same", self.plan_digest)
        second = ledger.begin("generate:same", self.plan_digest)
        self.assertEqual(first["job_id"], second["job_id"])
        with self.assertRaises(Refusal) as refusal:
            ledger.begin("generate:same", digest(b"changed"))
        self.assertEqual(refusal.exception.finding["rule_id"], "request.conflict")
        terminal = ledger.finish(first["job_id"], "cancelled")
        fresh = RequestLedger(self.project)
        self.assertEqual(fresh.begin("generate:same", self.plan_digest), terminal)
        self.assertEqual(fresh.result(first["job_id"]), terminal)
        imported = ledger.begin("import:same", self.plan_digest)
        self.assertNotEqual(imported["job_id"], first["job_id"])
        ledger.finish(imported["job_id"], "failed")
        with self.assertRaises(Refusal) as refusal:
            ledger.begin("generate:third", self.plan_digest)
        self.assertEqual(refusal.exception.finding["rule_id"], "request.limit")

    def test_project_wide_lock_contention_across_documents(self):
        ledger = RequestLedger(self.project)
        owner = ledger.begin("generate:one", self.plan_digest)
        other = RequestLedger(self.project, document="other")
        with self.assertRaises(Refusal) as error:
            other.begin("generate:two", self.plan_digest)
        self.assertEqual(error.exception.finding["rule_id"], "busy")
        self.assertIn(owner["job_id"], str(error.exception))
        ledger.finish(owner["job_id"], "cancelled")
        record = other.begin("generate:two", self.plan_digest)
        other.finish(record["job_id"], "cancelled")

    def test_dead_or_reused_pid_recovery_cancels_backend_and_fails_marker(self):
        for actual_identity in (None, "different-start-time"):
            with self.subTest(identity=actual_identity):
                ledger = RequestLedger(self.project)
                record = ledger.begin("generate:" + str(actual_identity), self.plan_digest)
                ledger.update(record["job_id"], state="running", backend_job_id="owned-backend", backend_id="mock", profile="default")
                cancellations = []
                recovered = RequestLedger(self.project, owner_start=lambda pid: actual_identity,
                                          recover_cancel=lambda item: cancellations.append(item["backend_job_id"]))
                recovered.recover()
                self.assertEqual(cancellations, ["owned-backend"])
                self.assertEqual(recovered.result(record["job_id"])["state"], "failed")
                self.assertFalse((recovered.root / ".meshgen.lock").exists())

    def test_orphan_marker_without_lock_is_reconciled(self):
        ledger = RequestLedger(self.project)
        record = ledger.begin("generate:orphan", self.plan_digest)
        (ledger.root / ".meshgen.lock").unlink()
        recovered = RequestLedger(self.project, owner_start=lambda pid: None)
        recovered.recover()
        self.assertEqual(recovered.result(record["job_id"])["state"], "failed")

    def test_terminal_hash_tampering_is_refused(self):
        ledger = RequestLedger(self.project)
        record = ledger.begin("generate:hash", self.plan_digest)
        ledger.finish(record["job_id"], "failed")
        (ledger.root / "_headless/jobs" / record["job_id"] / "terminal.json").write_bytes(b"{}\n")
        with self.assertRaises(Refusal) as error:
            ledger.result(record["job_id"])
        self.assertEqual(error.exception.finding["rule_id"], "request.conflict")

    def test_interrupted_terminal_publication_preserves_original_result(self):
        ledger = RequestLedger(self.project)
        record = ledger.begin("generate:publication", self.plan_digest)
        original_write = ledger._write
        def interrupted(name, value):
            if name.endswith("running.json"):
                raise OSError("Simulated crash before marker transition")
            original_write(name, value)
        errors = [Refusal("backend.protocol", "Original refusal").finding]
        with patch.object(ledger, "_write", side_effect=interrupted), self.assertRaises(OSError):
            ledger.finish(record["job_id"], "failed", errors=errors)
        original = ledger.result(record["job_id"])
        recovered = RequestLedger(self.project, owner_start=lambda pid: None)
        recovered.recover()
        self.assertEqual(recovered.result(record["job_id"]), original)
        self.assertEqual(recovered.status(record["job_id"])["errors"], errors)

    def test_active_duplicate_from_another_owner_cannot_reattach(self):
        ledger = RequestLedger(self.project)
        record = ledger.begin("generate:owned", self.plan_digest)
        other = RequestLedger(self.project)
        with self.assertRaises(Refusal) as error:
            other.begin("generate:owned", self.plan_digest)
        self.assertEqual(error.exception.finding["rule_id"], "busy")
        ledger.cancel(record["job_id"])

    def test_cancel_before_commit_and_cancel_after_committed_success(self):
        ledger = RequestLedger(self.project)
        record = ledger.begin("generate:before", self.plan_digest)
        ledger.update(record["job_id"], state="finishing")
        terminal = ledger.finish(record["job_id"], "cancelled")
        invoked = []
        self.assertEqual(ledger.commit_success(record["job_id"], lambda: invoked.append(True)), terminal)
        self.assertFalse(invoked)
        record = ledger.begin("generate:after", self.plan_digest)
        ledger.update(record["job_id"], state="finishing")
        terminal = ledger.commit_success(record["job_id"], lambda: {"fixture": "trusted candidate receipt"})
        self.assertEqual(ledger.finish(record["job_id"], "cancelled"), terminal)
        self.assertEqual(terminal["state"], "succeeded")

    def test_cancellation_and_candidate_commit_are_one_serialized_transition(self):
        ledger = RequestLedger(self.project)
        record = ledger.begin("generate:race", self.plan_digest)
        identifier = record["job_id"]
        ledger.update(identifier, state="finishing")
        entered, release, cancel_started = threading.Event(), threading.Event(), threading.Event()
        results = []
        def commit():
            entered.set()
            self.assertTrue(release.wait(2))
            return {"fixture": "sealed"}
        def seal():
            results.append(ledger.commit_success(identifier, commit))
        def cancel():
            cancel_started.set()
            results.append(ledger.finish(identifier, "cancelled"))
        sealing = threading.Thread(target=seal)
        sealing.start()
        self.assertTrue(entered.wait(2))
        cancelling = threading.Thread(target=cancel)
        cancelling.start()
        self.assertTrue(cancel_started.wait(2))
        release.set()
        sealing.join(2)
        cancelling.join(2)
        self.assertFalse(sealing.is_alive() or cancelling.is_alive())
        self.assertEqual(len(results), 2)
        self.assertEqual(results[0], results[1])
        self.assertEqual(results[0]["state"], "succeeded")

    def test_process_creation_identity_and_link_refusal(self):
        self.assertIsNotNone(process_start(os.getpid()))
        outside = self.project / "outside"
        outside.mkdir()
        try:
            (self.project / ".luminumbra-author-recipes").symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest("Symlink creation unavailable on this Windows runner")
        with self.assertRule("path.symlink"):
            RequestLedger(self.project).begin("generate:linked", self.plan_digest)
        self.assertEqual(list(outside.iterdir()), [])


class BrokerTests(RuleAssertions, unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.project = Path(self.temp.name)
        self.image = png()
        (self.project / "ref.png").write_bytes(self.image)
        self.request = planning(self.image)
        self.request["overrides"] = {"budgets": {"target_height_m": 1}}
        self.config_path = self.project / PROJECT_CONFIG
        self.config_path.parent.mkdir(parents=True)
        self.environment = patch.dict(os.environ, {"LUMINUMBRA_GEN_TOKEN": TOKEN})
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def configure(self, endpoint):
        config = project_fixture()
        config["backends"]["mock"]["endpoint"] = endpoint
        atomic_json(self.config_path, config)

    def test_mock_raw_ready_replay_retained_bytes_and_eof(self):
        with owned_server(("127.0.0.1", 0), TOKEN) as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            broker = BackendBroker(self.project, client_factory=client_factory(server))
            self.addCleanup(broker.close)
            plan = broker.plan(self.request)
            self.assertFalse(plan["eligibility"]["shippable"])
            self.assertFalse(broker.ledger.root.exists())
            record = broker.start(plan["plan_id"], plan["plan_digest"])
            identifier = record["job_id"]
            broker.futures[identifier].result(timeout=5)
            self.assertEqual(broker.ledger.status(identifier)["state"], "finishing")
            broker.start(plan["plan_id"], plan["plan_digest"])
            self.assertEqual(server.submissions, 1)
            inputs = broker.ledger.root / "_headless/jobs" / identifier / "inputs"
            self.assertEqual((inputs / "reference_image").read_bytes(), self.image)
            self.assertEqual((inputs / "raw_backend.glb").read_bytes(), synthetic_glb(seed=7))
            self.assertNotIn(TOKEN.encode(), b"".join(path.read_bytes() for path in inputs.iterdir()))
            from luminumbra_author.__main__ import serve
            stdin = io.TextIOWrapper(io.BytesIO(b""))
            with patch.dict(os.environ, {"LUMINUMBRA_AUTHOR_TOKEN": "s"*32}), patch("sys.stdin", stdin):
                serve(broker)
            self.assertEqual(broker.ledger.result(identifier)["state"], "cancelled")
            self.assertEqual(server.cancellations, 1)
            self.assertEqual(list(inputs.iterdir()), [])
            self.assertFalse((broker.ledger.root / ".meshgen.lock").exists())
            self.assertFalse(list(broker.ledger.root.rglob("candidate.sha256")))

    def test_capability_change_refuses_before_submission(self):
        with owned_server(("127.0.0.1", 0), TOKEN, mode="card_change") as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            with BackendBroker(self.project, client_factory=client_factory(server)) as broker:
                plan = broker.plan(self.request)
                record = broker.start(plan["plan_id"], plan["plan_digest"])
                broker.futures[record["job_id"]].result(timeout=5)
                terminal = broker.ledger.result(record["job_id"])
                self.assertEqual(terminal["errors"][0]["rule_id"], "backend.pin_mismatch")
                self.assertEqual(server.submissions, 0)

    def test_cancel_crash_hash_and_raw_geometry_fail_without_staging(self):
        for mode, rule in (("cancel", "backend.unavailable"), ("bad_hash", "backend.protocol"),
                           ("nonfinite", "glb.nonfinite"), ("external_uri", "glb.external"), ("oversize", "backend.limit")):
            with self.subTest(mode=mode), owned_server(("127.0.0.1", 0), TOKEN, mode=mode) as server:
                self.configure(f"http://127.0.0.1:{server.server_port}")
                request = deepcopy(self.request)
                request["request_id"] = "generate:" + mode
                with BackendBroker(self.project, client_factory=client_factory(server)) as broker:
                    plan = broker.plan(request)
                    record = broker.start(plan["plan_id"], plan["plan_digest"])
                    broker.futures[record["job_id"]].result(timeout=5)
                    terminal = broker.ledger.result(record["job_id"])
                    self.assertEqual(terminal["state"], "failed")
                    self.assertEqual(terminal["errors"][0]["rule_id"], rule)
                    self.assertNotIn(record["job_id"], broker.raw)
                    self.assertFalse(list(broker.ledger.root.rglob("candidate.sha256")))

    def test_injectable_clock_deadline_cancels_slow_backend(self):
        self.configure("http://127.0.0.1:1234")
        clock = [0]
        cancelled = []
        class SlowClient:
            def __init__(self, *args, **kwargs):
                pass
            def capabilities(self, **kwargs):
                return deepcopy(MOCK_CAPABILITY)
            def submit(self, *args, **kwargs):
                return digest(args[0]["request_id"].encode())[:32]
            def status(self, identifier, **kwargs):
                clock[0] = 61
                return {"job_id": identifier, "state": "running"}
            def cancel(self, identifier):
                cancelled.append(identifier)
                return {"job_id": identifier, "state": "cancelled"}
        with BackendBroker(self.project, clock=lambda: clock[0], client_factory=SlowClient) as broker:
            plan = broker.plan(self.request)
            record = broker.start(plan["plan_id"], plan["plan_digest"])
            broker.futures[record["job_id"]].result(timeout=10)
            terminal = broker.ledger.result(record["job_id"])
            self.assertEqual(terminal["errors"][0]["rule_id"], "backend.deadline")
            self.assertEqual(cancelled, [digest(self.request["request_id"].encode())[:32]])

    def test_eof_during_submission_cancels_late_backend_id(self):
        self.configure("http://127.0.0.1:1234")
        entered, release = threading.Event(), threading.Event()
        cancelled = []
        class SubmittingClient:
            def __init__(self, *args, **kwargs):
                pass
            def capabilities(self, **kwargs):
                return deepcopy(MOCK_CAPABILITY)
            def submit(self, *args, **kwargs):
                entered.set()
                if not release.wait(2):
                    raise RuntimeError("Test submission release was not signalled")
                return digest(args[0]["request_id"].encode())[:32]
            def cancel(self, identifier):
                cancelled.append(identifier)
                return {"job_id": identifier, "state": "cancelled"}
        broker = BackendBroker(self.project, client_factory=SubmittingClient)
        self.addCleanup(broker.close)
        plan = broker.plan(self.request)
        record = broker.start(plan["plan_id"], plan["plan_digest"])
        self.assertTrue(entered.wait(2))
        cancelling = threading.Thread(target=lambda: broker.cancel(record["job_id"]))
        cancelling.start()
        # Cancellation cannot release ownership while the POST is outstanding.
        with self.assertRaises(Refusal) as refusal:
            RequestLedger(self.project, document="other").begin("generate:contender", digest(b"other"))
        self.assertEqual(refusal.exception.finding["rule_id"], "busy")
        self.assertTrue((broker.ledger.root / ".meshgen.lock").exists())
        release.set()
        cancelling.join(3)
        self.assertFalse(cancelling.is_alive())
        broker.on_eof()
        self.assertEqual(set(cancelled), {digest(self.request["request_id"].encode())[:32]})
        self.assertEqual(broker.ledger.result(record["job_id"])["state"], "cancelled")
        self.assertFalse(broker.raw)

    def test_scene_revision_does_not_cancel_but_session_change_prevents_staging(self):
        self.configure("http://127.0.0.1:1234")
        data = synthetic_glb(seed=7)
        entered, release = threading.Event(), threading.Event()
        class PausedClient:
            def __init__(self, *args, **kwargs):
                pass
            def capabilities(self, **kwargs):
                return deepcopy(MOCK_CAPABILITY)
            def submit(self, *args, **kwargs):
                return digest(args[0]["request_id"].encode())[:32]
            def status(self, identifier, **kwargs):
                return {"state": "succeeded", "result": {"sha256": digest(data), "bytes": len(data), "logs_tail": "fixture",
                    "backend_reported": {"seed": 7, "steps_run": 0, "wall_s": 0, "peak_vram_mib": None}}}
            def result(self, *args, **kwargs):
                entered.set()
                if not release.wait(2):
                    raise RuntimeError("Test result release was not signalled")
                return data
            def cancel(self, identifier):
                return {"job_id": identifier, "state": "cancelled"}
        with BackendBroker(self.project, client_factory=PausedClient) as broker:
            broker.scene_revision = 1
            plan = broker.plan(self.request)
            broker.scene_revision = 2
            record = broker.start(plan["plan_id"], plan["plan_digest"])
            self.assertTrue(entered.wait(2))
            broker.session = "new-document"
            release.set()
            broker.futures[record["job_id"]].result(timeout=10)
            terminal = broker.ledger.result(record["job_id"])
            self.assertEqual(terminal["errors"][0]["rule_id"], "stale.plan")
            self.assertFalse(broker.raw)

    def test_real_backend_and_later_phase_requests_are_disabled(self):
        self.configure("http://127.0.0.1:1234")
        config = project_fixture()
        config["backends"]["mock"]["kind"] = "model"
        config["backends"]["mock"]["pins"] = {"model_revision": "0"*40, "weights_sha256": ["1"*64]}
        atomic_json(self.config_path, config)
        with BackendBroker(self.project) as broker:
            with self.assertRaises(Refusal) as error:
                broker.plan(self.request)
            self.assertEqual(error.exception.finding["rule_id"], "backend.unavailable")
            for kind in ("ops", "script"):
                with self.assertRule("operation.unsupported"):
                    broker.plan({"kind": kind})
        self.assertFalse((self.project / ".luminumbra-author-recipes").exists())

    def test_note_bytes_are_read_without_fabricating_evidence(self):
        policy = {"attestations": {"revenue_under_cap": {"value": True, "note_id": "revenue", "note_sha256": digest(b"note")}}}
        self.assertEqual(read_notes(self.project, policy), {})
        directory = self.config_path.parent / "notes"
        directory.mkdir()
        (directory / "revenue.md").write_bytes(b"note")
        self.assertEqual(read_notes(self.project, policy), {"revenue": b"note"})

    def test_sequential_four_candidates_retain_each_seed_without_overflow(self):
        with owned_server(("127.0.0.1", 0), TOKEN) as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            request = deepcopy(self.request)
            request["overrides"]["candidates"] = {"count": 4}
            with BackendBroker(self.project, client_factory=client_factory(server)) as broker:
                plan = broker.plan(request)
                record = broker.start(plan["plan_id"], plan["plan_digest"])
                broker.futures[record["job_id"]].result(timeout=5)
                self.assertEqual(server.submissions, 4)
                self.assertEqual([item["seed"] for item in broker.raw[record["job_id"]]["candidates"]], [7, 8, 9, 10])
                self.assertEqual(broker.ledger.status(record["job_id"])["seeds"], [7, 8, 9, 10])
                request["seed"] = 2**32-1
                with self.assertRule("request.invalid"):
                    broker.plan(request)

    def test_package_ships_broker_modules_and_excludes_mock_and_adapters(self):
        from package import build
        archive = build(self.project / "service.pyz")
        with zipfile.ZipFile(archive) as package:
            names = set(package.namelist())
            for module in ("meshgen_licence", "meshgen_backend", "meshgen_jobs"):
                self.assertIn("luminumbra_author/" + module + ".py", names)
            self.assertFalse(any("mock_backend" in name or "adapters/" in name for name in names))
            self.assertIn("luminumbra_author/schemas/meshgen.request.v1.schema.json", names)


if __name__ == "__main__":
    unittest.main()

"""Portable request ownership, replay, orphan recovery and EOF/cancel races."""
from concurrent.futures import Executor, FIRST_COMPLETED, Future, wait
from contextlib import contextmanager
from copy import deepcopy
import io
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import DEFAULT, patch
import zipfile

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
sys.path.insert(0, str(SERVICE.parent / "probes"))
from luminumbra_author.contracts import Refusal, atomic_json, canonical, digest
from luminumbra_author.meshgen_backend import MOCK_CAPABILITY
from luminumbra_author.meshgen_config import PROJECT_CONFIG, load_config
from luminumbra_author.meshgen_jobs import BackendBroker, RequestLedger, process_start, read_notes
from test_meshgen_config import project_fixture
from mock_backend import synthetic_glb
from test_meshgen_backend import TOKEN, planning, png
from meshgen_http_fixture import MemoryMockServer, client_factory, RuleAssertions


def completed_result(future, phase):
    # Synchronous execution or an independent handshake/join must establish
    # that this phase finished. A missing signal is then a protocol failure.
    assert future.done(), f"Missing completion for {phase}"
    return future.result(timeout=0)


def phase_reached(phase, task, name):
    # Call after synchronous durable work, or for a worker handshake with no
    # pending durable work. This bound detects a lost signal or deadlock only.
    wait((phase, task), timeout=2, return_when=FIRST_COMPLETED)
    if not phase.done():
        assert task.done(), f"Deadlock before {name}"
        try:
            outcome = task.result(timeout=0)
        except BaseException as error:
            raise AssertionError(f"Worker failed before {name}: {error}") from error
        raise AssertionError(f"Worker completed before {name}: {outcome!r}")
    return phase.result(timeout=0)


def cancellation_requested(started, completion_wait, task):
    # Entry into result() follows the durable update, even if its signal is lost.
    phase_reached(completion_wait, task, "cancellation completion wait")
    return completed_result(started, "cancellation request")


def start_worker(function, *args, **kwargs):
    future = Future()
    work_finished = threading.Event()
    def run():
        try:
            future.set_running_or_notify_cancel()
            try:
                future.set_result(function(*args, **kwargs))
            except BaseException as error:
                future.set_exception(error)
        finally:
            # Keep thread completion independent of its result handle; the
            # join helper checks this signal without waiting on it.
            work_finished.set()
    worker = threading.Thread(target=run, daemon=True)
    worker.future = future
    worker.work_finished = work_finished
    worker.start()
    return worker


def join_worker(worker):
    # Call after synchronous durable work; workers only wait or read its result.
    worker.join(timeout=2)
    assert not worker.is_alive(), "Deadlock joining released worker"
    assert worker.work_finished.is_set(), "Missing worker completion signal"
    return worker.future.result(timeout=0)


class WorkerExecutor(Executor):
    """Queue broker work so tests can run it and its drain callback inline."""
    def __init__(self):
        self.pending = []

    def submit(self, function, /, *args, **kwargs):
        future = Future()
        self.pending.append((future, function, args, kwargs))
        return future

    def run_pending(self):
        while self.pending:
            future, function, args, kwargs = self.pending.pop(0)
            future.set_running_or_notify_cancel()
            try:
                result = function(*args, **kwargs)
            except BaseException as error:
                future.set_exception(error)
            else:
                future.set_result(result)

    def shutdown(self, wait=True, *, cancel_futures=False):
        assert not self.pending, "Broker work must run before cleanup"


def close_broker(broker):
    broker.executor.shutdown(wait=True)
    # Check every wait handle before close can enter cancel(). An incomplete
    # handle must fail immediately even when its drained callback has returned.
    for future in broker.futures.values():
        completed_result(future, "broker drain after release")
    broker.close()


@contextmanager
def managed_broker(*args, **kwargs):
    broker = BackendBroker(*args, **kwargs)
    try:
        yield broker.__enter__()
    finally:
        close_broker(broker)


class SynchronousExecutor(Executor):
    """Run the task and its Future callbacks before returning to the test."""
    def submit(self, function, /, *args, **kwargs):
        future = Future()
        future.set_running_or_notify_cancel()
        try:
            future.set_result(function(*args, **kwargs))
        except BaseException as error:
            future.set_exception(error)
        return future


def fixed_clock():
    return 0


def broker_server(token, *, mode="normal"):
    # Keep the mock HTTP handler and response parser, without socket deadlines.
    server = MemoryMockServer(("127.0.0.1", 0), token, mode=mode)
    server.clock = fixed_clock
    return server


class WaitTests(unittest.TestCase):
    def test_missing_completion_fails_without_waiting(self):
        future = Future()
        with patch(__name__ + ".wait", side_effect=AssertionError("Unexpected completion wait")) as waiting, \
                patch.object(future, "result", side_effect=AssertionError("Unexpected result wait")) as result:
            with self.assertRaisesRegex(AssertionError, "Missing completion for broker drain"):
                completed_result(future, "broker drain")
        waiting.assert_not_called()
        result.assert_not_called()

    def test_missing_cancellation_signal_fails_after_completion_wait_entry(self):
        started, completion_wait, task = Future(), Future(), Future()
        completion_wait.set_result(True)
        original_wait = wait
        def bounded_wait(futures, *, timeout, return_when):
            self.assertEqual(futures, (completion_wait, task))
            self.assertEqual(timeout, 2)
            self.assertEqual(return_when, FIRST_COMPLETED)
            return original_wait(futures, timeout=timeout, return_when=return_when)
        with patch(__name__ + ".wait", side_effect=bounded_wait) as waiting:
            with self.assertRaisesRegex(AssertionError, "Missing completion for cancellation request"):
                cancellation_requested(started, completion_wait, task)
        waiting.assert_called_once()
        self.assertFalse(task.done())

    def test_deadlocked_cancellation_has_bounded_detection(self):
        started, completion_wait, task = Future(), Future(), Future()
        with patch(__name__ + ".wait", return_value=(set(), {completion_wait, task})) as waiting:
            with self.assertRaisesRegex(AssertionError, "Deadlock before cancellation completion wait"):
                cancellation_requested(started, completion_wait, task)
        waiting.assert_called_once_with((completion_wait, task), timeout=2, return_when=FIRST_COMPLETED)

    def test_failed_cancellation_reports_worker_error(self):
        started, completion_wait, task = Future(), Future(), Future()
        error = RuntimeError("Cancellation stopped before completion wait")
        task.set_exception(error)
        with self.assertRaisesRegex(AssertionError, "Worker failed before cancellation completion wait") as failure:
            cancellation_requested(started, completion_wait, task)
        self.assertIs(failure.exception.__cause__, error)


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
        entered, release, cancel_started = Future(), threading.Event(), Future()
        sealing = Future()
        cancelling = None
        results = []
        def commit():
            nonlocal cancelling
            entered.set_result(True)
            self.assertTrue(phase_reached(entered, sealing, "candidate commit"))
            cancelling = start_worker(cancel)
            self.assertTrue(phase_reached(cancel_started, cancelling.future, "cancellation start"))
            release.set()
            self.assertTrue(release.is_set(), "Deadlock waiting for commit release")
            return {"fixture": "sealed"}
        def seal():
            results.append(ledger.commit_success(identifier, commit))
        def cancel():
            cancel_started.set_result(True)
            results.append(ledger.finish(identifier, "cancelled"))
        try:
            # Publish on this thread before arming the cancellation join. The
            # competing finish then only reads the already committed terminal.
            sealing.set_result(seal())
        finally:
            release.set()
            if cancelling is not None:
                join_worker(cancelling)
        self.assertFalse(not sealing.done() or cancelling.is_alive())
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

    def assert_completed(self, broker, identifier):
        # Completion includes the broker's drained callback and durable writes.
        # Never block here: synchronous execution must already have signalled it.
        future = broker.futures[identifier]
        self.assertTrue(future.done())
        future.result(timeout=0)

    def test_mock_raw_ready_replay_retained_bytes_and_eof(self):
        with broker_server(TOKEN) as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            broker = BackendBroker(self.project, client_factory=client_factory(server),
                                   clock=fixed_clock, executor=SynchronousExecutor())
            self.addCleanup(close_broker, broker)
            plan = broker.plan(self.request)
            self.assertFalse(plan["eligibility"]["shippable"])
            self.assertFalse(broker.ledger.root.exists())
            record = broker.start(plan["plan_id"], plan["plan_digest"])
            identifier = record["job_id"]
            self.assert_completed(broker, identifier)
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
        with broker_server(TOKEN, mode="card_change") as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            with managed_broker(self.project, client_factory=client_factory(server),
                               clock=fixed_clock, executor=SynchronousExecutor()) as broker:
                plan = broker.plan(self.request)
                record = broker.start(plan["plan_id"], plan["plan_digest"])
                self.assert_completed(broker, record["job_id"])
                terminal = broker.ledger.result(record["job_id"])
                self.assertEqual(terminal["errors"][0]["rule_id"], "backend.pin_mismatch")
                self.assertEqual(server.submissions, 0)

    def test_cancel_crash_hash_and_raw_geometry_fail_without_staging(self):
        for mode, rule in (("cancel", "backend.unavailable"), ("bad_hash", "backend.protocol"),
                           ("nonfinite", "glb.nonfinite"), ("external_uri", "glb.external"), ("oversize", "backend.limit")):
            with self.subTest(mode=mode), broker_server(TOKEN, mode=mode) as server:
                self.configure(f"http://127.0.0.1:{server.server_port}")
                request = deepcopy(self.request)
                request["request_id"] = "generate:" + mode
                with managed_broker(self.project, client_factory=client_factory(server),
                                   clock=fixed_clock, executor=SynchronousExecutor()) as broker:
                    plan = broker.plan(request)
                    record = broker.start(plan["plan_id"], plan["plan_digest"])
                    self.assert_completed(broker, record["job_id"])
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
        with managed_broker(self.project, clock=lambda: clock[0], client_factory=SlowClient,
                           executor=SynchronousExecutor()) as broker:
            plan = broker.plan(self.request)
            record = broker.start(plan["plan_id"], plan["plan_digest"])
            self.assert_completed(broker, record["job_id"])
            terminal = broker.ledger.result(record["job_id"])
            self.assertEqual(terminal["errors"][0]["rule_id"], "backend.deadline")
            self.assertEqual(cancelled, [digest(self.request["request_id"].encode())[:32]])

    def test_eof_during_submission_cancels_late_backend_id(self):
        self.configure("http://127.0.0.1:1234")
        entered, release, cancel_started = Future(), threading.Event(), Future()
        completion_wait = Future()
        cancelled = []
        hook_errors = []
        cancelling = None
        class SubmittingClient:
            def __init__(self, *args, **kwargs):
                pass
            def capabilities(self, **kwargs):
                return deepcopy(MOCK_CAPABILITY)
            def submit(self, *args, **kwargs):
                try:
                    return self.submitting(*args, **kwargs)
                except AssertionError as error:
                    hook_errors.append(error)
                    raise
            def submitting(self, *args, **kwargs):
                nonlocal cancelling
                entered.set_result(True)
                self_case.assertTrue(phase_reached(entered, broker.tasks[identifier], "submission"))
                # Complete the real cancellation write inline. Replay only its
                # return at the worker's update boundary, so the wait detector
                # cannot time filesystem work or depend on cancel_started.
                requested = original_update(identifier, cancel_requested=True, state="cancelling")
                def update(*update_args, **changes):
                    self_case.assertEqual(update_args, (identifier,))
                    self_case.assertEqual(changes, {"cancel_requested": True, "state": "cancelling"})
                    cancel_started.set_result(True)
                    return requested
                def await_completion(timeout=None):
                    completion_wait.set_result(True)
                    return DEFAULT
                with patch.object(broker.ledger, "update", side_effect=update), \
                        patch.object(completed, "result", side_effect=await_completion, wraps=original_result):
                    cancelling = start_worker(broker.cancel, identifier)
                    # Entry follows the cancellation update, even if its signal
                    # is lost. All durable work preceding this wait is finished.
                    self_case.assertTrue(cancellation_requested(cancel_started, completion_wait, cancelling.future))
                    # Cancellation cannot release ownership while the POST is outstanding.
                    with self_case.assertRaises(Refusal) as refusal:
                        RequestLedger(self_case.project, document="other").begin("generate:contender", digest(b"other"))
                    self_case.assertEqual(refusal.exception.finding["rule_id"], "busy")
                    self_case.assertTrue((broker.ledger.root / ".meshgen.lock").exists())
                release.set()
                if not release.is_set():
                    raise AssertionError("Deadlock waiting for submission release")
                return digest(args[0]["request_id"].encode())[:32]
            def cancel(self, identifier):
                cancelled.append(identifier)
                return {"job_id": identifier, "state": "cancelled"}
        broker = BackendBroker(self.project, clock=fixed_clock, client_factory=SubmittingClient,
                               executor=WorkerExecutor())
        self_case = self
        self.addCleanup(close_broker, broker)
        self.addCleanup(release.set)
        plan = broker.plan(self.request)
        record = broker.start(plan["plan_id"], plan["plan_digest"])
        identifier = record["job_id"]
        completed = broker.futures[identifier]
        original_result = completed.result
        original_update = broker.ledger.update
        try:
            broker.executor.run_pending()
            # Backend orchestration converts callback errors to refusals. Keep
            # test assertion failures visible even when cancellation wins drain.
            if hook_errors:
                raise hook_errors[0]
        finally:
            release.set()
            broker.executor.shutdown(wait=True)
            completed_result(broker.futures[record["job_id"]], "submission drain after release")
            if cancelling is not None:
                join_worker(cancelling)
        self.assertFalse(cancelling.is_alive())
        broker.on_eof()
        self.assertEqual(set(cancelled), {digest(self.request["request_id"].encode())[:32]})
        self.assertEqual(broker.ledger.result(record["job_id"])["state"], "cancelled")
        self.assertFalse(broker.raw)

    def test_early_run_refusal_fails_submission_handshake_without_timeout(self):
        self.configure("http://127.0.0.1:1234")
        entered, refuse = Future(), [False]
        class RefusingClient:
            def __init__(self, *args, **kwargs):
                pass
            def capabilities(self, **kwargs):
                if refuse[0]:
                    raise Refusal("backend.pin_mismatch", "Capability refused before submission")
                return deepcopy(MOCK_CAPABILITY)
            def submit(self, *args, **kwargs):
                entered.set_result(True)
                raise AssertionError("Refused worker must not submit")
        with managed_broker(self.project, clock=fixed_clock, client_factory=RefusingClient,
                           executor=SynchronousExecutor()) as broker:
            plan = broker.plan(self.request)
            refuse[0] = True
            record = broker.start(plan["plan_id"], plan["plan_digest"])
            task = broker.tasks[record["job_id"]]
            self.assertTrue(task.done())
            self.assertFalse(entered.done())
            # An already completed task must wake the handshake immediately.
            # Check the wait inputs rather than measuring elapsed wall time.
            with patch(__name__ + ".wait", wraps=wait) as waiting:
                with self.assertRaisesRegex(AssertionError, "Worker completed before submission:.*backend.pin_mismatch"):
                    phase_reached(entered, task, "submission")
            waiting.assert_called_once_with((entered, task), timeout=2, return_when=FIRST_COMPLETED)
            terminal = broker.ledger.result(record["job_id"])
            self.assertEqual(terminal["state"], "failed")
            self.assertEqual(terminal["errors"][0]["rule_id"], "backend.pin_mismatch")

    def test_missing_drained_completion_fails_and_cleanup_finishes(self):
        for cleanup in ("registered", "context"):
            with self.subTest(cleanup=cleanup), broker_server(TOKEN) as server:
                self.configure(f"http://127.0.0.1:{server.server_port}")
                broker = BackendBroker(self.project, client_factory=client_factory(server),
                                       clock=fixed_clock, executor=SynchronousExecutor())
                original_drained = broker._drained
                def missing_completion(identifier, task, completed):
                    # Do all drain/terminal work but lose the real wait handle's
                    # signal, reproducing the cleanup failure without a sleeper.
                    original_drained(identifier, task, Future())
                def exercise(case):
                    plan = broker.plan({**self.request, "request_id": "generate:missing-" + cleanup})
                    record = broker.start(plan["plan_id"], plan["plan_digest"])
                    case.assertTrue(broker.tasks[record["job_id"]].done())
                    self.assert_completed(broker, record["job_id"])
                def run(case):
                    if cleanup == "registered":
                        case.addCleanup(close_broker, broker)
                        exercise(case)
                    else:
                        with patch(__name__ + ".BackendBroker", return_value=broker):
                            with managed_broker(self.project):
                                exercise(case)
                case = unittest.FunctionTestCase(lambda: run(case))
                result = unittest.TestResult()
                # Guard cancellation as well as observing it, so a regression in
                # the cleanup helper fails this test without hanging its runner.
                with patch.object(broker, "_drained", side_effect=missing_completion), \
                        patch.object(broker, "cancel", side_effect=AssertionError("Incomplete future reached cancellation")) as cancel, \
                        patch.object(broker, "close", wraps=broker.close) as close, \
                        patch(__name__ + ".wait", side_effect=AssertionError("Synchronous cleanup must not wait")) as waiting:
                    case.run(result)
                self.assertEqual(result.testsRun, 1)
                self.assertEqual(result.errors, [])
                self.assertEqual(len(result.failures), 2 if cleanup == "registered" else 1)
                self.assertIn("Missing completion for broker drain", result.failures[-1][1])
                cancel.assert_not_called()
                close.assert_not_called()
                waiting.assert_not_called()
                self.assertTrue(all(task.done() for task in broker.tasks.values()))
                self.assertTrue(all(not future.done() for future in broker.futures.values()))
                # Reconcile this deliberately abandoned owner before the next
                # subtest. Cleanup itself must leave its incomplete handle alone.
                broker.ledger.finish(broker.ledger.active, "cancelled")

    def test_scene_revision_does_not_cancel_but_session_change_prevents_staging(self):
        self.configure("http://127.0.0.1:1234")
        data = synthetic_glb(seed=7)
        entered, release = Future(), threading.Event()
        hook_errors = []
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
                try:
                    return self.receiving(*args, **kwargs)
                except AssertionError as error:
                    hook_errors.append(error)
                    raise
            def receiving(self, *args, **kwargs):
                entered.set_result(True)
                self_case.assertTrue(phase_reached(entered, broker.tasks[identifier], "result intake"))
                broker.session = "new-document"
                release.set()
                if not release.is_set():
                    raise AssertionError("Deadlock waiting for result release")
                return data
            def cancel(self, identifier):
                return {"job_id": identifier, "state": "cancelled"}
        broker = BackendBroker(self.project, clock=fixed_clock, client_factory=PausedClient,
                               executor=WorkerExecutor())
        self_case = self
        self.addCleanup(close_broker, broker)
        self.addCleanup(release.set)
        broker.scene_revision = 1
        plan = broker.plan(self.request)
        broker.scene_revision = 2
        record = broker.start(plan["plan_id"], plan["plan_digest"])
        identifier = record["job_id"]
        try:
            broker.executor.run_pending()
            if hook_errors:
                raise hook_errors[0]
        finally:
            release.set()
            broker.executor.shutdown(wait=True)
            completed_result(broker.futures[record["job_id"]], "result drain after release")
        terminal = broker.ledger.result(record["job_id"])
        self.assertEqual(terminal["errors"][0]["rule_id"], "stale.plan")
        self.assertFalse(broker.raw)

    def test_real_backend_and_later_phase_requests_are_disabled(self):
        self.configure("http://127.0.0.1:1234")
        config = project_fixture()
        config["backends"]["mock"]["kind"] = "model"
        config["backends"]["mock"]["pins"] = {"model_revision": "0"*40, "weights_sha256": ["1"*64]}
        atomic_json(self.config_path, config)
        with managed_broker(self.project) as broker:
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
        with broker_server(TOKEN) as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            request = deepcopy(self.request)
            request["overrides"]["candidates"] = {"count": 4}
            with managed_broker(self.project, client_factory=client_factory(server),
                               clock=fixed_clock, executor=SynchronousExecutor()) as broker:
                plan = broker.plan(request)
                record = broker.start(plan["plan_id"], plan["plan_digest"])
                self.assert_completed(broker, record["job_id"])
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

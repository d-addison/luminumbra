"""Regression evidence for the original E3 review and all round-two findings."""
from copy import deepcopy
import ctypes
import http.client
import json
import os
from pathlib import Path
import queue
import random
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch
import zlib

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
sys.path.insert(0, str(SERVICE.parent / "probes"))
from luminumbra_author import meshgen_jobs as jobs
from luminumbra_author.contracts import Refusal, canonical, digest
from luminumbra_author.meshgen_backend import BackendClient, DeadlineSocket, image_snapshot, MOCK_CAPABILITY
from luminumbra_author.meshgen_licence import gate
from luminumbra_author.meshgen_registry import defaults
from mock_backend import MODES, synthetic_glb
from meshgen_http_fixture import RuleAssertions, owned_server, client_factory
import test_meshgen_jobs as jobtests
from test_meshgen_backend import TOKEN, planning, png, changed_glb


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind+data) & 0xffffffff)


def jpeg(*, late=None, empty=False):
    def segment(marker, payload):
        return bytes((255, marker)) + struct.pack(">H", len(payload)+2) + payload
    # Supported sequential three-component frame, one component per scan.
    data = b"\xff\xd8" + segment(0xdb, b"\0" + b"\1"*64)
    data += segment(0xc4, b"\0" + b"\1" + b"\0"*15 + b"\0"
                    + b"\x10" + b"\1" + b"\0"*15 + b"\0")
    data += segment(0xc0, b"\x08\0\1\0\1\3\1\x11\0\2\x11\0\3\x11\0")
    for identity in (1, 2, 3):
        data += segment(0xda, bytes((1, identity, 0, 0, 63, 0))) + (b"" if empty else b"\x3f")
        if identity == 2 and late:
            data += segment(*late)
    return data + b"\xff\xd9"


def wait_child_line(child, timeout=5):
    """Windows pipe reads cannot be selected; bound readiness with a queue."""
    ready = queue.Queue(maxsize=1)
    def read():
        try:
            ready.put(child.stdout.readline())
        except Exception:
            ready.put("")
    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    try:
        line = ready.get(timeout=timeout)
        if not line:
            raise Refusal("backend.unavailable", "Child exited before readiness.")
        return line.strip()
    except queue.Empty as error:
        raise Refusal("backend.deadline", "Child readiness deadline expired.") from error
    finally:
        # The caller kills/reaps before joining a timed-out pipe reader.
        child.readiness_reader = reader


def reap_child(child):
    if child.poll() is None:
        child.kill()
    child.wait(timeout=5)
    reader = getattr(child, "readiness_reader", None)
    if reader is not None:
        reader.join(timeout=5)
        if reader.is_alive():
            raise AssertionError("Readiness reader did not stop after child exit")
    for stream in (child.stdin, child.stdout, child.stderr):
        if stream is not None:
            stream.close()


class IntakeReviewTests(RuleAssertions, unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.project = Path(self.temp.name)
        self.limits = defaults()["limits"]

    def snapshot(self, data):
        (self.project / "ref").write_bytes(data)
        return image_snapshot(self.project, {"path": "ref", "sha256": digest(data)}, self.limits,
                              "refuse_if_ai_training_reserved")

    def test_complete_jpeg_scans_and_metadata_after_later_scan(self):
        self.assertEqual(self.snapshot(jpeg())["format"], "jpeg")
        header = jpeg()[:jpeg().index(b"\xff\xda")]
        interleaved = header + b"\xff\xda\0\x0c\3\1\0\2\0\3\0\0\x3f\0\3\xff\xd9"
        self.assertEqual(self.snapshot(interleaved)["format"], "jpeg")
        restarted = jpeg().replace(b"\0\1\0\1\3", b"\0\1\0\x10\3", 1).replace(
            b"\x3f\xff", b"\x3f\xff\xd0\x3f\xff")
        restarted = restarted[:2] + b"\xff\xdd\0\4\0\1" + restarted[2:]
        self.assertEqual(self.snapshot(restarted)["dimensions"], [16, 1])
        with self.assertRule("request.invalid"):
            self.snapshot(restarted.replace(b"\xff\xd0", b"\xff\xd1", 1))
        for marker, payload in ((0xeb, b"credential"), (0xe1, b"Exif\0secret"),
                                (0xfe, b"xmp cawg.training-and-data-mining")):
            with self.subTest(marker=marker), self.assertRule("licence.image_credentials"):
                self.snapshot(jpeg(late=(marker, payload)))
        for data in (jpeg()[:-2], jpeg(empty=True), jpeg()+b"trailing", jpeg()[:70],
                     b"\xff\xd8\xff\xd9", jpeg().replace(b"\x3f\xff", b"\xff\x00\xff", 1),
                     jpeg().replace(b"\x3f\xff", b"\x3f\0\xff", 1),
                     jpeg().replace(b"\0\1\0\1\3", b"\0\xff\0\xff\3", 1)):
            with self.subTest(data=data[:15]), self.assertRule("request.invalid"):
                self.snapshot(data)

    def test_png_requires_complete_bounded_pixel_stream_and_grammar(self):
        header = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
        end = chunk(b"IEND", b"")
        for data in (header+end, header+chunk(b"IDAT", b"")+end,
                     header+chunk(b"IDAT", zlib.compress(b"\0"))+end,
                     header+chunk(b"IDAT", zlib.compress(b"\0"*5))+end,
                     header+chunk(b"IDAT", zlib.compress(b"\5\0\0\0"))+end,
                     header+chunk(b"IDAT", zlib.compress(b"\0\0\0\0")[:-1])+end,
                     header+chunk(b"IDAT", zlib.compress(b"\0\0\0\0")+b"trailing")+end,
                     header+header[8:]+end, png()[:-12], png()+b"extra",
                     header+chunk(b"what", b"hidden")+end,
                     header+chunk(b"IDAT", zlib.compress(b"\0\0\0\0"))
                        +chunk(b"tEXt", b"note\0safe")+chunk(b"IDAT", b"x")+end):
            with self.subTest(data=data[-30:]), self.assertRule("request.invalid"):
                self.snapshot(data)
        late = png()[:-12] + chunk(b"caBX", b"hidden credentials") + end
        with self.assertRule("licence.image_credentials"):
            self.snapshot(late)
        self.assertEqual(self.snapshot(png())["dimensions"], [1, 1])
        # Expansion is processed in bounded pieces, including across IDATs.
        pixels = zlib.compress((b"\0" + b"\0"*(128*3))*256)
        large = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 128, 256, 8, 2, 0, 0, 0))
        self.assertEqual(self.snapshot(large+chunk(b"IDAT", pixels[:10])+chunk(b"IDAT", pixels[10:])+end)["dimensions"], [128, 256])

    def test_unconsumed_glb_attributes_refused_and_aggregate_expansion_bounded(self):
        for name in ("NORMAL", "TEXCOORD_0", "COLOR_0", "TANGENT", "JOINTS_0", "_CUSTOM"):
            for reference in (0, 999999, False):
                with self.subTest(attribute=name, reference=reference), self.assertRule("glb.encoding"):
                    jobs.inspect_raw_glb(changed_glb(lambda doc: doc["meshes"][0]["primitives"][0]["attributes"].update({name: reference})), defaults())
        effective = defaults()
        effective["limits"]["glb_accessor_elements"] = 40  # 8 positions + 36 indices.
        with self.assertRule("glb.limit"):
            jobs.inspect_raw_glb(synthetic_glb(), effective)

    def test_restart_flood_refused_before_allocating_beyond_mcu_or_collection_bound(self):
        import luminumbra_author.meshgen_backend as backend
        # A one-MCU frame permits zero restarts, regardless of input byte cap.
        prefix = jpeg()[:jpeg().index(b"\xff\xda")+10]
        prefix = prefix[:2] + b"\xff\xdd\0\4\0\1" + prefix[2:]
        flood = prefix + b"\x3f" + b"\xff\xd0"*100000 + b"\xff\xd9"
        allocated = []
        def buffer():
            allocated.append(1)
            return bytearray()
        started = time.monotonic()
        with patch.object(backend, "bytearray", side_effect=buffer, create=True):
            with self.assertRule("request.invalid"):
                self.snapshot(flood)
        self.assertEqual(len(allocated), 1)
        # Derive the restart collection bound at SOS, before even the first
        # entropy buffer is allocated (large frame, interval of one).
        wide = prefix.replace(b"\0\1\0\1\3", b"\0\x40\0\x40\3", 1)
        self.limits["collection_items"] = 8
        with patch.object(backend, "bytearray", side_effect=buffer, create=True):
            with self.assertRule("request.limit"):
                self.snapshot(wide + b"\x3f\xff\xd9")
        self.assertEqual(len(allocated), 1)
        self.assertLess(time.monotonic()-started, 3)

    def test_png_large_idat_has_linear_bounded_inflate_work_and_exact_refusal(self):
        import luminumbra_author.meshgen_backend as backend
        width, height = 1024, 384
        rng = random.Random(19)
        pixels = b"".join(b"\0" + rng.randbytes(width*3) for _ in range(height))
        header = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        original = zlib.decompressobj
        for extra in (b"", b"x"):
            compressed = zlib.compress(pixels+extra, level=0)
            fed, tails = [], []
            class CountedDecoder:
                def __init__(self): self.decoder = original()
                def decompress(self, value, maximum):
                    fed.append(len(value))
                    result = self.decoder.decompress(value, maximum)
                    tails.append(len(self.decoder.unconsumed_tail))
                    return result
                def __getattr__(self, name): return getattr(self.decoder, name)
            started = time.monotonic()
            with self.subTest(extra=extra), patch.object(backend.zlib, "decompressobj", CountedDecoder):
                data = header + chunk(b"IDAT", compressed) + chunk(b"IEND", b"")
                if extra:
                    with self.assertRule("request.invalid"):
                        self.snapshot(data)
                else:
                    self.assertEqual(self.snapshot(data)["dimensions"], [width, height])
            self.assertLessEqual(max(fed), 65536)
            self.assertLessEqual(max(tails), 65536)
            self.assertLessEqual(sum(fed), 2*len(compressed))
            self.assertLess(time.monotonic()-started, 3)

    def test_truncated_jpeg_fill_returns_request_invalid(self):
        for data in (b"\xff\xd8\xff\xff", b"\xff\xd8"+b"\xff"*100000,
                     jpeg()[:-2]+b"\xff\xff"):
            with self.subTest(length=len(data)), self.assertRule("request.invalid"):
                self.snapshot(data)

    def test_raw_glb_container_refuses_before_shared_parser_or_payload_allocation(self):
        effective = defaults()
        def glb(body, *, magic=b"glTF", version=2, length=None):
            return struct.pack("<4sII", magic, version, 12+len(body) if length is None else length)+body
        json_chunk = struct.pack("<II", 4, 0x4e4f534a)+b"{}  "
        bin_chunk = struct.pack("<II", 0, 0x004e4942)
        cases = [(glb(json_chunk, magic=b"bad!"), "glb.header"),
                 (glb(json_chunk, version=1), "glb.header"),
                 (glb(json_chunk, length=999), "glb.header"),
                 (glb(json_chunk+b"short"), "glb.chunk"),
                 (glb(struct.pack("<II", 0, 0x004e4942)), "glb.chunk"),
                 (glb(struct.pack("<II", 0xfffffffc, 0x4e4f534a)), "glb.chunk"),
                 (glb(struct.pack("<II", 3, 0x4e4f534a)+b"abc"), "glb.chunk"),
                 (glb(json_chunk+bin_chunk*100000), "glb.chunk"),
                 (glb(struct.pack("<II", 1024**2+4, 0x4e4f534a)+b" "*(1024**2+4)), "glb.limit")]
        started = time.monotonic()
        for data, rule in cases:
            with self.subTest(rule=rule, size=len(data)), patch("luminumbra_author.formats.validate_glb") as shared:
                with self.assertRule(rule):
                    jobs.inspect_raw_glb(data, effective)
                shared.assert_not_called()
        self.assertLess(time.monotonic()-started, 3)

    def test_nc_dataset_executed_false_always_blocks(self):
        card = deepcopy(MOCK_CAPABILITY)
        lineage = deepcopy(card["components"][0]["licence"])
        lineage.update(executed=False, clauses=["non_commercial"])
        card["dataset_chain"]["licences"] = [lineage]
        policy = defaults()["policy"]["licence"]
        policy["allowed_status"] = ["blocked", "shippable"]
        result = gate(card, policy)
        self.assertEqual(result["status"], "blocked")
        self.assertFalse(result["runnable"])
        self.assertIn("licence.status_not_allowed", {item["rule_id"] for item in result["findings"]})


class LedgerReviewTests(RuleAssertions, unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.project = Path(self.temp.name)
        self.plan_digest = digest(b"plan")

    def test_atomic_owner_publication_and_malformed_owner_reconciliation(self):
        for index, malformed in enumerate((b"", b'{"pid":', b"{}")):
            ledger = jobs.RequestLedger(self.project)
            record = ledger.begin("generate:malformed"+str(index), self.plan_digest)
            (ledger.root / ".meshgen.lock").write_bytes(malformed)
            recovered = jobs.RequestLedger(self.project, owner_start=lambda pid: None)
            recovered.recover()
            terminal = recovered.result(record["job_id"])
            self.assertEqual(terminal["errors"][0]["rule_id"], "backend.unavailable")
            self.assertEqual(recovered.begin(record["request_id"], self.plan_digest), terminal)
            self.assertFalse((ledger.root / ".meshgen.lock").exists())
        ledger = jobs.RequestLedger(self.project)
        record = ledger.begin("generate:live-marker", self.plan_digest)
        (ledger.root / ".meshgen.lock").write_bytes(b"")
        with self.assertRule("busy"):
            jobs.RequestLedger(self.project, document="other").begin("generate:new", self.plan_digest)
        self.assertEqual(ledger._read(".meshgen.lock"), record["owner"])
        ledger.cancel(record["job_id"])

    def test_invalid_markers_cannot_reconcile_or_release_a_lock(self):
        ledger = jobs.RequestLedger(self.project)
        record = ledger.begin("generate:invalid", self.plan_digest)
        name = ledger._record_name(record["job_id"])
        for change in ({"plan_digest": False}, {"state": "unknown"}, {"state": "failed"}, {"owner": {"pid": True}}):
            with self.subTest(change=change):
                ledger._write(name, {**record, **change})
                with self.assertRule("request.conflict"):
                    jobs.RequestLedger(self.project, owner_start=lambda pid: None).recover()
                self.assertTrue((ledger.root / ".meshgen.lock").exists())
        ledger._write(name, record)
        ledger.cancel(record["job_id"])

    def test_unconfirmed_orphan_keeps_ownership_until_recovery_confirms_stop(self):
        ledger = jobs.RequestLedger(self.project)
        record = ledger.begin("generate:orphan", self.plan_digest)
        ledger.update(record["job_id"], state="running", backend_job_id="backend", backend_id="mock", profile="default")
        def unavailable(record):
            raise Refusal("backend.unavailable", "Unavailable")
        recovered = jobs.RequestLedger(self.project, owner_start=lambda pid: None, recover_cancel=unavailable)
        self.assertEqual(recovered.recover()["job_id"], record["job_id"])
        marker = recovered._read(recovered._record_name(record["job_id"]))
        self.assertEqual(marker["state"], "blocked")
        self.assertEqual(marker["errors"][0]["rule_id"], "backend.unavailable")
        with self.assertRule("busy"):
            recovered.begin("generate:other", self.plan_digest)
        recovered.recover_cancel = lambda record: None  # trusted confirmation callback
        recovered.recover()
        self.assertEqual(recovered.result(record["job_id"])["state"], "failed")
        self.assertFalse((ledger.root / ".meshgen.lock").exists())

    def test_reservation_reconstruction_conflicts_orphans_and_project_quotas(self):
        ledger = jobs.RequestLedger(self.project, terminal_limit=1)
        record = ledger.begin("generate:original", self.plan_digest)
        reservation = ledger.root / "_headless/requests" / (digest(b"generate:original")+".json")
        reservation.unlink()
        recovered = jobs.RequestLedger(self.project, terminal_limit=1, owner_start=lambda pid: None)
        recovered.recover()
        terminal = recovered.result(record["job_id"])
        self.assertEqual(recovered.begin("generate:original", self.plan_digest), terminal)
        with self.assertRule("request.limit"):
            jobs.RequestLedger(self.project, document="other", terminal_limit=1).begin("generate:new", self.plan_digest)
        reservation.write_bytes(canonical({"request_id": "generate:original", "job_id": record["job_id"], "plan_digest": "0"*64}))
        with self.assertRule("request.conflict"):
            recovered.recover()

    def test_orphan_directory_and_retained_bytes_count_toward_admission(self):
        ledger = jobs.RequestLedger(self.project, terminal_limit=1)
        ledger._path("_headless/jobs/"+"0"*32, directory=True)
        with self.assertRule("request.limit"):
            ledger.begin("generate:new", self.plan_digest)
        ledger.terminal_limit = 2
        ledger.byte_limit = 20000
        ledger._path("retained.bin").write_bytes(b"x"*18000)
        with self.assertRule("request.limit"):
            ledger.begin("generate:new", self.plan_digest)

        # A reservation without a job directory is distinct from the retained
        # orphan job directory; both count, rather than max(jobs, requests).
        ledger.byte_limit = jobs.PROJECT_BYTE_LIMIT
        ledger._write("_headless/requests/"+digest(b"generate:reserved")+".json", {
            "request_id": "generate:reserved", "plan_digest": self.plan_digest, "job_id": "1"*32})
        with self.assertRule("request.limit"):
            ledger.begin("generate:new", self.plan_digest)
        with self.assertRule("request.conflict"):
            ledger.begin("generate:reserved", self.plan_digest)

    def test_terminal_cannot_release_undrained_backend(self):
        ledger = jobs.RequestLedger(self.project)
        record = ledger.begin("generate:undrained", self.plan_digest)
        ledger.update(record["job_id"], backend_job_id="backend")
        with self.assertRule("busy"):
            ledger.cancel(record["job_id"])
        self.assertTrue((ledger.root / ".meshgen.lock").exists())
        ledger.update(record["job_id"], backend_stopped=True, task_drained=True)
        ledger.cancel(record["job_id"])

    def test_each_ledger_payload_is_checked_decoded_and_serialized(self):
        token = 'quote"\\' + "K"*32
        ledger = jobs.RequestLedger(self.project, secret_source=lambda: (token,))
        names = (".meshgen.lock", "_headless/jobs/"+"0"*32+"/running.json",
                 "_headless/requests/reservation.json", "_headless/jobs/"+"0"*32+"/terminal.json")
        for name in names:
            for value in ({"observation": token}, {token: "value"}):
                with self.subTest(name=name, value=value), self.assertRule("backend.protocol"):
                    ledger._write(name, value)
                with self.assertRule("backend.protocol"):
                    ledger.retain(name, canonical(value))
                self.assertFalse(ledger._path(name).exists())
        # Raw serialized bytes can contain a token across JSON syntax, even
        # when no individual decoded string contains it.
        ledger.secret_source = lambda: ('","b":"'+"X"*32,)
        with self.assertRule("backend.protocol"):
            ledger._write("raw.json", {"a": "", "b": "X"*32})

    def test_admission_rolls_back_every_write_failure_with_live_pid(self):
        for stage in ("marker", "reservation", "owner"):
            for after in (False, True):
                with self.subTest(stage=stage, after=after):
                    ledger = jobs.RequestLedger(self.project)
                    original = ledger._write
                    def write(name, value):
                        selected = (name.endswith("running.json") if stage == "marker" else
                                    "/requests/" in name if stage == "reservation" else name == ".meshgen.lock")
                        if selected and not after:
                            raise OSError("Injected publication failure")
                        original(name, value)
                        if selected:
                            raise OSError("Injected failure after publication")
                    with patch.object(ledger, "_write", side_effect=write), self.assertRule("backend.unavailable"):
                        ledger.begin("generate:write-failure", self.plan_digest)
                    self.assertIsNone(ledger.active)
                    self.assertFalse(list(ledger.root.rglob("running.json")))
                    self.assertFalse(list(ledger.root.glob("*/requests/*.json")))
                    self.assertFalse((ledger.root/".meshgen.lock").exists())
                    # Recovery and admission work before the old PID exits.
                    other = jobs.RequestLedger(self.project)
                    record = other.begin("generate:write-failure", self.plan_digest)
                    other.cancel(record["job_id"])
                    # Keep each injection independent of earlier replay.
                    for path in sorted(ledger.root.rglob("*"), key=lambda p: len(p.parts), reverse=True):
                        if path.is_file(): jobs.durable_remove(path)
                        elif path.is_dir(): path.rmdir()

    def test_subprocess_readiness_deadline_and_early_exit_always_reap(self):
        for source, rule in (("sys.stdin.read()", "backend.deadline"), ("sys.exit(3)", "backend.unavailable")):
            child = self.child(source, interactive=True)
            try:
                with self.assertRule(rule):
                    wait_child_line(child, timeout=.2 if rule == "backend.deadline" else 5)
            finally:
                reap_child(child)
            self.assertIsNotNone(child.returncode)
            self.assertFalse(child.readiness_reader.is_alive())
            self.assertTrue(all(stream.closed for stream in (child.stdin, child.stdout, child.stderr)))

    def test_failed_admission_rollback_is_drained_and_recoverable_while_pid_live(self):
        ledger = jobs.RequestLedger(self.project)
        write, remove = ledger._write, jobs.durable_remove
        def failing_write(name, value):
            write(name, value)
            if "/requests/" in name:
                raise OSError("Reservation publication failed")
        def failing_remove(path):
            if path.parent.name == "requests":
                raise OSError("Reservation rollback failed")
            remove(path)
        with patch.object(ledger, "_write", side_effect=failing_write), patch.object(jobs, "durable_remove", side_effect=failing_remove):
            with self.assertRule("backend.unavailable"):
                ledger.begin("generate:rollback-failure", self.plan_digest)
        marker = ledger.status(ledger.active)
        self.assertEqual(marker["state"], "blocked")
        self.assertTrue(marker["task_drained"])
        self.assertEqual(marker["pending_errors"][0]["rule_id"], "backend.unavailable")
        recovered = jobs.RequestLedger(self.project)
        recovered.recover()
        terminal = recovered.result(marker["job_id"])
        self.assertEqual(terminal["state"], "failed")
        self.assertEqual(terminal["errors"][0]["rule_id"], "backend.unavailable")
        self.assertFalse((ledger.root/".meshgen.lock").exists())

    def test_durable_publication_flushes_file_before_directory(self):
        if os.name == "nt":
            self.skipTest("POSIX directory fsync ordering; Windows replacement tested separately")
        actions = []
        original_fsync = os.fsync
        def fsync(fd):
            actions.append("directory" if stat.S_ISDIR(os.fstat(fd).st_mode) else "file")
            return original_fsync(fd)
        with patch.object(jobs.os, "fsync", side_effect=fsync):
            jobs.durable_write(self.project / "bytes", b"complete")
        self.assertEqual(actions, ["file", "directory"])
        actions.clear()
        with patch.object(jobs.os, "fsync", side_effect=fsync):
            jobs.durable_mkdir(self.project / "parent/child")
            jobs.durable_remove(self.project / "bytes")
        self.assertEqual(actions, ["directory", "directory", "directory"])

    def test_file_replace_uses_shared_publication_with_durability(self):
        source, target = self.project / "source", self.project / "target"
        with patch.object(jobs.file_io, "replace_file") as replace, patch.object(jobs, "sync_directory") as sync:
            jobs.durable_replace(source, target)
        replace.assert_called_once_with(source, target, durable=True)
        sync.assert_called_once_with(target.parent)

    def test_windows_directory_replace_uses_replace_existing_and_write_through(self):
        source, target = self.project / "source", self.project / "target"
        kernel = Mock()
        kernel.MoveFileExW.return_value = True
        with patch.object(jobs.os, "name", "nt"), patch.object(ctypes, "WinDLL", return_value=kernel, create=True):
            jobs.durable_replace_directory(source, target)
        kernel.MoveFileExW.assert_called_once_with(str(source), str(target), 0x1 | 0x8)

    def child(self, source, *arguments, interactive=False):
        # All child temporary storage stays under the caller's clone-local TMPDIR.
        code = "import sys; sys.path.insert(0, " + repr(str(SERVICE)) + "); " + source
        args = [sys.executable, "-I", "-S", "-B", "-c", code, str(self.project), *arguments]
        if interactive:
            return subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        return subprocess.run(args, capture_output=True, text=True, timeout=10)

    def test_separate_process_lock_contention(self):
        owner = self.child('from luminumbra_author.meshgen_jobs import RequestLedger; from luminumbra_author.contracts import digest; '
                           'l=RequestLedger(sys.argv[1]); r=l.begin("generate:owner",digest(b"plan")); '
                           'print(r["job_id"],flush=True); sys.stdin.readline(); l.cancel(r["job_id"])', interactive=True)
        try:
            identifier = wait_child_line(owner)
            self.assertRegex(identifier, r"^[0-9a-f]{32}$")
            contender = self.child('from luminumbra_author.meshgen_jobs import RequestLedger; from luminumbra_author.contracts import Refusal,digest\n'
                'try: RequestLedger(sys.argv[1],document="other").begin("generate:contender",digest(b"plan"))\n'
                'except Refusal as e: print(e.finding["rule_id"])')
            self.assertEqual(contender.returncode, 0, contender.stderr)
            self.assertEqual(contender.stdout.strip(), "busy")
            owner.stdin.write("stop\n")
            owner.stdin.flush()
            stdout, stderr = owner.communicate(timeout=10)
            self.assertEqual(owner.returncode, 0, stderr)
            self.assertEqual(jobs.RequestLedger(self.project).result(identifier)["state"], "cancelled")
        finally:
            reap_child(owner)

    def test_separate_process_crash_at_each_reservation_publication(self):
        for stage in ("marker", "reservation", "owner_prepare", "owner", "terminal", "hash", "transition"):
            with self.subTest(stage=stage):
                source = '''import os
from luminumbra_author import meshgen_jobs
from luminumbra_author.meshgen_jobs import RequestLedger
from luminumbra_author.contracts import digest,parse_json
l=RequestLedger(sys.argv[1],document=sys.argv[2]); original=l._write; retain=l.retain
stage=sys.argv[2]
def write(name,value):
    original(name,value)
    if (stage=="marker" and name.endswith("running.json")) or (stage=="reservation" and "/requests/" in name) or (stage=="owner" and name==".meshgen.lock") or (stage=="terminal" and name.endswith("terminal.json")) or (stage=="transition" and name.endswith("running.json") and value["state"]=="failed"):
        os._exit(73)
def retained(name,data):
    retain(name,data)
    if stage=="hash" and name.endswith("terminal.sha256"): os._exit(73)
replace=meshgen_jobs.durable_replace
def publish(source,destination):
    if stage=="owner_prepare" and destination.name==".meshgen.lock":
        assert not destination.exists()
        assert parse_json(source.read_bytes())["document"]==stage
        os._exit(73)
    replace(source,destination)
meshgen_jobs.durable_replace=publish
l._write=write; l.retain=retained
r=l.begin("generate:"+stage,digest(b"plan")); l.finish(r["job_id"],"failed",errors=[{"rule_id":"original.fixture"}])
'''
                crashed = self.child(source, stage)
                self.assertEqual(crashed.returncode, 73, crashed.stderr)
                ledger = jobs.RequestLedger(self.project, document=stage)
                ledger.recover()
                replay = ledger.begin("generate:"+stage, self.plan_digest)
                self.assertEqual(replay["state"], "failed")
                self.assertEqual(replay["errors"][0]["rule_id"], "original.fixture" if stage in ("hash", "transition") else "backend.unavailable")
                self.assertEqual(len(list((ledger.root / stage / "jobs").iterdir())), 1)
                self.assertFalse((ledger.root / ".meshgen.lock").exists())


class BrokerReviewTests(RuleAssertions, unittest.TestCase):
    setUp = jobtests.BrokerTests.setUp
    configure = jobtests.BrokerTests.configure

    def test_recovery_and_cancel_require_recorded_endpoint_and_config_digest(self):
        for drift in ("endpoint", "digest", "missing_identity"):
            with self.subTest(drift=drift), owned_server(("127.0.0.1", 0), TOKEN) as server:
                endpoint = f"http://127.0.0.1:{server.server_port}"
                self.configure(endpoint)
                broker = jobs.BackendBroker(self.project, client_factory=client_factory(server))
                self.addCleanup(broker.close)
                request = deepcopy(self.request); request["request_id"] = "generate:identity-"+drift
                plan = broker.plan(request)
                record = broker.start(plan["plan_id"], plan["plan_digest"])
                identifier = record["job_id"]
                broker.futures[identifier].result(timeout=5)
                marker = broker.ledger.status(identifier)
                self.assertEqual(marker["backend_endpoint"], endpoint)
                self.assertEqual(marker["config_digest"], plan["config_digest"])
                original_config = self.config_path.read_bytes()
                if drift == "endpoint":
                    self.configure("http://127.0.0.1:54321" if server.server_port != 54321 else "http://127.0.0.1:54322")
                elif drift == "digest":
                    value = json.loads(original_config)
                    value["backends"]["mock"]["timeouts"]["connect_s"] = 4
                    self.config_path.write_bytes(canonical(value))
                else:
                    marker.pop("backend_endpoint")
                    broker.ledger._write(broker.ledger._record_name(identifier), marker)
                # Direct recovery refuses the exact rule without contacting
                # a potentially different mock bearing the same job ID.
                with self.assertRule("backend.pin_mismatch"):
                    broker._recover_cancel(broker.ledger.status(identifier))
                blocked = broker.cancel(identifier)
                self.assertEqual(blocked["state"], "blocked")
                self.assertEqual(blocked["errors"][0]["rule_id"], "backend.pin_mismatch")
                recovery = jobs.BackendBroker(self.project, client_factory=client_factory(server))
                self.addCleanup(recovery.close)
                self.assertEqual(recovery.ledger.recover()["job_id"], identifier)
                marker = recovery.ledger._read(recovery.ledger._record_name(identifier))
                self.assertEqual(marker["state"], "blocked")
                self.assertEqual(marker["errors"][0]["rule_id"], "backend.pin_mismatch")
                self.assertEqual(server.cancellations, 0)
                with self.assertRule("busy"):
                    recovery.ledger.begin("generate:contender", digest(b"other"))
                self.config_path.write_bytes(original_config)
                if drift == "missing_identity":
                    broker.ledger.update(identifier, backend_endpoint=endpoint)
                recovery.ledger.recover()
                self.assertEqual(recovery.ledger.result(identifier)["state"], "cancelled")
                self.assertEqual(server.cancellations, 1)
                broker.close(); recovery.close()

    def test_backend_identity_is_durable_before_submit(self):
        with owned_server(("127.0.0.1", 0), TOKEN) as server:
            endpoint = f"http://127.0.0.1:{server.server_port}"
            self.configure(endpoint)
            factory = client_factory(server)
            seen = []
            def create(*args, **kwargs):
                client = factory(*args, **kwargs)
                original = client.submit
                def submit(value, **kw):
                    marker = broker.ledger.status(broker.ledger.active)
                    seen.append(marker)
                    self.assertEqual(marker["backend_job_id"], digest(value["request_id"].encode())[:32])
                    self.assertEqual(marker["backend_endpoint"], endpoint)
                    self.assertEqual(marker["config_digest"], plan["config_digest"])
                    return original(value, **kw)
                client.submit = submit
                return client
            with jobs.BackendBroker(self.project, client_factory=create) as broker:
                plan = broker.plan(self.request)
                record = broker.start(plan["plan_id"], plan["plan_digest"])
                broker.futures[record["job_id"]].result(timeout=5)
                self.assertEqual(broker.ledger.status(record["job_id"])["state"], "finishing")
                self.assertEqual(len(seen), 1)

    def test_document_token_and_rotated_request_token_refuse_before_admission(self):
        for field in ("document", "request_id"):
            with self.subTest(field=field), owned_server(("127.0.0.1", 0), TOKEN) as server:
                self.configure(f"http://127.0.0.1:{server.server_port}")
                rotated = "R"*32
                request = deepcopy(self.request)
                request["request_id"] = "generate:"+rotated
                broker = jobs.BackendBroker(self.project, document=TOKEN if field == "document" else "_headless",
                                           client_factory=client_factory(server))
                with broker:
                    plan = broker.plan(request)
                    before = self.config_path.read_bytes()
                    with patch.dict(os.environ, {"LUMINUMBRA_GEN_TOKEN": rotated if field == "request_id" else TOKEN}):
                        with self.assertRule("backend.protocol"):
                            broker.start(plan["plan_id"], plan["plan_digest"])
                    self.assertEqual(self.config_path.read_bytes(), before)
                    self.assertIsNone(broker.ledger.active)
                    self.assertFalse(broker.ledger.root.exists())
                    self.assertEqual(server.submissions, 0)
                # The public ledger API resolves configured credentials too.
                with self.assertRule("backend.protocol"):
                    jobs.RequestLedger(self.project, document=TOKEN).begin("generate:direct", digest(b"plan"))

    def test_post_admission_marker_failure_has_terminal_owned_cleanup(self):
        with owned_server(("127.0.0.1", 0), TOKEN) as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            with jobs.BackendBroker(self.project, client_factory=client_factory(server)) as broker:
                plan = broker.plan(self.request)
                original = broker.ledger.update
                failed = []
                def update(identifier, **changes):
                    if changes.get("state") == "running" and not failed:
                        failed.append(identifier)
                        raise OSError("Injected running-marker failure")
                    return original(identifier, **changes)
                with patch.object(broker.ledger, "update", side_effect=update):
                    terminal = broker.start(plan["plan_id"], plan["plan_digest"])
                self.assertEqual(terminal["state"], "failed")
                self.assertEqual(terminal["errors"][0]["rule_id"], "backend.unavailable")
                self.assertIsNone(broker.ledger.active)
                self.assertFalse((broker.ledger.root/".meshgen.lock").exists())
                self.assertEqual(server.submissions, 0)

    def test_failure_and_cancellation_block_until_confirmed_backend_stop(self):
        for cause in ("failure", "cancel"):
            with self.subTest(cause=cause), owned_server(("127.0.0.1", 0), TOKEN) as server:
                self.configure(f"http://127.0.0.1:{server.server_port}")
                factory = client_factory(server)
                blocked = [True]
                def create(*args, **kwargs):
                    client = factory(*args, **kwargs)
                    original_cancel, original_result = client.cancel, client.result
                    def cancel(identifier, **kwargs):
                        if blocked[0]:
                            raise Refusal("backend.unavailable", "Unconfirmed stop")
                        return original_cancel(identifier, **kwargs)
                    client.cancel = cancel
                    if cause == "failure":
                        def result(*args, **kwargs):
                            raise Refusal("backend.protocol", "Bad result")
                        client.result = result
                    return client
                broker = jobs.BackendBroker(self.project, client_factory=create)
                request = deepcopy(self.request)
                request["request_id"] = "generate:"+cause
                plan = broker.plan(request)
                record = broker.start(plan["plan_id"], plan["plan_digest"])
                identifier = record["job_id"]
                broker.futures[identifier].result(timeout=5)
                if cause == "cancel":
                    broker.cancel(identifier)
                self.assertTrue(broker.tasks[identifier].done())
                self.assertEqual(broker.ledger.status(identifier)["state"], "blocked")
                self.assertEqual(broker.ledger.status(identifier)["errors"][0]["rule_id"], "backend.unavailable")
                with self.assertRule("busy"):
                    jobs.RequestLedger(self.project, document="other").begin("generate:other", digest(b"other"))
                blocked[0] = False
                # A new broker can retry recovery while the drained owner's PID
                # remains alive. The pending terminal result is preserved.
                recovery = jobs.RequestLedger(self.project, recover_cancel=broker._recover_cancel)
                recovery.recover()
                self.assertEqual(recovery.result(identifier)["state"], "failed" if cause == "failure" else "cancelled")
                broker.close()

    def test_executor_submission_failure_is_terminal_and_close_serializes_admission(self):
        with owned_server(("127.0.0.1", 0), TOKEN) as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            with jobs.BackendBroker(self.project, client_factory=client_factory(server)) as broker:
                plan = broker.plan(self.request)
                with patch.object(broker.executor, "submit", side_effect=RuntimeError("Rejected")):
                    terminal = broker.start(plan["plan_id"], plan["plan_digest"])
                self.assertEqual(terminal["state"], "failed")
                self.assertEqual(terminal["errors"][0]["rule_id"], "backend.unavailable")
                self.assertFalse((broker.ledger.root / ".meshgen.lock").exists())
            with self.assertRule("stale.plan"):
                broker.start(plan["plan_id"], plan["plan_digest"])
            # Pause submission with events, then start close concurrently.
            broker = jobs.BackendBroker(self.project, client_factory=client_factory(server))
            request = deepcopy(self.request); request["request_id"] = "generate:race"
            plan = broker.plan(request)
            entered, release, closing = threading.Event(), threading.Event(), threading.Event()
            original = broker.executor.submit
            def submit(*args):
                entered.set()
                if not release.wait(3):
                    raise RuntimeError("Submission release missing")
                return original(*args)
            records = []
            def close():
                closing.set()
                broker.close()
            with patch.object(broker.executor, "submit", side_effect=submit):
                starter = threading.Thread(target=lambda: records.append(broker.start(plan["plan_id"], plan["plan_digest"])))
                starter.start(); self.assertTrue(entered.wait(3))
                closer = threading.Thread(target=close)
                closer.start(); self.assertTrue(closing.wait(3))
                release.set(); starter.join(5); closer.join(5)
            self.assertFalse(starter.is_alive() or closer.is_alive())
            self.assertEqual(broker.ledger.result(records[0]["job_id"])["state"], "cancelled")
            self.assertFalse((broker.ledger.root / ".meshgen.lock").exists())

    def test_escaped_tokens_in_decoded_logs_glb_json_and_raw_bytes_are_never_retained(self):
        for token in ('quote"' + "x"*32, "backslash\\" + "x"*32):
            for location in ("log", "glb_json", "glb_binary", "capability", "refusal"):
                with self.subTest(token=token[:10], location=location), patch.dict(os.environ, {"LUMINUMBRA_GEN_TOKEN": token}), owned_server(("127.0.0.1", 0), token) as server:
                    self.configure(f"http://127.0.0.1:{server.server_port}")
                    factory = client_factory(server)
                    data = changed_glb(lambda doc: doc.update(extras={"observation": token})) if location == "glb_json" else synthetic_glb(seed=7)
                    if location == "glb_binary":
                        data += token.encode()
                    def create(*args, **kwargs):
                        client = factory(*args, **kwargs)
                        status, capabilities = client.status, client.capabilities
                        def observed_status(identifier, **kwargs):
                            value = status(identifier, **kwargs)
                            value["result"].update(sha256=digest(data), bytes=len(data))
                            if location == "log": value["result"]["logs_tail"] = token
                            return value
                        client.status = observed_status
                        client.result = lambda *args, **kwargs: data
                        if location == "refusal":
                            def refused(*args, **kwargs):
                                raise Refusal("config.unknown_key", "unknown: " + token, token)
                            client.result = refused
                        if location == "capability":
                            def card(**kwargs):
                                value = capabilities(**kwargs)
                                value["dataset_chain"]["notes"] = token
                                return value
                            client.capabilities = card
                        return client
                    with jobs.BackendBroker(self.project, client_factory=create) as broker:
                        request = deepcopy(self.request)
                        request["request_id"] = "generate:"+digest((token+location).encode())
                        if location == "capability":
                            with self.assertRule("backend.protocol"):
                                broker.plan(request)
                            continue
                        plan = broker.plan(request)
                        record = broker.start(plan["plan_id"], plan["plan_digest"])
                        broker.futures[record["job_id"]].result(timeout=5)
                        terminal = broker.ledger.result(record["job_id"])
                        self.assertEqual(terminal["errors"][0]["rule_id"], "backend.protocol")
                        self.assertFalse(list(broker.ledger.root.rglob("raw_backend.glb")))
                        self.assertFalse(list(broker.ledger.root.rglob("backend.log")))
                        for path in broker.ledger.root.rglob("*.json"):
                            self.assertNotIn(token, path.read_text())

    def test_real_mock_slow_mode_reaches_deadline_and_confirms_cancellation(self):
        with owned_server(("127.0.0.1", 0), TOKEN, mode="slow") as server:
            self.configure(f"http://127.0.0.1:{server.server_port}")
            clock = [0]
            factory = client_factory(server)
            def create(*args, **kwargs):
                client = factory(*args, **kwargs)
                original = client.status
                def status(*args, **kwargs):
                    value = original(*args, **kwargs)
                    self.assertEqual(value["state"], "running")
                    clock[0] = 61
                    return value
                client.status = status
                return client
            with jobs.BackendBroker(self.project, clock=lambda: clock[0], client_factory=create) as broker:
                plan = broker.plan(self.request)
                record = broker.start(plan["plan_id"], plan["plan_digest"])
                broker.futures[record["job_id"]].result(timeout=5)
                terminal = broker.ledger.result(record["job_id"])
                self.assertEqual(terminal["errors"][0]["rule_id"], "backend.deadline")
                self.assertEqual(server.cancellations, 1)
                self.assertFalse((broker.ledger.root / ".meshgen.lock").exists())

    def test_mock_record_and_byte_quotas_preserve_replay(self):
        for bound in ("records", "bytes"):
            with self.subTest(bound=bound), owned_server(("127.0.0.1", 0), TOKEN) as server:
                server.record_limit = 1
                server.byte_limit = 65536
                client = client_factory(server)(f"http://127.0.0.1:{server.server_port}", TOKEN)
                from luminumbra_author.meshgen_backend import submission
                request = planning(self.image)
                wire = submission(request, {"bytes": self.image}, {}, 60)
                identifier = client.submit(wire)
                if bound == "bytes":
                    server.record_limit = 256
                    server.byte_limit = 1
                request["request_id"] = "generate:second"
                with self.assertRule("backend.unavailable"):
                    client.submit(submission(request, {"bytes": self.image}, {}, 60))
                self.assertEqual(client.submit(wire), identifier)
                self.assertEqual(client.result(identifier), synthetic_glb(seed=7))
                self.assertEqual(len(server.jobs), 1)
                self.assertEqual(server.submissions, 1)


class DeadlineReviewTests(RuleAssertions, unittest.TestCase):
    def test_every_send_uses_remaining_deadline_and_bounded_chunks(self):
        for cost in (.4, 1.1):
            clock, timeouts, chunks = [0], [], []
            class Sock:
                def settimeout(self, value): timeouts.append(value)
                def sendall(self, data, flags=0):
                    chunks.append(len(data)); clock[0] += cost
            def remaining():
                if clock[0] >= 1: raise Refusal("backend.deadline", "Expired")
                return 1-clock[0]
            sock = DeadlineSocket(Sock(), remaining)
            with self.assertRule("backend.deadline"):
                sock.sendall(b"x"*(4*65536))
            self.assertEqual(timeouts, [1, .6, .19999999999999996] if cost == .4 else [1])
            self.assertTrue(all(0 < length <= 65536 for length in chunks))
            self.assertLessEqual(len(chunks), 3)

    def test_http_headers_and_upload_share_the_absolute_deadline(self):
        clock, timeouts, sends = [0], [], []
        class Sock:
            def getpeername(self): return "127.0.0.1", 1234
            def settimeout(self, value): timeouts.append(value)
            def sendall(self, data, flags=0): sends.append(len(data)); clock[0] += .4
            def close(self): pass
        class Connection(http.client.HTTPConnection):
            def connect(self): self.sock = Sock()
        client = BackendClient("http://127.0.0.1:1234", TOKEN, clock=lambda: clock[0], connection_factory=Connection)
        with self.assertRule("backend.deadline"):
            client.request("POST", "/v1/jobs", body={"image": "x"*(4*65536)}, deadline=1)
        self.assertEqual(sends[1:], [65536, 65536])
        self.assertTrue(all(0 < value <= 1 for value in timeouts))
        self.assertAlmostEqual(timeouts[-1], .2)

    def test_every_receive_reapplies_remaining_timeout_and_rejects_late_bytes(self):
        from luminumbra_author.meshgen_backend import DeadlineReader
        clock, timeouts, reads = [0], [], []
        class Stream:
            def read1(self, size): reads.append(size); clock[0] += .4; return b"x"
        class Sock:
            def settimeout(self, value): timeouts.append(value)
        def remaining():
            if clock[0] >= 1: raise Refusal("backend.deadline", "Expired")
            return 1-clock[0]
        reader = DeadlineReader(Stream(), Sock(), remaining)
        with self.assertRule("backend.deadline"):
            reader.read(10)
        self.assertEqual(reads, [10, 9, 8])
        self.assertEqual(timeouts, [1, .6, .19999999999999996])

    def test_all_mock_modes_have_explicit_portable_evidence(self):
        # Keep the inventory and its tested endpoint/rule expectations in sync.
        self.assertEqual(set(MODES), {"normal", "oversize", "nonfinite", "external_uri", "redirect", "nonloopback",
            "slow", "cancel", "crash", "bad_job_id", "bad_json", "duplicate_json", "deep_json", "bad_length",
            "duplicate_length", "chunked", "short_body", "card_change", "bad_hash"})

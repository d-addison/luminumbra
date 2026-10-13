"""Owned port-0 loopback mock and bounded transport refusal evidence."""
from copy import deepcopy
from email.message import Message
import http.client
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
sys.path.insert(0, str(SERVICE.parent / "probes"))
from luminumbra_author.contracts import Refusal, canonical, digest, parse_json
from luminumbra_author.meshgen_backend import (BackendClient, MAX_RESULT, MOCK_CAPABILITY,
    MOCK_CAPABILITY_DIGEST, bounded_json, image_snapshot, submission, validate_capability,
    validate_submission, token_from_source)
from luminumbra_author.meshgen_jobs import inspect_raw_glb
from luminumbra_author.meshgen_registry import defaults
from mock_backend import MockServer, synthetic_glb
from meshgen_http_fixture import owned_server, client_factory, MemoryMockServer, RuleAssertions

TOKEN = "test-only-token-" + "x"*32


def png(*, metadata=None, width=1, height=1):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind+payload) & 0xffffffff)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    if metadata:
        data += chunk(*metadata)
    return data + chunk(b"IDAT", zlib.compress(b"\0\0\0\0")) + chunk(b"IEND", b"")


def planning(image):
    return {"kind": "backend", "asset_class": "default", "backend_id": "mock",
            "image_ref": {"path": "ref.png", "sha256": digest(image)}, "declared_source": "own_work",
            "seed": 7, "params": {"shape": "cube"}, "request_id": "generate:fixture"}


def changed_glb(change):
    data = synthetic_glb()
    size = struct.unpack_from("<I", data, 12)[0]
    document = parse_json(data[20:20+size])
    change(document)
    payload = canonical(document)
    payload += b" " * (-len(payload) % 4)
    tail = data[20+size:]
    return struct.pack("<4sIIII", b"glTF", 2, 20+len(payload)+len(tail), len(payload), 0x4e4f534a) + payload + tail


class BackendTests(RuleAssertions, unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = png()
        (self.root / "ref.png").write_bytes(self.image)
        self.limits = defaults()["limits"]

    def server(self, mode="normal"):
        return owned_server(("127.0.0.1", 0), TOKEN, mode=mode)

    def client(self, server, **kwargs):
        return client_factory(server)(f"http://127.0.0.1:{server.server_port}", TOKEN, **kwargs)

    def wire(self):
        return submission(planning(self.image), {"bytes": self.image}, {"shape": "cube"}, 60)

    def test_five_endpoints_and_deterministic_shapes(self):
        with self.server() as server:
            client = self.client(server)
            self.assertEqual(validate_capability(client.capabilities(), "mock", {"kind": "mock"}), MOCK_CAPABILITY_DIGEST)
            identifier = client.submit(self.wire())
            self.assertEqual(identifier, client.submit(self.wire()))
            self.assertEqual(server.submissions, 1)
            status = client.status(identifier)
            data = client.result(identifier)
            self.assertEqual(status["result"]["sha256"], digest(data))
            self.assertIsNone(status["result"]["backend_reported"]["peak_vram_mib"])
            self.assertEqual(inspect_raw_glb(data, defaults())["triangles"], 12)
            self.assertEqual(client.cancel(identifier)["state"], "cancelled")
            self.assertEqual(server.cancellations, 1)
        for shape, count in (("cube", 12), ("icosphere", 20)):
            data = synthetic_glb(shape, 7)
            self.assertEqual(data, synthetic_glb(shape, 7))
            self.assertNotEqual(data, synthetic_glb(shape, 8))
            self.assertEqual(inspect_raw_glb(data, defaults())["triangles"], count)

    def test_exact_wire_grammar_and_base64_allowance(self):
        self.assertEqual(validate_submission(self.wire()), self.image)
        for key, value, rule in (("url", "http://example.invalid", "schema.unknown_field"), ("seed", True, "config.type"),
                           ("deadline_s", float("inf"), "config.out_of_bounds"), ("image_b64", "!!!!", "request.invalid"),
                           ("params", {"unknown": 1}, "config.unknown_key"), ("prompt", "unsupported", "request.invalid")):
            with self.subTest(key=key):
                wire = self.wire()
                wire[key] = value
                with self.assertRule(rule):
                    validate_submission(wire)
        wire = self.wire()
        wire["image_ref"]["sha256"] = "0"*64
        with self.assertRule("request.invalid"):
            validate_submission(wire)
        card = deepcopy(MOCK_CAPABILITY)
        card["inputs"]["image"]["max_bytes"] = 1
        with self.assertRule("request.limit"):
            validate_submission(self.wire(), card)
        client = BackendClient("http://127.0.0.1:1234", TOKEN)
        for method, path in (("GET", "/v1/jobs/test/cancel"), ("POST", "/v1/jobs/test/result"),
                             ("DELETE", "/v1/jobs/test"), ("GET", "/other")):
            with self.subTest(method=method, path=path), self.assertRule("backend.protocol"):
                client.request(method, path)

    def test_authentication_and_host_check(self):
        with self.server() as server:
            wrong = client_factory(server)(f"http://127.0.0.1:{server.server_port}", "z"*32)
            with self.assertRule("backend.unavailable"):
                wrong.capabilities()
            connection = (server.connection_factory() if isinstance(server, MemoryMockServer)
                          else http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=1))
            try:
                connection.request("GET", "/v1/capabilities", headers={"Host": "localhost", "Authorization": "Bearer " + TOKEN})
                self.assertEqual(connection.getresponse().status, 400)
            finally:
                connection.close()

    def test_literal_loopback_bind_and_no_proxy_routing(self):
        for endpoint in ("http://localhost:1234", "http://[::1]:1234", "http://127.1:1234", "https://127.0.0.1:1234",
                         "http://192.0.2.1:1234", "http://127.0.0.1:0", "http://127.0.0.1:65536", "http://127.0.0.1:1234/"):
            with self.subTest(endpoint=endpoint), self.assertRule("backend.loopback"):
                BackendClient(endpoint, TOKEN)
        with self.assertRule("backend.loopback"):
            MockServer(("0.0.0.0", 0), TOKEN)
        with patch.dict(os.environ, {"HTTP_PROXY": "http://192.0.2.1:1", "http_proxy": "http://192.0.2.1:1", "NO_PROXY": ""}):
            with self.server() as server:
                self.assertEqual(self.client(server).capabilities(), MOCK_CAPABILITY)

    def test_redirect_nonloopback_and_strict_framing_modes(self):
        expected = {"redirect": "backend.redirect", "nonloopback": "backend.redirect", "bad_length": "backend.framing",
                    "duplicate_length": "backend.framing", "chunked": "backend.framing", "short_body": "backend.framing",
                    "crash": "backend.unavailable", "bad_json": "backend.protocol", "duplicate_json": "json.duplicate", "deep_json": "backend.limit"}
        for mode, rule in expected.items():
            with self.subTest(mode=mode), self.server(mode) as server:
                with self.assertRaises(Refusal) as refusal:
                    self.client(server).capabilities()
                self.assertEqual(refusal.exception.finding["rule_id"], rule)

    def test_oversize_malformed_job_ids_and_geometry_refusals(self):
        with self.server("oversize") as server:
            client = self.client(server)
            identifier = client.submit(self.wire())
            with self.assertRaises(Refusal) as error:
                client.result(identifier)
            self.assertEqual(error.exception.finding["rule_id"], "backend.limit")
        with self.server("bad_job_id") as server:
            with self.assertRule("backend.protocol"):
                self.client(server).submit(self.wire())
        for mode, rule in (("nonfinite", "glb.nonfinite"), ("external_uri", "glb.external")):
            with self.subTest(mode=mode):
                with self.assertRaises(Refusal) as error:
                    inspect_raw_glb(synthetic_glb(mode=mode), defaults())
                self.assertEqual(error.exception.finding["rule_id"], rule)
        for identifier in ("../job", "a/b", "a?x", "%2f", "", "a"*65):
            with self.assertRule("backend.protocol"):
                BackendClient("http://127.0.0.1:1", TOKEN).status(identifier)

    def test_complete_card_changes_and_model_pins(self):
        card = deepcopy(MOCK_CAPABILITY)
        card["backend"]["id"] = "model"
        config = {"kind": "model", "pins": {"model_revision": "0"*40, "weights_sha256": ["1"*64]}}
        card["components"][0]["weights_sha256"] = ["1"*64]
        validate_capability(card, "model", config)
        for field, value in (("revision", "2"*40), ("weights_sha256", ["3"*64])):
            altered = deepcopy(card)
            altered["components"][0][field] = value
            with self.assertRule("backend.pin_mismatch"):
                validate_capability(altered, "model", config)
        with self.server("card_change") as server:
            client = self.client(server)
            validate_capability(client.capabilities(), "mock", {"kind": "mock"})
            with self.assertRule("backend.pin_mismatch"):
                validate_capability(client.capabilities(), "mock", {"kind": "mock"})

    def test_raw_accessor_bounds_encodings_indices_and_scene_expansion(self):
        changes = [
            (lambda doc: doc["accessors"][0].update(count=1000000), "glb.bounds"),
            (lambda doc: doc["accessors"][0].update(sparse={}), "glb.encoding"),
            (lambda doc: doc["bufferViews"][0].update(buffer=False), "glb.encoding"),
            (lambda doc: doc["meshes"][0]["primitives"][0].update(extensions={"KHR_draco_mesh_compression": {}}), "glb.encoding"),
            (lambda doc: doc["meshes"][0]["primitives"][0]["attributes"].update(POSITION=1), "glb.encoding"),
            (lambda doc: doc["nodes"][0].update(children=[0]), "glb.bounds"),
            (lambda doc: doc["nodes"][0].update(matrix=[1]*15), "glb.nonfinite"),
            (lambda doc: doc.update(meshes=[None]), "glb.structure"),
        ]
        for change, rule in changes:
            with self.subTest(change=change), self.assertRule(rule):
                inspect_raw_glb(changed_glb(change), defaults())
        data = bytearray(synthetic_glb())
        json_size = struct.unpack_from("<I", data, 12)[0]
        # Known cube has 8 positions (96 bytes), followed by u16 indices.
        struct.pack_into("<H", data, 28+json_size+96, 65535)
        with self.assertRule("glb.bounds"):
            inspect_raw_glb(data, defaults())
        effective = defaults()
        effective["budgets"]["max_tris_prefinish"] = 12
        def instances(doc):
            doc["nodes"].append({"mesh": 0})
            doc["scenes"][0]["nodes"].append(1)
        with self.assertRule("glb.limit"):
            inspect_raw_glb(changed_glb(instances), effective)

    def test_bounded_json_duplicate_nonfinite_depth_and_collections(self):
        for data, rule in ((b'{"x":1,"x":2}', "json.duplicate"), (b'{"x":NaN}', "json.nonfinite"),
                           (b'{"x":1e999}', "backend.protocol"), (b"["*100+b"0"+b"]"*100, "backend.limit"),
                           (b" "*(1024**2+1), "backend.limit")):
            with self.subTest(data=data[:20]), self.assertRule(rule):
                bounded_json(data)
        with self.assertRule("config.out_of_bounds"):
            bounded_json(b"[1,2,3]", items=2)

    def test_image_snapshot_digest_dimensions_metadata_and_rights(self):
        descriptor = planning(self.image)["image_ref"]
        snapshot = image_snapshot(self.root, descriptor, self.limits, "refuse_if_ai_training_reserved")
        self.assertEqual(snapshot["bytes"], self.image)
        self.assertEqual(snapshot["credentials"], "absent")
        with self.assertRule("licence.image_unknown"):
            image_snapshot(self.root, descriptor, self.limits, "refuse_unknown")
        for value in ("notAllowed", "constrained", "unrecognized"):
            image = png(metadata=(b"tEXt", b"cawg.training-and-data-mining\0" + canonical({"cawg.ai_training": value})))
            (self.root / "ref.png").write_bytes(image)
            with self.assertRule("licence.image_reserved"):
                image_snapshot(self.root, {"path": "ref.png", "sha256": digest(image)}, self.limits, "refuse_if_ai_training_reserved")
        for image, rule in ((png(width=16385), "request.limit"), (png(width=4097, height=4096), "request.limit"),
                            (png(metadata=(b"caBX", b"unsupported C2PA")), "licence.image_credentials")):
            (self.root / "ref.png").write_bytes(image)
            with self.assertRule(rule):
                image_snapshot(self.root, {"path": "ref.png", "sha256": digest(image)}, self.limits, "refuse_if_ai_training_reserved")
        with self.assertRule("request.conflict"):
            image_snapshot(self.root, descriptor, self.limits, "refuse_if_ai_training_reserved")

    def test_tokens_are_references_only_and_not_in_wire(self):
        with patch.dict(os.environ, {"FIXTURE_TOKEN": TOKEN}):
            self.assertEqual(token_from_source(self.root, {"env": "FIXTURE_TOKEN"}), TOKEN)
        self.assertNotIn(TOKEN.encode(), canonical(self.wire()))
        (self.root / "token").write_text(TOKEN)
        self.assertEqual(token_from_source(self.root, {"file": "token"}), TOKEN)


class StreamingTests(RuleAssertions, unittest.TestCase):
    def test_absolute_deadline_also_bounds_drip_fed_headers(self):
        from luminumbra_author.meshgen_backend import DeadlineReader
        clock = [0]
        class Stream:
            def read1(self, size):
                clock[0] += .4
                return b"x"
        class Sock:
            def settimeout(self, value):
                pass
        def remaining():
            if clock[0] >= 1:
                raise Refusal("backend.deadline", "Expired")
            return 1-clock[0]
        reader = DeadlineReader(Stream(), Sock(), remaining)
        with self.assertRule("backend.deadline"):
            reader.readline(65537)
        self.assertLess(clock[0], 1.7)

    def test_runtime_peer_and_absolute_deadline_use_bounded_reads(self):
        class Sock:
            def __init__(self, peer):
                self.peer = peer
            def getpeername(self):
                return self.peer, 1234
            def settimeout(self, value):
                pass
        class Response:
            status = 200
            def __init__(self, clock):
                self.headers = Message()
                self.headers["Content-Length"] = "10"
                self.headers["Content-Type"] = "model/gltf-binary"
                self.clock, self.reads = clock, []
            def read1(self, size):
                self.reads.append(size)
                self.clock[0] += .4
                return b"x"
        class Connection:
            def __init__(self, peer, clock):
                self.sock, self.response = Sock(peer), Response(clock)
                self.closed = False
            def connect(self):
                pass
            def request(self, *args, **kwargs):
                pass
            def getresponse(self):
                return self.response
            def close(self):
                self.closed = True
        for peer, rule in (("192.0.2.1", "backend.loopback"), ("127.0.0.1", "backend.deadline")):
            clock = [0]
            connection = Connection(peer, clock)
            calls = []
            def factory(*args, **kwargs):
                calls.append((args, kwargs))
                return connection
            client = BackendClient("http://127.0.0.1:1234", TOKEN, clock=lambda: clock[0], connection_factory=factory)
            with self.assertRaises(Refusal) as error:
                client.result("fixture", deadline=1)
            self.assertEqual(error.exception.finding["rule_id"], rule)
            self.assertEqual(calls[0][1]["source_address"], ("127.0.0.1", 0))
            self.assertTrue(connection.closed)
            self.assertTrue(all(0 < size <= 65536 for size in connection.response.reads))
            self.assertLessEqual(len(connection.response.reads), 3)


if __name__ == "__main__":
    unittest.main()

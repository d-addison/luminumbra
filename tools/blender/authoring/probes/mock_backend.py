"""Owned stdlib MG2 mock. Synthetic fixtures only; never loads a model.

Fault mode is launcher-controlled, never a request parameter. Bind explicitly
to 127.0.0.1 (or ::1 for listener tests), and use port 0 in portable tests.
"""
from copy import deepcopy
import argparse
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "service"))
from luminumbra_author.contracts import Refusal, canonical, digest, require
from luminumbra_author.meshgen_backend import (MAX_RESULT, MAX_UPLOAD, MOCK_CAPABILITY,
    DeadlineReader, bounded_json, job_id, validate_submission)

MODES = ("normal", "oversize", "nonfinite", "external_uri", "redirect", "nonloopback",
         "slow", "cancel", "crash", "bad_job_id", "bad_json", "duplicate_json", "deep_json",
         "bad_length", "duplicate_length", "chunked", "short_body", "card_change", "bad_hash")


MAX_RETAINED_JOBS = 256
MAX_RETAINED_BYTES = 64 * 1024**2

def synthetic_glb(shape="cube", seed=0, *, mode="normal"):
    """A cube or an icosphere at subdivision zero, generated without dependencies."""
    if shape == "cube":
        positions = [(x, y, z) for z in (-.5, .5) for y in (-.5, .5) for x in (-.5, .5)]
        faces = [(0, 2, 1), (1, 2, 3), (4, 5, 6), (5, 7, 6), (0, 1, 4), (1, 5, 4),
                 (2, 6, 3), (3, 6, 7), (0, 4, 2), (2, 4, 6), (1, 3, 5), (3, 7, 5)]
    else:
        require(shape == "icosphere", "request.invalid", "Unknown synthetic shape.")
        phi = (1 + 5 ** .5) / 2
        positions = [(-1, phi, 0), (1, phi, 0), (-1, -phi, 0), (1, -phi, 0),
                     (0, -1, phi), (0, 1, phi), (0, -1, -phi), (0, 1, -phi),
                     (phi, 0, -1), (phi, 0, 1), (-phi, 0, -1), (-phi, 0, 1)]
        positions = [tuple(component / (1 + phi*phi) ** .5 for component in vertex) for vertex in positions]
        faces = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11),
                 (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
                 (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9),
                 (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]
    if mode == "nonfinite":
        positions[0] = (float("nan"), 0, 0)
    vertices = b"".join(struct.pack("<3f", *vertex) for vertex in positions)
    indices = b"".join(struct.pack("<3H", *face) for face in faces)
    binary = vertices + indices
    document = {"asset": {"version": "2.0", "generator": "synthetic-mock"},
        "extras": {"synthetic_seed": seed}, "buffers": [{"byteLength": len(binary)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": len(vertices)},
                        {"buffer": 0, "byteOffset": len(vertices), "byteLength": len(indices)}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "type": "VEC3", "count": len(positions)},
                      {"bufferView": 1, "componentType": 5123, "type": "SCALAR", "count": len(faces)*3}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "mode": 4}]}],
        "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0}
    if mode == "external_uri":
        document["buffers"][0]["uri"] = "https://invalid.example/never-fetch"
    payload = canonical(document)
    payload += b" " * (-len(payload) % 4)
    binary += b"\0" * (-len(binary) % 4)
    return (struct.pack("<4sII", b"glTF", 2, 28+len(payload)+len(binary))
            + struct.pack("<II", len(payload), 0x4e4f534a) + payload
            + struct.pack("<II", len(binary), 0x004e4942) + binary)


class MockServer(ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = True

    def __init__(self, address, token, *, mode="normal", clock=time.monotonic):
        require(address[0] in ("127.0.0.1", "::1"), "backend.loopback", "Mock must bind to a literal loopback address.")
        require(type(address[1]) is int and 0 <= address[1] <= 65535, "request.invalid", "Invalid mock port.")
        require(mode in MODES, "request.invalid", "Unknown mock fault mode.")
        require(isinstance(token, str) and len(token) >= 32 and token.isascii()
                and all(33 <= ord(char) <= 126 for char in token), "session.token", "Invalid mock token.")
        if address[0] == "::1":
            self.address_family = socket.AF_INET6
        self.token, self.mode = token, mode
        self.clock, self.connections = clock, threading.BoundedSemaphore(4)
        self.jobs, self.requests = {}, {}
        self.record_limit, self.byte_limit = MAX_RETAINED_JOBS, MAX_RETAINED_BYTES
        self.mutex = threading.RLock()
        self.submissions, self.cancellations, self.capability_reads = 0, 0, 0
        # HTTPServer.server_bind does a reverse lookup for server_name. Avoid
        # even that lookup: the fixture has an explicit literal loopback bind.
        super().__init__(address, Handler, bind_and_activate=False)
        self.socket.bind(address)
        self.server_address = self.socket.getsockname()
        self.server_name, self.server_port = address[0], self.server_address[1]
        self.server_activate()

    def handle_error(self, request, client_address):
        pass  # Deliberate crash/truncation fixtures do not dump request/token logs.

    def process_request(self, request, client_address):
        if not self.connections.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.connections.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.connections.release()

    def __enter__(self):
        self.thread = threading.Thread(target=self.serve_forever, kwargs={"poll_interval": .02}, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, *args):
        self.shutdown()
        self.server_close()
        self.thread.join()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(5)
        deadline = self.server.clock() + 5
        def remaining():
            value = deadline - self.server.clock()
            require(value > 0, "backend.deadline", "Mock HTTP request deadline expired.")
            return value
        self.rfile = DeadlineReader(self.rfile, self.connection, remaining)

    def log_message(self, *args):
        pass  # Tokens, images and prompts are never logged.

    def response(self, payload, *, status=200, binary=False):
        data = payload if binary else canonical(payload)
        self.send_response(status)
        self.send_header("Content-Type", "model/gltf-binary" if binary else "application/json")
        length = len(data)
        if binary and self.server.mode == "oversize":
            length = MAX_RESULT + 1
        if self.server.mode == "bad_length":
            self.send_header("Content-Length", "-1")
        else:
            self.send_header("Content-Length", str(length))
        if self.server.mode == "duplicate_length":
            self.send_header("Content-Length", str(length))
        if self.server.mode == "chunked":
            self.send_header("Transfer-Encoding", "chunked")
        if status == 302:
            self.send_header("Location", "http://192.0.2.1:1/never-contact")
        self.end_headers()
        if self.server.mode == "short_body":
            data = data[:max(1, len(data)//2)]
            self.close_connection = True
        self.wfile.write(data)

    def do_GET(self):
        self.handle_request("GET")

    def do_POST(self):
        self.handle_request("POST")

    def handle_request(self, method):
        try:
            host = self.server.server_address[0]
            authority = f"[{host}]:{self.server.server_port}" if host == "::1" else f"{host}:{self.server.server_port}"
            require(self.headers.get_all("Host", []) == [authority], "backend.loopback", "Wrong Host header.")
            credentials = self.headers.get_all("Authorization", [])
            require(len(credentials) == 1 and hmac.compare_digest(credentials[0], "Bearer " + self.server.token),
                    "session.unauthorized", "Wrong bearer token.")
            require(not self.headers.get_all("Transfer-Encoding"), "backend.framing", "Chunked uploads are unsupported.")
            lengths = self.headers.get_all("Content-Length", [])
            if method == "POST":
                require(len(lengths) == 1 and re.fullmatch(r"[0-9]{1,12}", lengths[0])
                        and 0 < int(lengths[0]) <= MAX_UPLOAD and self.headers.get_all("Content-Type", []) == ["application/json"],
                        "backend.framing", "Invalid bounded upload framing.")
                pieces, received = [], 0
                while received < int(lengths[0]):
                    chunk = self.rfile.read1(min(65536, int(lengths[0])-received))
                    require(bool(chunk), "backend.framing", "Truncated upload.")
                    pieces.append(chunk)
                    received += len(chunk)
                data = b"".join(pieces)
                require(len(data) == int(lengths[0]), "backend.framing", "Truncated upload.")
                # Large image upload has its own calculated cap, while JSON
                # depth/collection bounds still apply to the request structure.
                body = bounded_json(data, max_bytes=MAX_UPLOAD)
            else:
                require(not lengths or lengths == ["0"], "backend.framing", "GET body is unsupported.")
                body = None
            self.route(method, body)
        except (Refusal, ValueError, OSError, KeyError, TypeError, RecursionError):
            self.close_connection = True
            self.response({"error": "request_refused"}, status=400)

    def route(self, method, body):
        server = self.server
        if server.mode == "crash":
            self.connection.shutdown(socket.SHUT_RDWR)
            self.close_connection = True
            return
        if server.mode in ("redirect", "nonloopback"):
            self.response({"error": "redirect"}, status=302)
            return
        with server.mutex:
            if method == "GET" and self.path == "/v1/capabilities":
                server.capability_reads += 1
                card = deepcopy(MOCK_CAPABILITY)
                if server.mode == "card_change" and server.capability_reads > 1:
                    card["determinism"]["notes"] = "changed card"
                if server.mode in ("bad_json", "duplicate_json", "deep_json"):
                    wire = {"bad_json": b"{", "duplicate_json": b'{"contract":1,"contract":2}',
                            "deep_json": b"["*40 + b"0" + b"]"*40}[server.mode]
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(wire)))
                    self.end_headers()
                    self.wfile.write(wire)
                else:
                    self.response(card)
                return
            if method == "POST" and self.path == "/v1/jobs":
                validate_submission(body)
                request_digest = digest(canonical(body))
                previous = server.requests.get(body["request_id"])
                if previous:
                    require(previous["digest"] == request_digest, "request.conflict", "Conflicting backend replay.")
                    self.response({"job_id": previous["job_id"]})
                    return
                require(not any(item["state"] in ("running", "queued") for item in server.jobs.values()), "busy", "Mock has an outstanding job.")
                identifier = digest(body["request_id"].encode())[:32]
                data = synthetic_glb(body["params"].get("shape", "cube"), body["seed"], mode=server.mode)
                require(len(server.requests) < server.record_limit and len(server.jobs) < server.record_limit,
                        "request.limit", "Mock replay ledger is full; records are never evicted.")
                retained = sum(len(item["data"]) + len(canonical(item["result"])) + 4096 for item in server.jobs.values())
                require(retained + len(data) + 8192 <= server.byte_limit, "request.limit", "Mock retained byte quota is full.")
                metadata = {"sha256": "0"*64 if server.mode == "bad_hash" else digest(data), "bytes": len(data),
                            "backend_reported": {"seed": body["seed"], "steps_run": 0, "wall_s": 0, "peak_vram_mib": None},
                            "logs_tail": "synthetic mock; GPU measurements unavailable"}
                server.jobs[identifier] = {"state": "running" if server.mode == "slow" else "cancelled" if server.mode == "cancel" else "succeeded",
                                           "data": data, "result": metadata}
                server.requests[body["request_id"]] = {"job_id": identifier, "digest": request_digest}
                server.submissions += 1
                self.response({"job_id": "../invalid" if server.mode == "bad_job_id" else identifier})
                return
            match = re.fullmatch(r"/v1/jobs/([A-Za-z0-9][A-Za-z0-9_-]{0,63})(/cancel|/result)?", self.path)
            require(match is not None, "backend.protocol", "Unknown endpoint.")
            identifier, suffix = job_id(match[1]), match[2]
            require(identifier in server.jobs, "backend.protocol", "Unknown mock job.")
            record = server.jobs[identifier]
            if method == "POST" and suffix == "/cancel":
                require(body == {}, "schema.unknown_field", "Cancel body must be empty.")
                record["state"] = "cancelled"
                server.cancellations += 1
                self.response({"job_id": identifier, "state": "cancelled"})
            elif method == "GET" and suffix == "/result":
                require(record["state"] == "succeeded", "stage.out_of_order", "Mock result is unavailable.")
                self.response(record["data"], binary=True)
            elif method == "GET" and suffix is None:
                status = {"job_id": identifier, "state": record["state"]}
                if record["state"] == "succeeded":
                    status["result"] = record["result"]
                self.response(status)
            else:
                raise Refusal("backend.protocol", "Unsupported endpoint method.")


if __name__ == "__main__":
    import os
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", choices=("127.0.0.1", "::1"), default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--mode", choices=MODES, default="normal")
    args = parser.parse_args()
    with MockServer((args.bind, args.port), os.environ.pop("LUMINUMBRA_GEN_TOKEN", ""), mode=args.mode) as server:
        print(canonical({"bind": args.bind, "port": server.server_port, "synthetic": True}).decode(), flush=True)
        try:
            server.thread.join()
        except KeyboardInterrupt:
            pass

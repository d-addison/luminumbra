"""Socket-free wire harness for explicitly network-disabled developer runs.

CI defaults to the real port-0 loopback listener. This harness preserves the
mock HTTP handler and stdlib HTTPResponse framing parser, but cannot qualify
kernel binding, peer checks or socket timeouts.
"""
import http.client
import io
import os
import threading
import time

from mock_backend import Handler, MockServer, MAX_RETAINED_JOBS, MAX_RETAINED_BYTES


class WireSocket:
    def __init__(self, data=b""):
        self.inbound, self.outbound = io.BytesIO(data), io.BytesIO()
    def makefile(self, mode, *args):
        return self.inbound
    def sendall(self, data, flags=0):
        self.outbound.write(data)
    def settimeout(self, value):
        pass
    def getpeername(self):
        return "127.0.0.1", 1
    def shutdown(self, how):
        pass
    def close(self):
        pass


class MemoryMockServer:
    def __init__(self, address, token, *, mode="normal"):
        from luminumbra_author.contracts import require
        require(address[0] in ("127.0.0.1", "::1"), "backend.loopback", "Loopback is required.")
        self.server_address, self.server_port = (address[0], 12345), 12345
        self.token, self.mode = token, mode
        self.clock = time.monotonic
        self.jobs, self.requests = {}, {}
        self.record_limit, self.byte_limit = MAX_RETAINED_JOBS, MAX_RETAINED_BYTES
        self.mutex = threading.RLock()
        self.submissions = self.cancellations = self.capability_reads = 0

    def connection_factory(self, *args, **kwargs):
        return MemoryConnection(self)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass


class MemoryConnection:
    def __init__(self, server):
        self.server, self.sock, self.response_socket = server, WireSocket(), None
    def connect(self):
        pass
    def request(self, method, path, body=None, headers=None):
        headers = dict(headers or {})
        headers.setdefault("Host", f"127.0.0.1:{self.server.server_port}")
        headers["Connection"] = "close"
        if body is not None:
            headers["Content-Length"] = str(len(body))
        wire = f"{method} {path} HTTP/1.1\r\n".encode()
        wire += b"".join(f"{key}: {value}\r\n".encode() for key, value in headers.items()) + b"\r\n" + (body or b"")
        request_socket = WireSocket(wire)
        Handler(request_socket, ("127.0.0.1", 1), self.server)
        self.response_socket = WireSocket(request_socket.outbound.getvalue())
    def getresponse(self):
        response = http.client.HTTPResponse(self.response_socket)
        response.begin()
        return response
    def close(self):
        pass


def owned_server(address, token, *, mode="normal"):
    implementation = MemoryMockServer if os.environ.get("LUMINUMBRA_TEST_NO_SOCKETS") == "1" else MockServer
    return implementation(address, token, mode=mode)


def client_factory(server):
    from luminumbra_author.meshgen_backend import BackendClient
    def create(*args, **kwargs):
        if isinstance(server, MemoryMockServer):
            kwargs["connection_factory"] = server.connection_factory
        return BackendClient(*args, **kwargs)
    return create


class RuleAssertions:
    def assertRule(self, rule):
        from contextlib import contextmanager
        from luminumbra_author.contracts import Refusal
        @contextmanager
        def assertion():
            with self.assertRaises(Refusal) as refusal:
                yield refusal
            self.assertEqual(refusal.exception.finding["rule_id"], rule)
        return assertion()
